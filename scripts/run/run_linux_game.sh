#!/usr/bin/env bash
# Linux spike: play an installed Quest title through AXRB on the emulator and
# a Linux OpenXR runtime (WiVRn to a headset, or a local Monado). One command
# for the sequence in docs/linux_spike.md. Not a supported path.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
AVD="${AXRB_AVD:-axrb-managed-api36}"
PORT="${AXRB_PORT:-5584}"
SERIAL="emulator-$PORT"
EMU_SCRIPT="$ROOT/scripts/emulator/linux_android_emulator.sh"
RUNTIME_PACKAGE=com.axrb.openxrruntime
WIVRN_APP=io.github.wivrn.wivrn
BRIDGE_PORT=38490

export ANDROID_HOME="$SDK"
export ANDROID_ADB_SERVER_PORT=5038
export ADB_LOCAL_TRANSPORT_MAX_PORT=5683
export AXRB_GPU_SHARING="${AXRB_GPU_SHARING:-1}"
ADB="$SDK/platform-tools/adb"

usage() {
    cat <<EOF
Usage: $(basename "$0") --package PKG [options]

Starts (or reuses) the AXRB emulator, builds the host bridge and the emulator
GPU layer if they are out of date, starts the bridge on an OpenXR runtime and
launches the game. Ctrl-C (or the game exiting) stops the bridge, the game,
a Monado the script started, and an emulator the script started.

  --package PKG        Android package of the title (required).
  --activity ACT       Activity to start (default: the package's launcher
                       activity, resolved in the guest).
  --title NAME         Name the bridge gives the OpenXR application
                       (default: the package name).
  --runtime RT         wivrn (default): WiVRn flatpak, server already running
                         with the headset connected;
                       monado: local monado-service with a simulated HMD in a
                         window at 90 Hz (started if none is running);
                       PATH: any OpenXR runtime manifest (.json).
  --rebuild-runtime-apk  Rebuild runtime/apk (build_apk.sh, ABI of the title)
                       and reinstall it before launching.
  --keep-emulator      Leave an emulator this script started running on exit.
  --prop KEY=VALUE     Set a guest property before launch (repeatable), e.g.
                       --prop debug.axrb.phase_target_us=4000.
  --log-dir DIR        Where logs go (default: \$XDG_STATE_HOME/axrb/logs/run-<time>).
  -h, --help           This text.

Example (Resident Evil 4 VR):
  $(basename "$0") --package com.Armature.VR4 \\
      --activity com.epicgames.ue4.GameActivity --title "Resident Evil 4" --runtime wivrn

Environment passed through to the bridge (all optional):
  AXRB_POSE_LEAD_PERIODS=N   locate head poses N display periods further ahead
  AXRB_LATENCY_PROBE=1       log render-pose-to-submit latency
  AXRB_EYE_EXTENT=WxH        per-eye resolution requested from the title
  AXRB_FRESH_FRAME_WAIT_US=N how long the host waits for a fresh guest frame
  AXRB_FRAME_HISTORY=N       pose history depth
  AXRB_HEADLESS=1            pose-only host (XR_MND_headless), no presentation
  AXRB_CAPTURE_PREFIX=PATH   write four presented left-eye frames as PPM
  AXRB_CAPTURE_AFTER=N       ... after N frames (default 2000)
Emulator (linux_android_emulator.sh):
  AXRB_GPU_SHARING=0         send frames as pixels instead of shared GPU images
  AXRB_NO_SNAPSHOT=0         allow quickboot snapshots (default here: cold boot,
                             no snapshot saved; see linux_android_emulator.sh)
  AXRB_SHOW_WINDOW=1, AXRB_CORES, AXRB_MEMORY_MB, AXRB_AVD,
  AXRB_PORT, ANDROID_HOME
EOF
}

die() { echo "run_linux_game: $*" >&2; exit 1; }
log() { echo "[run_linux_game] $*"; }
adbs() { "$ADB" -s "$SERIAL" "$@"; }
sh_out() { timeout 20 "$ADB" -s "$SERIAL" shell "$@" 2>/dev/null | tr -d '\r'; }

PACKAGE="" ACTIVITY="" TITLE="" RUNTIME=wivrn REBUILD_APK=0 KEEP_EMULATOR=0 LOG_DIR=""
PROPS=()
while (($#)); do
    case "$1" in
        --package) PACKAGE="${2:?--package needs a value}"; shift 2 ;;
        --activity) ACTIVITY="${2:?--activity needs a value}"; shift 2 ;;
        --title) TITLE="${2:?--title needs a value}"; shift 2 ;;
        --runtime) RUNTIME="${2:?--runtime needs a value}"; shift 2 ;;
        --rebuild-runtime-apk) REBUILD_APK=1; shift ;;
        --keep-emulator) KEEP_EMULATOR=1; shift ;;
        --prop) [[ "${2:-}" == *=* ]] || die "--prop needs KEY=VALUE"; PROPS+=("$2"); shift 2 ;;
        --log-dir) LOG_DIR="${2:?--log-dir needs a value}"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown argument: $1" ;;
    esac
