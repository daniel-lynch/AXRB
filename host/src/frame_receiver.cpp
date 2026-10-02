#include "openxr_session.h"
#include "gpu_frame_packet.h"

namespace axrb::host::detail {

bool OpenXrSession::receive_image(const axrb::protocol::ImageFrameHeader& header,
                   const axrb::protocol::ImageProjection& projection,
                   std::vector<uint8_t>&& pixels)
{
#if defined(_WIN32)
    static axrb::protocol::PerfStats stats("host-receive-image");
    axrb::protocol::PerfScope scope(stats);
    std::unique_lock handoffLock(frameHandoffMutex_, std::defer_lock);
    {
        static axrb::protocol::PerfStats lockStats("host-receive-lock");
        axrb::protocol::PerfScope lockScope(lockStats);
        if (!concurrentGpuFrames_) handoffLock.lock();
    }
    if (header.version == axrb::protocol::kEmptyImageFrameVersion) {
        pendingGpuFrame_.reset(); pendingMixedCount_ = 0;
        imageFrame_->store(header, {}, {});
        gpuFrames_.trim([](auto& slot) { slot.trim(0); });
        return true;
    }
    if (header.version == axrb::protocol::kGpuBatchFrameVersion) {
        using namespace axrb::protocol;
        pendingGpuFrame_.reset(); pendingMixedCount_ = 0;
        if (!valid_gpu_batch(header, pixels.data(), pixels.size())) return false;
        std::vector<GpuBatchPart> parts(header.reserved);
        for (uint32_t i=0;i<header.reserved;++i)
            parts[i]=decode_gpu_batch_part(pixels.data(),pixels.size()/header.reserved,i);
        // Validate every part before issuing any GPU work.
        for (uint32_t i = 0; i < header.reserved; ++i)
            if (parts[i].header.width > projectionWidth_ || parts[i].header.height > projectionHeight_) return false;
        auto frame = gpuFrames_.acquire();
        if (!frame) return false;
        frame->resize(header.reserved);
        bool queued = true;
        for (uint32_t i = 0; i < frame->count; ++i) {
            const auto& input = parts[i];
            auto& part = frame->parts[i];
            if (!part.receiver.enqueue_receive(receiveDevice_.get(), receiveContext_.get(), input.gpu,
                    input.header.sequence, input.header.width, input.header.height, static_cast<DXGI_FORMAT>(projectionFormat_),
                    &receiveColorRenderer_,&input.projection)) {
                queued = false; break;
            }
            part.header = input.header; part.projection = input.projection;
        }
        // Also drain a partial enqueue before retiring its storage. Never ACK
        // or publish until the complete frame has finished copying.
        const bool completed = batchReceiveCompletion_.wait(receiveDevice_.get(), receiveContext_.get());
        if (!queued || !completed) { gpuFrames_.retire(frame); return false; }
        for (uint32_t i = 0; i < frame->count; ++i)
            frame->parts[i].receiver.commit_receive(parts[i].header.sequence);
        auto complete = parts[0].header; complete.sequence = header.sequence;
        std::vector<uint8_t> first(sizeof(WindowsGpuFrame));
        std::memcpy(first.data(), &parts[0].gpu, first.size());
        imageFrame_->store(complete, std::move(first), parts[0].projection, frame);
        gpuFrames_.trim([count = header.reserved](auto& slot) { slot.trim(count); });
        static bool reported = false;
        if (!reported) { std::fprintf(stderr, "AXRB GPU: whole-frame receive and acknowledgment active\n"); reported = true; }
        return true;
    }
    const bool mixed = axrb::protocol::mixed_gpu_version(header.version);
    const bool gpuFrame = mixed || header.version == axrb::protocol::kWindowsGpuFrameVersion ||
                                  axrb::protocol::equirect_gpu_version(header.version) || header.version == axrb::protocol::kQuadGpuFrameVersion;
    if (gpuFrame) {
        if (pixels.size() != sizeof(axrb::protocol::WindowsGpuFrame) ||
            header.width > projectionWidth_ || header.height > projectionHeight_ ||
            (mixed && !axrb::protocol::valid_mixed_part(header.version, header.reserved))) {
            pendingGpuFrame_.reset(); pendingMixedCount_ = 0; return false;
        }
        const uint32_t count = mixed ? header.reserved >> 16 : 1;
        const uint32_t index = mixed ? header.reserved & 0xffff : 0;
        if (index == 0) {
            pendingGpuFrame_.reset();
            static axrb::protocol::PerfStats acquireStats("host-frame-slot");
            { axrb::protocol::PerfScope acquireScope(acquireStats); pendingGpuFrame_ = gpuFrames_.acquire(); }
            if (!pendingGpuFrame_) return false;
            pendingGpuFrame_->resize(count);
            pendingMixedCount_ = count; pendingMixedIndex_ = 0; pendingMixedSequence_ = header.sequence;
        }
        if (!pendingGpuFrame_ || count != pendingMixedCount_ || index != pendingMixedIndex_ ||
            header.sequence != pendingMixedSequence_) {
            pendingGpuFrame_.reset(); pendingMixedCount_ = 0; return false;
        }
        auto& part = pendingGpuFrame_->parts[index];
        axrb::protocol::WindowsGpuFrame gpu{}; std::memcpy(&gpu, pixels.data(), sizeof(gpu));
        // This slot is exclusively owned by the receiver. No publication lock
        // is held while copying, opening shared resources or waiting for the GPU.
        if (!part.receiver.receive(receiveDevice_.get(), receiveContext_.get(), gpu, header.sequence,
                header.width, header.height, static_cast<DXGI_FORMAT>(projectionFormat_),&receiveColorRenderer_,&projection)) {
            gpuFrames_.retire(pendingGpuFrame_);
            pendingGpuFrame_.reset(); pendingMixedCount_ = 0; return false;
        }
        part.header = header; part.projection = projection;
        ++pendingMixedIndex_; ++pendingMixedSequence_;
        if (pendingMixedIndex_ == count) {
            auto complete = pendingGpuFrame_->parts[0].header;
            complete.sequence = header.sequence;
            imageFrame_->store(complete, std::move(pixels), pendingGpuFrame_->parts[0].projection, pendingGpuFrame_);
            gpuFrames_.trim([count](auto& slot) { slot.trim(count); });
            pendingGpuFrame_.reset(); pendingMixedCount_ = 0;
        }
        // ACK only after this part's GPU copy completes. The app can safely
        // reuse its source, even while the host displays an older pinned slot.
        return true;
    }
    pendingGpuFrame_.reset(); pendingMixedCount_ = 0;
#else
    using namespace axrb::protocol;
    const bool singleGpu = header.version == kWindowsGpuFrameVersion || header.version == kQuadGpuFrameVersion ||
        header.version == kEquirectGpuFrameVersion;
    if (singleGpu || header.version == kGpuBatchFrameVersion) {
        // Layers exported by the emulator's Vulkan layer. ACK (return true)
        // only after every host-owned copy has completed.
        std::vector<GpuBatchPart> parts;
        if (singleGpu) {
            if (pixels.size() != sizeof(WindowsGpuFrame)) return false;
            GpuBatchPart part{header, projection, {}};
            std::memcpy(&part.gpu, pixels.data(), sizeof(part.gpu));
            parts.push_back(part);
        } else {
            if (!valid_gpu_batch(header, pixels.data(), pixels.size())) return false;
            for (uint32_t i = 0; i < header.reserved; ++i)
                parts.push_back(decode_gpu_batch_part(pixels.data(), pixels.size() / header.reserved, i));
        }
        if (!vulkan_) return false;
        for (const auto& part : parts)
            if (part.header.width > projectionWidth_ || part.header.height > projectionHeight_) return false;
        auto frame = vulkan_->receive(parts);
        if (!frame) return false;
        if (!reportedGpuImage_) {
            std::fprintf(stderr, "AXRB GPU: shared eye images active (Vulkan OPAQUE_FD); no pixel TCP transfer\n");
            reportedGpuImage_ = true;
        }
        imageFrame_->store(header, std::move(pixels), projection, std::move(frame));
        return true;
    }
    if (mixed_gpu_version(header.version)) {
        static bool reported = false;
        if (!reported) {
            std::fprintf(stderr, "AXRB GPU: per-part mixed GPU frames (version %u) are not implemented on Linux; the guest sends whole-frame batches\n", header.version);
            reported = true;
        }
        return false;
    }
#endif
    // CPU-transfer fallback uses the same layer-local fade semantics.
    const size_t eyeBytes=static_cast<size_t>(header.width)*header.height*4;
    for(uint32_t eye=0;eye<header.layers && eye<2;++eye) {
        const auto& color=projection.colors[eye];
        if(color.identity()) continue;
        for(size_t at=eye*eyeBytes;at<(eye+1)*eyeBytes && at+3<pixels.size();at+=4) {
            float value[4];for(int c=0;c<4;++c)value[c]=pixels[at+c]/255.f;
            protocol::apply_layer_color(value,color,protocol::image_layer_flags(projection,eye));
            for(int c=0;c<4;++c)pixels[at+c]=static_cast<uint8_t>((std::max)(0.f,(std::min)(1.f,value[c]))*255.f+0.5f);
        }
    }
    imageFrame_->store(header, std::move(pixels), projection);
    return true;
}

} // namespace axrb::host::detail
