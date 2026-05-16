# ShellBox Performance Benchmark

> **Generated:** 2026-05-07 00:01:50
> **Host:** macOS 26.4.1 / arm64
> **x86:** unavailable
> **ARM64:** ish (649K, fakefs)
> **Runs:** 3 (median) | **Timeout:** 120s

| | x86 Emulation | ARM64 JIT |
|---|:---:|:---:|
| Engine | Interpreter (Jitter) | JIT Compiler (Asbestos) |
| Guest | i386 → ARM64 host | AArch64 → AArch64 host |
| Address | 32-bit (4 GB) | 48-bit (256 TB) |
| SIMD | Partial SSE/SSE2 | Full NEON + Crypto |
| Node/Go/Rust | Not possible | Supported |

---

## 1. ARM64 Shell Benchmark (Native vs ARM64)

> **Guest-side timing** — each test measured inside ARM64 ShellBox with
> monotonic clock. Startup overhead (fakefs init) is excluded.
> Use `./run.sh arm64` for a fast modern-iPhone/ARM64 performance pass
> when x86 rootfs is not available.

### System

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| echo x1000 | 36ms | 350ms | **9.7x** |
| uname -a x100 | 134ms | 100ms | **0.7x** |
| ls /bin x100 | 145ms | 260ms | **1.8x** |
| cat file x200 | 269ms | 200ms | **0.7x** |
| wc -l x200 | 349ms | 560ms | **1.6x** |
| date x200 | 305ms | 210ms | **0.7x** |
| env x100 | 130ms | 110ms | **0.8x** |

### Compute

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| loop 1000 | 5ms | 210ms | **42.0x** |
| loop 5000 | 14ms | 1050ms | **75.0x** |
| loop 10000 | 25ms | 2120ms | **84.8x** |
| seq+awk 10K | 6ms | 70ms | **11.7x** |
| seq+awk 50K | 11ms | 360ms | **32.7x** |
| seq+awk 100K | 17ms | 720ms | **42.4x** |
| expr loop 500 | 661ms | 490ms | **0.7x** |
| bc sqrt x50 | 102ms | 430ms | **4.2x** |
| bc pi x50 | 100ms | 120ms | **1.2x** |

### Text

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| sed replace x200 | 337ms | 250ms | **0.7x** |
| sort 1K | 4ms | 30ms | **7.5x** |
| sort 5K | 5ms | 160ms | **32.0x** |
| uniq count 5K | 5ms | 60ms | **12.0x** |
| grep count | 4ms | 40ms | **10.0x** |
| tr lowercase 10K | 5ms | 20ms | **4.0x** |

### File-IO

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| create 100 | 16ms | 20ms | **1.2x** |
| create 500 | 59ms | 100ms | **1.7x** |
| find /bin x20 | 38ms | 50ms | **1.3x** |
| dd 4MB x50 | 165ms | 90ms | **0.5x** |

### Crypto

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| md5sum x100 | 186ms | 120ms | **0.6x** |
| sha256sum x100 | 184ms | 110ms | **0.6x** |

### Process

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| fork+exec 100 | 50ms | 60ms | **1.2x** |
| fork+exec 300 | 140ms | 180ms | **1.3x** |
| pipe chain x100 | 246ms | 740ms | **3.0x** |

### C

| Test | Native | ARM64 | **ARM64/Native** |
|------|:---:|:---:|:---:|
| int_arith_8M | 35ms | 81ms | **2.3x** |
| float_arith_4M | 20ms | 45ms | **2.2x** |
| mem_seq_256MB | 21ms | 139ms | **6.6x** |
| mem_rand_16M | 15ms | 58ms | **3.9x** |
| func_call_50M | 22ms | 101ms | **4.6x** |
| branch_50M | 44ms | 150ms | **3.4x** |
| matrix_384x384 | 44ms | 134ms | **3.0x** |
| string_1M | 9ms | 611ms | **67.9x** |
| strlen_2M | 9ms | 293ms | **32.6x** |
| strcmp_2M | 11ms | 1721ms | **156.5x** |

