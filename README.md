# [iSH](https://ish.app)

> ## 🚀 ARM64 Fork Notice
>
> **This repository is a fork of [ish-app/ish](https://github.com/ish-app/ish)** that adds a
> **native ARM64 guest backend** to upstream iSH's threaded-code interpreter (*Asbestos*,
> renamed from *jit* upstream in 2024 — [ish-app/ish@d375656f](https://github.com/ish-app/ish/commit/d375656f)).
> It emulates AArch64 Linux on Apple Silicon. Upstream's x86 (i386) backend was removed along
> with the iOS app, so AArch64 is the only guest this builds.
>
> Asbestos is **not a JIT** — it doesn't emit machine code at runtime. For each basic block
> it builds an array of pointers to pre-compiled native "gadget" functions that tail-call
> each other (the threaded-code technique Forth interpreters use). This fork's contribution
> is the **ARM64 guest backend** inside that framework: new hand-written ARM64 gadgets that
> map each AArch64 guest instruction to just a handful of host instructions — same-architecture
> dispatch, so the overhead per guest instruction is small.
>
> **Key enhancements over upstream:**
> - **Native ARM64 gadget dispatch** — same-architecture, 2-12x faster than x86 for compute
> - **48-bit virtual address space** — 4-level page table supports V8/Go/Rust runtimes
> - **Node.js 22 / npm / npx** — V8 guard pages, binary patch, `--jitless` injection
> - **Go and Rust** — large VA reservations, signal frame alignment, FUTEX_WAIT_BITSET, PMULL
> - **Full NEON + Crypto** — AES/SHA/CRC32 instructions for TLS and hashing at native-ish speed
> - **Native Offload** — bypass emulation for selected binaries, symbols, or whole functions
> - **Bind mounts** — share host directories into the guest filesystem without copying
>
> The iOS app is not part of this repository: it is built as three static libraries and embedded
> in a host app. See [Build for iOS](#build-for-ios).
>
> **Performance**, measured while both backends were still here (compute-heavy, ARM64 vs x86):
> C `int_arith_2M` **12x faster**, Python `fib(30)` **9.2x faster**, `sum(1M)` **10.2x faster**,
> shell `seq+awk 100K` **7.2x faster**. The x86 column is no longer reproducible from this
> repository.
>
> **Full docs:** [README_arm64.md](README_arm64.md) · [中文版](README_arm64_zh.md) ·
> [Performance report](benchmark/BENCHMARK_PERF.md) · [Compatibility report](benchmark/BENCHMARK_COMPAT.md)
>
> ---

[![Build Status](https://github.com/ish-app/ish/actions/workflows/ci.yml/badge.svg)](https://github.com/ish-app/ish/actions)
[![goto counter](https://img.shields.io/github/search/ish-app/ish/goto.svg)](https://github.com/ish-app/ish/search?q=goto)
[![fuck counter](https://img.shields.io/github/search/ish-app/ish/fuck.svg)](https://github.com/ish-app/ish/search?q=fuck)
[![shit counter](https://img.shields.io/github/search/ish-app/ish/shit.svg)](https://github.com/ish-app/ish/search?q=shit)

<p align="center">
<a href="https://ish.app">
<img src="https://ish.app/assets/github-readme.png">
</a>
</p>

A project to get a Linux shell running on iOS, using usermode AArch64 emulation and syscall
translation.

For the current status of the project, check the issues tab, and the commit logs.

- [Discord server](https://discord.gg/HFAXj44)
- [Wiki with help and tutorials](https://github.com/ish-app/ish/wiki) (upstream's, and about the app)

# Hacking

You'll need these things to build the project:

 - Python 3
   + Meson (`pip3 install meson`)
 - Ninja
 - Clang and LLD (on mac, `brew install llvm`, on linux, `sudo apt install clang lld` or `sudo pacman -S clang lld` or whatever)
 - sqlite3 (this is so common it may already be installed on linux and is definitely already installed on mac. if not, do something like `sudo apt install libsqlite3-dev`)
 - libarchive (`brew install libarchive`, `sudo port install libarchive`, `sudo apt install libarchive-dev`) TODO: bundle this dependency

## Build for iOS

This repository holds no iOS app. It is built as three static libraries — `libish.a`,
`libish_emu.a`, `libfakefs.a` — and embedded in a host app, which supplies the terminal UI and
calls into the engine itself. Cross-compile them with a meson cross file naming the SDK:

```bash
meson setup build-ios . -Dguest_arch=arm64 --buildtype=release --cross-file ios.ini
ninja -C build-ios libish.a libish_emu.a libfakefs.a
```

`ninja` with no target fails here: `tools/fakefsify` links the host's libarchive and cannot be
built for a phone. `.github/workflows/static-libs.yml` does exactly this for `iphoneos` and
`iphonesimulator` on every push to `main` and publishes the result as a `vX.Y.Z` release;
[ServerBox](https://github.com/lollipopkit/flutter_server_box) carries this repository as a
submodule and downloads those artifacts rather than building them.

## Build command line tool for testing

To set up your environment, cd to the project and run `meson build` to create a build directory in `build`. Then cd to the build directory and run `ninja`.

To set up a self-contained Alpine linux filesystem, download the Alpine minirootfs tarball for aarch64 from the [Alpine website](https://alpinelinux.org/downloads/) and run `./tools/fakefsify`, with the minirootfs tarball as the first argument and the name of the output directory as the second argument. Then you can run things inside the Alpine filesystem with `./ish -f alpine /bin/sh`, assuming the output directory is called `alpine`. If `tools/fakefsify` doesn't exist for you in your build directory, that might be because it couldn't find libarchive on your system (see above for ways to install it.)

The host must be arm64: the gadgets a guest instruction dispatches to are native code for the machine running them, and `asbestos/guest-arm64/gadgets-aarch64` is the only set there is.

## Logging

iSH has several logging channels which can be enabled at build time. By default, all of them are disabled. To enable them:

- Run `meson configure -Dlog="<space-separated list of log channels>"` in the build directory.
  An embedding app passes the same thing as `-DDEBUG_<channel>=1` to its own compiler.

Available channels:

- `strace`: The most useful channel, logs the parameters and return value of almost every system call.
- `instr`: Logs every instruction executed by the emulator. This slows things down a lot.
- `verbose`: Debug logs that don't fit into another category.
- Grep for `DEFAULT_CHANNEL` to see if more log channels have been added since this list was updated.

# A note on the interpreter

Possibly the most interesting thing I wrote as part of iSH is the interpreter. It's not quite a JIT since it doesn't target machine code. Instead it generates an array of pointers to functions called gadgets, and each gadget ends with a tailcall to the next function; like the threaded code technique used by some Forth interpreters. The result is a speedup of roughly 3-5x compared to emulation using a simpler switch dispatch.

Unfortunately, I made the decision to write nearly all of the gadgets in assembly language. This was probably a good decision with regards to performance (though I'll never know for sure), but a horrible decision with regards to readability, maintainability, and my sanity. The amount of bullshit I've had to put up with from the compiler/assembler/linker is insane. It's like there's a demon in there that makes sure my code is sufficiently deformed, and if not, makes up stupid reasons why it shouldn't compile. In order to stay sane while writing this code, I've had to ignore best practices in code structure and naming. You'll find macros and variables with such descriptive names as `ss` and `s` and `a`. Assembler macros nested beyond belief. And to top it off, there are almost no comments.

So a warning: Long-term exposure to this code may cause loss of sanity, nightmares about GAS macros and linker errors, or any number of other debilitating side effects. This code is known to the State of California to cause cancer, birth defects, and reproductive harm.
