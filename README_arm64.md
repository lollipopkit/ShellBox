# Shell Box — ARM64 Linux on iOS via Native Threaded-Code Interpreter

**Fork of [ish-app/ish](https://github.com/ish-app/ish)** — a userspace Linux emulator for iOS.

This fork adds a **native ARM64 guest backend** to upstream iSH's threaded-code interpreter
(*Asbestos*, formerly called *jit* — renamed upstream in 2024 because it doesn't actually emit
machine code). The new backend emulates AArch64 Linux on Apple Silicon, running alongside
the original x86 (i386) guest backend. The result is a dramatically faster and more
compatible Linux environment capable of running **Python, Node.js, Go, Rust, and native
CLI tools** directly on iPhone and iPad.

> ## 🚢 Production Use
>
> This engine is shipping in **[OpenMinis](https://openminis.app)** as the **Agent Shell Sandbox**,
> where it has been **stably used by over 10,000 users** to run Linux tools and shell workloads
> on iOS. The numbers and stability claims in this README are grounded in that real-world
> deployment, not just synthetic benchmarks.

> **Naming note**: *Asbestos* is the upstream project's name for its threaded-code
> interpreter (see the upstream commit [`d375656f` "Rename the JIT"](https://github.com/ish-app/ish/commit/d375656f)
> from June 2024). It is **not a JIT** — neither Asbestos nor its predecessor emits machine
> code at runtime. For each basic block it builds an array of pointers to pre-compiled
> native "gadget" functions that tail-call one another (the technique Forth interpreters use).
>
> What this fork adds is an **ARM64 guest backend** inside that same Asbestos infrastructure:
> new gadgets (`asbestos/guest-arm64/gadgets-aarch64/`) that map AArch64 guest instructions
> to a few ARM64 host instructions each — same-architecture dispatch, so each guest
> instruction costs only a handful of host instructions. The upstream x86 backend continues
> to ship unchanged alongside it. Some prose below says "JIT" as convenient shorthand —
> read it as "same-arch gadget dispatch," not runtime codegen.

---

## Why ARM64?

The original iSH translates **x86 (i386) instructions** on an ARM64 host — every guest instruction
must be cross-architecture decoded and emulated. This works well for simple tools but creates
fundamental limits:

| Limitation | x86 (original) | ARM64 (this fork) |
|---|---|---|
| Architecture translation | i386 → ARM64 (cross) | AArch64 → AArch64 (same) |
| Address space | 32-bit (4 GB) | 48-bit (256 TB) |
| SIMD | Partial SSE/SSE2 | Full NEON + Crypto |
| Node.js / V8 | Not possible (needs >4 GB VA) | Supported |
| Go / Rust | Not possible (large VA requirements) | Supported |
| Compute overhead | 14-208x native | 0.5-157x native in the current ARM64 pass |

## Architecture Overview

```
+--------------------------------------------------------------+
|  iOS App (Shell Box)                                         |
|                                                              |
|  +--------------------------------------------------------+  |
|  |  Asbestos (threaded-code interpreter)                  |  |
|  |                                                        |  |
|  |   Decoder  -->  Gadget program  -->  Fiber Blocks      |  |
|  |   (gen.c)       builder              (block cache)     |  |
|  |                                                        |  |
|  |   --- 48-bit Virtual Memory (4-level page table) ---   |  |
|  |       TLB (8192 entries) + CoW + Lazy Reservations     |  |
|  +--------------------------------------------------------+  |
|                                                              |
|  +-------------------+    +-------------------------------+  |
|  |  Linux Kernel     |    |  Agent Integration            |  |
|  |  (syscalls,       |    |  - ISHShellExecutor           |  |
|  |   signals,        |    |  - DebugServer (JSON-RPC)     |  |
|  |   futex, epoll)   |    |  - Native Offload             |  |
|  +-------------------+    |  - Bind Mounts                |  |
|                           +-------------------------------+  |
|  +-------------------+                                       |
|  |  Filesystem       |                                       |
|  |  fakefs + realfs  |                                       |
|  |  + bind mounts    |                                       |
|  +-------------------+                                       |
+--------------------------------------------------------------+
```

---

## Key Changes from Upstream

### 1. ARM64 Guest Backend inside Asbestos

This fork's main contribution. It plugs into upstream Asbestos (the existing threaded-code
interpreter) and replaces the per-instruction cost model: for each guest basic block the new
backend builds a **gadget program** — an array of `unsigned long` values alternating pointers
to pre-compiled ARM64 gadget functions with inline operands. Execution is a chain of tail
calls — each gadget loads the next pointer from the program stream and branches to it
(`br x8`). No executable memory is allocated, no machine code is generated at runtime.
The host-code overhead per guest instruction is a few ARM64 instructions inside the
corresponding gadget.

**Key files:**
- `asbestos/asbestos.c` — Block cache, block management, RCU-like jetsam cleanup
- `asbestos/guest-arm64/gen.c` — Instruction decoder + gadget program builder (~200+ opcodes)
- `asbestos/guest-arm64/gadgets-aarch64/` — Hand-written ARM64 assembly gadgets:
  - `entry.S` — fiber_enter/exit, crash recovery trampoline
  - `memory.S` — Load/store with inline TLB lookup (~12 instructions fast path)
  - `control.S` — Branches, conditionals, fused compare-and-branch
  - `math.S` — Arithmetic, shifts, bit manipulation, NEON/SIMD
  - `crypto.S` — AES, SHA, CRC32 instructions

**Design highlights:**
- **Block chaining**: Sequential basic blocks link directly, skipping dispatch overhead
- **Persistent TLB**: 8192-entry TLB survives across syscalls (not flushed on every entry)
- **Crash recovery**: SIGSEGV inside a gadget redirects to a trampoline for CoW resolution
- **Full NEON**: All 128-bit SIMD operations including crypto extensions

### 2. 48-bit Virtual Address Space

4-level page table (L0→L1→L2→L3, 9 bits each = 36-bit page number + 12-bit offset = 48 bits).

- Supports V8's 128GB+ pointer cage (via `MAP_NORESERVE` lazy reservations)
- Go's large virtual address requirements for heap/stack
- Guard pages at 0x0-0x100000 for V8 compressed pointer safety
- Layout kept compact (stack at `0xffffe000`, mmap at `0xefffd`) for TLB efficiency

**Key files:** `kernel/memory.h`, `kernel/memory.c`, `emu/tlb.h`

### 3. Node.js / V8 Support

Running Node.js on a userspace emulator required solving multiple V8-specific problems:

- **128GB MAP_NORESERVE**: Lazy address reservations that don't consume physical memory
- **Guard pages at 0x0-0x100000**: V8 compressed pointers dereference small integers —
  mapping the low 1MB as readable zeros prevents SIGSEGV
- **V8 binary patch**: 9-instruction code cave patch for `InterpreterEntryTrampoline`
  derived constructor bug (zero emulator overhead)
- **`--jitless --no-lazy`**: V8 flags to avoid Wasm compilation and lazy parsing issues
- **Exit cleanup**: Safety valves for stuck V8 threads during process exit

**Result**: `npm install`, `npm exec`, `npx`, and `create-next-app` all work.

### 4. Agent Integration

Mechanisms designed for AI agent orchestration on iOS:

#### ISHShellExecutor (`app/ISHShellExecutor.h`)

Objective-C API for programmatic shell execution with streaming output:

```objc
[ISHShellExecutor executeCommand:@"pip install requests"
                    lineCallback:^(NSString *line, BOOL isStdErr) {
                        NSLog(@"%@", line);
                    }
                      completion:^(ISHShellExecutionResult *result) {
                          NSLog(@"Exit code: %d", result.exitCode);
                      }];
```

#### DebugServer (`app/DebugServer.c`)

JSON-RPC over HTTP server for guest introspection:

```bash
# List files
curl localhost:1234 -d '{"jsonrpc":"2.0","id":1,"method":"fs.readdir","params":{"path":"/usr/bin"}}'

# Execute command
curl localhost:1234 -d '{"jsonrpc":"2.0","id":1,"method":"guest.exec","params":{"command":"python3 --version"}}'

# Inspect processes
curl localhost:1234 -d '{"jsonrpc":"2.0","id":1,"method":"task.list"}'
```

#### Native Offload (`kernel/native_offload.c`)

Bypass emulation entirely for registered binaries. Guest `execve()` is intercepted and
routed to a native handler or host binary:

```c
// Register handler (call once at startup)
native_offload_add_handler("ffmpeg", ffmpeg_main);

// Now guest `ffmpeg -i input.mp4 output.mp3` runs natively
// Arguments auto-translated from guest paths to host paths
```

Supports both in-process handlers (iOS + macOS) and `posix_spawn` delegation (macOS CLI).

#### Bind Mounts (`fs/fake.c`)

Mount host directories into the guest filesystem:

```c
// Read-only bind mount of host directory
fakefs_bind_mount("/host/path/to/data", "/mnt/data", /*read_only=*/true);
```

Enables AI agents to share files between the host app and the Linux guest without copying.

### 5. Rootfs Management

- **Debian bookworm aarch64** with full apt package manager
- **RootfsPatch.bundle**: Versioned overlay system for incremental rootfs updates
- **Polyfills**: WebAssembly polyfill for undici/llhttp, fetch polyfill for HTTP downloads
- **OPENSSL_armcap=0** and **GODEBUG/GOMAXPROCS** injection in `sys_execve`

---

## Build Configuration

| Target | Scheme | xcconfig | Guest Arch | Bundle ID Suffix |
|--------|--------|----------|------------|------------------|
| x86 (original) | iSH | `App.xcconfig` | i386 | — |
| ARM64 | Shell Box ARM64 | `AppARM64.xcconfig` | aarch64 | `com.lollipopkit.shellbox` |
| ARM64 + FFmpeg | Shell Box FFmpeg | `AppARM64-ffmpeg.xcconfig` | aarch64 | `.arm64` |

The ARM64 target links meson-built libraries (`libish.a`, `libish_emu.a`, `libfakefs.a`) directly
from `build-arm64-release/`, bypassing Xcode's auto-discovery of x86 library targets.

Modern iPhone / iOS 26 validation is tracked separately in
**[benchmark/MODERN_IPHONE_VERIFICATION.md](benchmark/MODERN_IPHONE_VERIFICATION.md)**.
That matrix records the current Xcode 26 / iPhone 17 simulator coverage and keeps
physical iPhone 17-family install, runtime, and performance checks pending until
they are captured on real hardware.

```bash
# One-time local build tooling
python3 -m venv .venv
.venv/bin/python -m pip install meson

# Build ARM64 CLI (macOS, for testing)
.venv/bin/meson setup build-arm64-release -Dguest_arch=arm64 --buildtype=release
ninja -C build-arm64-release

# Run
./build-arm64-release/ish -f ./debian-arm64-fakefs /bin/sh
```

---

## Performance

Measured with `benchmark/run.sh` on macOS 26.4.1 / Apple Silicon using guest-side
timing (startup overhead excluded). Full details in
**[benchmark/BENCHMARK_PERF.md](benchmark/BENCHMARK_PERF.md)**.

### Current ARM64 overhead vs Native

The latest checked-in report is an ARM64-only pass because the x86 rootfs was
not available on the test host. Treat x86 comparisons as historical unless
`benchmark/run.sh all` is rerun with both root filesystems present.

| Category | ARM64/Native | Notes |
|---|:---:|---|
| Shell and coreutils | 0.5-84.8x | Many file/crypto/process tests are within 0.5-3.0x; tight shell loops remain slow |
| C arithmetic/branch/memory | 2.2-6.6x | `int_arith_8M`, `float_arith_4M`, `branch_50M`, memory and matrix rows |
| C leaf calls | 4.6x | `func_call_50M`, after the ARM64 `BL` leaf-call fast path |
| C string scans | 32.6-156.5x | Current largest remaining C hotspot: `strlen_2M` / `strcmp_2M` |

### Headline numbers (compute-heavy)

- **C `int_arith_8M`**: ARM64 is **2.3x native** (81ms vs 35ms)
- **C `func_call_50M`**: ARM64 is **4.6x native** (101ms vs 22ms)
- **C `matrix_384x384`**: ARM64 is **3.0x native** (134ms vs 44ms)
- **C `branch_50M`**: ARM64 is **3.4x native** (150ms vs 44ms)
- **C string scans**: `strlen_2M` is **32.6x native** and `strcmp_2M` is
  **156.5x native**, so string/TLB-heavy loops are still the main optimization target.

> **Why ARM64 wins**: same-architecture gadget dispatch (each guest instruction costs only a
> few ARM64 host instructions inside its gadget), full NEON + crypto extensions, 48-bit
> address space for V8/Go/Rust, and Node.js-specific fixes (V8 binary patch, guard pages,
> `--jitless` injection, io_uring syscall) that the upstream x86 branch lacks. Node.js 22
> cannot run on x86 iSH (missing syscall 425 / `io_uring_setup`).

## Compatibility

205 tests across 18 categories (Core OS, FileOps, Text, Build, Python, Node.js, Go/Rust/Perl/…,
Network, VCS, Editors, Shell, DB, Media, Crypto, SysMon, Debug, PkgMgr, Signal). Both
architectures tested under fakefs with the same installed package set. Full report:
**[benchmark/BENCHMARK_COMPAT.md](benchmark/BENCHMARK_COMPAT.md)**.

| Architecture | Pass | Fail | Rate |
|---|:---:|:---:|:---:|
| **x86** (Jitter, threaded-code) | 201 | 4 | **98%** |
| **ARM64** (Asbestos, threaded-code) | 205 | 0 | **100%** |

**x86's 4 failures** are all genuine limitations (not benchmark bugs):
`automake`, `perl` (/dev/null write quirk), `go env`, `go compile` (32-bit VA).

---

## Supported Software

### Fully Working

| Category | Examples |
|----------|---------|
| **Package managers** | apt, pip, npm, npx, uv |
| **Languages** | Python 3, Node.js 22, Go, Perl, Ruby, Lua |
| **Dev tools** | git, curl, wget, ssh, vim, nano |
| **Build tools** | gcc, g++, cmake, make, meson |
| **Data tools** | sqlite3, jq, yt-dlp, ffmpeg (via native offload) |
| **Network** | curl, wget, dig, netstat, ss |
| **Node frameworks** | Express, Koa, Fastify, Axios, Socket.io |
| **npm ecosystem** | lodash, moment, dayjs, uuid, chalk, commander, glob, semver |

### Not Supported

- **GUI applications** (no X11/Wayland)
- **Docker / containers** (no kernel namespace support)
- **Kernel modules** (userspace emulator)
- **Hardware access** (no /dev/gpu, no USB passthrough)

---

## Commit History

86 commits on `feature-arm64`, 101 files changed, +23,198 / -7,620 lines.

Major milestones:
1. **Interpreter foundation**: fiber_enter/exit, basic block compilation (to gadget program), TLB
2. **Instruction coverage**: 200+ ARM64 opcodes including full NEON/Crypto
3. **48-bit address space**: 4-level page table, lazy reservations
4. **Node.js support**: V8 guard pages, MAP_NORESERVE, binary patch, exit cleanup
5. **Go support**: Signal frame alignment, sigreturn fixes, NZCV preservation
6. **Rust/uv support**: FUTEX_WAIT_BITSET, PMULL, BFM, demand-mapped reads
7. **Agent integration**: ISHShellExecutor, DebugServer, Native Offload, Bind Mounts
8. **Stability**: 50+ bug fixes for concurrency, memory leaks, use-after-free, deadlocks

---

## Project Structure

```
ShellBox/
├── asbestos/                    # ARM64 threaded-code interpreter
│   ├── asbestos.c/h             # Block cache, RCU cleanup
│   └── guest-arm64/
│       ├── gen.c                # Instruction decoder → gadgets
│       ├── crypto_helpers.c     # AES/SHA/CRC32 helpers
│       └── gadgets-aarch64/     # Assembly gadgets
│           ├── entry.S          # Fiber enter/exit, crash handler
│           ├── memory.S         # Load/store, TLB inline lookup
│           ├── control.S        # Branches, conditionals
│           ├── math.S           # ALU, shifts, NEON/SIMD
│           ├── crypto.S         # AES, SHA, PMULL, CRC32
│           ├── bits.S           # Bitfield operations
│           └── gadgets.h        # Register map, TLB macros
├── emu/
│   ├── tlb.c/h                  # TLB miss handling, cross-page
│   └── arch/arm64/
│       ├── cpu.h                # CPU state (regs, NEON, flags)
│       └── decode.h             # Instruction field extraction
├── kernel/
│   ├── arch/arm64/calls.c       # ARM64 syscall table
│   ├── memory.c/h               # Page table, CoW, fault handling
│   ├── mmap.c                   # mmap, lazy reservations
│   ├── native_offload.c/h       # Binary offload system
│   ├── signal.c/h               # Signal delivery/frame
│   ├── futex.c                  # Futex with pipe wakeup
│   ├── exec.c                   # ELF loader, V8 guard pages
│   └── exit.c                   # Thread cleanup, safety valves
├── fs/
│   ├── fake.c/h                 # fakefs + bind mount support
│   ├── real.c                   # Host filesystem access
│   ├── sock.c/h                 # Socket emulation
│   └── poll.c                   # epoll/poll/select
├── app/
│   ├── AppARM64.xcconfig        # ARM64 build config
│   ├── GuestARM64.xcconfig      # Guest arch definition
│   ├── ISHShellExecutor.h/m     # Shell execution API
│   ├── DebugServer.c/h          # JSON-RPC debug server
│   └── RootfsPatch.bundle/      # Versioned rootfs overlay
└── benchmark/
    ├── run.sh                    # Unified benchmark entry point
    ├── assets/                   # shellbench.sh + cbench_lite + prebuilt binaries
    ├── BENCHMARK_PERF.md         # Performance report
    └── BENCHMARK_COMPAT.md       # Compatibility report
```

---

## License

Same as upstream iSH. See [LICENSE](LICENSE).