done
[[ -n "$PACKAGE" ]] || { usage >&2; die "--package is required"; }
[[ "$PACKAGE" =~ ^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+$ ]] || die "invalid package: $PACKAGE"
[[ -z "$ACTIVITY" || "$ACTIVITY" =~ ^[A-Za-z0-9_.$/]+$ ]] || die "invalid activity: $ACTIVITY"
TITLE="${TITLE:-$PACKAGE}"
LOG_DIR="${LOG_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/axrb/logs/run-$(date +%Y%m%d-%H%M%S)}"

# ---- preflight -------------------------------------------------------------
[[ -r /dev/kvm && -w /dev/kvm ]] || die "/dev/kvm is missing or not accessible (enable virtualization; add yourself to the kvm group)"
[[ -x "$SDK/emulator/emulator" && -x "$ADB" ]] || die "Android SDK emulator/platform-tools not found under $SDK (set ANDROID_HOME)"
[[ -f "${ANDROID_AVD_HOME:-$HOME/.android/avd}/$AVD.avd/config.ini" ]] || die "AVD $AVD not found; create it with: $EMU_SCRIPT setup"
command -v cmake >/dev/null && command -v ninja >/dev/null || die "cmake and ninja are required to build the bridge"

MONADO_NEEDED=0
case "$RUNTIME" in
    wivrn)
        command -v flatpak >/dev/null || die "--runtime wivrn needs the WiVRn flatpak (flatpak not installed)"
        loc="$(flatpak info --show-location "$WIVRN_APP" 2>/dev/null)" || die "WiVRn flatpak ($WIVRN_APP) is not installed"
        XR_JSON="$loc/files/share/openxr/1/openxr_wivrn.json"
        [[ -f "$XR_JSON" ]] || die "WiVRn runtime manifest not found at $XR_JSON"
        # The server exposes io.github.wivrn.Server on the session bus and the
        # OpenXR IPC socket under XDG_RUNTIME_DIR (shared with the flatpak).
        if [[ ! -S "${XDG_RUNTIME_DIR:-/run/user/$UID}/wivrn/comp_ipc" ]] ||
            ! busctl --user status io.github.wivrn.Server >/dev/null 2>&1; then
            die "WiVRn server is not running. Start WiVRn (the app, or: flatpak run --command=wivrn-server $WIVRN_APP), connect the Quest, then rerun."
        fi
        connected="$(busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server io.github.wivrn.Server HeadsetConnected 2>/dev/null || true)"
        [[ "$connected" == "b true" ]] || die "WiVRn server is running but no headset is connected. Open WiVRn on the Quest and connect, then rerun."
        ;;
    monado)
        command -v monado-service >/dev/null || die "--runtime monado needs monado-service (dnf install monado)"
        XR_JSON=/usr/share/openxr/1/openxr_monado.json
        [[ -f "$XR_JSON" ]] || die "Monado runtime manifest not found at $XR_JSON"
        pgrep -x monado-service >/dev/null || MONADO_NEEDED=1
        ;;
    *)
        XR_JSON="$(realpath -e "$RUNTIME" 2>/dev/null)" || die "--runtime must be wivrn, monado or an existing manifest path: $RUNTIME"
        ;;
esac
log "OpenXR runtime manifest: $XR_JSON"
if ss -Hltn "sport = :$BRIDGE_PORT" | grep -q .; then
    die "port $BRIDGE_PORT is in use (another axrb-host-bridge? pgrep -a axrb-host-bridge)"
fi

mkdir -p "$LOG_DIR"

# ---- cleanup ---------------------------------------------------------------
EMULATOR_STARTED=0 MONADO_PID="" MONADO_FEEDER="" BRIDGE_PID="" LOGCAT_PID="" GAME_STARTED=0
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    log "cleaning up"
    [[ -n "$LOGCAT_PID" ]] && kill "$LOGCAT_PID" 2>/dev/null
    ((GAME_STARTED)) && timeout 20 "$ADB" -s "$SERIAL" shell am force-stop "$PACKAGE" 2>/dev/null
    if [[ -n "$BRIDGE_PID" ]] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill -TERM "$BRIDGE_PID"  # background jobs ignore SIGINT
        for _ in $(seq 50); do kill -0 "$BRIDGE_PID" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$BRIDGE_PID" 2>/dev/null
    fi
    if [[ -n "$MONADO_PID" ]]; then
        kill -TERM "$MONADO_PID" 2>/dev/null
        for _ in $(seq 50); do kill -0 "$MONADO_PID" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$MONADO_PID" 2>/dev/null
    fi
    [[ -n "$MONADO_FEEDER" ]] && kill "$MONADO_FEEDER" 2>/dev/null
    if ((EMULATOR_STARTED && !KEEP_EMULATOR)); then
        log "stopping the emulator (--keep-emulator leaves it running)"
        timeout 30 "$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1
    fi
    wait 2>/dev/null
    rm -f "$LOG_DIR/.monado-stdin" "$LOG_DIR/.monado-start"
    log "logs: $LOG_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ---- build (cmake/ninja decide what is out of date) -------------------------
