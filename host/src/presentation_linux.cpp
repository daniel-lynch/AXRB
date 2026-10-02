// Linux counterpart of presentation.cpp: a stereo projection swapchain on the
// runtime's Vulkan device. Each frame copies the newest received image (shared
// GPU copy or pixel transfer) into the swapchain and submits it with the
// guest's render poses and FOVs, so the runtime reprojects as it would for a
// native application. Quad, equirect and multi-layer frames are not yet ported.
#include "openxr_session.h"
#if !defined(_WIN32)

namespace axrb::host::detail {

bool OpenXrSession::create_projection_swapchain()
{
    uint32_t viewCount = 0;
    if (enumerateViewConfigurationViews_(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            0, &viewCount, nullptr) != XR_SUCCESS || viewCount != 2) return false;
    std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
    if (enumerateViewConfigurationViews_(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            2, &viewCount, views.data()) != XR_SUCCESS) return false;
    for (const auto& view : views) {
        projectionWidth_ = (std::max)(projectionWidth_, view.recommendedImageRectWidth);
        projectionHeight_ = (std::max)(projectionHeight_, view.recommendedImageRectHeight);
    }
    // Benchmark/compatibility override of the eye extent the guest renders at.
    if (const char* extent = std::getenv("AXRB_EYE_EXTENT")) {
        unsigned width = 0, height = 0;
        char trailing = 0;
        if (std::sscanf(extent, "%ux%u%c", &width, &height, &trailing) != 2 ||
            !axrb::protocol::valid_render_extent(width, height)) return false;
        projectionWidth_ = width;
        projectionHeight_ = height;
    }
    if (!axrb::protocol::valid_render_extent(projectionWidth_, projectionHeight_)) {
        std::fprintf(stderr, "AXRB OpenXR: unsupported eye extent %ux%u\n", projectionWidth_, projectionHeight_);
        return false;
    }
    uint32_t formatCount = 0;
    if (enumerateSwapchainFormats_(session_, 0, &formatCount, nullptr) != XR_SUCCESS || !formatCount) return false;
    std::vector<int64_t> formats(formatCount);
    if (enumerateSwapchainFormats_(session_, formatCount, &formatCount, formats.data()) != XR_SUCCESS) return false;
    projectionFormat_ = 0;
    for (int64_t preferred : {int64_t(VK_FORMAT_R8G8B8A8_SRGB), int64_t(VK_FORMAT_B8G8R8A8_SRGB),
                              int64_t(VK_FORMAT_R8G8B8A8_UNORM), int64_t(VK_FORMAT_B8G8R8A8_UNORM)}) {
        if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) { projectionFormat_ = preferred; break; }
    }
    if (!projectionFormat_) {
        std::fprintf(stderr, "AXRB OpenXR: runtime offers no 8-bit RGBA swapchain format\n");
        return false;
    }
    if (!ensure_layer_swapchains(1)) return false;
    std::fprintf(stderr, "AXRB OpenXR: Vulkan layer swapchains %ux%u x2 format=%lld color_scale_bias=%s\n",
        projectionWidth_, projectionHeight_, static_cast<long long>(projectionFormat_), colorScaleBiasEnabled_ ? "native" : "unavailable");
    return true;
}

bool OpenXrSession::ensure_layer_swapchains(size_t count)
{
    while (layerSwapchains_.size() < count) {
        LayerSwapchain layer;
        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
            XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        info.format = projectionFormat_;
        info.sampleCount = 1;
        info.width = projectionWidth_;
        info.height = projectionHeight_;
        info.faceCount = 1;
        info.arraySize = 2;
        info.mipCount = 1;
        XrResult result = createSwapchain_(session_, &info, &layer.swapchain);
        if (result != XR_SUCCESS) {
            std::fprintf(stderr, "AXRB OpenXR: xrCreateSwapchain failed: %s (%d)\n", xr_result_name(result), result);
            return false;
        }
        uint32_t imageCount = 0;
        if (enumerateSwapchainImages_(layer.swapchain, 0, &imageCount, nullptr) != XR_SUCCESS || !imageCount) {
            destroySwapchain_(layer.swapchain); return false;
        }
        layer.images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (enumerateSwapchainImages_(layer.swapchain, imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(layer.images.data())) != XR_SUCCESS) {
            destroySwapchain_(layer.swapchain); return false;
        }
        layer.uploadedSequence.assign(imageCount, UINT64_MAX);
        layerSwapchains_.push_back(std::move(layer));
        if (layerSwapchains_.size() > 1)
            std::fprintf(stderr, "AXRB OpenXR: layer swapchain %zu created\n", layerSwapchains_.size());
    }
    return true;
}

