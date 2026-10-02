# Linux spike (2026-10-01, not a supported path)

The question: can AXRB's Android side run a VrApi/OVRPlugin Quest title (RE4VR,
UE 4.25) on a Linux PC, and present it through a Linux OpenXR runtime at
headset rate? The host now presents through Vulkan (XR_KHR_vulkan_enable2)
with GPU-shared eye images; see Frame rate. Rig: Fedora 44, RTX 3080 Ti (NVIDIA 610.57.04), KVM, emulator 37.1.11,
`system-images;android-36;google_apis;x86_64` r07.

## Reproduce

One-time setup (AVD `axrb-managed-api36`, runtime APK, the title's APK):

```sh
scripts/emulator/linux_android_emulator.sh setup
ANDROID_ABI=arm64-v8a runtime/apk/build_apk.sh        # needs a JDK (Fedora: java-25-openjdk-devel)
AXRB_GPU_SHARING=1 scripts/emulator/linux_android_emulator.sh start
scripts/emulator/linux_android_emulator.sh install out/android/runtime/axrb-openxr-runtime-debug.apk <title.apk>
```

Then one command per session:

```sh
# Headset: start WiVRn and connect the Quest first.
scripts/run/run_linux_game.sh --package com.Armature.VR4 \
    --activity com.epicgames.ue4.GameActivity --title "Resident Evil 4" --runtime wivrn
# Desktop: a local Monado with a simulated HMD in a window at 90 Hz.
scripts/run/run_linux_game.sh --package com.Armature.VR4 --runtime monado
```

`run_linux_game.sh` checks `/dev/kvm`, the SDK, the AVD, the runtime
(WiVRn: the flatpak's manifest via `flatpak info --show-location`, the
server's IPC socket and `HeadsetConnected` on its D-Bus interface; it does
not start WiVRn), builds the bridge and the emulator layer when out of date,
starts the emulator cold with `AXRB_GPU_SHARING=1` unless one is running,
applies the runtime policy and `debug.axrb.cached_buffer_memory=0`, starts
`monado-service` only if `--runtime monado` and none is running, then the
bridge and the title (launcher activity resolved in the guest when
`--activity` is omitted). Ctrl-C, or the title exiting, stops the bridge, the
title, a Monado it started and an emulator it started (`--keep-emulator`
leaves that running). Logs, including the guest's `AXRB.Perf` lines, go to
`~/.local/state/axrb/logs/run-<time>/`. `--rebuild-runtime-apk` rebuilds and
reinstalls the runtime for the title's ABI; `--prop KEY=VALUE` sets guest
properties; the bridge's diagnostic variables (`AXRB_POSE_LEAD_PERIODS`,
`AXRB_LATENCY_PROBE`, `AXRB_EYE_EXTENT`, ...) pass through; see `--help`.
The system `active_runtime.json` is never touched (`XR_RUNTIME_JSON` is set
for the bridge only).

Two things the script handles that the manual steps get wrong easily: the
runtime policy restarts adbd as root, which drops the `adb reverse`
forwards, so they are re-applied after it (without them the guest logs
"GPU consumer unavailable" and the bridge presents nothing); and the
emulator runs with `-no-snapshot` (`AXRB_NO_SNAPSHOT=1`), because a
quickboot snapshot saved after a GPU-sharing session crashed qemu
(`AdbVsockPipe`) seconds after loading.

Neither build needs a Vulkan SDK or a shader compiler: the Vulkan headers
come from the NDK and the host loads `libvulkan.so.1` at run time. Without
`AXRB_GPU_SHARING=1` (or with `debug.axrb.gpu_share 0`) frames travel as
pixels, see below. `AXRB_HEADLESS=1` keeps the older pose-only host
(XR_MND_headless), which a runtime without XR_KHR_vulkan_enable2 also gets.
`start` sets `debug.axrb.coherent_memory 1`, `debug.axrb.gpu_share` and
`debug.axrb.defer_gpu_ack` from `AXRB_GPU_SHARING`.

## Frame rate (2026-10-02)

RE4VR title screen, 1024x1024 per eye unless noted, Monado 25.1 simulated
HMD with its real compositor in a window at 90 Hz, RTX 3080 Ti. Guest
numbers are the runtime's `AXRB.Perf` logcat lines, host numbers the
bridge's; five-second windows.

| | pixels over adb reverse (before) | shared GPU images (now) |
|---|---|---|
| guest frames/s (end-frame rate) | 25.1 | 90.0 |
| guest xrEndFrame | 33.1 ms (image-send 30.8 ms) | 0.3 ms (send 0.04 ms, deferred ACK) |
| host unique frames presented/s | 24.8 | 89.6-90.0 (phase-locked) |
| host receive (GPU copy, then ACK) | - | 0.45 ms |
| host copy into the runtime swapchain | 1.6 ms (staging upload) | 0.4 ms |
| reception to submission | 7.6 ms (random phase) | 4.0 ms (p99 5.0 ms) |
| CPU: qemu / bridge / monado | - | 470 % / 3 % / 4 % |

