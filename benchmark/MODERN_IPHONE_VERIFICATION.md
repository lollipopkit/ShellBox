# Modern iPhone / iOS 26 Verification

This file tracks the verification surface for the ARM64 guest build on current
iPhone-class devices and SDKs. It is intentionally evidence-oriented: keep a
row as pending unless it was run on the named target.

## Current Host Baseline

| Item | Value |
|---|---|
| Host | macOS 26.4.1 / arm64 |
| Xcode SDKs observed | iPhoneOS 26.4, iPhoneSimulator 26.4 |
| ARM64 scheme | `Shell Box` |
| ARM64 target | `Shell Box ARM64` |
| Local Meson | `.venv/bin/meson` 1.11.1 |
| Xcode 26 asset build note | `Shell Box ARM64` uses `AppARM64.xcconfig`; `GuestARM64.xcconfig` excludes `x86_64` for iPhone Simulator so `actool` and generated-source phases run against the valid ARM64 simulator slice |

## Verification Matrix

| Surface | Target | Status | Evidence |
|---|---|---|---|
| CLI ARM64 JIT smoke | macOS host binary + ARM64 fakefs | Pass | `build-arm64-release/ish -f alpine-arm64-fakefs /bin/echo hello` prints `hello` |
| ARM64 JIT BL/LR semantic smoke | macOS host binary + ARM64 fakefs | Pass | `benchmark/jit_smoke_arm64.sh` compiles a real `add; ret` leaf and verifies `BL` leaves `X30` equal to the return address |
| CLI ARM64 performance | macOS host binary + ARM64 fakefs | Pass | `benchmark/run.sh arm64`, latest report in `benchmark/BENCHMARK_PERF.md` |
| iPhoneOS app build | Shell Box ARM64 / Release / iPhoneOS 26.4 SDK | Pass | `xcodebuild -project ShellBox.xcodeproj -target "Shell Box ARM64" -configuration Release CODE_SIGNING_ALLOWED=NO build` |
| iPhone 17 simulator build | Shell Box / Release / iPhone 17 / iOS 26.4.1 Simulator | Pass | `xcodebuild -project ShellBox.xcodeproj -scheme "Shell Box" -configuration Release -destination 'platform=iOS Simulator,name=iPhone 17,OS=26.4.1' CODE_SIGNING_ALLOWED=NO build` |
| iPhone 17 simulator install/launch | Shell Box / Release / iPhone 17 / iOS 26.4.1 Simulator | Pass | `benchmark/verify_modern_iphone.sh --build-simulator --install-simulator --launch-simulator` boots the iPhone 17 simulator, installs the app bundle, launches `com.lollipopkit.shellbox`, verifies the launched process stays alive, and probes the ARM64 debug server |
| iPhone 17 simulator destination discovery | Shell Box scheme | Pass | `xcodebuild -project ShellBox.xcodeproj -scheme "Shell Box" -showdestinations` lists iPhone 17, 17 Pro, 17 Pro Max, 17e, and iPhone Air simulators |
| Physical iOS device availability | Xcode device list | Pending | `benchmark/verify_modern_iphone.sh` reports no connected physical iPhone/iPad; Xcode has an offline `iPhone 16 Pro (iPhone17,1)` on iOS 26.4.1 and an offline iPad Pro |
| Real iPhone 17-family install | Physical iPhone on iOS 26.x | Pending | Requires a connected physical iPhone; simulator and offline devices are not sufficient |
| Real iPhone 17-family runtime smoke | Physical iPhone on iOS 26.x | Pending | Requires a connected device and manual/automated terminal smoke |
| Real iPhone 17-family performance | Physical iPhone on iOS 26.x | Pending | Requires on-device timing capture; host CLI benchmark is not a substitute |

## Repeatable Commands

Set up the project-local Meson used by both CLI and Xcode script phases:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install meson
.venv/bin/meson --version
```

Run the local verifier for the repeatable host-side checks:

```sh
benchmark/verify_modern_iphone.sh
```

Optional heavier gates:

```sh
benchmark/verify_modern_iphone.sh --build-simulator --build-device --run-jit-smoke --run-perf
```

Run the iPhone 17 simulator install/launch smoke:

```sh
benchmark/verify_modern_iphone.sh --build-simulator --install-simulator --launch-simulator
```

The simulator gate uses `SIM_DEVICE_ID` if set, otherwise it uses the first
available simulator named `iPhone 17`. If `SIM_APP_PATH` is omitted, it is
inferred from Xcode build settings after the simulator build. After launch, the
verifier terminates any bundle ids listed in `SIM_TERMINATE_BUNDLE_IDS` to avoid
legacy debug-server port conflicts, then probes `DEBUG_SERVER_URL`, which
defaults to `http://127.0.0.1:1234/`.

Require a connected physical iPhone/iPad before running on-device gates:

```sh
benchmark/verify_modern_iphone.sh --require-physical
```

Install and launch a signed app bundle on a connected physical device:

```sh
DEVICE_ID=<device-uuid-or-name> \
APP_PATH="/path/to/Shell Box.app" \
BUNDLE_ID=com.lollipopkit.shellbox \
  benchmark/verify_modern_iphone.sh --require-physical --install-device --launch-device
```

If `DEVICE_ID` is omitted, the verifier tries to use the first online iPhone or
iPad reported by `xcrun devicectl list devices`. `APP_PATH` must point to a
signed `.app` bundle; the generic `CODE_SIGNING_ALLOWED=NO` build is only a
compile gate and is not valid install evidence. The physical launch gate uses
`devicectl --json-output` to capture the launched PID, waits
`DEVICE_LAUNCH_SETTLE_SECONDS` seconds, and checks `devicectl device info
processes` to make sure that PID is still visible.

List the shared ARM64 scheme and modern destinations:

```sh
xcodebuild -project ShellBox.xcodeproj -list
xcodebuild -project ShellBox.xcodeproj -scheme "Shell Box" -showdestinations
```

Build the ARM64 app for the iPhone 17 simulator:

```sh
env PATH="$PWD/.venv/bin:$PATH" \
  xcodebuild -project ShellBox.xcodeproj \
    -scheme "Shell Box" \
    -configuration Release \
    -destination 'platform=iOS Simulator,name=iPhone 17,OS=26.4.1' \
    CODE_SIGNING_ALLOWED=NO \
    build
```

Build the ARM64 app for generic iPhoneOS:

```sh
env PATH="$PWD/.venv/bin:$PATH" \
  xcodebuild -project ShellBox.xcodeproj \
    -target "Shell Box ARM64" \
    -configuration Release \
    CODE_SIGNING_ALLOWED=NO \
    build
```

Run the local ARM64 performance pass:

```sh
benchmark/run.sh arm64
```

## On-device Criteria

Before marking modern iPhone support complete, capture these on at least one
physical iPhone 17-family device running iOS 26.x:

| Check | Expected evidence |
|---|---|
| Install | Device UDID, iOS version, build command, install result |
| Launch | App reaches terminal without crash or watchdog termination |
| Shell smoke | `uname -a`, `cat /proc/uptime`, `echo hello`, simple file create/read |
| Compiler smoke | Compile and run `benchmark/assets/cbench_lite.c` inside the guest |
| Performance | Same C rows as `BENCHMARK_PERF.md`, recorded as device medians |
| Thermal note | Device model, power state, thermal state if available |

Do not treat simulator-only success as real-device completion. The simulator is
useful for SDK, asset, storyboard, and architecture coverage, but it does not
exercise iOS device scheduling, thermal behavior, code signing/install paths, or
real hardware performance.
