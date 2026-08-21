# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

ShellBox is a fork of [ish-app/ish](https://github.com/ish-app/ish) — a userspace Linux emulator
for iOS. Upstream emulates an **i386 guest**; this fork replaced it with an **AArch64 guest
backend** so ARM64 Linux binaries run on Apple Silicon through the same threaded-code interpreter
(*Asbestos*).

The iOS app, the x86 backend, the `-Dkernel=linux` mode and the unicorn engine were all removed:
what is left is the engine an embedder links. Merging from upstream is no longer a goal — do not
preserve upstream shape at the cost of clarity.

Consumers link this as a library: `.github/workflows/static-libs.yml` builds `libish.a`,
`libish_emu.a` and `libfakefs.a` for `iphoneos` and `iphonesimulator` on every push to `main` and
publishes them as a `vX.Y.Z` release. ServerBox carries this repo as a submodule and downloads
those artifacts instead of building them. Changing the set of libraries, their names, or the
release tag scheme breaks that consumer.

No submodules.

## Build and test

The host must be arm64. Gadgets are `asbestos/guest-arm64/gadgets-<host cpu family>` and
`gadgets-aarch64` is the only set that exists, so anything else fails at setup.

```bash
meson setup build
ninja -C build
```

Prerequisites: python3, meson, ninja, clang + lld (`brew install llvm lld`), sqlite3, libarchive.

`-Dguest_arch=arm64` is accepted and is the only value. The option exists because ServerBox's
scripts pass it and meson fails a setup naming an option it does not have.

Tests are meson tests:

```bash
ninja -C build test              # everything
meson test -C build sock         # one test by name
meson test -C build --verbose fakedb
```

Registered tests: `sock`, `fakedb`, `fchdir`, all in `tests/unit/`; `fakedb` and `fchdir` link the
whole kernel. A cross build registers none of them — the binary would be built for a phone.

End-to-end coverage is `tests/regress/run.sh`, which is **not** a meson test: it needs an aarch64
guest cross-compiler (`$CC_GUEST`, default `aarch64-linux-musl-gcc`) and a rootfs, and skips when
they are absent. It pins the O_DIRECTORY/O_NOFOLLOW, fakefs stat, and waitpid/waitid fixes.

### Running the emulator

```bash
./build/tools/fakefsify alpine-minirootfs.tar.gz alpine   # rootfs tarball -> fakefs dir
./build/ish -f alpine /bin/sh
```

`ish` options: `-f`/`-r` root, `-d` workdir, `-c` console, `-n` register a native-offload binary
(`-n ffmpeg`, `-n ffprobe=/usr/local/bin/ffprobe`, macOS only).
`benchmark/create_fakefs.py` builds a `meta.db` from an already-unpacked rootfs directory.
`benchmark/run.sh` wants `build-arm64-release/` and `alpine-arm64-fakefs/`; its x86 columns are
filled in only if a pre-removal `build-x86-release/` happens to be lying around, and are otherwise
printed as `—`.

### iOS

There is no iOS app and no Xcode project here — both were removed; the engine ships as three
static libraries. Cross-compile them with a meson cross file naming the SDK, and ask for the
library targets by name (`ninja` alone fails: `tools/fakefsify` links the host's libarchive and
cannot be built for a phone):

```bash
meson setup build-ios . -Dguest_arch=arm64 --buildtype=release --cross-file ios.ini
ninja -C build-ios libish.a libish_emu.a libfakefs.a
```

`.github/workflows/static-libs.yml` and `.github/workflows/ci.yml` both contain a working cross
file; ServerBox's `scripts/build-ish-ios.sh` generates the same one.

### Logging

Channels are compiled in, off by default: `meson configure -Dlog='strace verbose'`, or
`-DDEBUG_<channel>=1` from an embedder's own build. Channels are `verbose`, `instr`, `debug`,
`strace`, `memory` (see `debug.h`). A
file picks its channel with `#define DEFAULT_CHANNEL <name>` before including `debug.h`; `TRACE()`
then routes there and compiles to nothing when the channel is off.

Runtime diagnostics are environment variables, all prefixed `ISH_` (`ISH_EXEC_TRACE`,
`ISH_LOCK_TRACE`, `ISH_WATCH_PAGE`, `ISH_OFFLOAD_STATS`, `ISH_NO_CHAIN`, …). Grep `getenv("ISH_`
for the current set. Some require a build flag (`-DLOCK_SLOW_TRACE=1`, `-DISH_GADGET_PROFILE`).

## Architecture

Layers, roughly outermost to innermost:

- **`main.c` + `xX_main_Xx.h`** — the host CLI, and the only in-repo example of driving the engine:
  `mount_root()` → `become_first_process()` → `do_execve()` → `task_start()`. An embedder writes
  its own; ServerBox's is `ios/Runner/ish/sbm_ish.c`.
- **`kernel/`** — syscall implementations, process/thread lifecycle, signals, memory, futex, ELF
  loading. `kernel/arch/arm64/calls.c` holds the syscall table.
- **`fs/`** — VFS with pluggable mounts: `fake.c` (fakefs), `real.c` (host passthrough), `proc*`,
  `sock.c`, `tty.c`/`pty.c`, `dev.c`, `tmp.c`.
- **`asbestos/`** — the threaded-code interpreter. Not a JIT: it builds an array of pointers to
  pre-compiled assembly gadgets that tail-call each other. `asbestos.c` is the block cache and
  RCU-like reclamation; `guest-arm64/gen.c` decodes guest instructions into gadget programs;
  `guest-arm64/gadgets-aarch64/*.S` are the gadgets.
- **`emu/`** — guest CPU state (`emu/arch/arm64/cpu.h`) and the software TLB (`emu/tlb.[ch]`).
- **`tools/`** — `fakefsify`, `prebuilt_gadget_gen/`, `staticdefine.sh`.

### GUEST_ARM64

meson passes `-DGUEST_ARM64=1` and the headers read it: it decides `addr_t` and `time_t_`
(`misc.h`). It survived the removal of the x86 backend because an **embedder's** build compiles
those headers, not this one — ServerBox sets it in `ios/Flutter/Ish.xcconfig`. Nothing else is
conditional on the guest any more; `unifdef -DGUEST_ARM64 -UGUEST_X86` took the dead branches
out.

### Syscall path

`handle_interrupt()` in `kernel/calls.c` dispatches `INT_SYSCALL` through `syscall_table[]`.
Conventions:

- `sys_*` return a negative errno from `kernel/errno.h` (`_EINVAL`, `_EFAULT`, …) — the leading
  underscore distinguishes guest errnos from host `errno.h` values. Errors are propagated as
  `int_t`/`dword_t` return values, not `errno`.
- Guest memory is only touched via `user_read`/`user_write`/`user_get`/`user_put` (declared
  `must_check` in `kernel/calls.h`). Never dereference a guest `addr_t`.
- `f_get()` returns a `struct fd *` with a reference **taken**, recorded on the task and released
  by `syscall_refs_drain()` when the syscall returns. Do not `fd_close()` an `f_get` result.
- Unimplemented syscalls point at `syscall_stub`.

### Assembly ↔ C offsets

Gadgets reference `struct cpu_state` fields through `cpu-offsets.h`, generated at build time from
`asbestos/offsets.c` by `tools/staticdefine.sh`. Adding a CPU-state field that assembly needs means
adding an `OFFSET(CPU, cpu_state, field)` line there. Registers reserved by the gadgets (`_cpu`,
`_tlb`, `_pc`, `_tmp`, …) are declared in `asbestos/guest-arm64/gadgets-aarch64/gadgets.h`; a
gadget that clobbers one of them corrupts guest state in ways that surface far from the cause.

### fakefs

A fakefs directory is `data/` (real files, flat-ish, paths with the leading `/` stripped — see
`fs/fix_path.h`) plus `meta.db`, a SQLite database of `paths` (path blob → inode) and `stats`
(inode → packed stat blob). The root is stored as the **empty blob**, not `"/"`. Ownership, modes
and device nodes live in `meta.db` because iOS gives an app no real ones. `fs/fake-migrate.c` and
`fake-rebuild.c` handle schema upgrades and recovery; bump `PRAGMA user_version` in step with them.

Bind mounts (`fakefs_bind_mount` in `fs/fake.h`) graft a host directory into the guest tree and can
be created after boot by the embedder; `fakefs_record_change` feeds a change-event ring buffer that
the host app consumes.

### Native offload

Three independent mechanisms let native code replace emulated code, all in `kernel/`:

1. **Binary level** (`native_offload.c`) — guest `execve()` of a registered name runs an in-process
   handler or (macOS) a host binary via `posix_spawn`, with guest paths translated to host paths.
2. **Symbol level** (`native_offload_sym.c`) — replace individual guest functions.
3. **Prebuilt gadget** (`native_offload_prebuilt.c`) — whole-function specialized gadget blocks;
   `tools/prebuilt_gadget_gen/gen.sh` generates a spec from a guest ELF symbol.

Verification handlers for 2 and 3 live under `kernel/offload_tests/` and compile only with
`-Doffload_test_symbol=true` / `-Doffload_test_prebuilt=true`. They self-register via constructors,
so meson `link_whole`s them; the product core has no test hook.

### Concurrency

Locks are `lock_t` from `util/sync.h` (a pthread mutex plus owner tracking). Struct fields carry
`// locked by X` comments — keep them accurate, they are the only record of the locking scheme.
`-DLOCK_DEBUG=1` catches recursive locking and unlock-without-lock; `-DLOCK_SLOW_TRACE=1` plus
`ISH_LOCK_TRACE` reports waits over 3s, which is how lock-order inversions get found. `wait_for()`
is the interruptible condition wait and returns `_EINTR`/`_ETIMEDOUT` — it is `must_check`.

## Conventions

- C11 (`gnu11`), `warning_level=2`, 4-space indent, LF, UTF-8 (`.editorconfig`).
- Commit messages: `type(scope): summary`, e.g. `fix(fs): ...`, `feat(epoll): ...`,
  `ci(static-libs): ...`.
- Comments in this fork explain *why* — particularly the failure a fix addresses. Several files
  carry long comments recording bugs that were expensive to find; do not compress them away.
- Non-trivial behavioural changes come with a unit test under `tests/unit/`. Where the code cannot
  be reached from a standalone binary there is currently nothing to write it in — say so rather
  than leaving the change untested silently.

## A finding is not a bug until a test says so

This applies to review comments, issue reports, and anything else that arrives claiming the code
is wrong. **Reproduce it first, then open a pull request — not the other way round.**

1. **Verify the claim against the current code.** Several have been wrong: a workflow said to be
   missing was in the tree, an instruction set said to need `-march` assembles at baseline, and a
   leak predicted from a real conformance gap turned out not to happen because something else
   collected the garbage.
2. **Write a test that fails without the fix.** Passing proves nothing on its own — back the fix
   out and watch the test go red. Every fix in this tree that has a test was checked that way, and
   the check has caught tests that were asserting the wrong thing.
3. **If it cannot be reproduced, say so and do not open a pull request.** Report the measurement
   instead. An issue that records "read from the source, not measured" is worth more than a change
   nobody can justify.

### The probe is usually where it goes wrong

Three probes were wrong before one was right while confirming a single zombie-reaping bug, and each
looked convincing:

- `/proc/<pid>` does not show a zombie — this procfs answers through `pid_get_task`, which returns
  NULL for one. The obvious test passes against the bug. `pid_get_task_zombie` distinguishes them.
- A shell cannot demonstrate it: `trap '' CHLD` still leaves busybox waiting for its own jobs.
- `waitpid` is the probe that works: a kept child is returned, a released one answers `ECHILD`.

If a test passes on the first try, suspect the probe before believing the code.

### Where a test goes

| Reaches | Home | Runs in |
| --- | --- | --- |
| A function that links standalone | `tests/unit/` | `ninja test`, and CI |
| The embedder path — boot, pty session, exit code | `tests/integration/sbm_api_test.c` | the smoke gate |
| Guest behaviour: a shell, a redirect, a signal | `tests/integration/smoke.sh` | the smoke gate |
| `fork`, `sigaction`, real syscall semantics | `tests/regress/regress_syscall.c` | by hand |
| A package manager or a toolchain | `tests/integration/full.sh` | nightly, on demand |

`regress_syscall.c` wants an aarch64 cross-compiler that is usually not installed. It does not have
to be: copy the source into a rootfs and build it *inside* the guest, which needs nothing but
`apk add gcc musl-dev` and takes about fifteen seconds.

```bash
cp tests/regress/regress_syscall.c build/integration/alpine/tmp/rs.c
build/ish -r build/integration/alpine /bin/sh -c \
    'apk add --no-progress -q gcc musl-dev; gcc -O0 -static -o /tmp/rs /tmp/rs.c && /tmp/rs'
```
