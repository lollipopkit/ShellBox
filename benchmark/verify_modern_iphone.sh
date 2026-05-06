#!/bin/bash
# Verify the local, repeatable parts of the modern iPhone/iOS 26 matrix.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SCHEME="iSH-ARM64"
SIM_DESTINATION="${SIM_DESTINATION:-platform=iOS Simulator,name=iPhone 17,OS=26.4.1}"
RUN_SIM_BUILD=0
RUN_DEVICE_BUILD=0
RUN_PERF=0
REQUIRE_PHYSICAL=0
RUN_JIT_SMOKE=0
RUN_INSTALL_SIM=0
RUN_LAUNCH_SIM=0
RUN_INSTALL_DEVICE=0
RUN_LAUNCH_DEVICE=0
SIM_DEVICE_ID="${SIM_DEVICE_ID:-}"
SIM_APP_PATH="${SIM_APP_PATH:-}"
DEBUG_SERVER_URL="${DEBUG_SERVER_URL:-http://127.0.0.1:1234/}"
DEVICE_LAUNCH_SETTLE_SECONDS="${DEVICE_LAUNCH_SETTLE_SECONDS:-2}"
DEVICE_ID="${DEVICE_ID:-}"
APP_PATH="${APP_PATH:-}"
BUNDLE_ID="${BUNDLE_ID:-app.ish.iSH.arm64}"

usage() {
    cat <<EOF
Usage: benchmark/verify_modern_iphone.sh [options]

Options:
  --build-simulator   Build ${SCHEME} for ${SIM_DESTINATION}
  --build-device      Build ${SCHEME} for generic iPhoneOS
  --run-jit-smoke     Run ARM64 JIT semantic smoke tests
  --run-perf          Run benchmark/run.sh arm64
  --require-physical  Fail unless a connected physical iPhone/iPad is visible
  --install-simulator Install the simulator app bundle onto the iPhone 17 simulator
  --launch-simulator  Launch BUNDLE_ID on the iPhone 17 simulator
  --install-device    Install APP_PATH onto a connected physical device
  --launch-device     Launch BUNDLE_ID on a connected physical device
  -h, --help          Show this help

Environment:
  SIM_DESTINATION     Override simulator destination.
  SIM_DEVICE_ID       Simulator UDID. If omitted, the first available "iPhone 17" simulator is used.
  SIM_APP_PATH        Simulator .app bundle to install. If omitted, the verifier tries to infer it from xcodebuild settings.
  SIM_LAUNCH_SETTLE_SECONDS
                     Seconds to wait before checking simulator process survival. Default: 2.
  DEBUG_SERVER_URL   URL probed after simulator launch. Default: ${DEBUG_SERVER_URL}
  DEVICE_LAUNCH_SETTLE_SECONDS
                     Seconds to wait before checking physical-device process survival. Default: ${DEVICE_LAUNCH_SETTLE_SECONDS}
  DEVICE_ID           Physical device id/name. If omitted, the first online CoreDevice iPhone/iPad is used.
  APP_PATH            Signed .app bundle to install when --install-device is used.
  BUNDLE_ID           Bundle id to launch when --launch-device is used. Default: ${BUNDLE_ID}
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --build-simulator)
            RUN_SIM_BUILD=1
            ;;
        --build-device)
            RUN_DEVICE_BUILD=1
            ;;
        --run-perf)
            RUN_PERF=1
            ;;
        --run-jit-smoke)
            RUN_JIT_SMOKE=1
            ;;
        --install-simulator)
            RUN_INSTALL_SIM=1
            ;;
        --launch-simulator)
            RUN_LAUNCH_SIM=1
            ;;
        --require-physical)
            REQUIRE_PHYSICAL=1
            ;;
        --install-device)
            RUN_INSTALL_DEVICE=1
            REQUIRE_PHYSICAL=1
            ;;
        --launch-device)
            RUN_LAUNCH_DEVICE=1
            REQUIRE_PHYSICAL=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