build() {
    local src="$1" dir="$2"; shift 2
    [[ -f "$dir/build.ninja" ]] || cmake -S "$src" -B "$dir" -G Ninja "$@" >"$LOG_DIR/configure-$(basename "$dir").log" ||
        die "configure of $src failed; see $LOG_DIR/configure-$(basename "$dir").log"
    cmake --build "$dir" >"$LOG_DIR/build-$(basename "$dir").log" 2>&1 ||
        { tail -20 "$LOG_DIR/build-$(basename "$dir").log" >&2; die "build of $src failed"; }
}
log "building host bridge and emulator GPU layer (if out of date)"
build "$ROOT" "$ROOT/out/linux" -DAXRB_BUILD_ANDROID_RUNTIME=OFF -DAXRB_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=Release
build "$ROOT/host/gpu" "$ROOT/out/linux-gpu" -DCMAKE_BUILD_TYPE=Release
BRIDGE="$ROOT/out/linux/bin/axrb-host-bridge"
[[ -x "$BRIDGE" ]] || die "bridge binary missing after build: $BRIDGE"

# ---- emulator --------------------------------------------------------------
"$ADB" start-server >/dev/null 2>&1
if [[ "$(sh_out getprop sys.boot_completed)" == 1 ]]; then
    log "reusing running emulator $SERIAL (left running on exit)"
    [[ "$(sh_out getprop debug.axrb.gpu_share)" == "$AXRB_GPU_SHARING" ]] ||
        log "warning: emulator was started with debug.axrb.gpu_share=$(sh_out getprop debug.axrb.gpu_share), AXRB_GPU_SHARING=$AXRB_GPU_SHARING here; restart it to change GPU sharing"
else
    log "starting emulator $AVD (AXRB_GPU_SHARING=$AXRB_GPU_SHARING); output in $LOG_DIR/emulator-start.log"
    EMULATOR_STARTED=1
    AXRB_AVD="$AVD" AXRB_PORT="$PORT" AXRB_NO_SNAPSHOT="${AXRB_NO_SNAPSHOT:-1}" "$EMU_SCRIPT" start >"$LOG_DIR/emulator-start.log" 2>&1 ||
        { tail -20 "$LOG_DIR/emulator-start.log" >&2; die "emulator start failed"; }
    grep -E '^(Vulkan|GLES|Ready):' "$LOG_DIR/emulator-start.log" || true
fi

sh_out pm path "$PACKAGE" | grep -q '^package:' || die "$PACKAGE is not installed in $AVD (adb -s $SERIAL install <apk>)"

if ((REBUILD_APK)); then
    abi="$(sh_out dumpsys package "$PACKAGE" | sed -n 's/^ *primaryCpuAbi=//p' | head -1)"
    [[ "$abi" == arm64-v8a || "$abi" == x86_64 ]] || abi=arm64-v8a
    log "rebuilding the runtime APK for $abi (output in $LOG_DIR/build-apk.log)"
    ANDROID_ABI="$abi" bash "$ROOT/runtime/apk/build_apk.sh" >"$LOG_DIR/build-apk.log" 2>&1 ||
        { tail -20 "$LOG_DIR/build-apk.log" >&2; die "runtime APK build failed"; }
    adbs install --no-incremental --force-queryable -r "$ROOT/out/android/runtime/axrb-openxr-runtime-debug.apk" >/dev/null ||
        die "runtime APK install failed"
    log "runtime APK installed"
fi
sh_out pm path "$RUNTIME_PACKAGE" | grep -q '^package:' ||
    die "AXRB runtime ($RUNTIME_PACKAGE) is not installed; rerun with --rebuild-runtime-apk or: $EMU_SCRIPT install <runtime.apk>"