Where the 28 ms went: transport. `adb reverse` moves 314 MB/s through the
emulator's pipe (`dd | nc` from the guest, 512 MiB), and a frame is 8 MiB, so
the link alone caps the pixel path near 39 frames/s; measured image-send was
30.8 ms of the 33 ms frame. The guest readback (vulkan-frame-copy) was 1.9 ms
and the host side never limited anything. 90 Hz pixels would need 720 MB/s,
so no tuning of the CPU path reaches the target; it remains the fallback.
The same link's round trip is 0.23 ms p50 (2.6 ms p95 under game load), cheap
enough for per-frame metadata and acknowledgments.

What carries frames now (no pixel crosses the guest boundary):

1. The guest runtime is unchanged on the wire: it marks its eye blits with
   the 64-byte `vkCmdUpdateBuffer` marker and sends 176 bytes of metadata.
2. `host/gpu/layer.cpp`, built for Linux, runs inside the emulator process
   (gfxstream on host Vulkan, same GPU). Instead of named D3D11 textures it
   allocates each export session's eye images as exportable Vulkan memory
   (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`) and serves the file
   descriptors on `$XDG_RUNTIME_DIR/axrb-gpu-share.sock`
   (`protocol/linux_gpu_share.h`, SCM_RIGHTS), the counterpart of
   `OpenSharedResourceByName`.
3. The host bridge creates its Vulkan device through the runtime
   (XR_KHR_vulkan_enable2, `host/src/linux_vulkan.cpp`), pulls a session's
   descriptors on first use, checks device and driver UUIDs, imports them,
   and on each frame copies every layer into a pooled host image on a
   second queue before acknowledging. Synchronization is the existing one:
   the guest's fence has completed before it sends, and the host's before it
   ACKs; ownership moves through `VK_QUEUE_FAMILY_EXTERNAL`.
4. `host/src/presentation_linux.cpp` keeps one two-slice swapchain per
   application layer and submits projection, quad and equirect2 layers
   natively in application order (fades via
   XR_KHR_composition_layer_color_scale_bias). Copies are transfer commands
   (blit where formats differ), so no shaders are involved.

Pacing. With both sides at 90 Hz on separate clocks, the guest's frames
drifted in phase against the host's pick (about 35 ppm here): whenever they
crossed it, unique frames fell to 81-83/s for tens of seconds. The host now
reports how long it held each new frame (`PoseFrame::frame_slack`) and the
guest's `xrWaitFrame` steers its schedule toward a 4 ms hold
(`debug.axrb.phase_target_us`, `debug.axrb.phase_lock=0` disables). A 2.5 ms
target cut latency further but missed more host frames (86-89/s).

Latency (`AXRB_LATENCY_PROBE=1`, matching each presented frame's render pose
to the head poses the host published): a rendered pose reaches xrEndFrame
22.7 ms (p50) after the host published it, two host frames, and Monado
predicts display 9.4 ms after submission. The poses are located at the
host's predicted display time, so content is about two frames (22 ms) older
than a native application's; rotation is still reprojected by the runtime.
`AXRB_POSE_LEAD_PERIODS=2` locates poses two periods further ahead to cancel
that; it runs, but whether it feels better has not been tried in a headset.

Resolution is not the limit on this GPU: the title screen holds 90 frames/s
at 1440x1584 and 1832x1920 per eye (`AXRB_EYE_EXTENT`; RE4VR renders at 1.2x
and blits down), GPU 16 % busy. Gameplay has not been measured.

Headset through WiVRn (not yet tried): `run_linux_game.sh --runtime wivrn`
with the WiVRn server running and the headset connected; everything else is
the same. The bridge refuses OPAQUE_FD sharing if WiVRn's Vulkan device is
not the GPU the emulator renders on (logged); the eye extent and refresh
rate then come from WiVRn.

Not done on Linux: precomposition when the runtime's layer limit is
exceeded, the per-part (non-batch) mixed GPU messages, the desktop mirror
and FPS HUD, per-eye color differences on one projection layer, and color
scale/bias on runtimes without the extension (dropped, logged once).
`AXRB_CAPTURE_PREFIX` (+ `AXRB_CAPTURE_AFTER`) writes four presented
left-eye frames as PPM, as on Windows.

## Findings

- Guest gets the NVIDIA GPU through gfxstream on host Vulkan (guest Vulkan
  1.3, GLES via the GL translator). The guest clock is `kvm-clock`; clock reads
  are under 0.5 % of guest samples, so the WHPX clock helper (which fixes
  `read_hpet` at 45 % of samples under WHPX) has no Linux counterpart to port.
- `runtime/apk/build_apk.sh` built the runtime without `CMAKE_BUILD_TYPE`
  (unoptimised; `build_apk.ps1` passes Release). Under ARM translation that
  made each frame readback cost about 185 ms instead of about 3 ms. Fixed here.
- With the default cached-buffer filter, UE 4.25 aborts in
  `FVulkanDevice::InitGPU` → `FResourceHeapManager::AllocateBuffer` →
  `VerifyVulkanResult` (ForceQuit → `System.exit(0)`), before any
  `vkAllocateMemory` reaches the layer. `debug.axrb.cached_buffer_memory=0`
  avoids it. The filter narrows the buffer's types from 0x3b to 0x13, which drops type 5
  (DEVICE_LOCAL|HOST_VISIBLE|COHERENT). That this is the cause is unverified.
- Textures rendered corrupted (stripes, block patches, rainbow noise) because
  gfxstream's memory type 3 (`HOST_VISIBLE|HOST_COHERENT`, not cached, system
  heap) is not coherent between the guest CPU and the GPU on this host. The
  guest writes it at about 2 GB/s, i.e. cached; Windows maps it uncached
  (docs/cpu_pressure.md). UE's 4 MiB texture staging ring lives in type 3, so
  the GPU copied partly stale bytes in 64-byte runs. Fixed by the runtime
  layer allocating type 3 from type 4 (the same heap, `HOST_CACHED` added):
  `runtime/vulkan/coherent_memory_policy.h`, on with
  `debug.axrb.coherent_memory=1`, which `linux_android_emulator.sh start`
  sets. Why KVM gives the guest a cached mapping (guest PAT ignored for
  these pages) is a likely explanation that has not been verified.
  - Not ASTC: this build's textures are ETC2 (`VK_FORMAT_ETC2_*`, decoded
    by gfxstream's GPU ETC2 emulation), so `debug.axrb.astc_to_bc7` never
    touched them; that is why it made no difference.
  - Not the host: corruption is already in the guest's swapchain readback
    with no `monado-service` and no host bridge running.
  - Not gfxstream's ETC2 decoder: `tests/android/vulkan_etc2_android.cpp`
    with `tools/compare_etc2.py` decodes random ETC2 blocks (all modes, all
    mips) exactly when its staging and readback buffers are type 4 or type 5
    (`cached`/`bar` modes, 0/54 levels wrong in 5/5 runs each), and wrongly
    in type 3 (19-33/54 levels wrong in 5/5 runs; wrong texels are stale
    readback fill or stale staging, in 16-texel runs).
  - Type 5 (`DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`) is coherent but the
    guest writes it at about 180 MB/s.
- Without GPU sharing, every frame is read back and sent as RGBA over
  `adb reverse`. At 2×1024² that caps the stream at about 25-28 fps (see Frame
  rate). With no consumer attached, the game renders 90 fps.

## Appendix: the manual sequence

What `run_linux_game.sh` does, by hand (`ANDROID_ADB_SERVER_PORT=5038`
throughout, as on Windows):

```sh
cmake -S . -B out/linux -G Ninja -DAXRB_BUILD_ANDROID_RUNTIME=OFF -DAXRB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release && cmake --build out/linux
cmake -S host/gpu -B out/linux-gpu -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build out/linux-gpu
AXRB_GPU_SHARING=1 AXRB_NO_SNAPSHOT=1 scripts/emulator/linux_android_emulator.sh start
python3 scripts/emulator/android_runtime_policy.py --sdk ~/Android/Sdk --serial emulator-5584 --package <pkg>
adb -s emulator-5584 reverse tcp:38490 tcp:38490; adb -s emulator-5584 reverse tcp:38491 tcp:38491
adb -s emulator-5584 shell setprop debug.axrb.cached_buffer_memory 0
sleep infinity | SIMULATED_ENABLE=1 XRT_COMPOSITOR_FORCE_XCB=1 XRT_COMPOSITOR_DEFAULT_FRAMERATE=90 monado-service &   # Monado only; stdin must not be /dev/null
XR_RUNTIME_JSON=<runtime manifest> out/linux/bin/axrb-host-bridge --serve-openxr 38490 0 "<title>" &
adb -s emulator-5584 shell am start -n <pkg>/<activity>
```

Runtime manifests: Monado `/usr/share/openxr/1/openxr_monado.json`; WiVRn
`$(flatpak info --show-location io.github.wivrn.wivrn)/files/share/openxr/1/openxr_wivrn.json`.
