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
- Textures render visibly corrupted (blocky stripes) both with the AXRB
  ASTC→BC7 substitution and with it off (`debug.axrb.astc_to_bc7=0`). Open.
- Without GPU sharing, every frame is read back and sent as RGBA over
  `adb reverse`. At 2×1024² that caps the stream at about 28 fps. With no consumer
  attached, the game renders 90 fps.