cd "$PROJECT_DIR"
export PATH="$PROJECT_DIR/.venv/bin:$PATH"

ok() {
    echo "OK  $*"
}

note() {
    echo "NOTE $*"
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "error: missing required command: $1" >&2
        exit 127
    fi
}

require_cmd xcodebuild
require_cmd meson
require_cmd xcrun

ok "meson $(meson --version)"

infer_sim_app_path() {
    xcodebuild -project iSH.xcodeproj \
        -scheme "$SCHEME" \
        -configuration Release \
        -destination "$SIM_DESTINATION" \
        -showBuildSettings 2>/dev/null | awk '
            /^[[:space:]]*BUILT_PRODUCTS_DIR = / {
                sub(/^[[:space:]]*BUILT_PRODUCTS_DIR = /, "")
                built = $0
            }
            /^[[:space:]]*WRAPPER_NAME = / {
                sub(/^[[:space:]]*WRAPPER_NAME = /, "")
                wrapper = $0
            }
            END {
                if (built != "" && wrapper != "") {
                    print built "/" wrapper
                }
            }'
}

schemes="$(mktemp)"
destinations="$(mktemp)"
devices="$(mktemp)"
core_devices="$(mktemp)"
cleanup() {
    rm -f "$schemes" "$destinations" "$devices" "$core_devices"
}
trap cleanup EXIT

xcodebuild -project iSH.xcodeproj -list >"$schemes"
if grep -q "^[[:space:]]*${SCHEME}$" "$schemes"; then
    ok "scheme ${SCHEME} is shared"
else
    echo "error: scheme ${SCHEME} was not listed by xcodebuild" >&2
    exit 1
fi

xcodebuild -project iSH.xcodeproj -scheme "$SCHEME" -showdestinations >"$destinations"
if grep -q "name:iPhone 17" "$destinations"; then
    ok "iPhone 17 simulator destination is available"
else
    echo "error: iPhone 17 simulator destination was not listed" >&2
    exit 1
fi

if grep -q "iPhone 17 Pro" "$destinations"; then
    ok "iPhone 17-family simulator destinations are visible"
fi

if grep -q "Any iOS Device" "$destinations"; then
    note "generic iPhoneOS destination is visible; physical device install/runtime/perf still require real hardware"
fi

online_ios=""
offline_ios=""
core_online_ios=""
core_offline_ios=""

if xcrun xctrace list devices >"$devices" 2>/dev/null; then
    online_ios="$(awk '
        /^== Devices ==/ {section = "devices"; next}
        /^==/ {section = ""}
        section == "devices" && /iPhone|iPad/ {print}
    ' "$devices")"
else
    note "xcrun xctrace list devices failed; skipping physical-device availability check"
fi

if xcrun devicectl list devices >"$core_devices" 2>/dev/null; then
    core_online_ios="$(grep -E 'iPhone|iPad' "$core_devices" | grep -v 'unavailable' || true)"
    core_offline_ios="$(grep -E 'iPhone|iPad' "$core_devices" | grep 'unavailable' || true)"
    if [ -z "$online_ios" ]; then
        online_ios="$core_online_ios"
    fi
    offline_ios="$core_offline_ios"
fi

if [ -n "$online_ios" ]; then
    ok "connected physical iOS/iPadOS device is visible"
    printf '%s\n' "$online_ios" | sed 's/^/    /'
else
    note "no connected physical iPhone/iPad is currently available"
fi

if [ -n "$offline_ios" ]; then
    note "offline physical iOS/iPadOS devices seen by Xcode:"
    printf '%s\n' "$offline_ios" | sed 's/^/    /'
fi

if [ "$REQUIRE_PHYSICAL" -eq 1 ] && [ -z "$online_ios" ]; then
    echo "error: --require-physical requested, but no connected physical iPhone/iPad is visible" >&2
    exit 1
fi

if [ -z "$DEVICE_ID" ] && [ -n "$core_online_ios" ]; then
    DEVICE_ID="$(printf '%s\n' "$core_online_ios" | awk 'NF >= 3 {print $3; exit}')"
