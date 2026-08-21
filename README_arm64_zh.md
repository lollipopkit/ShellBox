# iSH ARM64 — 通过原生 threaded-code 解释器在 iOS 上运行 Linux

**Fork 自 [ish-app/ish](https://github.com/ish-app/ish)** — iOS 上的用户态 Linux 模拟器。

本 fork 在上游 iSH 的 threaded-code 解释器（**Asbestos**，2024 年之前叫 *jit*，上游重命名是因为
它本来就不是真正的 JIT）之上**新增了 ARM64 guest 后端**，在 Apple Silicon 上模拟 AArch64 Linux，
上游的 x86 (i386) guest 后端已随 iOS app 一并移除。结果是一个性能和兼容性大幅提升的 Linux 环境，
能在 iPhone / iPad 上直接运行 **Python、Node.js、Go、Rust 和原生 CLI 工具**。

> ## 🚢 生产环境使用
>
> 本引擎已在 **[OpenMinis](https://openminis.app)** 中作为 **Agent Shell Sandbox** 投入使用，
> 经过 **10,000+ 用户**在 iOS 上稳定运行 Linux 工具和 shell 负载的真实检验。README 中的性能
> 数据和稳定性声明均来自这个真实线上部署，而不仅仅是合成基准测试。

> **命名说明**：*Asbestos* 是上游项目给自家 threaded-code 解释器起的名字（见上游 commit
> [`d375656f` "Rename the JIT"](https://github.com/ish-app/ish/commit/d375656f)，2024 年 6 月）。
> 它**不是 JIT** —— Asbestos 及其前身都不会在运行时生成机器码，而是为每个基本块构造一个
> 指针数组（指向预编译的原生 gadget 函数），各 gadget 通过尾调用衔接（Forth 解释器采用的技术）。
>
> 本 fork 所做的是在这个 Asbestos 框架里**新增一个 ARM64 guest 后端**：
> 新的 gadget（`asbestos/guest-arm64/gadgets-aarch64/`）把 AArch64 guest 指令映射到若干条
> ARM64 host 指令——同架构分派，每条 guest 指令只需几条 host 指令。
> 下文部分地方用 "JIT" 作为简写，请理解为"同架构 gadget 分派"，而非运行时代码生成。
>
> 英文版: [README_arm64.md](README_arm64.md)

---

## 为什么要做 ARM64 版本？

原始 iSH 在 ARM64 主机上翻译 **x86 (i386) 指令** — 每条 guest 指令都要跨架构解码和模拟。
这对简单工具还行，但有根本限制：

| 限制 | x86（原版） | ARM64（本 fork） |
|---|---|---|
| 架构翻译 | i386 → ARM64（跨架构） | AArch64 → AArch64（同架构） |
| 地址空间 | 32-bit (4 GB) | 48-bit (256 TB) |
| SIMD | 部分 SSE/SSE2 | 完整 NEON + Crypto |
| Node.js / V8 | 无法运行（需要 >4 GB 虚拟地址） | 支持 |
| Go / Rust | 无法运行（需要大虚拟地址） | 支持 |
| 计算开销 | 相对原生 15-100x | 相对原生 3-30x |

## 架构总览

```
+--------------------------------------------------------------+
|  嵌入方 app（终端 UI、rootfs、生命周期）                     |
|  .............................................................|
|  libish.a + libish_emu.a + libfakefs.a                       |
|                                                              |
|  +--------------------------------------------------------+  |
|  |  Asbestos (threaded-code 解释器)                       |  |
|  |                                                        |  |
|  |   Decoder  -->  Gadget program  -->  Fiber Blocks      |  |
|  |   (gen.c)       builder              (block cache)     |  |
|  |                                                        |  |
|  |   --- 48-bit 虚拟内存 (4 级页表) ---                    |  |
|  |       TLB (8192 项) + CoW + 惰性预留                   |  |
|  +--------------------------------------------------------+  |
|                                                              |
|  +-------------------+    +-------------------------------+  |
|  |  Linux 内核       |    |  Host 集成                    |  |
|  |  (syscalls,       |    |  - Native Offload             |  |
|  |   signals,        |    |  - Bind Mounts                |  |
|  |   futex, epoll)   |    |  - pty 会话                   |  |
|  +-------------------+    +-------------------------------+  |
|  +-------------------+                                       |
|  |  文件系统         |                                       |
|  |  fakefs + realfs  |                                       |
|  |  + bind mounts    |                                       |
|  +-------------------+                                       |
+--------------------------------------------------------------+
```

---

## 相对上游的主要增强

### 1. Asbestos 框架内的 ARM64 guest 后端

本 fork 的主要贡献。在上游既有的 Asbestos threaded-code 解释器里接入一个 ARM64 guest 后端，
改写了每条指令的成本模型：每个 guest 基本块被编译成一个 **gadget 程序** —— 一个
`unsigned long` 数组，交替存放预编译 ARM64 gadget 函数指针和内联操作数。执行流程是一串尾调用 ——
每个 gadget 从程序流读取下一条指针并 `br` 跳过去。**不分配可执行内存，不在运行时生成任何机器码**。
每条 guest 指令的 host 指令开销就是对应 gadget 中的几条 ARM64 指令。

**关键文件:**
- `asbestos/asbestos.c` — 块缓存、块管理、RCU 风格的 jetsam 清理
- `asbestos/guest-arm64/gen.c` — 指令解码器 + gadget 程序构建（200+ 指令）
- `asbestos/guest-arm64/gadgets-aarch64/` — 手写 ARM64 汇编 gadget:
  - `entry.S` — fiber_enter/exit、崩溃恢复 trampoline
  - `memory.S` — 内联 TLB 查找的 load/store（快路径约 12 条指令）
  - `control.S` — 分支、条件、融合 compare-and-branch
  - `math.S` — 算术、移位、位操作、NEON/SIMD
  - `crypto.S` — AES、SHA、CRC32 指令

**设计亮点:**
- **块链接**: 顺序基本块直接链接，跳过 dispatch 开销
- **持久 TLB**: 8192 项 TLB 跨 syscall 保留（不会每次进出 gadget 分派都刷）
- **崩溃恢复**: gadget 中 SIGSEGV 重定向到 trampoline 进行 CoW 处理
- **完整 NEON**: 所有 128-bit SIMD 操作，含加密扩展

### 2. 48-bit 虚拟地址空间

4 级页表（L0→L1→L2→L3，每级 9 bit = 36-bit 页号 + 12-bit 偏移 = 48-bit）。

- 支持 V8 的 128GB+ 指针笼（通过 `MAP_NORESERVE` 惰性预留）
- Go 对堆栈的大虚拟地址需求
- `0x0-0x100000` 的守护页防止 V8 compressed pointer 崩溃
- 布局保持紧凑（stack `0xffffe000`，mmap `0xefffd`）以提高 TLB 效率

**关键文件:** `kernel/memory.h`、`kernel/memory.c`、`emu/tlb.h`

### 3. Node.js / V8 支持

在用户态模拟器上跑 Node.js 需要解决多个 V8 特有问题：

- **128GB MAP_NORESERVE**: 不占物理内存的惰性地址预留
- **0x0-0x100000 守护页**: V8 compressed pointer 会解引用小整数 —
  把低 1MB 映射成可读的零页能避免 SIGSEGV
- **V8 二进制补丁**: 在代码 cave 中植入 9 条指令修复 `InterpreterEntryTrampoline`
  派生构造函数 bug（零模拟器开销）
- **`--jitless --no-lazy`**: V8 启动 flag，避开 Wasm 编译和懒解析问题
- **退出清理**: V8 线程卡死时的 safety valve

**效果**: `npm install`、`npm exec`、`npx`、`create-next-app` 全部可用。

### 4. 嵌入接口

host app 驱动引擎所用的接口。原先架在这些接口之上的 Objective-C 封装
（`ISHShellExecutor`、JSON-RPC 的 `DebugServer`）属于 iOS app，已不在本仓库中；
嵌入方基于下列 header 自行实现。

#### 启动与会话（`kernel/init.h`、`fs/tty.h`、`fs/path.h`）

`mount_root()` 为 guest 挂上文件系统，`become_first_process()`、`do_execve()` 与
`task_start()` 启动 init，一个会话是 `pty_open_fake()` 返回的 pty，由 host 读写。
完整流程可参考 ServerBox 的 `ios/Runner/ish/sbm_ish.c`。

#### Native Offload（`kernel/native_offload.c`）

完全绕过模拟，对注册的二进制直接由原生处理。guest 的 `execve()` 被拦截并路由到
原生 handler 或 host 二进制：

```c
// 注册 handler（启动时调用一次）
native_offload_add_handler("ffmpeg", ffmpeg_main);

// 现在 guest 的 `ffmpeg -i input.mp4 output.mp3` 会原生执行
// 参数中的 guest 路径自动转换为 host 路径
```

同时支持进程内 handler（iOS + macOS）和通过 `posix_spawn` 的委托（macOS CLI）。

#### Bind Mounts（`fs/fake.c`）

把 host 目录挂载到 guest 文件系统：

```c
// 只读 bind mount host 目录
fakefs_bind_mount("/host/path/to/data", "/mnt/data", /*read_only=*/true);
```

让 AI agent 能在 host app 与 Linux guest 之间共享文件而无需复制。

### 5. Guest 环境

- **Alpine aarch64**，自带完整 apk 包管理器
- **OPENSSL_armcap=0** 和 **GODEBUG/GOMAXPROCS** 在 `sys_execve` 中注入

rootfs 本身属于嵌入方：`realfs` 挂载普通目录树，`fakefs` 挂载 `fakefsify` 的输出，
两者本仓库都不附带。

---

## 构建配置

host 必须是 ARM64：gadget 路径是 `asbestos/guest-arm64/gadgets-<host cpu family>`，
而只存在 `gadgets-aarch64` 这一套。

| Option | 取值 | 默认 |
|---|---|---|
| `guest_arch` | `arm64` | `arm64` |
| `log`、`nolog` | 空格分隔的 channel 名 | 空 |
| `log_handler` | `dprintf` 等 | `dprintf` |
| `offload_test_symbol`、`offload_test_prebuilt` | `true`、`false` | `false` |

`guest_arch` 只剩一个合法取值，保留它是因为 ServerBox 的脚本在传 `-Dguest_arch=arm64`，
而 meson 遇到不存在的 option 会直接失败。

```bash
# host CLI（macOS，测试用）
meson setup build-arm64-release -Dguest_arch=arm64 --buildtype=release
ninja -C build-arm64-release
./build-arm64-release/ish -f ./alpine-arm64-fakefs /bin/sh

# 嵌入方链接的三个库，针对 device SDK
meson setup build-ios . -Dguest_arch=arm64 --buildtype=release --cross-file ios.ini
ninja -C build-ios libish.a libish_emu.a libfakefs.a
```

cross build 必须指定 target 而不能只跑 `ninja`：`tools/fakefsify` 链接 host 的
libarchive，无法为手机构建。`.github/workflows/static-libs.yml` 对 `iphoneos` 与
`iphonesimulator` 各跑一次，并把产物作为 `vX.Y.Z` release 发布。

---

## 性能

数据采集于两个后端都还在的时期。x86 那一列现在已无法从本仓库复现，保留它是因为它正是
ARM64 后端被写出来的理由。

使用 `benchmark/run.sh` 在 macOS 26.4.1 / Apple Silicon 上测试，采用 guest 内置计时
（排除启动开销）。完整数据见
**[benchmark/BENCHMARK_PERF.md](benchmark/BENCHMARK_PERF.md)**。

### 相对原生的开销（按负载类型）

| 类别 | x86/Native | ARM64/Native | **ARM64 vs x86** |
|---|:---:|:---:|:---:|
| C 纯计算 | 13-212x | 0.5-30x | **0.5-3.8x** |
| Shell 管道 | 2-164x | 1-127x | **1.0-4.3x** |
| Python | 5-77x | 2-77x | **1.7-3.2x** |
| Go 启动 | 2.6-6.2x | 2.4-5.5x | **1.1x** |
| Node.js | 7-25x | 5-22x | **0.5-1.3x** |

### 亮点数据（计算密集型）

- **Shell `seq+awk 100K`**: ARM64 比 x86 **快 4.2x**（828ms vs 3447ms）
- **C `int_arith_2M`**: ARM64 **快 3.8x**（62ms vs 233ms）
- **Shell `grep count`**: ARM64 **快 3.6x**（49ms vs 174ms）
- **Python `sort 100K`**: ARM64 **快 3.2x**（1570ms vs 5053ms）
- **Python `json roundtrip`**: ARM64 **快 2.5x**（1270ms vs 3149ms）
- **Crypto `md5sum`**: ARM64 借助硬件 crypto **快 2.2x**（6ms vs 13ms）

> **ARM64 为什么快**: 同架构 gadget 分派（每条 guest 指令只需对应 gadget 中的几条 host 指令）、完整 NEON + 加密扩展、
> 48-bit 地址空间支持 V8/Go/Rust，以及 Node.js 专项修复（V8 二进制补丁、守护页、`--jitless`
> 注入、io_uring syscall）。在少数微基准上 x86 解释器与 ARM64 持平甚至更快（Node.js `sum 1M`、
> `JSON 10K`、C `mem_seq`/`func_call`）—— 多为小分配或紧密循环，JIT 单次编译成本不划算的场景。

## 兼容性

223 项测试覆盖 18 个分类（基础 OS、文件操作、文本处理、构建、Python、Node.js、
Go/Rust/Perl/…、网络、VCS、编辑器、Shell、数据库、多媒体、加密、系统监控、调试、
包管理、信号）。两个架构在相同 fakefs 环境下安装相同软件包后测试。完整报告见
**[benchmark/BENCHMARK_COMPAT.md](benchmark/BENCHMARK_COMPAT.md)**。

| 架构 | 通过 | 失败 | 通过率 |
|---|:---:|:---:|:---:|
| **x86** (Jitter, threaded-code) | 221 | 2 | **99%** |
| **ARM64** (Asbestos, threaded-code) | 223 | 0 | **100%** |

**x86 的 2 项失败**为 DNS 查询测试（`nslookup localhost`、`nslookup 8.8.8.8`）——
x86 minirootfs 缺 bind-tools/解析器配置，非模拟器 bug。

---

## 支持的软件

### 完全可用

| 类别 | 示例 |
|------|------|
| **包管理器** | apk、pip、npm、npx、uv |
| **语言** | Python 3、Node.js 22、Go、Perl、Ruby、Lua |
| **开发工具** | git、curl、wget、ssh、vim、nano |
| **构建工具** | gcc、g++、cmake、make、meson |
| **数据工具** | sqlite3、jq、yt-dlp、ffmpeg（通过 native offload） |
| **网络** | curl、wget、dig、netstat、ss |
| **Node 框架** | Express、Koa、Fastify、Axios、Socket.io |
| **npm 生态** | lodash、moment、dayjs、uuid、chalk、commander、glob、semver |

### 不支持

- **GUI 应用**（无 X11/Wayland）
- **Docker / 容器**（无内核命名空间支持）
- **内核模块**（用户态模拟器）
- **硬件访问**（无 /dev/gpu、无 USB 透传）

---

## 提交历史

`feature-arm64` 分支上 86+ 提交，101+ 个文件变更，+23,000+ / -7,600+ 行。

主要里程碑:
1. **解释器基础**: fiber_enter/exit、基本块编译（到 gadget 程序）、TLB
2. **指令覆盖**: 200+ ARM64 指令含完整 NEON/Crypto
3. **48-bit 地址空间**: 4 级页表、惰性预留
4. **Node.js 支持**: V8 守护页、MAP_NORESERVE、二进制补丁、退出清理
5. **Go 支持**: 信号帧对齐、sigreturn 修复、NZCV 保留
6. **Rust/uv 支持**: FUTEX_WAIT_BITSET、PMULL、BFM、按需映射读取
7. **Agent 集成**: Native Offload、Bind Mounts；ISHShellExecutor 与 DebugServer 属于
   iOS app，已随之移除
8. **稳定性**: 50+ 个 bug 修复（并发、内存泄漏、use-after-free、死锁）

---

## 工程结构

```
iSH/
├── asbestos/                    # ARM64 threaded-code 解释器
│   ├── asbestos.c/h             # 块缓存、RCU 清理
│   └── guest-arm64/
│       ├── gen.c                # 指令解码器 → gadgets
│       ├── crypto_helpers.c     # AES/SHA/CRC32 helpers
│       └── gadgets-aarch64/     # 汇编 gadgets
│           ├── entry.S          # Fiber 入口/退出、崩溃 handler
│           ├── memory.S         # Load/store、TLB 内联查找
│           ├── control.S        # 分支、条件
│           ├── math.S           # ALU、移位、NEON/SIMD
│           ├── crypto.S         # AES、SHA、PMULL、CRC32
│           ├── bits.S           # 位域操作
│           └── gadgets.h        # 寄存器映射、TLB 宏
├── emu/
│   ├── tlb.c/h                  # TLB miss 处理、跨页
│   └── arch/arm64/
│       ├── cpu.h                # CPU 状态（寄存器、NEON、flags）
│       └── decode.h             # 指令字段提取
├── kernel/
│   ├── arch/arm64/calls.c       # ARM64 syscall 表
│   ├── memory.c/h               # 页表、CoW、缺页
│   ├── mmap.c                   # mmap、惰性预留
│   ├── native_offload.c/h       # 二进制 offload 系统
│   ├── signal.c/h               # 信号投递/帧
│   ├── futex.c                  # 基于 pipe 唤醒的 futex
│   ├── exec.c                   # ELF 加载器、V8 守护页
│   └── exit.c                   # 线程清理、safety valve
├── fs/
│   ├── fake.c/h                 # fakefs + bind mount
│   ├── real.c                   # Host 文件系统访问
│   ├── sock.c/h                 # Socket 模拟
│   └── poll.c                   # epoll/poll/select
├── main.c, xX_main_Xx.h         # host CLI；嵌入方自行实现对应部分
└── benchmark/
    ├── run.sh                   # 统一入口
    ├── assets/                  # 测试脚本和预编译二进制
    ├── BENCHMARK_PERF.md        # 性能报告
    └── BENCHMARK_COMPAT.md      # 兼容性报告
```

---

## 许可

与上游 iSH 相同。见 [LICENSE](LICENSE)。
