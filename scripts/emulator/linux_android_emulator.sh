#!/usr/bin/env bash
# Linux spike: the subset of windows_android_emulator.ps1 needed to bring up
# the AXRB Android 16 guest on KVM with the host GPU. Not a supported path.
#
#   linux_android_emulator.sh setup|start|verify|install|stop
#
# Mirrors the Windows defaults of a managed install: AVD axrb-managed-api36,
# console 5584 / adb 5585, private adb server on 5038, 4 vCPUs, 8 GB RAM,
# system-images;android-36;google_apis;x86_64 (r07, ships libndk_translation).
# What it does NOT do: the WHPX clock hook (host/clock is Windows-only; KVM
# has a stable kvm-clock, see docs) or the multicore CPUID-patched backend
# (Windows-only workaround). AXRB_GPU_SHARING=1 loads the Linux build of the
# host/gpu Vulkan layer (cmake -S host/gpu -B out/linux-gpu) into the emulator;
# it serves eye images as OPAQUE_FD memory to axrb-host-bridge.
set -euo pipefail

SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
AVD="${AXRB_AVD:-axrb-managed-api36}"
PORT="${AXRB_PORT:-5584}"
ADB_PORT=$((PORT + 1))
CORES="${AXRB_CORES:-4}"
MEMORY_MB="${AXRB_MEMORY_MB:-8192}"
STORAGE_GB="${AXRB_STORAGE_GB:-32}"
TRANSPORT="${AXRB_GL_TRANSPORT:-asg}"
GPU_SHARING="${AXRB_GPU_SHARING:-0}"
GPU_LAYER_DIR="${AXRB_GPU_LAYER_DIR:-$(cd "$(dirname "$0")/../.." && pwd)/out/linux-gpu}"
LOGS="${AXRB_LOGS:-${XDG_STATE_HOME:-$HOME/.local/state}/axrb/logs/emulator}"
IMAGE_PKG="system-images;android-36;google_apis;x86_64"
IMAGE_DIR="$SDK/system-images/android-36/google_apis/x86_64"
AVD_HOME="${ANDROID_AVD_HOME:-$HOME/.android/avd}"
SERIAL="emulator-$PORT"

export ANDROID_ADB_SERVER_PORT=5038
export ADB_LOCAL_TRANSPORT_MAX_PORT=5683
ADB="$SDK/platform-tools/adb"
EMULATOR="$SDK/emulator/emulator"

XR_FEATURES=(
    android.hardware.vr.headtracking
    android.hardware.vr.high_performance
    android.software.vr.mode
    android.software.xr.api.openxr
    android.software.xr.api.spatial
    android.hardware.xr.input.controller
    android.hardware.xr.input.hand_tracking
    android.hardware.xr.input.eye_tracking
    oculus.software.handtracking
    oculus.software.eye_tracking
    oculus.software.face_tracking
    oculus.software.body_tracking
    oculus.software.overlay_keyboard
    com.oculus.feature.PASSTHROUGH
    com.oculus.feature.RENDER_MODEL
)

adbs() { "$ADB" -s "$SERIAL" "$@"; }

wait_boot() {
    local deadline=$((SECONDS + ${1:-480}))
    while ((SECONDS < deadline)); do
        if [[ "$(timeout 10 "$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == 1 ]] &&
            timeout 15 "$ADB" -s "$SERIAL" shell cmd package path android 2>/dev/null | grep -q package:; then
            return 0
        fi
        sleep 2
    done
    return 1
}

setup() {
    "$SDK/cmdline-tools/latest/bin/sdkmanager" "$IMAGE_PKG"
    local dir="$AVD_HOME/$AVD.avd"
    mkdir -p "$dir"
    # Same keys as launcher/core/setup.mjs avdConfig().
    cat >"$dir/config.ini" <<EOF
avd.ini.encoding=UTF-8
AvdId=$AVD
avd.ini.displayname=AXRB
abi.type=x86_64
hw.cpu.arch=x86_64
hw.cpu.ncore=$CORES
hw.ramSize=$MEMORY_MB
hw.gpu.enabled=yes
hw.gpu.mode=host
hw.audioOutput=yes
hw.audioInput=yes
hw.lcd.width=1080
hw.lcd.height=1920
hw.lcd.density=420
hw.keyboard=no
hw.mainKeys=no
hw.useext4=yes
disk.dataPartition.size=${STORAGE_GB}G
disk.cachePartition.size=66MB
vm.heapSize=576
image.sysdir.1=$IMAGE_DIR/
tag.id=google_apis
target=android-36
fastboot.forceColdBoot=no
fastboot.forceFastBoot=yes
showDeviceFrame=no
runtime.network.speed=full
runtime.network.latency=none
PlayStore.enabled=no
hw.gltransport=$TRANSPORT
EOF
    printf 'avd.ini.encoding=UTF-8\npath=%s\ntarget=android-36\n' "$dir" >"$AVD_HOME/$AVD.ini"
    echo "AVD $AVD written to $dir"
}