fi

if [ "$RUN_INSTALL_DEVICE" -eq 1 ] || [ "$RUN_LAUNCH_DEVICE" -eq 1 ]; then
    require_cmd xcrun
    if [ -z "$DEVICE_ID" ]; then
        echo "error: DEVICE_ID is required for device install/launch, and no online CoreDevice id could be inferred" >&2
        exit 1
    fi
fi
if [ "$RUN_LAUNCH_DEVICE" -eq 1 ]; then
    require_cmd python3
fi

if [ "$RUN_SIM_BUILD" -eq 1 ]; then
    xcodebuild -project iSH.xcodeproj \
        -scheme "$SCHEME" \
        -configuration Release \
        -destination "$SIM_DESTINATION" \
        CODE_SIGNING_ALLOWED=NO \
        build
    ok "simulator build passed: $SIM_DESTINATION"
fi

if [ -z "$SIM_DEVICE_ID" ] && { [ "$RUN_INSTALL_SIM" -eq 1 ] || [ "$RUN_LAUNCH_SIM" -eq 1 ]; }; then
    SIM_DEVICE_ID="$(xcrun simctl list devices available 2>/dev/null |
        sed -n 's/.*iPhone 17 (\([A-F0-9-]*\)).*/\1/p' |
        head -n 1)"
fi

if [ "$RUN_INSTALL_SIM" -eq 1 ] || [ "$RUN_LAUNCH_SIM" -eq 1 ]; then
    if [ -z "$SIM_DEVICE_ID" ]; then
        echo "error: SIM_DEVICE_ID is required for simulator install/launch, and no available iPhone 17 simulator could be inferred" >&2
        exit 1
    fi
    xcrun simctl boot "$SIM_DEVICE_ID" 2>/dev/null || true
    xcrun simctl bootstatus "$SIM_DEVICE_ID" -b
fi

if [ "$RUN_INSTALL_SIM" -eq 1 ]; then
    if [ -z "$SIM_APP_PATH" ]; then
        SIM_APP_PATH="$(infer_sim_app_path)"
    fi
    if [ -z "$SIM_APP_PATH" ] || [ ! -d "$SIM_APP_PATH" ]; then
        echo "error: SIM_APP_PATH does not point to an existing simulator app bundle: ${SIM_APP_PATH:-<empty>}" >&2
        echo "hint: run --build-simulator first or set SIM_APP_PATH explicitly" >&2
        exit 1
    fi
    xcrun simctl install "$SIM_DEVICE_ID" "$SIM_APP_PATH"
    ok "installed simulator app on iPhone 17 simulator: $SIM_DEVICE_ID"
fi

if [ "$RUN_LAUNCH_SIM" -eq 1 ]; then
    require_cmd curl
    launch_output="$(xcrun simctl launch --terminate-running-process "$SIM_DEVICE_ID" "$BUNDLE_ID" 2>&1)" || {
        printf '%s\n' "$launch_output" >&2
        exit 1
    }
    printf '%s\n' "$launch_output"
    launch_pid="$(printf '%s\n' "$launch_output" | awk -F': ' 'NF >= 2 {print $2; exit}')"
    if [ -z "$launch_pid" ]; then
        echo "error: could not parse simulator launch pid from: $launch_output" >&2
        exit 1
    fi
    sleep "${SIM_LAUNCH_SETTLE_SECONDS:-2}"
    launch_state="$(mktemp)"
    if ! xcrun simctl spawn "$SIM_DEVICE_ID" launchctl print "pid/$launch_pid" >"$launch_state" 2>&1; then
        cat "$launch_state" >&2
        rm -f "$launch_state"
        echo "error: simulator app process $launch_pid is no longer alive after launch" >&2
        exit 1
    fi
    if grep -q "properties = slain" "$launch_state"; then
        cat "$launch_state" >&2
        rm -f "$launch_state"
        echo "error: simulator app process $launch_pid was slain after launch" >&2
        exit 1
    fi
    rm -f "$launch_state"
    ok "launched and kept $BUNDLE_ID alive on iPhone 17 simulator: $SIM_DEVICE_ID"

    debug_response="$(curl --fail --silent --show-error --max-time 3 "$DEBUG_SERVER_URL")" || {
        echo "error: simulator debug server did not respond at $DEBUG_SERVER_URL" >&2
        exit 1
    }
    printf '%s\n' "$debug_response" | grep -q '"status":"ok"' || {
        echo "error: simulator debug server returned an unexpected response: $debug_response" >&2
        exit 1
    }
    ok "simulator debug server responded at $DEBUG_SERVER_URL"
