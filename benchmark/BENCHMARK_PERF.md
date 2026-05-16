# ShellBox Performance Benchmark

> **Generated:** 2026-05-16 17:55:37
> **Host:** macOS 26.5 / arm64
> **x86:** unavailable
> **ARM64:** ish (704K, fakefs)
> **Runs:** 3 (median) | **Timeout:** 120s

| | x86 Emulation | ARM64 JIT |
|---|:---:|:---:|
| Engine | Interpreter (Jitter) | Threaded-code (Asbestos) |
| Guest | i386 → ARM64 host | AArch64 → AArch64 host |
| Address | 32-bit (4 GB) | 48-bit (256 TB) |
| SIMD | Partial SSE/SSE2 | Full NEON + Crypto |
| Node/Go/Rust | Not possible | Supported |

---

## ARM64 Real C Workload (Emulated/JIT)

> Native macOS and ARM64 Linux guest binaries are rebuilt from
> `benchmark/assets/cbench_lite.c` for each run. The ARM64 column runs
> the ARM64 Linux guest ELF inside iSH without native offload. This is
> the real emulated/JIT workload gate; `arm64-micro` is only hotspot triage.

### C

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| int_arith_8M | 55939000ns | 125129000ns | **2.2x** |
| float_arith_4M | 31381000ns | 77551000ns | **2.5x** |
| mem_seq_256MB | 27068000ns | 154098000ns | **5.7x** |
| mem_rand_16M | 23416000ns | 102509000ns | **4.4x** |
| func_call_50M | 35070000ns | 174041000ns | **5.0x** |
| branch_50M | 70435000ns | 228901000ns | **3.2x** |
| matrix_384x384 | 56777000ns | 421355000ns | **7.4x** |
| string_1M | 15189000ns | 71650000ns | **4.7x** |
| strlen_2M | 14506000ns | 31847000ns | **2.2x** |
| strcmp_2M | 17088000ns | 59897000ns | **3.5x** |