bool OpenXrSession::update_projection_layers(XrTime, std::vector<const XrCompositionLayerBaseHeader*>& layers)
{
    static axrb::protocol::PerfStats stats("host-projection");
    axrb::protocol::PerfScope scope(stats);
    using namespace axrb::protocol;
    HostImageSnapshot frame;
    if (imageFrame_) frame = imageFrame_->snapshot();
    const auto& header = frame.header;
    if (!frame.pixels || header.version == kEmptyImageFrameVersion || !header.width || !header.height) return false;

    // Every layer of the frame, in application order: received GPU images, or
    // the single layer of a pixel-transfer frame.
    struct Part {
        const ImageFrameHeader* header;
        const ImageProjection* projection;
        const HostImage* image;
    };
    std::vector<Part> parts;
    if (frame.gpu) {
        for (const auto& part : frame.gpu->parts) parts.push_back({&part.header, &part.projection, part.image.get()});
    } else if (header.version == kImageFrameVersion) {
        return false; // Legacy frames carry no projection metadata.
    } else {
        parts.push_back({&header, &frame.projection, nullptr});
    }
    if (parts.empty() || !ensure_layer_swapchains(parts.size())) return false;
    if (layerStorage_.size() < parts.size()) layerStorage_.resize(parts.size());

    struct Release {
        PFN_xrReleaseSwapchainImage release;
        std::vector<XrSwapchain> acquired;
        ~Release() {
            for (auto swapchain : acquired) {
                XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                release(swapchain, &info);
            }
        }
    } release{releaseSwapchainImage_, {}};

    bool hasProjection = false;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& part = parts[i];
        const auto& metadata = *part.projection;
        const uint32_t width = part.header->width, height = part.header->height;
        if (width > projectionWidth_ || height > projectionHeight_) return false;
        auto& target = layerSwapchains_[i];
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (acquireSwapchainImage_(target.swapchain, &acquireInfo, &imageIndex) != XR_SUCCESS ||
            imageIndex >= target.images.size()) return false;
        release.acquired.push_back(target.swapchain);
        XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        waitInfo.timeout = XR_INFINITE_DURATION;
        {
            static axrb::protocol::PerfStats waitStats("host-swapchain-wait");
            axrb::protocol::PerfScope waitScope(waitStats);
            if (waitSwapchainImage_(target.swapchain, &waitInfo) != XR_SUCCESS) return false;
        }
        if (target.uploadedSequence[imageIndex] != header.sequence) {
            static axrb::protocol::PerfStats copyStats("host-texture-upload");
            axrb::protocol::PerfScope copyScope(copyStats);
            target.uploadedSequence[imageIndex] = UINT64_MAX;
            // Opt-in diagnostics, as on Windows: four left-eye captures of the
            // first layer, five seconds apart, after loading.
            static const std::string capturePrefix = [] { const char* p = std::getenv("AXRB_CAPTURE_PREFIX"); return p ? std::string(p) : std::string(); }();
            static unsigned captured = 0;
            static auto previousCapture = std::chrono::steady_clock::now();
            std::string capturePath;
            if (!capturePrefix.empty() && i == 0 && captured < 4 && header.sequence >= 2000 &&
                std::chrono::steady_clock::now() - previousCapture >= std::chrono::seconds(5)) {
                previousCapture = std::chrono::steady_clock::now();
                capturePath = capturePrefix + "-" + std::to_string(captured++) + "-" + std::to_string(header.sequence) + ".ppm";
            }
            if (!vulkan_->fill(target.images[imageIndex].image, static_cast<VkFormat>(projectionFormat_), width, height,
                    part.image, part.image ? nullptr : frame.pixels.get(), part.header->layers,
                    capturePath.empty() ? nullptr : capturePath.c_str())) return false;
            target.uploadedSequence[imageIndex] = header.sequence;
        }

        auto& storage = layerStorage_[i];
        const XrExtent2Di extent{static_cast<int32_t>(width), static_cast<int32_t>(height)};
        // Fades and tints: the runtime applies them natively when it can.
        auto chainColor = [&](uint32_t slot, uint32_t eye) -> const void* {
            const auto& color = metadata.colors[eye];
            if (color.identity()) return nullptr;
            if (!colorScaleBiasEnabled_) {
                static bool reported = false;
                if (!reported) { std::fprintf(stderr, "AXRB OpenXR: runtime lacks color scale/bias; layer fades are dropped\n"); reported = true; }
                return nullptr;
            }
            auto& out = storage.colors[slot];
            out = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
            out.colorScale = {color.scale[0], color.scale[1], color.scale[2], color.scale[3]};
            out.colorBias = {color.bias[0], color.bias[1], color.bias[2], color.bias[3]};
            return &out;
        };
        if (metadata.is_equirect()) {
            if (!equirectEnabled_) continue;
            const auto& source = metadata.equirect;
            auto& sphere = storage.sphere;
            sphere = {XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR};
            sphere.next = chainColor(0, 0);
            sphere.space = localSpace_;
            sphere.layerFlags = source.layer_flags;
            sphere.eyeVisibility = static_cast<XrEyeVisibility>(source.eye_visibility);
            sphere.pose.position = {source.pose.x, source.pose.y, source.pose.z};
            sphere.pose.orientation = {source.pose.qx, source.pose.qy, source.pose.qz, source.pose.qw};
            sphere.radius = source.radius;
            sphere.centralHorizontalAngle = source.horizontal_angle;
            sphere.upperVerticalAngle = source.upper_angle;
            sphere.lowerVerticalAngle = source.lower_angle;
            sphere.subImage.swapchain = target.swapchain;
            sphere.subImage.imageRect.extent = extent;
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&sphere));
        } else if (const uint32_t quads = metadata.quad_count()) {
            for (uint32_t q = 0; q < quads && q < 2; ++q) {
                const auto& source = metadata.quads[q];
                auto& quad = storage.quads[q];
                quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
                quad.next = chainColor(q, q);
                quad.space = localSpace_;
                quad.layerFlags = source.layer_flags;
                quad.eyeVisibility = static_cast<XrEyeVisibility>(source.eye_visibility);
                quad.pose.position = {source.pose.x, source.pose.y, source.pose.z};
                quad.pose.orientation = {source.pose.qx, source.pose.qy, source.pose.qz, source.pose.qw};
                quad.size = {source.width, source.height};
                quad.subImage.swapchain = target.swapchain;
                quad.subImage.imageArrayIndex = q;
                quad.subImage.imageRect.extent = extent;
                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
            }
        } else if (metadata.view_count == 2) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                auto& view = storage.views[eye];
                const auto& source = metadata.views[eye];
                view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                view.pose.position = {source.pose.x, source.pose.y, source.pose.z};
                view.pose.orientation = {source.pose.qx, source.pose.qy, source.pose.qz, source.pose.qw};
                view.fov = {source.angle_left, source.angle_right, source.angle_up, source.angle_down};
                view.subImage.swapchain = target.swapchain;
                view.subImage.imageArrayIndex = eye;
                view.subImage.imageRect.extent = extent;
            }
            // The extension applies per layer; a per-eye difference is not representable.
            storage.projection = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            storage.projection.next = chainColor(0, 0);
            storage.projection.space = metadata.is_view_space() ? viewSpace_ : localSpace_;
            storage.projection.layerFlags = metadata.layer_flags & kCompositionLayerFlagsMask;
            storage.projection.viewCount = 2;
            storage.projection.views = storage.views.data();
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&storage.projection));
            hasProjection = true;
        }
    }
    submittedSequence_ = header.sequence;
    if (!reportedProjectionSubmit_ && !layers.empty()) {
        std::fprintf(stderr, "AXRB OpenXR: submitting %zu %s layer(s) %ux%u to the runtime\n",
            layers.size(), frame.gpu ? "shared-GPU" : "pixel-transfer", header.width, header.height);
        reportedProjectionSubmit_ = true;
    }
    return hasProjection;
}

} // namespace axrb::host::detail
#endif