fi

if [ "$RUN_DEVICE_BUILD" -eq 1 ]; then
    xcodebuild -project iSH.xcodeproj \
        -target "$SCHEME" \
        -configuration Release \
        CODE_SIGNING_ALLOWED=NO \
        build
    ok "generic iPhoneOS build passed"
fi

if [ "$RUN_JIT_SMOKE" -eq 1 ]; then
    "$SCRIPT_DIR/jit_smoke_arm64.sh"
    ok "ARM64 JIT semantic smoke passed"
fi

if [ "$RUN_PERF" -eq 1 ]; then
    "$SCRIPT_DIR/run.sh" arm64
    ok "ARM64 local performance pass completed"
fi

if [ "$RUN_INSTALL_DEVICE" -eq 1 ]; then
    if [ -z "$APP_PATH" ]; then
        echo "error: APP_PATH is required for --install-device" >&2
        exit 1
    fi
    if [ ! -d "$APP_PATH" ]; then
        echo "error: APP_PATH does not point to an app bundle directory: $APP_PATH" >&2
        exit 1
    fi
    xcrun devicectl device install app --device "$DEVICE_ID" "$APP_PATH"
    ok "installed app on physical device: $DEVICE_ID"
fi

if [ "$RUN_LAUNCH_DEVICE" -eq 1 ]; then
    device_launch_json="$(mktemp)"
    device_processes_json="$(mktemp)"
    xcrun devicectl device process launch \
        --device "$DEVICE_ID" \
        --terminate-existing \
        --json-output "$device_launch_json" \
        "$BUNDLE_ID"
    launch_pid="$(python3 - "$device_launch_json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)

keys = {"pid", "processID", "processId", "processIdentifier"}

def walk(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key in keys and isinstance(item, int):
                print(item)
                return True
        for item in value.values():
            if walk(item):
                return True
    elif isinstance(value, list):
        for item in value:
            if walk(item):
                return True
    return False

walk(data)
PY
)"
    if [ -z "$launch_pid" ]; then
        cat "$device_launch_json" >&2
        rm -f "$device_launch_json" "$device_processes_json"
        echo "error: could not parse physical-device launch pid from devicectl JSON output" >&2
        exit 1
    fi

    sleep "$DEVICE_LAUNCH_SETTLE_SECONDS"
    xcrun devicectl device info processes \
        --device "$DEVICE_ID" \
        --json-output "$device_processes_json" >/dev/null
    if ! python3 - "$device_processes_json" "$launch_pid" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
pid = int(sys.argv[2])
keys = {"pid", "processID", "processId", "processIdentifier"}

def has_pid(value):
    if isinstance(value, dict):
        if any(key in value and value[key] == pid for key in keys):
            return True
        return any(has_pid(item) for item in value.values())
    if isinstance(value, list):
        return any(has_pid(item) for item in value)
    return False

sys.exit(0 if has_pid(data) else 1)
PY
    then
        cat "$device_processes_json" >&2
        rm -f "$device_launch_json" "$device_processes_json"
        echo "error: physical-device app process $launch_pid is no longer visible after launch" >&2
        exit 1
    fi
    rm -f "$device_launch_json" "$device_processes_json"
    ok "launched and kept $BUNDLE_ID alive on physical device: $DEVICE_ID (pid $launch_pid)"
fi

note "real iPhone 17-family install, runtime smoke, and device performance remain pending until run on physical iOS 26 hardware"