start() {
    mkdir -p "$LOGS"
    "$EMULATOR" -accel-check
    "$ADB" start-server
    local args=(-avd "$AVD" -ports "$PORT,$ADB_PORT" -gpu host -accel on -no-boot-anim -no-metrics
        -memory "$MEMORY_MB" -cores "$CORES" -writable-system)
    [[ "${AXRB_SHOW_WINDOW:-0}" == 1 ]] || args+=(-no-window)
    [[ "${AXRB_COLD_BOOT:-0}" == 1 ]] && args+=(-no-snapshot-load)
    local layer_env=()
    if [[ "$GPU_SHARING" == 1 ]]; then
        [[ -f "$GPU_LAYER_DIR/axrb_gpu_layer.json" ]] || { echo "Build host/gpu first: cmake -S host/gpu -B out/linux-gpu && cmake --build out/linux-gpu" >&2; exit 1; }
        # Scoped to the emulator process; nothing is registered system-wide.
        layer_env=(env VK_LAYER_PATH="$GPU_LAYER_DIR" VK_INSTANCE_LAYERS=VK_LAYER_AXRB_gpu_share)
    fi
    echo "Starting: ${layer_env[*]} $EMULATOR ${args[*]}"
    nohup "${layer_env[@]}" "$EMULATOR" "${args[@]}" >"$LOGS/emulator.stdout.log" 2>"$LOGS/emulator.stderr.log" &
    echo "emulator pid $!"
    wait_boot 480 || { echo "Android did not boot; see $LOGS" >&2; exit 1; }
    verify || true
    ensure_xr_features
    adbs shell cmd media_session volume --stream 3 --set 25 >/dev/null 2>&1 || true
    adbs reverse tcp:38490 tcp:38490
    adbs reverse tcp:38491 tcp:38491
    adbs shell setprop debug.axrb.gpu_share "$GPU_SHARING"
    # The Linux host copies shared images before it acknowledges them, so the
    # guest can defer that wait until it would reuse them (about 1 ms per frame).
    adbs shell setprop debug.axrb.defer_gpu_ack "$GPU_SHARING"
    # Uncached coherent guest memory is not coherent with the GPU under KVM;
    # the runtime layer allocates it from the cached type (coherent_memory_policy.h).
    adbs shell setprop debug.axrb.coherent_memory 1
    echo "Ready: $SERIAL. Images use adb reverse :38491; native pose stream uses 10.0.2.2:38490."
}

verify() {
    mkdir -p "$LOGS"
    adbs shell dumpsys SurfaceFlinger | grep '^GLES:' | tee "$LOGS/guest-gles.txt"
    timeout 20 "$ADB" -s "$SERIAL" shell cmd gpu vkjson >"$LOGS/guest-vulkan.json" || true  # can hang after a snapshot load
    python3 - "$LOGS/guest-vulkan.json" <<'PY'
import json, sys
for d in json.load(open(sys.argv[1])).get('devices', []):
    p = d['properties']
    print(f"Vulkan: {p['deviceName']} (vendor {p['vendorID']}, type {p['deviceType']}, api {int(p['apiVersion'])>>22}.{(int(p['apiVersion'])>>12)&0x3ff})")
PY
    echo "ABIs: $(adbs shell getprop ro.product.cpu.abilist | tr -d '\r'); native bridge: $(adbs shell getprop ro.dalvik.vm.native.bridge | tr -d '\r')"
    echo "vCPUs: $(adbs shell getconf _NPROCESSORS_ONLN | tr -d '\r'); clocksource: $(adbs shell su 0 cat /sys/devices/system/clocksource/clocksource0/current_clocksource | tr -d '\r')"
}

ensure_xr_features() {
    local present missing=()
    present="$(adbs shell pm list features | tr -d '\r')"
    for f in "${XR_FEATURES[@]}"; do grep -qx "feature:$f" <<<"$present" || missing+=("$f"); done
    if ((${#missing[@]} == 0)); then echo "Headset features: all ${#XR_FEATURES[@]} already declared."; return; fi
    local xml
    xml="$(mktemp)"
    { echo '<?xml version="1.0" encoding="utf-8"?>'; echo '<permissions>'
      for f in "${XR_FEATURES[@]}"; do echo "    <feature name=\"$f\" />"; done
      echo '</permissions>'; } >"$xml"
    adbs root; sleep 3; adbs wait-for-device
    # A fresh image needs one reboot after verity is disabled before /system
    # becomes writable (overlayfs); the Windows script relies on the same
    # -writable-system overlay.
    if adbs remount 2>&1 | grep -q 'reboot'; then
        adbs reboot; sleep 5; wait_boot 480
        adbs root; sleep 3; adbs wait-for-device; adbs remount
    fi
    adbs push "$xml" /data/local/tmp/axrb-xr-features.xml
    adbs shell cp /data/local/tmp/axrb-xr-features.xml /system/etc/permissions/axrb-xr-features.xml
    adbs shell chmod 644 /system/etc/permissions/axrb-xr-features.xml
    adbs shell chcon u:object_r:system_file:s0 /system/etc/permissions/axrb-xr-features.xml
    adbs shell rm -f /data/local/tmp/axrb-xr-features.xml
    adbs shell stop; adbs shell start
    rm -f "$xml"
    wait_boot 240
    adbs unroot || true; sleep 2; adbs wait-for-device
    echo "Headset features: declared ${#XR_FEATURES[@]}."
}

install() {
    local runtime_apk="${1:?runtime apk}" app_apk="${2:-}"
    adbs install --no-incremental --force-queryable -r "$runtime_apk"
    adbs reverse tcp:38490 tcp:38490
    adbs reverse tcp:38491 tcp:38491
    [[ -z "$app_apk" ]] || adbs install --no-incremental -r "$app_apk"
}

stop() { adbs emu kill; }

case "${1:-verify}" in
    setup) setup ;;
    start) start ;;
    verify) verify ;;
    install) shift; install "$@" ;;
    stop) stop ;;
    xrfeatures) ensure_xr_features ;;
    *) echo "usage: $0 setup|start|verify|install <runtime.apk> [app.apk]|stop" >&2; exit 2 ;;
esac
