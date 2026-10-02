# Linux emulator-side spike (2026-10-01, not a supported path)

The question: can AXRB's Android side run a VrApi/OVRPlugin Quest title (RE4VR,
UE 4.25) on a Linux PC before the host's D3D11 presentation is ported to
Vulkan? Rig: Fedora 44, RTX 3080 Ti (NVIDIA 610.57.04), KVM, emulator 37.1.11,
`system-images;android-36;google_apis;x86_64` r07.

## Reproduce

```sh
scripts/emulator/linux_android_emulator.sh setup      # AVD axrb-managed-api36
scripts/emulator/linux_android_emulator.sh start      # KVM + -gpu host, XR features
ANDROID_ABI=arm64-v8a runtime/apk/build_apk.sh        # needs a JDK with javac
scripts/emulator/linux_android_emulator.sh install out/android/runtime/axrb-openxr-runtime-debug.apk
cmake -S . -B out/linux -G Ninja -DAXRB_BUILD_ANDROID_RUNTIME=OFF -DAXRB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build out/linux
sleep infinity | SIMULATED_ENABLE=1 XRT_COMPOSITOR_NULL=1 monado-service &   # stdin must not be /dev/null
XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json out/linux/bin/axrb-host-bridge --serve-openxr 38490 0 Game &
python3 scripts/emulator/android_runtime_policy.py --sdk ~/Android/Sdk --serial emulator-5584 --package <pkg>
adb -s emulator-5584 shell setprop debug.axrb.cached_buffer_memory 0         # see below
adb -s emulator-5584 shell setprop debug.axrb.coherent_memory 1              # set by start; see below
```

(`ANDROID_ADB_SERVER_PORT=5038` throughout, as on Windows.)

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
  `adb reverse`. At 2×1024² that caps the stream at about 28 fps. With no consumer
  attached, the game renders 90 fps.