if [[ -z "$ACTIVITY" ]]; then
    for category in android.intent.category.LAUNCHER com.oculus.intent.category.VR android.intent.category.INFO; do
        ACTIVITY="$(sh_out cmd package resolve-activity --brief -a android.intent.action.MAIN -c "$category" "$PACKAGE" | tail -1)"
        [[ "$ACTIVITY" == */* ]] && break
        ACTIVITY=""
    done
    [[ -n "$ACTIVITY" ]] || die "could not resolve a launcher activity for $PACKAGE; pass --activity"
    ACTIVITY="${ACTIVITY#*/}"
    log "launcher activity: $ACTIVITY"
fi

log "applying the emulator runtime policy for $PACKAGE"
python3 "$ROOT/scripts/emulator/android_runtime_policy.py" --sdk "$SDK" --serial "$SERIAL" --package "$PACKAGE" \
    >"$LOG_DIR/runtime-policy.log" 2>&1 || { tail -5 "$LOG_DIR/runtime-policy.log" >&2; die "runtime policy failed"; }
# The policy restarts adbd as root, which drops adb reverse forwards; without
# them the guest finds no image consumer and the bridge presents nothing.
adbs reverse tcp:38490 tcp:38490 >/dev/null
adbs reverse tcp:38491 tcp:38491 >/dev/null
# UE 4.25 aborts in FVulkanDevice::InitGPU with the cached-buffer filter on (docs/linux_spike.md).
adbs shell setprop debug.axrb.cached_buffer_memory 0
for prop in ${PROPS[@]+"${PROPS[@]}"}; do adbs shell setprop "${prop%%=*}" "${prop#*=}"; done

# ---- runtime ---------------------------------------------------------------
if ((MONADO_NEEDED)); then
    sock="${XDG_RUNTIME_DIR:-/run/user/$UID}/monado_comp_ipc"
    marker="$LOG_DIR/.monado-start"; : >"$marker"
    log "starting monado-service (simulated HMD, 90 Hz); log in $LOG_DIR/monado.log"
    # monado-service exits when stdin reaches EOF, so feed it a pipe that stays open.
    fifo="$LOG_DIR/.monado-stdin"; rm -f "$fifo"; mkfifo "$fifo"
    sleep infinity >"$fifo" &
    MONADO_FEEDER=$!
    SIMULATED_ENABLE=1 XRT_COMPOSITOR_FORCE_XCB=1 XRT_COMPOSITOR_DEFAULT_FRAMERATE=90 \
        monado-service <"$fifo" >"$LOG_DIR/monado.log" 2>&1 &
    MONADO_PID=$!
    for _ in $(seq 100); do
        kill -0 "$MONADO_PID" 2>/dev/null || { tail -20 "$LOG_DIR/monado.log" >&2; die "monado-service exited"; }
        [[ -S "$sock" && "$sock" -nt "$marker" ]] && break
        sleep 0.1
    done
    [[ -S "$sock" && "$sock" -nt "$marker" ]] || die "monado-service did not create $sock within 10 s"
elif [[ "$RUNTIME" == monado ]]; then
    log "using the monado-service that is already running"
fi

# ---- bridge and game -------------------------------------------------------
log "starting axrb-host-bridge (\"$TITLE\"); log in $LOG_DIR/bridge.log"
XR_RUNTIME_JSON="$XR_JSON" "$BRIDGE" --serve-openxr "$BRIDGE_PORT" 0 "$TITLE" > >(tee "$LOG_DIR/bridge.log") 2>&1 &
BRIDGE_PID=$!
for _ in $(seq 300); do
    kill -0 "$BRIDGE_PID" 2>/dev/null || die "axrb-host-bridge exited during startup; see $LOG_DIR/bridge.log"
    ss -Hltn "sport = :$BRIDGE_PORT" | grep -q . && break
    sleep 0.1
done
ss -Hltn "sport = :$BRIDGE_PORT" | grep -q . || die "axrb-host-bridge is not listening on :$BRIDGE_PORT after 30 s"

adbs logcat -c 2>/dev/null || true
adbs logcat -v time -s AXRB.Perf:I AXRB.Pacing:I AXRB.GPU:I AXRB.Accel:I >"$LOG_DIR/guest-axrb.log" 2>&1 &
LOGCAT_PID=$!

log "launching $PACKAGE/$ACTIVITY"
GAME_STARTED=1
adbs shell am start -W -n "$PACKAGE/$ACTIVITY" | grep -E 'Status|LaunchState|TotalTime' || true
game_pid=""
for _ in $(seq 60); do game_pid="$(sh_out pidof "$PACKAGE")"; [[ -n "$game_pid" ]] && break; sleep 0.5; done
[[ -n "$game_pid" ]] || die "$PACKAGE did not start"
log "running (guest pid $game_pid). Ctrl-C to stop. Guest frame-rate lines: $LOG_DIR/guest-axrb.log"

misses=0
while kill -0 "$BRIDGE_PID" 2>/dev/null; do
    # One failed adb call is not an exit; two in a row (or a new pid) is.
    if [[ "$(sh_out pidof "$PACKAGE")" == "$game_pid" ]]; then misses=0; else misses=$((misses + 1)); fi
    ((misses < 2)) || { log "$PACKAGE exited"; break; }
    sleep 2
done
kill -0 "$BRIDGE_PID" 2>/dev/null || log "axrb-host-bridge exited"
