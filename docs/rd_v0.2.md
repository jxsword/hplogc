# hplogc - 纯 C 语言高性能日志库：需求规格说明书（v0.2）

---

## 0. 版本说明与破坏性变更声明（v0.2 新增）

> **本文来源**：以 rd_v0.1.md 为基线修订而成，**rd_v0.1.md 与 include/hplogc.h 均未被修改**。
> **规范性配套头文件**：`include/hplogc.h`（v0.2，已取代 v0.1 的同名文件）。
> 本文 §7.6/§7.3 定义的所有符号与该头文件逐一对齐。

### 0.1 本次修订的两条主线

1. **rd 与头文件双向对齐**：把 rd_v0.1.md 已定义、而 **v0.1 版的 `hplogc.h`** 未表达或表达有误的部分落实到位；
   把 v0.1 版的 `hplogc.h` 自身的不完善项反向写回规范（清单见 §7.8，处置结论已并入各节）。
   > **命名约定**：本文中的 `include/hplogc.h` 自 v0.2 起指**修订后的正式公共头文件**
   > （由 `hplogc-0.2.h` 转正）；凡涉及 v0.1 原文件处均显式标注"（v0.1 版）"。
2. **引入多 sink 输出体系与结构化字段通道**：吸收 `tmp/sink_desin.md` 与 `tmp/clog.h` 的
   vtable + 注册表设计，取代固定的 output 枚举。

### 0.2 破坏性变更清单（迁移时必须处理）

| # | v0.1 形态 | v0.2 形态 | 影响面 |
|---|-----------|-----------|--------|
| B1 | `hplogc_output_t` + `hplogc_output_type_t`（固定枚举的 output 数组） | `hplogc_sink_config_t`（type 名 + 私有键值） | **结构体移除**：代码内配置的 `outputs` 数组 |
| B2 | `hplogc_rotate_t` / `hplogc_time_unit_t` 作为公共结构体字段取值 | 保留为 `rollingfile` sink 的配置键取值词表 | 仅语义归属变化，值集合不变 |
| B3 | rule 引用 output 名 | rule 引用 **sink 实例名**（`hplogc_rule_t::sinks`） | 字段名 `outputs`→`sinks` |
| B4 | `hplogc_config_t` 无 ABI 自检字段 | 首字段新增 `struct_size` | POD 布局变化；调用前必须先执行 `hplogc_config_default()` |
| B5 | 部分配置键"只能通过配置文件设置" | 统一可由 `hplogc_kv_t` 全局键值表达 | **修复** min 预设（`INI=OFF`）无法设置时区/时间格式等的结构性缺陷 |
| B6 | per-category 级别清除用 `(hplogc_level_t)-1` 哨兵（头文件自创，规范未定义） | 改为显式 `hplogc_clear_level_for_category()` | 公共契约不再依赖魔法值 |
| B7 | `HPLOGC_COMPILE_TIME_LEVEL` 允许写 `HPLOGC_LEVEL_INFO` 等符号名 | 仅允许数值或 `HPLOGC_CTL_*` | 修复"静默不裁剪"缺陷（§7.3） |
| B8 | 错误码 5 个 | 新增 `HPLOGC_ERR_UNSUPPORTED`（-6） | "值合法但构建不支持 / 未注册 sink 类型"的返回值更精确 |
| B9 | socket output 定义即启动失败（`HPLOGC_ERR_CONFIG`） | 统一为"未注册 / 被裁剪的 sink 类型"返回 `HPLOGC_ERR_UNSUPPORTED` | §10.4 fail-fast 语义不变，错误码更精确 |

### 0.3 非目标（本次明确不做）

- 不引入 MDC（附录 A.1-9 结论维持不采纳）。
- 不提供 `hplogc_strerror()`（维持 §7.4 的 P2 定位，本版不提供）。
- 不在库中引入任何第三方客户端依赖（§6 零依赖约束），kafka / loki / elasticsearch /
  clickhouse / s3 / mysql 等一律不作为内置 sink 进入仓库（§4.10.5 裁决）。

---

## 1. 项目概述

| 属性 | 描述 |
|------|------|
| **文档版本** | v0.2（2026-09-28，含同日"评审修订"；修订记录见文末） |
| **项目名称** | hplogc (High Performance Log in C) |
| **项目定位** | 纯 C 语言、零/最小依赖、跨平台、高性能日志库 |
| **目标场景** | 从嵌入式设备到高并发服务端，适用于对性能、体积、可移植性有严格要求的系统 |
| **语言标准** | 纯 C99 / C11 双标准支持，禁止使用任何 C++ 特性（无 `.cpp` 编译单元、无 `std::`、无模板/异常/RTTI/STL） |
| **构建系统** | CMake（全平台统一构建，支持 `find_package` 集成） |
| **许可证** | MIT（定案；LICENSE 文件与分发文案均以 MIT 为准） |

> 本文档为实现依据：所有行为定义均为规范性（normative），实现不得引入文档未定义的对外行为；实现细节（算法、内部数据结构）不在约束范围内。
>
> 配置文件的词法与解析参考 [zlog](https://github.com/HardySimpson/zlog) 的实现（见 §10.1）；zlog 最新代码中可能更优的设计对比见附录 A（待决策项，不在本文规范性范围内）。

---

## 2. 跨平台与编译器支持

### 2.1 目标平台矩阵

| 操作系统 | 编译器 | CPU 架构 | 位宽 |
|----------|--------|----------|------|
| Linux | GCC (≥ 4.9), Clang (≥ 3.5) | x86, ARM, MIPS, RISC-V 等 | 32 / 64 bit |
| macOS | Apple Clang (Xcode ≥ 10) | x86_64, ARM64 (Apple Silicon) | 64 bit |
| Windows | MSVC (≥ 2015), MinGW-w64 (GCC ≥ 7) | x86, x64, ARM64 | 32 / 64 bit |

> **编译器版本与语言标准的约束关系**：Linux GCC 最低版本取 4.9，因 C11 构建依赖 `<stdatomic.h>`（GCC 4.9 引入；C99 路径仅依赖 `__atomic_*`，GCC 4.7+ 即可）。Windows：C99 构建最低 MSVC 2015；C11 构建（`/std:c11`）需 MSVC 2019 16.8 及以上，低版本 MSVC 仅承诺 C99 构建（Windows 原子后端与语言标准无关，见 §2.2/§4.3）。

### 2.2 允许的底层设施

| 类别 | 具体设施 |
|------|----------|
| 线程与同步 | POSIX `pthread`、Win32 API（`CRITICAL_SECTION`、`SRWLOCK`、`Condition Variable`、线程 API） |
| 原子操作 | **Linux**：C11 `<stdatomic.h>`；C99 GCC/Clang `__atomic_*`（不可用时回退 `__sync_*`）。**macOS**：C11 `<stdatomic.h>`；C99 Apple Clang `__atomic_*`（禁止使用已废弃的 `OSAtomic*`）。**Windows**：全部语言标准一律使用 MSVC `Interlocked*`。后端选择矩阵见 §4.3 |
| 文件 I/O | 标准同步文件 I/O（`fopen`/`fwrite`/`write` 等同步写调用） |
| 语言标准 | C99 / C11 双标准支持，CMake `HPLOGC_C_STANDARD`（99 / 11，默认 99）选择；两种标准下原子操作均须可用（§4.3、§11） |

---

## 3. 架构设计原则

### 3.1 接口与实现分离

- **公共接口层**：统一头文件（`hplogc.h`），声明所有对外 API、数据结构、枚举常量。
- **平台实现层**：按决策矩阵选择组织方式（见下）。

### 3.2 实现方式决策矩阵

| 场景分类 | 实现策略 | 理由 |
|----------|----------|------|
| 线程管理、文件 I/O、时间处理、路径/目录、文件监视（热加载） | ✅ **CMake + 按平台分目录独立 `.c` 文件**（§3.3） | 架构级差异，代码量大、逻辑复杂，独立文件便于维护 |
| 环形缓冲（有锁 / 无锁） | ✅ **CMake + 独立 `.c` 文件**（`ringbuf_locked.c` / `ringbuf_lockfree.c`，二选一编译） | 两套完整算法实现，独立演进、独立测试，避免巨型 `#ifdef` 单文件 |
| 原子操作后端 | ✅ **按后端独立头文件**（`atomic_stdatomic.h` / `atomic_gcc.h` / `atomic_msvc.h`，CMake 选择） | 平台 × 语言标准组合多（Linux/macOS × C99/C11 + Windows），单文件 `#ifdef` 膨胀难维护 |
| 结构体定义（平台相关字段） | ✅ **头文件 + `#ifdef`** | 差异修补，保持声明统一 |
| 功能开关（裁剪控制） | ✅ **`#ifdef` 编译宏** | 编译期裁剪，零运行时开销 |
| 热路径小函数 | ✅ **`inline` + `#ifdef`** | 避免函数调用开销 |
| 复杂 OS 行为 | ✅ **独立 `.c` 文件** | 架构设计层面，逻辑复杂 |

> **核心原则**：`#ifdef` 用于**差异修补**；CMake + 独立文件用于**架构设计**。日志库、底层库、长期维护项目**永远选后者**。环形缓冲与原子后端虽属"算法/语法级"差异，但因平台 × 标准组合爆炸与测试隔离需要，同样按独立文件组织。

### 3.3 源码组织与平台分层（规范性）

```
include/hplogc.h               公共 API（唯一对外头文件；v0.2 起由 v0.2 提案版正式取代，随库安装）
src/ring/ringbuf_locked.c       有锁环形缓冲（HPLOGC_LOCKFREE=OFF 时编译，此时 lockfree 版不参与编译）
src/ring/ringbuf_lockfree.c     无锁环形缓冲（HPLOGC_LOCKFREE=ON 时编译，此时有锁版不参与编译）
src/atomic/hplogc_atomic.h     原子操作统一内联接口（按宏选择后端）
src/atomic/atomic_stdatomic.h   后端：C11 <stdatomic.h>（Linux / macOS）
src/atomic/atomic_gcc.h         后端：C99 __atomic_*（回退 __sync_*）（Linux / macOS）
src/atomic/atomic_msvc.h        后端：MSVC Interlocked*（Windows，全部语言标准）
src/platform/posix/…            POSIX 共享层：pthread 同步原语、时间、文件/目录（Linux 与 macOS 共用）
src/platform/linux/…            Linux 专属：inotify 热加载监视器等
src/platform/darwin/…           macOS 专属：kqueue/轮询回退监视器、pthread_threadid_np 等（Phase 3 实现）
src/platform/win32/…            Windows 专属：SRWLOCK/CONDITION_VARIABLE、路径与编码等（Phase 2 实现）
src/sink/sink_registry.c/h      sink 类型注册表（type -> ops）、内置注册与查找（init 后只读、免锁访问）
src/sink/sink_console.c         console sink：stdout/stderr 分流、ANSI 彩色
src/sink/sink_rollingfile.c     rollingfile sink：文件写 + 轮转/备份/命名模板/软链/fsync
src/sink/sink_syslog.c          syslog sink：POSIX syslog（可选内置，min 预设排除）
src/sink/sink_null.c            null sink：丢弃一切；契约参考实现与测试替身（§4.7.1、§13.1、§16.1）
```

- **平台接口契约**（Phase 1 冻结，作为 win32/darwin 的移植依据）：同步原语（mutex/cond 封装）、线程封装、高精度时钟与本地时间转换、文件打开/写/fsync/轮转（rename/unlink/目录扫描语义）、目录创建与权限、路径处理、配置文件监视（watcher）、线程 ID 获取、动态库导出属性。
- CMake 按 `WIN32` / `APPLE` / 其余 选择对应平台目录的源文件参与编译；`src/platform/win32/`、`src/platform/darwin/` 在对应阶段之前仅含接口头文件与占位说明，不参与编译（见 §16）。
- 有锁环形缓冲本体保持单文件可移植：其互斥/条件变量通过平台层原语封装（`platform/posix/`、`platform/win32/` 各自实现同一契约接口）。
- **sink 层属于平台无关的公共层**：`src/sink/` 只能通过 §3.3 的平台接口契约访问 OS，不得在 sink 实现中直接调用平台 API；新增一种 sink 类型 = 新增一个 `.c` 文件 + 一行注册，核心、路由、异步队列与埋点宏均不得改动（§4.10）。
- **约束**：Phase 2 / Phase 3 期间，`src/` 中非平台目录（`ring/`、`atomic/` 统一接口、core、conf、format 等）与 `include/` 不得修改（"公共代码零修改"验收，见 §16）。

---

## 4. 核心功能需求

### 4.1 日志级别

7 个枚举值，按严重程度递增（定义见 §7.6）：

```
HPLOGC_LEVEL_TRACE(0) < HPLOGC_LEVEL_DEBUG(1) < HPLOGC_LEVEL_INFO(2) < HPLOGC_LEVEL_WARN(3)
    < HPLOGC_LEVEL_ERROR(4) < HPLOGC_LEVEL_FATAL(5) < HPLOGC_LEVEL_OFF(6)
```

- `HPLOGC_LEVEL_TRACE` ~ `HPLOGC_LEVEL_FATAL` 是合法的日志级别；`HPLOGC_LEVEL_OFF` **仅用于运行时过滤阈值**（`hplogc_set_level(HPLOGC_LEVEL_OFF)` 关闭全部输出），作为写入 API 的 level 参数时静默丢弃。
- 每条日志携带级别标签（级别名大写：`TRACE`/`DEBUG`/`INFO`/`WARN`/`ERROR`/`FATAL`）。
- **运行时**可动态修改全局过滤阈值（`hplogc_set_level`）；启用 Category 时可用 `hplogc_set_level_for_category` 按 category 覆盖（**覆盖语义**：该 category 已设置级别时以其为准，未设置时使用全局级别）。
- **编译期**可通过宏定义 `HPLOGC_COMPILE_TIME_LEVEL` 移除低于阈值的代码（零开销抽象）：裁剪点在便捷宏 `HPLOGC_xxx` 与 `HPLOGC_xxx_F` 的预处理分支上，低于阈值的宏展开为空语句；对直接调用 `hplogc_log()` / `hplogc_log_fields()` 的代码无效。**取值必须为数值 `0`~`6` 或 `HPLOGC_CTL_*` 数值宏，严禁使用 `HPLOGC_LEVEL_xxx` 枚举符号名**（它们在 `#if` 中被求值为 0，会导致"想裁剪却全部保留"的静默错误，§4.8、§7.3）。`HPLOGC_AUDIT()` / `HPLOGC_METRIC()` **不受**该宏裁剪（§7.3）。

### 4.2 并发模型

| 特性 | 说明 |
|------|------|
| 线程安全 | 所有对外 API 均为线程安全（`hplogc_shutdown` 除外，见 §7.5） |
| 并发模式 | 支持 **SPSC**（单生产者单消费者）和 **MPSC**（多生产者单消费者） |
| **模式选择** | **编译期通过 `HPLOGC_CONCURRENCY` 选择（SPSC / MPSC），不可运行时更改** |
| 批量提交 | 支持批量 IO 请求提交（Batching），合并多次写入为单次同步写入调用 |

### 4.3 环形缓冲队列

提供两种实现，**编译期通过 `HPLOGC_LOCKFREE` 选项选择，不可运行时更改**；两者**分别以独立源文件实现**（§3.3），CMake 条件编译二选一。

#### 有锁版本（`HPLOGC_LOCKFREE=OFF`，默认）

| 平台 | 互斥原语 | 条件变量 |
|------|----------|----------|
| Linux / macOS | `pthread_mutex_t`（经平台层封装） | `pthread_cond_t`（经平台层封装） |
| Windows | `CRITICAL_SECTION` 或 `SRWLOCK`（经平台层封装） | `CONDITION_VARIABLE` |

#### 无锁版本（`HPLOGC_LOCKFREE=ON`）

原子原语按 **平台 × 语言标准** 矩阵选择（与 §2.2 一致）：

| 平台 | C11 构建（`HPLOGC_C_STANDARD=11`） | C99 构建（`HPLOGC_C_STANDARD=99`） |
|------|--------------------------------------|--------------------------------------|
| Linux | `<stdatomic.h>` | GCC/Clang `__atomic_*`；不可用时回退 `__sync_*` |
| macOS | `<stdatomic.h>` | Apple Clang `__atomic_*`（禁止已废弃的 `OSAtomic*`） |
| Windows | MSVC `Interlocked*` | MSVC `Interlocked*` |

- Windows 上无论 C 标准，一律使用 MSVC `Interlocked*`（MinGW-w64 亦通过 `intrin.h` 使用同一族内建，保证 Windows 后端唯一）。
- 后端默认由 CMake 按 `HPLOGC_C_STANDARD` 与编译器特性探测自动选择（如 `__STDC_NO_ATOMICS__`、编译器版本检测）；可用 `HPLOGC_ATOMIC_BACKEND` 强制指定（§11）。
- Linux 与 macOS 的 C11、C99 两条路径**分别独立实现并分别测试**（附录 A 中双映射环形缓冲等实现技巧不影响该要求）。

- 缓冲区大小**仅在初始化时指定**（配置文件 `[buffer]` 或代码内配置），运行期不可更改，热加载忽略该项（§10.5）。init 时若 `buffer size < 2 × max log length`（含条目元数据余量），自动提升至 `max(4KB, 2 × max log length)` 并输出 stderr 警告（保证任何单条日志均可入队，消除 wait 策略死锁边界，见 §9）。
- **溢出策略**（init 时配置，可选集合受编译期约束）：

| 策略 | 行为 | 有锁版本 | 无锁版本 |
|------|------|----------|----------|
| **discard** | 丢弃**新**日志；`dropped` 计数 +1（`hplogc_get_stats` 可见），写入 API 不返回错误 | ✅ | ✅ |
| **overwrite** | 覆盖最旧的未消费条目；`overwritten` 计数 +1 | ✅ | ✅ 仅 SPSC（无锁 MPSC 构建下不可用：多生产者协同推进队首实现风险高，配置为 overwrite 时启动失败） |
| **wait** | 阻塞生产者直到有空闲空间（保证不丢日志） | ✅ | ❌ 不可用，配置为 wait 时启动失败（值合法但构建不支持，见 §10.4） |

> 溢出策略可用集合由编译期构建（并发模式 × 锁实现）决定；"值合法但构建不支持"时启动失败（§10.4 fail-fast）。

> 注意：并发模式（`concurrency`）与是否无锁（`lockfree`）由编译期构建决定，**不可通过配置文件设置**。

### 4.4 I/O 模型

- **同步/异步模式由构建版本决定**（见 §5），不可运行时切换。
- **所有文件写入均为同步文件写入**：无论同步模式还是异步模式，日志落盘只使用标准同步文件 I/O 调用（`fwrite`/`write` 等）。异步模式中的"异步"仅指后台消费者线程的存在，**不引入任何 OS 级异步 I/O 设施（无 io_uring / POSIX AIO / IOCP）**。
- 异步模式下，后台消费者线程对取出的日志条目执行**同步文件写入**，不阻塞生产者线程。
- 批量提交机制：消费者线程攒批多条日志后一次性同步写出（运行时参数：`batch size`、`flush interval`，配置文件键名见 §10.3）。

### 4.5 配置系统

| 属性 | 描述 |
|------|------|
| 配置格式 | INI 风格（zlog conf 兼容词法，§10.1），解析器完全内置，无外部依赖 |
| 风格 | 词法与解析参考 zlog conf.c 的实现；支持 categories、rules、formats（§10） |
| 可扩展性 | 预留接口，后续支持 JSON/YAML |
| 校验 | 解析失败**拒绝启动**，返回 `HPLOGC_ERR_CONFIG` + 行号 |
| 热加载 | 编译期 `HPLOGC_ENABLE_HOT_RELOAD=ON` 时支持；Linux 优先 inotify，SIGHUP 触发需 `signal reload = true`（库安装 handler，默认关闭，§10.3）；Windows/macOS 及 inotify 不可用时，按 `hot reload interval`（秒）轮询文件 mtime/大小回退。热加载生效范围见 §10.5 |
| 多 Category | 编译期 `HPLOGC_ENABLE_CATEGORY=ON` 时支持 |

> **热加载检查的执行体（规范性）**：配置变更检查由库内部的一个**独立监视线程**执行。该线程不属于 §4.2 的生产者/消费者模型，**不参与日志管线**（不生产、不消费日志），生命周期与库一致：`hplogc_init*()` 成功后创建，`hplogc_shutdown()` 时退出。三种触发源统一由该线程处理：
>
> 1. **inotify 事件**（Linux 且可用时优先）；
> 2. **`hot reload interval` 轮询** mtime / 大小（inotify 不可用或其它平台；`0` 表示禁用轮询）；
> 3. **`signal reload = true`** 时注册的 SIGHUP handler ——handler **仅设置一个原子标志**（async-signal-safe），由该线程在下一轮循环中检测标志并执行实际重载；handler 内不得做任何非 async-signal-safe 操作。
>
> 该线程在 `HPLOGC_ENABLE_HOT_RELOAD=ON` 时创建，**与 `HPLOGC_ENABLE_ASYNC` 无关**：因此 `sync_thread` 预设（`ASYNC=OFF` + `HOT_RELOAD=ON`）同样拥有该线程；`min` 预设因 `HOT_RELOAD=OFF` 而不创建。热加载的实际替换语义见 §10.5。

### 4.6 日志轮转

| 策略 | 说明 |
|------|------|
| 按大小 | 达到阈值触发 |
| 按时间 | 按 hour / day / week / month（时间桶边界跨越时触发） |
| 组合 | both（同时按大小和时间，任一条件满足即轮转） |
| 备份数量 | 可配置（0 = 不限制），超出自动清理最旧备份 |
| 命名模板 | 可配置，占位符：`{base}`（基础文件名）、`{timestamp}`（轮转发生时间，格式固定 `%Y%m%d_%H%M%S`，时区跟随 `global.timezone`）、`{index}`（按 base 单调递增序号，从 1 起、不补零）；默认 `{base}.{timestamp}.{index}.log`。模板必须包含 `{index}` 与 `{timestamp}` 至少其一，否则配置错误（§10.4）。仅使用 `{index}`（不含 `{timestamp}`）时等效纯序号风格（对比 zlog `#s`，见附录 A.1-11） |
| 轮转机制 | **活动文件始终为配置 `path`**：触发轮转时将当前活动文件重命名为模板生成的归档名（目标名冲突时 `{index}` 递增至可用名），随后新建活动文件继续写入。"最旧备份"判定：含 `{timestamp}` 的按时间戳排序，仅含 `{index}` 的按序号排序 |
| 时间桶边界 | day = 自然日；week = 周一 00:00 起始（ISO-8601）；month = 自然月；均跟随 `global.timezone` |
| 符号链接 | 可选创建 `.latest` 软链接指向当前活动文件；Windows 上忽略该配置并输出警告（符号链接需特权） |
| 检查时机 | 每条日志写出前检查（size 与当前时间桶），检查开销须为 O(1)（不得逐条 `stat`） |

### 4.7 输出目标：多 sink 体系（v0.2 重写，规范性）

> v0.1 的"output = 固定枚举 + 扁平结构体"已被**废除**。输出目标统一由 **sink** 表达：
> 一个 sink = 一种类型（`hplogc_sink_ops_t`）+ 一个实例（名字 + 私有配置 + 私有状态）。
> §4.10 定义 sink 契约，本节定义 hplogc 随库提供的**内置 sink 清单与各自的配置键**。

#### 4.7.1 内置 sink 清单

| sink type | 配置别名 | 说明 | 能力位 | 依赖 | 裁剪选项 |
|-----------|----------|------|--------|------|----------|
| `console` | — | stdout/stderr 输出；ANSI 彩色（仅终端设备，`HPLOGC_ENABLE_COLOR`） | `SYNC \| ASYNC` | 无 | 不可裁剪（默认配置的兜底 sink） |
| `rollingfile` | `file` | 文件写入，含轮转（size/time/both）、备份数、命名模板、`.latest` 软链、fsync | `SYNC \| ASYNC \| LINE_ATOMIC \| FSYNC` | 无 | 轮转受 `HPLOGC_ENABLE_ROTATE` 裁剪；sink 本体不可裁剪 |
| `syslog` | — | POSIX `openlog`/`syslog` 直通 | `SYNC` | libc syslog | `HPLOGC_SINKS` 未含该类型即为排除（P2） |
| `null` | — | 丢弃一切（测试/基线；也是"审计/指标下线"的路由落点，§7.3） | `SYNC \| ASYNC` | 无 | **默认注册**；`min` 预设排除（§5） |

- `file` 是 `rollingfile` 的**配置别名**：它仅把 `rotate` 的**默认值**设为 `none`，其余键与 `rollingfile` 完全一致；显式书写 `rotate=size|time|both` 时以显式值为准（别名与显式 `rotate` 可共存，不视为冲突）。保留该别名以保证 v0.1 配置文件可直接迁移。
- `HPLOGC_MAX_SINKS`（默认 16）限制 sink **实例**数量；`HPLOGC_MAX_SINK_TYPES`（默认 32）限制**类型**数量（内置 + 自定义）。
- `[outputs]` 中定义的所有 file 类 sink 在 init 时即打开（沿用 v0.1 §4.7 的 fail-fast 语义）。

#### 4.7.2 内置 sink 的配置键（行内 key=value）

| sink | 键 | 含义 | 默认值 | 平台差异 |
|------|----|------|--------|----------|
| console | `stream` | `stdout` / `stderr` | `stdout` | — |
| console | `color` | 是否启用 ANSI 彩色 | 由 `HPLOGC_ENABLE_COLOR` 决定 | Windows 需 VT 处理（§16.2） |
| rollingfile | `path` | 日志文件路径 | 必填 | — |
| rollingfile | `rotate` | `none` / `size` / `time` / `both` | `none` | — |
| rollingfile | `max size` | 按大小轮转阈值（支持尺寸后缀） | 必填（rotate 含 size 时） | — |
| rollingfile | `time unit` | `hour` / `day` / `week` / `month` | 必填（rotate 含 time 时） | — |
| rollingfile | `max files` | 备份数量，0 = 不限制 | 0 | — |
| rollingfile | `fsync` | 每条日志 fsync（与全局 `crash_safety` 取更严格者） | false | — |
| rollingfile | `symlink latest` | 创建 `.latest` 软链 | false | Windows 忽略并输出警告 |
| rollingfile | `rotate naming` | 归档命名模板 | `{base}.{timestamp}.{index}.log` | — |
| rollingfile | `file perms` | 文件权限（八进制） | 0644 | Windows 忽略并输出警告 |
| rollingfile | `dir perms` | 目录权限（八进制） | 0755 | Windows 忽略并输出警告 |
| syslog | `facility` | syslog facility | `user` | Windows 上该类型不可注册 |

- 上述键即 v0.1 `hplogc_output_t` 的字段集合，`rotate`/`time unit` 的取值集合沿用
  `hplogc_rotate_t` / `hplogc_time_unit_t` 两个枚举（二者保留在公共头，作为 rollingfile 的
  **配置值词表**，不再作为公共结构体字段类型，见 §7.6）。
- 未识别的键交由 §10.1 的未知键处理流程（`strict init` 决定成败）。

> Socket 输出：**v0.1 的 socket 骨架预留已删除**。需要网络输出时以**自定义 sink** 形态实现
> （§4.10.5），并在 `[outputs]` 中 `type = <自定义类型>`；未注册的类型一律 fail-fast（§10.4）。

### 4.8 编译期裁剪体系

通过以下 CMake 选项精确控制：

| 选项 | 默认 | 功能 |
|------|------|------|
| `HPLOGC_ENABLE_ASYNC` | ON | 异步写入（后台消费者线程）+ 批量提交 |
| `HPLOGC_ENABLE_COLOR` | ON | ANSI 彩色输出 |
| `HPLOGC_ENABLE_ROTATE` | ON | 日志轮转 |
| `HPLOGC_ENABLE_INI` | ON | INI 配置解析器 |
| `HPLOGC_ENABLE_HOT_RELOAD` | ON | 配置热加载 |
| `HPLOGC_ENABLE_CATEGORY` | ON | 多 Category 路由与 per-category 级别 |
| `HPLOGC_ENABLE_THROTTLE` | OFF | 限流与采样 |
| `HPLOGC_ENABLE_SOURCE_LOC` | ON | `__FILE__`/`__LINE__` 捕获 |
| `HPLOGC_LOCKFREE` | OFF | 无锁队列（选择 `ringbuf_lockfree.c` / `ringbuf_locked.c`，§3.3） |
| `HPLOGC_CONCURRENCY` | MPSC | SPSC / MPSC |
| `HPLOGC_COMPILE_TIME_LEVEL` | TRACE（即不裁剪） | 编译期移除低于该级别的日志代码（仅作用于便捷宏，见 §4.1）；**只接受数值 0~6 或 `HPLOGC_CTL_*` 数值宏**（采用 `HPLOGC_LEVEL_xxx` 符号名会导致静默失效，见 §7.3） |
| `HPLOGC_SINKS` | `console;rollingfile;null` | 参与编译的**内置 sink 类型**列表（§4.7.1）。未列入的内置类型不注册；列表中出现无法识别的类型时视为 CMake 配置错误。`null` 默认注册（§13.1/§16.1 要求其可用），`min` 预设覆盖为 `console;rollingfile`（§5）；`syslog` 需显式追加（P2，§17） |

> **被裁剪功能的 API 行为**：对外 API 符号仍保留（stub 实现），调用时返回 `HPLOGC_ERR_CONFIG`（如 `HPLOGC_ENABLE_CATEGORY=OFF` 时的 `hplogc_set_level_for_category`），不产生未定义行为。
>
> **被裁剪的 sink 类型**：不在 `HPLOGC_SINKS` 列表中的类型视同未注册——`hplogc_sink_is_registered()` 返回 `HPLOGC_ERR_UNSUPPORTED`，配置文件引用该类型时 init 失败并返回 `HPLOGC_ERR_UNSUPPORTED`（fail-fast，不静默降级，§10.4）。用户可用 `enabled=false` 保留条目而不实际创建实例。
>
> **`HPLOGC_ENABLE_CATEGORY=OFF` 时的路由语义**：所有路由规则按 `*` 匹配处理（忽略规则中 category 选择器的具体内容，仅 level 范围与 format/outputs 生效）；`%category` 展开为空串。

### 4.9 日志处理管线（规范性）

一条日志从调用到落盘的处理顺序**固定**如下；各步骤的执行线程由同步/异步模式决定：

```
HPLOGC_xxx 宏 / hplogc_log()
  ① 编译期裁剪        （HPLOGC_COMPILE_TIME_LEVEL，低于阈值的宏展开为空，直接调用不受限）
  ② 级别过滤          （per-category 级别覆盖全局级别：该 category 已设置级别时以其为准，
                       未设置时用全局级别；低于阈值静默丢弃）
  ③ 限流/采样         （HPLOGC_ENABLE_THROTTLE=ON 时：先限流后采样；超限丢弃并计入 throttled）
  ④ 消息体渲染 + 入队  （%msg 展开、**结构化字段深拷贝**、缓冲区满时按 overflow_policy 处理；
                       渲染长度上限为 max log length，字段占用同一预算，见下与 §9、§4.11）
  ⑤ 取出              （同步模式：调用线程直接继续；异步模式：单一消费者线程批量取出）
  ⑥ 路由              （rules 从上到下首条命中，确定 format 与 sinks；无命中规则时按
                       default format + default sinks 兜底输出，default sinks 为空则丢弃）
  ⑦ 整行格式化        （按命中规则的 format 生成完整日志行；json 格式仅执行 JSON 转义、
                       跳过注入转义，见 §12）
  ⑧ 输出              （按 sink 分组投递：异步模式下先对每个 sink 分组攒批调用 emit_batch；
                       未实现 emit_batch 的 sink 退化为逐条 emit（§4.10.3 能力位矩阵）；
                       按 crash_safety 策略 fsync，见 §4.10.4）
```

> **术语澄清**：规范中**不存在**"异步模式的同步旁路"这一机制。异步构建（`HPLOGC_ENABLE_ASYNC=ON`）下，步骤 ⑧ 的输出**一律由唯一的消费者线程执行**——即使某个 sink 只具备 `HPLOGC_CAP_SYNC`，也由消费者线程逐条调用其 `emit`，而**不**回落到调用者线程（否则生产者会被 I/O 阻塞，与 §4.4 的异步语义冲突）。`HPLOGC_CAP_SYNC` 仅在同步构建下决定"由调用者线程执行 emit"。

- **同步模式**（`HPLOGC_ENABLE_ASYNC=OFF` 或构建预设 min/sync_thread）：②-⑧ 全部在调用者线程执行，无后台线程。
- **异步模式**：②③④（含**消息体渲染**）在生产者线程，⑤⑥⑦⑧ 在唯一消费者线程。消息体渲染指 `%msg` 的展开（`fmt` + `va_list`）——`va_list` 不可跨线程，**必须**在生产者线程完成；渲染结果长度上限为 `max log length`（超出部分静默丢弃，不加 marker；整行截断与 `truncation marker` 在步骤⑦整行层面附加，§9）。入队载荷为"消息体（受 `max log length` 约束）+ 元数据（level、category、file、line、func、时间戳）"。消费者线程完成路由（⑥）、整行组装（⑦，将消息体嵌入格式模板并渲染其余占位符）与输出（⑧），并通过**同步文件写入**完成落盘。
- **格式预编译（规范性）**：格式模板与路由规则在 init/热加载时预编译为内部动作序列（含占位符类型解析）；热路径不得对格式模板做字符串二次解析。
- **注入防护（`escape injection`）在步骤 ⑦ 执行**：仅转义消息体（`%msg` 展开结果）中的换行符（`\n`/`\r` → `\n` 字面转义文本）和 ANSI 转义序列（`ESC` → `\x1b` 字面文本）；不影响库自身生成的格式与颜色序列。json 格式例外（§12）。
- **category 语义**：无需注册，写入时直接传字符串；库内部按需登记（用于 per-category 级别缓存与路由加速），登记表容量为 `HPLOGC_MAX_CATEGORIES`（默认 64，编译期可覆盖），表满后新 category 按 `*` 兜底规则路由并输出一次 stderr 警告（每个被拒绝的 category 仅一次）。category 为 NULL 或空串等价于类别名 `"*"`（仅匹配选择器 `*`，`%category` 展开为空串）。category 名为任意非空字节串，`.` 保留为层级分隔符，其余字符按字面匹配（库不做字符集校验，避免热路径开销）。

### 4.10 Sink 抽象与生命周期（v0.2 新增，规范性）

> 设计来源：`tmp/sink_desin.md` 与 `tmp/clog.h` 的"核心不认识任何具体 sink，只认识 ops 指针"模型。
> **收益**：新增一种 sink = 加一个 `.c` 文件 + 注册一行，核心、路由、异步队列、埋点宏均不改动。

#### 4.10.1 三层结构

```
hplogc_sink_ops_t（类型：vtable，静态常量，注册进注册表）
        │ 1 : N
hplogc_sink_t（实例：不透明句柄 = name + priv + 每实例统计 + 路由引用）
        │ N : M
route / rule（通过实例名引用 sink）
```

- **注册表**：`type` → `ops` 的映射，init 期构建完成后只读，运行期查找无锁。
- **实例**：由配置文件 `[outputs]` 的条目或代码内 `hplogc_sink_config_t` 创建；名称唯一。
- 核心只持有 `ops` 指针，不认识 console / rollingfile / 自定义类型中的任何一个。

#### 4.10.2 生命周期顺序（规范性）

```
create ──► configure* ──► init ──► start ──► ( emit | emit_batch )* ──► flush ──► destroy
```

> `init` 与 `start` 同属"启动阶段"，由 `hplogc_sink_start()` 依次触发；配置文件路径下由 init 流程自动走到该阶段。

| 阶段 | 触发时机 | 回调 | 允许的操作 |
|------|----------|------|------------|
| create | 解析到输出条目 / `hplogc_sink_create()` | — | 分配并**清零** `priv_size` 大小的私有数据 |
| configure | 逐项读入私有配置键 | `configure` | 解析键值；未识别的键返回非 0 |
| start | 全部配置读入后 | `init` → `start` | **`init` 只做默认值填充**，`start` 打开真实资源 |
| emit | 管线步骤 ⑧ | `emit` / `emit_batch` | 写出；失败不得返回错误给调用方，计入 per-sink 统计 |
| flush | `hplogc_flush()`、热加载替换前、shutdown | `flush` | 冲刷缓冲（不 fsync） |
| destroy | 实例被移除 / shutdown | `destroy` | 关闭 fd、释放私有资源；核心随后释放实例本体 |

> **关键约束（移植自 clog 的踩坑经验，规范性）**：`init` 在**全部** `configure` 之后才执行，
> 因此 `init` 内只能写 `if (p->field == 0) p->field = default;` 形式的条件赋值，
> **严禁无条件赋值**——否则会把刚读入的配置值清零。该约束必须写入自定义 sink 的开发 checklist。

#### 4.10.3 能力位与线程模型

| 能力位 | 含义 | 核心行为 |
|--------|------|----------|
| `HPLOGC_CAP_SYNC` | 可在调用线程直接写 | 仅在**同步构建**（`HPLOGC_ENABLE_ASYNC=OFF`）下由**调用者线程**执行 `emit`；异步构建下本能力位不决定线程归属（一律由消费者线程执行，见下方矩阵与 §4.9⑧） |
| `HPLOGC_CAP_ASYNC` | 支持后台线程批量写 | 异步模式下由**唯一消费者线程**调用 `emit_batch` |
| `HPLOGC_CAP_STRUCT` | 能消费结构化字段 | 核心**始终**传递 `hplogc_event_t::fields`（无字段时为 NULL，不按能力位分叉事件布局）；声明该位的 sink 消费之，未声明的 sink **必须忽略**该成员 |
| `HPLOGC_CAP_LINE_ATOMIC` | 需要行级原子写保护 | 核心保证整行一次 `write`（≤ `PIPE_BUF`），或对该实例串行化写入 |
| `HPLOGC_CAP_FSYNC` | 支持 fsync | `hplogc_sync()` 对该实例调用 fsync |

- 必须至少实现 `emit` 或 `emit_batch` 之一；二者都缺失时注册失败（`HPLOGC_ERR_INVALID_ARG`）。
- 只有 `emit` 而处于异步模式时，核心退化为"逐条调用 `emit`"并输出一次性提示。
- **返回值契约（规范性）**：`emit_batch` 返回**成功写出的条数**（取值 `0 <= n <= 入参 n`），负值表示整批失败。核心据此记账：部分成功时 `n` 计入该 sink 的 `written`、剩余 `入参 n - n` 计入 `failed`；整批失败（负值）时全部计入 `failed`（§9）。`emit` 为 `void`，失败只能通过 per-sink 统计暴露。
- **能力位 × 构建模式矩阵（规范性，补齐组合边界）**：

| 构建模式 | sink 具备的能力位 | 输出执行者与调用 |
|----------|-------------------|------------------|
| 同步（`HPLOGC_ENABLE_ASYNC=OFF`） | `CAP_SYNC` | **调用者线程**执行 `emit` |
| 同步 | **无** `CAP_SYNC`（仅 `CAP_ASYNC`） | **init 失败**，返回 `HPLOGC_ERR_UNSUPPORTED`（该 sink 在本构建下不可用，fail-fast，§10.4） |
| 异步（`HPLOGC_ENABLE_ASYNC=ON`） | `CAP_ASYNC` | **消费者线程**调用 `emit_batch`；未实现 `emit_batch` 则逐条 `emit` |
| 异步 | **无** `CAP_ASYNC`（仅 `CAP_SYNC`） | **消费者线程逐条 `emit`**（不回落到调用者线程，见 §4.9⑧ 注） |
| 任一 | `emit` 与 `emit_batch` 皆无 | **注册失败** `HPLOGC_ERR_INVALID_ARG`（见上文"必须至少实现其一"） |

- **per-sink `async` 键的作用域（规范性）**：`async` 只是在上表矩阵内做**显式选择**，不创造矩阵外的组合：
  - `async = true` 要求 sink 具备 `CAP_ASYNC`；不具备时**告警并忽略该键**（§10.4，非错误）。
  - `async = false` 要求 sink 具备 `CAP_SYNC`；不具备时同样**告警并忽略**（异步构建下仍可由消费者线程逐条 `emit`，不使 init 失败）。
  - 未指定（`async` 缺省）时按矩阵自动选择。
- **sink 回调内的禁止事项**：
  1. 不得调用除 `hplogc_sink_*` 之外的库 API（防重入死锁）；
  2. 除首次私有初始化外，不得动态分配内存（§9 稳态零 malloc）；
  3. 不得缓存 `hplogc_event_t` 内的任何指针（事件仅在回调期间有效，见 §7.6）。

#### 4.10.4 投递、攒批与背压

- 异步模式下，消费者线程取出一批事件后**先按 sink 分组**，再对每组调用一次 `emit_batch`
  （同一 sink 的连续事件合并为一次网络/文件往返）；这是抵消 vtable 间接调用开销的关键手段，
  保证 §8 的延迟目标不被间接层侵蚀。
- 单个 sink 的容量限制（`HPLOGC_MAX_SINKS`、`HPLOGC_MAX_SINK_TYPES`）在 init 时校验。
- 背压：sink 自身拥塞时**不得阻塞整条管线**——丢弃并计入该 sink 的 `dropped`，
  故障详情按 §9 的限频 stderr 告警（每 5 秒至多 1 条）。

#### 4.10.5 自定义 sink 与零依赖裁决（规范性）

- `hplogc_sink_register()` 属于**公开 ABI**：应用或第三方可在 init 之前注册自定义类型，
  随后在配置文件中以 `type = <ops->type>` 引用。
- **所有权**：核心保存 `ops` **指针本身**（不拷贝其内容），调用方须保证其生命周期覆盖
  整个库使用期，建议指向静态常量；已初始化后调用返回 `HPLOGC_ERR_STATE`。
- **ABI 契约**：`hplogc_sink_ops_t` 的字段顺序在 `HPLOGC_SINK_ABI_VERSION` 主版本内冻结；
  新增回调只能通过消耗 `reserved[4]` 槽位追加；未使用的槽位必须为 NULL。
- **版本号编码（规范性）**：`HPLOGC_SINK_ABI_VERSION = (主版本 << 16) | 次版本`（当前为
  `1 << 16 | 0`）。注册表用 `HPLOGC_SINK_ABI_MAJOR_OF()` 抽取主版本并**要求与库内主版本严格
  相等**，不等则注册失败并返回 `HPLOGC_ERR_UNSUPPORTED`；次版本不做相等要求（向下兼容的尾部追加）。
- **零依赖裁决**：kafka / loki / elasticsearch / clickhouse / s3 / mysql 等需要第三方客户端的
  sink **不得进入 hplogc 仓库**（与 §6 零第三方依赖、§5 min 体积预算冲突）。
  这些目标一律以"仓库外的自定义 sink"形态实现，本文不为其承诺任何内置支持；
  `HPLOGC_SINKS` 列表只允许包含零依赖类型。

### 4.11 结构化字段通道（v0.2 新增，规范性）

> 设计目的：今天就算不实现任何检索型 sink，也要把**字段通道**打通，
> 将来接入自定义 sink 时埋点代码无需改动（这是"预留接口"最实际的价值）。

#### 4.11.1 语义

- 写入侧提供 `hplogc_log_fields()` / `hplogc_vlog_fields()` 及 `HPLOGC_xxx_F` 便捷宏，
  以及两条语义子流：`HPLOGC_AUDIT()`（category 固定 `"audit"`）、`HPLOGC_METRIC()`（固定 `"metric"`）。
- 字段为 `hplogc_field_t`：`key` + 类型标签（`hplogc_vtype_t`）+ 联合值；
  类型支持 int / uint / double / str / bool。
- **字段只被 `HPLOGC_CAP_STRUCT` 的 sink 消费**；内置文本格式（含 `json` 格式的字符串模板）
  **不渲染字段**——即 `%msg` 不因字段而改变，`%category`/`%time` 等占位符集合保持不变。

#### 4.11.2 生命周期：必须深拷贝（与 clog 刻意不同）

- hplogc 采用**环形缓冲跨线程异步**模型，`va_list` 与栈上临时缓冲都无法跨越线程边界；
  因此字段（**含 `key` 与 `HPLOGC_V_STR` 的字符串值**）连同消息体在**生产者线程**被深拷贝进
  环形缓冲——**调用方无需保证指针生命周期**，允许传入栈上字符串。`key` 与字符串值同受
  `HPLOGC_MAX_FIELD_STR_LEN` 截断约束。这与 clog 的"借用指针、必须是字面量"相比更安全、
  也更贴合 hplogc 模型。
- 代价与约束（**下列三种成因均计入** `hplogc_stats_t::fields_dropped` 与
  `hplogc_sink_stats_t::fields_dropped`，口径统一，便于 §13.2 的计数断言）：
  - 字段总受 `max_log_length` 预算约束（与消息体共享），超出部分被丢弃；
  - 单条字符串的拷贝上限为 `HPLOGC_MAX_FIELD_STR_LEN`（默认 128 字节，不含 NUL），
    超出部分静默截断（**截断亦计入 `fields_dropped`**）；
  - 单条记录的字段数上限为 `HPLOGC_MAX_FIELDS`（默认 16），超出部分丢弃并计数；
  - §4.3 的 `buffer size ≥ 2 × max_log_length` 下限已包含字段开销（字段不额外扩容缓冲区）。
- `hplogc_log_signal_safe()` **不携带字段**：字段拷贝违反 async-signal-safe 要求。

#### 4.11.3 典型用法

```c
hplogc_field_t af[] = {
    HPLOGC_FIELD_STR("actor", "alice"),
    HPLOGC_FIELD_INT("amount", 5000)
};
hplogc_field_t mf[] = { HPLOGC_FIELD_DOUBLE("value", 12.5) };

HPLOGC_AUDIT(af, 2, "%s", "转账");                        /* → 审计 sink */
HPLOGC_METRIC(mf, 1, "http_request_duration_ms");         /* → 指标 sink */
```

#### 4.11.4 开销纪律

- 字段渲染与拷贝发生在生产者线程，属于**热路径**：禁止在此期间分配堆内存
  （§9 允许的 per-thread 缓冲例外同样适用于字段区）。
- 未调用 `*_fields()` 的记录不得引入任何字段相关的额外开销（空字段区直接跳过）。

---

## 5. 构建版本预设

| 版本 | 强制 CMake 选项（覆盖默认值） | 可用配置节 | 适用场景 |
|------|------------------------------|-----------|----------|
| **full** | 无强制（全部功能 ON，`CONCURRENCY=MPSC`，`LOCKFREE=OFF`） | 全部节 | 通用服务端 |
| **min** | `ASYNC=OFF`、`ROTATE=OFF`、`COLOR=OFF`、`CATEGORY=OFF`、`HOT_RELOAD=OFF`、`THROTTLE=OFF`、`INI=OFF`、`CONCURRENCY=SPSC`、`LOCKFREE=OFF`、`SINKS=console;rollingfile` | 无（INI=OFF，不解析配置文件）：仅代码内配置 `hplogc_config_t` | 嵌入式/资源受限 |
| **sync_thread** | `ASYNC=OFF`、`LOCKFREE=OFF`（其余默认，`CONCURRENCY=MPSC`） | global, formats, outputs, buffer(mpsc), rules, advanced（[async] 节跳过） | 单线程/低并发 |
| **async_single** | `ASYNC=ON`、`LOCKFREE=ON`、`CONCURRENCY=SPSC` | global, formats, outputs, buffer(无锁/spsc), async, rules, advanced | 高吞吐生产 |

> **预设与用户 CMake 选项冲突时，以预设为准**，并在 CMake 配置期输出警告。预设仅服务于其定位场景：需要"资源受限 + 配置文件"组合的用户可不使用预设，直接以 `HPLOGC_ENABLE_*` 选项自行组合。
>
> **min 版本可配置项**（代码内）：`escape_injection`、`max_log_length`、`truncation_marker`、`crash_safety`、`signal_safe`、`fork behavior` 等高级项保留；stats 相关项在 min 下忽略（无后台线程）。**时区、`time format`、`newline`、`pid/tid format` 等原本"只能改配置文件"的键，在 v0.2 统一可经 `hplogc_kv_t` 全局键值表达**（§7.6 破坏性变更 B5），min 预设因此不再存在"配置项不可达"的结构性缺陷。
>
> **各预设可用溢出策略**受 §4.3 构建约束：如 async_single（无锁 SPSC）支持 discard/overwrite；无锁 MPSC 组合仅支持 discard。
>
> **min 版本体积验证条件**：Linux x86_64，GCC `-Os`，strip 后 `.text` 段 ≤ 8 KB（INI 解析器已随 INI=OFF 排除在 min 之外）。
>
> **sink 层的体积预算（新增验收项）**：注册表 + vtable 间接调用层相对 v0.1 的直接调用，
> 在 min 预设下的 `.text` 增量须 ≤ 0.5 KB；超出则视为 §8 之外的回归项。

---

## 6. 依赖与体积

| 要求 | 说明 |
|------|------|
| 语言依赖 | 仅标准 C 库（`libc`） |
| OS 依赖 | 仅 OS 原生 API |
| 第三方库 | **零依赖**；INI 解析器完全自包含（词法参考 zlog conf.c，自研实现） |
| 最小体积 | `min` 版本代码段（`.text`）≤ 8 KB（验证条件见 §5；与 §8 的"基线内存 < 32 KB"为不同口径，后者含数据段与运行期结构） |
| 头文件 | 公共 API 集中在 `hplogc.h`，内部头文件不对外暴露 |

---

## 7. API 与接口设计

### 7.1 命名与导出

- 所有 API 使用 `hplogc_` 前缀。
- 导出宏 `HPLOGC_API`：Windows → `__declspec(dllexport/dllimport)`；GCC/Clang → `__attribute__((visibility("default")))`。
- 版本宏：`HPLOGC_VERSION_MAJOR` / `HPLOGC_VERSION_MINOR` / `HPLOGC_VERSION_PATCH`。

### 7.2 初始化方式

| 方式 | API |
|------|-----|
| 配置文件路径 | `int hplogc_init_from_file(const char* path)` |
| 代码内配置 | `int hplogc_init(const hplogc_config_t* cfg)` |
| 默认配置 | `int hplogc_init_default(void)` |
| 填充默认配置结构 | `void hplogc_config_default(hplogc_config_t* cfg)` |
| 销毁 | `void hplogc_shutdown(void)` |
| 级别预检 | `int hplogc_level_enabled(hplogc_level_t level, const char* category)` |
| 撤销 category 级别覆盖 | `int hplogc_clear_level_for_category(const char* category)` |
| sink 类型注册（init 前调用） | `int hplogc_sink_register(const hplogc_sink_ops_t* ops)` |

- `hplogc_init(NULL)` 等价 `hplogc_init_default()`；`hplogc_init_from_file(NULL)` 视为参数非法，返回 `HPLOGC_ERR_INVALID_ARG`。
- `hplogc_config_default(cfg)` 以全部默认值填充 `hplogc_config_t`（cfg 为 NULL 时安全忽略），并把 `struct_size` 置为 `sizeof(hplogc_config_t)`。**调用方必须先调用本函数初始化结构体（或对全部字段显式赋值）后再调用 `hplogc_init`；未经初始化的字段内容未定义**。`struct_size` 与核心编译期尺寸不一致时 init 返回 `HPLOGC_ERR_CONFIG`（ABI 自检，见 §7.6）。
- 默认配置（`hplogc_init_default()`）语义：level=INFO、standard 格式、**一个内置的 stderr console sink**、buffer 1MB、溢出 discard。
- **代码内配置与配置文件的能力边界（v0.2 破坏性变更 B5）**：二者**能力对等**。v0.1 中"只能通过配置文件设置"的键——`timezone`、`timestamp source`、`time format`、`encoding`、`newline`、`pid format`/`tid format`、`capture source loc`、`hot reload interval`、`signal reload`、`strict init`、`[throttle]` 全部参数、`[advanced]` stats 项——统一经 `hplogc_kv_t` 数组的 `global_options` 表达（键名与配置文件完全一致）。此项修复了 min 预设（`INI=OFF`）下这些配置不可达的结构性缺陷。
- 仍保留的限制：**命名 formats 表只能由配置文件定义**（代码内无等价载体）；多路由规则通过 `hplogc_rule_t` 数组表达，**rule 的 `format` 字段仅允许引用 5 个内置格式名**（`minimal`/`standard`/`categorized`/`detailed`/`json`）。
- **内置格式名的可用性与覆盖规则（规范性）**：5 个内置格式名**始终可用**——无论配置文件是否含 `[formats]` 节、该节是否列出它们，引用内置名都**不**触发"引用未定义 format"的启动失败（§10.4），未显式定义的条目使用内置默认模板（模板内容见 §10.3）。若 `[formats]` 中显式定义了与内置名同名的条目，则**该条目覆盖内置默认模板**（合法，不报错；显式配置优先）。`hplogc_init_default()` 与 `hplogc_init()` 的默认格式 `standard` 按同一规则解析。
- sink 实例在代码内由 `hplogc_sink_config_t` 数组表达，与配置文件的 `[outputs]` 条目一一对应（§4.7.2、§7.6）。
- **代码内配置的值非法**（范围外、非法枚举值、引用未定义名称、rule 的 min_level > max_level 等）→ init 返回 `HPLOGC_ERR_INVALID_ARG` 或 `HPLOGC_ERR_CONFIG`，**不做 clamp**（clamp + 警告仅适用于配置文件路径，§10.4）。

### 7.3 核心 API 签名

```c
/* 初始化与销毁 */
int  hplogc_init(const hplogc_config_t* cfg);
int  hplogc_init_from_file(const char* config_path);
int  hplogc_init_default(void);
void hplogc_config_default(hplogc_config_t* cfg);  /* 填充默认值（含 struct_size）；cfg 为 NULL 安全忽略 */
void hplogc_shutdown(void);

/* 强制刷新：flush 将队列中已入队日志尽量写出，并对每个 sink 调用其 flush 回调
 * （异步模式通知消费者立即提交，同步模式刷新 stdio），不 fsync；
 * sync = flush + 对所有声明 HPLOGC_CAP_FSYNC 的 sink 强制 fsync。
 * 未初始化时返回 HPLOGC_ERR_STATE。 */
int  hplogc_flush(void);
int  hplogc_sync(void);

/* 运行时动态控制（未初始化时返回 HPLOGC_ERR_STATE）。
 * set_level 接受 HPLOGC_LEVEL_OFF 关闭全部输出；
 * set_level_for_category 覆盖全局级别（覆盖语义：该 category 已设置级别时以其为准）；
 * clear_level_for_category 撤销覆盖、回退到全局级别（幂等）；
 * 三者 category 为 NULL 时返回 HPLOGC_ERR_INVALID_ARG。
 * level_enabled 为快速预检：用于在构造昂贵参数之前短路（可在任意时刻调用）。 */
int  hplogc_set_level(hplogc_level_t level);
int  hplogc_set_level_for_category(const char* category, hplogc_level_t level);
int  hplogc_clear_level_for_category(const char* category);
int  hplogc_level_enabled(hplogc_level_t level, const char* category);

/* 级别名互转（可在任意时刻调用，不要求已初始化）
 * level_name ：返回级别名常量字符串（"TRACE".."OFF"），未知级别返回 "UNKNOWN"；
 *              返回指针指向静态存储，不得释放。
 * level_parse：大小写不敏感，接受 "TRACE".."FATAL" 与 "OFF"；
 *              名字非法或任一参数为 NULL 时返回 HPLOGC_ERR_INVALID_ARG。 */
const char* hplogc_level_name(hplogc_level_t level);
int         hplogc_level_parse(const char* name, hplogc_level_t* out);

/* 构建信息查询：info 为 NULL 时安全忽略；可在任意时刻调用（不要求已初始化）。 */
typedef struct {
    const char* build_version;       /* "full" / "min" / "sync_thread" / "async_single" / "custom" */
    int         has_async;
    int         has_color;
    int         has_rotate;
    int         has_hot_reload;
    int         has_category;
    int         has_throttle;
    int         has_ini;
    int         lockfree;
    const char* concurrency;         /* "spsc" / "mpsc" */
    int         has_fields;          /* 结构化字段通道（v0.2 起恒为 1） */
    const char* sinks;               /* 已编译进本库的 sink 类型，逗号分隔 */
} hplogc_build_info_t;               /* 只允许尾部追加字段（§15） */
void hplogc_get_build_info(hplogc_build_info_t* info);

/* 运行统计（线程安全，原子读取；未初始化时返回 HPLOGC_ERR_STATE，stats 为 NULL 返回 HPLOGC_ERR_INVALID_ARG） */
typedef struct {
    unsigned long long accepted;     /* 通过过滤进入队列的条数 */
    unsigned long long dropped;      /* 按条计数，一条日志至多 +1：
                                      *   discard 溢出丢弃 + 限流/采样之外"未进入任何 sink
                                      *   输出阶段"的丢弃 + 全部 sink 均写失败的丢弃（§12.4） */
    unsigned long long overwritten;  /* overwrite 策略覆盖的条数 */
    unsigned long long throttled;    /* 限流/采样丢弃条数（THROTTLE=OFF 时恒为 0） */
    unsigned long long written;      /* 已写出条数：按条计数，不按 sink 计数——
                                      *   一条日志投递至 N 个 sink 时，**至少 1 个** sink
                                      *   成功写出即 +1；全部失败则不计入 written
                                      *   （改计入 dropped，§12.4） */
    size_t             buffer_used;  /* 当前缓冲区占用字节 */
    size_t             buffer_size;  /* 缓冲区总大小字节 */
    unsigned long long fields_dropped; /* 因超数量/超长度被丢弃的结构化字段数 */
} hplogc_stats_t;                    /* 只允许尾部追加字段（§15） */
int  hplogc_get_stats(hplogc_stats_t* stats);

/* 日志写入（未初始化时静默丢弃；level 为 HPLOGC_LEVEL_OFF 或非法时静默丢弃） */
void hplogc_log(hplogc_level_t level, const char* category,
                 const char* file, int line, const char* func,
                 const char* fmt, ...);
void hplogc_vlog(hplogc_level_t level, const char* category,
                  const char* file, int line, const char* func,
                  const char* fmt, va_list ap);

/* 带结构化字段的写入（fields 在生产者线程深拷贝入队，无需保证指针生命周期，§4.11.2） */
void hplogc_log_fields(hplogc_level_t level, const char* category,
                       const char* file, int line, const char* func,
                       const hplogc_field_t* fields, size_t field_count,
                       const char* fmt, ...);
void hplogc_vlog_fields(hplogc_level_t level, const char* category,
                        const char* file, int line, const char* func,
                        const hplogc_field_t* fields, size_t field_count,
                        const char* fmt, va_list ap);

/* async-signal-safe 受限写入（本函数始终为 async-signal-safe，并保存/恢复调用线程的 errno）。
 * signal_safe = false（默认）时调用本函数静默丢弃（无未定义行为）；
 * signal_safe = true 时库在 init 时预分配信号通道资源。
 * 无锁、无格式化、无内存分配，通过 write() 直写 stderr；
 * msg 必须为调用方提供的以 '\0' 结尾的静态/栈上缓冲区。 */
void hplogc_log_signal_safe(hplogc_level_t level, const char* msg);  /* 不携带字段，§4.11.2 */

/* ---- Sink 注册与生命周期（自定义 sink 的接入点，§4.10） ---- */
int  hplogc_sink_register(const hplogc_sink_ops_t* ops);   /* init 之前调用 */
int  hplogc_sink_unregister(const char* type);             /* 无活跃实例时才允许 */
int  hplogc_sink_is_registered(const char* type);          /* 特性探测，可在 init 前调用 */
int  hplogc_sink_create(const char* type, const char* name, hplogc_sink_t** out);
int  hplogc_sink_configure(hplogc_sink_t* sink, const char* key, const char* val);
int  hplogc_sink_start(hplogc_sink_t* sink);               /* configure 全部完成后调用 */
void* hplogc_sink_priv(hplogc_sink_t* sink);
int  hplogc_sink_flush(hplogc_sink_t* sink);
int  hplogc_sink_get_stats(hplogc_sink_t* sink, hplogc_sink_stats_t* stats);
hplogc_sink_t* hplogc_sink_find(const char* name);          /* 观测用途，不长期缓存 */
void hplogc_sink_destroy(hplogc_sink_t* sink);

/* ---- 编译期裁剪阈值（必须为数值，见下方说明） ---- */
#define HPLOGC_CTL_TRACE 0
#define HPLOGC_CTL_DEBUG 1
#define HPLOGC_CTL_INFO  2
#define HPLOGC_CTL_WARN  3
#define HPLOGC_CTL_ERROR 4
#define HPLOGC_CTL_FATAL 5
#define HPLOGC_CTL_OFF   6

/* 便捷宏（HPLOGC_COMPILE_TIME_LEVEL 裁剪点）。
 * 签名形式为 (cat, ...)：fmt 是必需参数，作为首个可变参数传入；头文件内部直接展开
 * __VA_ARGS__，不经 HPLOGC_VA_ARGS 中转（见下方可移植性说明）。 */
#define HPLOGC_TRACE(cat, ...)  hplogc_log(HPLOGC_LEVEL_TRACE, cat, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define HPLOGC_DEBUG(cat, ...)  hplogc_log(HPLOGC_LEVEL_DEBUG, cat, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define HPLOGC_INFO(cat, ...)   hplogc_log(HPLOGC_LEVEL_INFO,  cat, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define HPLOGC_WARN(cat, ...)   hplogc_log(HPLOGC_LEVEL_WARN,  cat, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define HPLOGC_ERROR(cat, ...)  hplogc_log(HPLOGC_LEVEL_ERROR, cat, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define HPLOGC_FATAL(cat, ...)  hplogc_log(HPLOGC_LEVEL_FATAL, cat, __FILE__, __LINE__, __func__, __VA_ARGS__)

/* 带字段的便捷宏（fields / nfields 版；同样受 HPLOGC_COMPILE_TIME_LEVEL 裁剪） */
#define HPLOGC_TRACE_F(cat, fields, nfields, ...)  hplogc_log_fields(HPLOGC_LEVEL_TRACE, cat, __FILE__, __LINE__, __func__, fields, nfields, __VA_ARGS__)
#define HPLOGC_DEBUG_F(cat, fields, nfields, ...)  hplogc_log_fields(HPLOGC_LEVEL_DEBUG, cat, __FILE__, __LINE__, __func__, fields, nfields, __VA_ARGS__)
#define HPLOGC_INFO_F(cat, fields, nfields, ...)   hplogc_log_fields(HPLOGC_LEVEL_INFO,  cat, __FILE__, __LINE__, __func__, fields, nfields, __VA_ARGS__)
#define HPLOGC_WARN_F(cat, fields, nfields, ...)   hplogc_log_fields(HPLOGC_LEVEL_WARN,  cat, __FILE__, __LINE__, __func__, fields, nfields, __VA_ARGS__)
#define HPLOGC_ERROR_F(cat, fields, nfields, ...)  hplogc_log_fields(HPLOGC_LEVEL_ERROR, cat, __FILE__, __LINE__, __func__, fields, nfields, __VA_ARGS__)
#define HPLOGC_FATAL_F(cat, fields, nfields, ...)  hplogc_log_fields(HPLOGC_LEVEL_FATAL, cat, __FILE__, __LINE__, __func__, fields, nfields, __VA_ARGS__)

/* 级别预检宏（昂贵参数构造前短路；不裁剪） */
#define HPLOGC_TRACE_ENABLED(cat)  hplogc_level_enabled(HPLOGC_LEVEL_TRACE, cat)
#define HPLOGC_DEBUG_ENABLED(cat)  hplogc_level_enabled(HPLOGC_LEVEL_DEBUG, cat)
#define HPLOGC_INFO_ENABLED(cat)   hplogc_level_enabled(HPLOGC_LEVEL_INFO,  cat)
#define HPLOGC_WARN_ENABLED(cat)   hplogc_level_enabled(HPLOGC_LEVEL_WARN,  cat)
#define HPLOGC_ERROR_ENABLED(cat)  hplogc_level_enabled(HPLOGC_LEVEL_ERROR, cat)
#define HPLOGC_FATAL_ENABLED(cat)  hplogc_level_enabled(HPLOGC_LEVEL_FATAL, cat)

/* 语义子流：category 固定、级别 INFO；**不受 HPLOGC_COMPILE_TIME_LEVEL 裁剪**（见下方说明） */
#define HPLOGC_AUDIT(fields, nfields, fmt, ...)   /* category 固定 "audit" */
#define HPLOGC_METRIC(fields, nfields, fmt, ...)  /* category 固定 "metric" */
```

> **可移植性说明（`##__VA_ARGS__`）**：头文件按以下顺序自动选择宏实现，并以 `HPLOGC_HAS_VA_ARGS` 标识当前实现：
> 1. GCC/Clang：`fmt, ##__VA_ARGS__`（GNU 扩展，允许零个可变参数）；
> 2. MSVC 传统预处理器（`_MSC_VER` 定义且 `_MSVC_TRADITIONAL` 未定义或 `!= 0`）：`fmt, __VA_ARGS__`（依赖 MSVC 对空 `__VA_ARGS__` 的自动吞逗号行为）；
> 3. MSVC `/Zc:preprocessor`（`_MSVC_TRADITIONAL == 0`）或 `__VA_OPT__` 可用（C23/C++20）：`fmt __VA_OPT__(,) __VA_ARGS__`；
> 4. 其余严格 C99 环境：`fmt, __VA_ARGS__`（要求调用时至少提供一个可变参数）。
>
> **实现注记（v0.2）**：由于 log / log_fields 的 `fmt` 是必需参数，头文件的便捷宏内部直接展开
> `__VA_ARGS__` 而不经过 `HPLOGC_VA_ARGS` 中转——这样在 `-pedantic` 下也不会触发
> "ISO C99 requires at least one argument" 告警。`HPLOGC_VA_ARGS` 仅作为调用方自行封装
> 零参宏时的兼容层保留。

> **`HPLOGC_COMPILE_TIME_LEVEL` 取值形式（v0.2 修正，规范性）**：**只接受数值 0~6 或
> `HPLOGC_CTL_*` 数值宏**。原因是 `HPLOGC_LEVEL_xxx` 为枚举常量而非预处理宏，在 `#if` 中被求值
> 为 0，导致"写了 `HPLOGC_LEVEL_INFO` 却裁掉全部 TRACE/DEBUG/INFO 之外毫无作用"的静默错误
> （v0.1 的头实现 + v0.1 的规范描述组合起来必然踩此坑）。修正后的分工：
>
> | 角色 | 职责 |
> |------|------|
> | CMake | 接收 `HPLOGC_COMPILE_TIME_LEVEL = TRACE\|DEBUG\|INFO\|WARN\|ERROR\|FATAL\|OFF` 字符串，**映射为数值**后再定义该宏；空值（不裁剪）**必须映射为 `0`（`HPLOGC_CTL_TRACE`）而非空宏定义**。CMake 是**唯一有效的防线**：它必须拒绝白名单之外的任何输入（含 `HPLOGC_LEVEL_INFO` 这类枚举符号名） |
> | 头文件 | 对取值做范围校验，越界（`<0` 或 `>6`）即 `#error`；提供 `HPLOGC_CTL_*` 供直接写数值的场景提升可读性。**该校验无法拦截枚举符号名**——枚举常量在 `#if` 中被求值为 `0`，`0` 落在 `[0,6]` 内不会触发 `#error`，因此"想裁剪却全部保留"的静默错误只能由 CMake 侧拦下 |
>
> 便捷宏 `HPLOGC_xxx` 与 `HPLOGC_xxx_F` 同为裁剪点；`HPLOGC_xxx_ENABLED` 预检宏**不裁剪**，
> 用于在任意构建下做运行时短路。
>
> **`HPLOGC_AUDIT()` / `HPLOGC_METRIC()` 不受 `HPLOGC_COMPILE_TIME_LEVEL` 裁剪**（规范性）：
> 审计与指标埋点承载合规与可观测性职责，不得因构建预设被静默移除。需要彻底去除时应由
> 调用方在源码层面删除埋点，或由路由规则把 `audit` / `metric` 两个 category 指向 `null`
> sink。头文件实现为直接展开 `HPLOGC__LOG_F`，不套裁剪分支。

### 7.4 错误处理

- 初始化/配置/控制类 API 的返回类型为 `hplogc_error_t`（定义见 §7.6），以 `int` 承载：`HPLOGC_OK`（`0`）= 成功，负值 = 错误类型。
- 日志写入 API 返回 `void`，**不依赖 `errno`**（且不得破坏调用者的 `errno`，见 §9），**不使用 `longjmp`**，丢弃类失败通过 `hplogc_get_stats` 计数暴露。
- 错误类型：
  - `HPLOGC_ERR_INVALID_ARG`：参数非法（NULL、越界枚举值、代码内配置值超出合法范围等）
  - `HPLOGC_ERR_NO_MEM`：内存分配失败
  - `HPLOGC_ERR_IO`：文件/设备 I/O 失败（init 时输出打开失败等）
  - `HPLOGC_ERR_CONFIG`：配置缺失或非法（含键/值非法、引用未定义名称、节顺序错误等）
  - `HPLOGC_ERR_STATE`：状态错误（未初始化、重复初始化、已 shutdown 后调用）
  - `HPLOGC_ERR_UNSUPPORTED`（v0.2 新增）：**值合法但当前构建不支持**——未注册 / 被裁剪的
    sink 类型（含 v0.1 的 socket 输出）、当前构建不支持的溢出策略等。此前这些情况与配置非法
    混用 `HPLOGC_ERR_CONFIG`，不利于调用方区分"改配置"与"换构建"（§0.2 B8/B9）
- **诊断信息**：init/配置错误的详情（配置文件名、行号、失败原因）统一输出至 stderr；本版本不提供错误字符串查询 API（`hplogc_strerror()` 列入 P2，§17）。

### 7.5 生命周期语义

| 场景 | 行为 |
|------|------|
| 未初始化时调用写入 API（`hplogc_log` 等） | 静默丢弃，不崩溃、不输出 |
| 未初始化时调用控制 API（`set_level`/`flush`/`sync`/`get_stats`） | 返回 `HPLOGC_ERR_STATE` |
| 重复调用 init（未 shutdown 再次 init） | 返回 `HPLOGC_ERR_STATE`，已有实例不受影响 |
| init 失败 | 库保持未初始化状态，已打开的资源全部释放，可安全重试 init |
| init 与其他 API 并发 | **不承诺线程安全**：调用方须保证 init 完成后其他线程才可使用日志 API |
| shutdown 重复调用 | 幂等，安全返回 |
| shutdown 内部行为 | 先**停止 §4.5 的热加载监视线程**（避免它与 shutdown 竞争替换 sink 实例），再 flush 并等待队列排空（至多 `shutdown_timeout_ms`），然后关闭所有输出、释放资源 |
| shutdown 之后调用写入 API | 安全返回/丢弃，不崩溃 |
| shutdown 与其他 API 并发 | **不承诺线程安全**，调用方需保证 shutdown 与日志写入不并发 |
| 未初始化时调用 `hplogc_sink_get_stats()` | 返回 `HPLOGC_ERR_STATE`；`sink` 或 `stats` 为 NULL 返回 `HPLOGC_ERR_INVALID_ARG` |
| `hplogc_sink_flush()` 对未声明缓冲语义的 sink | 返回 `HPLOGC_ERR_UNSUPPORTED`（非错误，不产生日志） |
| `hplogc_sink_find()` / `hplogc_sink_destroy()` 传入 NULL | 分别返回 NULL / 安全返回（不崩溃） |
| 热加载替换 sink 实例后继续使用旧句柄 | **禁止**：`hplogc_sink_find()` 返回的句柄仅用于即时观测，实例可能在热加载中被 destroy，长期持有会悬垂（§10.5） |

### 7.6 公共数据结构定义（规范性）

```c
/* ---- 版本宏、导出属性与 printf 格式属性（§7.1） ---- */
#define HPLOGC_VERSION_MAJOR 0
#define HPLOGC_VERSION_MINOR 2
#define HPLOGC_VERSION_PATCH 0
#define HPLOGC_VERSION_STRING "0.2.0"      /* 版本字符串字面量 */

/* HPLOGC_API：Windows → __declspec(dllexport)（构建 DLL 时定义 HPLOGC_BUILDING_DLL）
 * / __declspec(dllimport)（使用 DLL 时定义 HPLOGC_USE_DLL）；
 * GCC/Clang → __attribute__((visibility("default")))。展开式见头文件同名宏。 */
#define HPLOGC_API ...

/* HPLOGC_PRINTF(fmt_idx, va_idx)：printf 格式属性包装宏，用于编译期格式串检查；
 * 展开式见头文件同名宏。 */
#define HPLOGC_PRINTF(fmt_idx, va_idx) ...

/* ---- 错误码类型（§7.4） ---- */
typedef enum {
    HPLOGC_OK              =  0,  /* 成功 */
    HPLOGC_ERR_INVALID_ARG = -1,  /* 参数非法 */
    HPLOGC_ERR_NO_MEM      = -2,  /* 内存分配失败 */
    HPLOGC_ERR_IO          = -3,  /* 文件/设备 I/O 失败 */
    HPLOGC_ERR_CONFIG      = -4,  /* 配置缺失或非法 */
    HPLOGC_ERR_STATE       = -5,  /* 状态错误 */
    HPLOGC_ERR_UNSUPPORTED = -6   /* 值合法但当前构建不支持 */
} hplogc_error_t;

/* ---- 日志级别（见 §4.1） ---- */
typedef enum {
    HPLOGC_LEVEL_TRACE = 0,
    HPLOGC_LEVEL_DEBUG = 1,
    HPLOGC_LEVEL_INFO  = 2,
    HPLOGC_LEVEL_WARN  = 3,
    HPLOGC_LEVEL_ERROR = 4,
    HPLOGC_LEVEL_FATAL = 5,
    HPLOGC_LEVEL_OFF   = 6    /* 仅用于过滤阈值，不是合法的日志级别 */
} hplogc_level_t;

#define HPLOGC_LEVEL_ALL HPLOGC_LEVEL_TRACE   /* 别名，等价于最低级别 */

/* ---- 容量上限（编译期可通过 -D 覆盖） ---- */
#define HPLOGC_MAX_SINKS        16   /* 代码内配置 sink 实例数组上限；配置文件同此上限 */
#define HPLOGC_MAX_OUTPUTS      HPLOGC_MAX_SINKS  /* 已废弃别名（v0.1 名称），将在下一主版本移除 */
#define HPLOGC_MAX_SINK_TYPES   32   /* 可注册的 sink 类型上限（内置 + 自定义） */
#define HPLOGC_MAX_OPTIONS      64   /* 单个 sink 私有配置 / 全局配置键的数量上限 */
#define HPLOGC_MAX_FIELDS       16   /* 单条日志携带的结构化字段数量上限 */
#define HPLOGC_MAX_FIELD_STR_LEN 128 /* 单个字符串字段的入队拷贝上限（字节，不含 NUL） */
#define HPLOGC_MAX_RULES        64   /* 代码内配置 rule 数组上限；配置文件同此上限 */
#define HPLOGC_MAX_CATEGORIES   64   /* 内部 category 登记表容量 */
#define HPLOGC_MAX_NAME_LEN     64   /* format/sink 实例名/sink 类型名/category 名称长度（字节数，含结尾 NUL） */
#define HPLOGC_MAX_PATH_LEN     512  /* 文件路径长度（字节数，含结尾 NUL） */
#define HPLOGC_MAX_FMT_LEN      256  /* 格式模板/配置字符串值长度（字节数，含结尾 NUL） */

/* ---- 枚举 ---- */

/* 溢出策略（补齐 v0.1 头文件的缺失定义：该枚举在 v0.1 的 hplogc.h 中被
 * hplogc_config_t 引用却从未定义，导致头文件无法编译） */
typedef enum {
    HPLOGC_OVERFLOW_DISCARD   = 0,
    HPLOGC_OVERFLOW_OVERWRITE = 1,
    HPLOGC_OVERFLOW_WAIT      = 2
} hplogc_overflow_policy_t;

/* fsync 策略（同样为 v0.1 头文件的缺失定义，补齐） */
typedef enum {
    HPLOGC_CRASH_NONE     = 0,  /* 不 fsync */
    HPLOGC_CRASH_PERIODIC = 1,  /* 消费者按 flush_interval 周期 fsync（异步）；同步模式等效 shutdown */
    HPLOGC_CRASH_ENTRY    = 2,  /* 每条日志 fsync */
    HPLOGC_CRASH_SHUTDOWN = 3   /* 仅 shutdown 时 fsync */
} hplogc_crash_safety_t;

/* 注意：crash_safety 枚举值顺序 ≠ fsync 严格度顺序；
 * 严格度排序为 none < shutdown < periodic < entry（§9 与 per-sink fsync 取更严格者）。 */

typedef enum {
    HPLOGC_TS_REALTIME = 0,   /* CLOCK_REALTIME / 系统挂钟 */
    HPLOGC_TS_MONOTONIC = 1   /* 单调时钟，相对 init 的秒.微秒 */
} hplogc_timestamp_source_t;

typedef enum {
    HPLOGC_NEWLINE_AUTO = 0,  /* Unix=\n / Windows=\r\n */
    HPLOGC_NEWLINE_LF   = 1,
    HPLOGC_NEWLINE_CRLF = 2
} hplogc_newline_t;

/* 以下两个枚举不再作为公共结构体字段类型，而是 rollingfile sink 的
 * 配置键取值词表（rotate / time unit），值集合保持不变，§4.7.2） */
typedef enum {
    HPLOGC_ROTATE_NONE = 0,
    HPLOGC_ROTATE_SIZE = 1,
    HPLOGC_ROTATE_TIME = 2,
    HPLOGC_ROTATE_BOTH = 3
} hplogc_rotate_t;

typedef enum {
    HPLOGC_TU_HOUR = 0, HPLOGC_TU_DAY = 1,
    HPLOGC_TU_WEEK = 2, HPLOGC_TU_MONTH = 3
} hplogc_time_unit_t;

/* 结构化字段的值类型（v0.2 新增，§4.11） */
typedef enum {
    HPLOGC_V_INT = 0, HPLOGC_V_UINT = 1, HPLOGC_V_DOUBLE = 2,
    HPLOGC_V_STR = 3, HPLOGC_V_BOOL = 4
} hplogc_vtype_t;

/* ---- sink 能力位（v0.2 新增，§4.10.3） ---- */
#define HPLOGC_CAP_SYNC         (1u << 0)  /* 可在调用线程直接写 */
#define HPLOGC_CAP_ASYNC        (1u << 1)  /* 支持后台线程批量写 */
#define HPLOGC_CAP_STRUCT       (1u << 2)  /* 能消费结构化字段 */
#define HPLOGC_CAP_LINE_ATOMIC  (1u << 3)  /* 需要行级原子写保护 */
#define HPLOGC_CAP_FSYNC        (1u << 4)  /* 支持 fsync */

/* ---- 结构化字段（v0.2 新增，§4.11） ---- */
typedef struct {
    const char*    key;   /* 字段名；入队时随字段整体深拷贝，允许指向栈上缓冲（§4.11.2） */
    hplogc_vtype_t type;  /* 值类型 */
    union {
        long long          i;  /* HPLOGC_V_INT */
        unsigned long long u;  /* HPLOGC_V_UINT */
        double             d;  /* HPLOGC_V_DOUBLE */
        const char*        s;  /* HPLOGC_V_STR（入队时深拷贝） */
        int                b;  /* HPLOGC_V_BOOL */
    } v;
} hplogc_field_t;

/* 字段构造宏：展开为 C99 指定初始化器，因此要求 C99 或 C++20
 * （C++20 之前依赖编译器扩展；严格模式下请改用 hplogc_field_t 显式赋值） */
#define HPLOGC_FIELD_INT(k, x)    { (k), HPLOGC_V_INT,    { .i = (long long)(x) } }
#define HPLOGC_FIELD_UINT(k, x)   { (k), HPLOGC_V_UINT,   { .u = (unsigned long long)(x) } }
#define HPLOGC_FIELD_DOUBLE(k, x) { (k), HPLOGC_V_DOUBLE, { .d = (double)(x) } }
#define HPLOGC_FIELD_STR(k, x)    { (k), HPLOGC_V_STR,    { .s = (const char*)(x) } }
#define HPLOGC_FIELD_BOOL(k, x)   { (k), HPLOGC_V_BOOL,   { .b = (int)(x) } }

/* ---- 日志事件：sink 的唯一输入（§4.10） ----
 * 事件内所有指针均指向环形缓冲中的条目存储，仅在 emit 回调执行期间有效；
 * sink 需要长期留存时必须自行拷贝。 */
typedef struct {
    hplogc_level_t        level;
    const char*           category;       /* NULL/空串等价 "*" */
    uint64_t              ts_ns;          /* 纳秒；realtime 为墙上时钟，monotonic 为相对 init */
    uint32_t              pid;
    uint64_t              tid;
    const char*           file;           /* 未捕获时为 NULL */
    int                   line;
    const char*           func;           /* 未捕获时为 NULL */
    const char*           msg;            /* %msg 展开结果 */
    size_t                msg_len;
    const char*           formatted;      /* 按命中规则格式化后的完整日志行（不含结尾换行） */
    size_t                formatted_len;
    const hplogc_field_t* fields;         /* 无字段时为 NULL */
    size_t                field_count;
} hplogc_event_t;

/* ---- Sink 句柄与操作表（v0.2 新增，§4.10；替代 v0.1 的 hplogc_output_t） ---- */
struct hplogc_sink;                              /* 不透明（§15） */
typedef struct hplogc_sink hplogc_sink_t;

/* sink ops 的 ABI 版本：编码为 (主版本 << 16) | 次版本（§4.10.5） */
#define HPLOGC_SINK_ABI_VERSION_MAJOR 1u
#define HPLOGC_SINK_ABI_VERSION_MINOR 0u
#define HPLOGC_SINK_ABI_MAJOR_OF(v)  ((uint32_t)(v) >> 16)
#define HPLOGC_SINK_ABI_MINOR_OF(v)  ((uint32_t)(v) & 0xFFFFu)
#define HPLOGC_SINK_ABI_VERSION \
    ((HPLOGC_SINK_ABI_VERSION_MAJOR << 16) | HPLOGC_SINK_ABI_VERSION_MINOR)

typedef struct hplogc_sink_ops {
    const char* type;         /* 配置里的 type 名，如 "rollingfile" */
    uint32_t    abi_version;  /* 填 HPLOGC_SINK_ABI_VERSION */
    uint32_t    caps;         /* HPLOGC_CAP_* 组合 */
    size_t      priv_size;    /* 核心在 create 时分配并清零；0 = 无需私有数据 */

    int  (*configure )(struct hplogc_sink* sink, const char* key, const char* val);
    int  (*init      )(struct hplogc_sink* sink);            /* 全部 configure 之后调用 */
    int  (*start     )(struct hplogc_sink* sink);            /* 打开真实资源 */
    void (*emit      )(struct hplogc_sink* sink, const hplogc_event_t* ev);
    int  (*emit_batch)(struct hplogc_sink* sink, const hplogc_event_t* const* evs, size_t n);
    int  (*flush     )(struct hplogc_sink* sink);
    void (*destroy   )(struct hplogc_sink* sink);
    void (*reserved[4])(void);  /* ABI 扩展槽；必须全部为 NULL */
} hplogc_sink_ops_t;

/* 单个 sink 实例的运行统计（per-sink，不进入全局 hplogc_stats_t） */
typedef struct {
    unsigned long long written;
    unsigned long long dropped;
    unsigned long long failed;
    unsigned long long fields_dropped;
    unsigned long long bytes_written;
} hplogc_sink_stats_t;

/* ---- 通用键值对：承载无法用结构体字段穷举的配置项（§7.2 B5） ---- */
typedef struct {
    const char* key;    /* 与配置文件同名（空格分词风格） */
    const char* value;
} hplogc_kv_t;

/* ---- sink 实例的代码内配置（替代 v0.1 的 hplogc_output_t） ---- */
typedef struct {
    const char*       type;          /* "console" / "rollingfile" / 自定义类型 */
    const char*       name;          /* 实例名，供 rule 引用 */
    int               enabled;       /* 默认 1；0 时条目存在但不创建实例 */
    int               async;         /* -1 = 由 caps 决定（默认），0/1 = 强制 */
    const hplogc_kv_t* options;      /* 私有配置，键名见 §4.7.2 */
    size_t            option_count;  /* ≤ HPLOGC_MAX_OPTIONS */
} hplogc_sink_config_t;

/* ---- 路由规则（代码内配置；format 仅允许内置格式名） ---- */
typedef struct {
    const char*       category;      /* 精确名 / "name.*" / "*"；NULL/空串等价 "*" */
    hplogc_level_t    min_level;     /* 闭区间；min_level > max_level → 配置错误 */
    hplogc_level_t    max_level;     /* 闭区间 */
    const char*       format;        /* "minimal"/"standard"/"categorized"/"detailed"/"json" */
    const char* const* sinks;        /* sink 实例名数组（v0.1 名为 outputs） */
    size_t            sink_count;
} hplogc_rule_t;

/* ---- 代码内配置（公开 POD；须先经 hplogc_config_default() 初始化，见 §7.2/§7.3） ---- */
typedef struct {
    /* ABI 自检（首字段） */
    uint32_t             struct_size;  /* = sizeof(hplogc_config_t)；不一致时 init 返回 HPLOGC_ERR_CONFIG */

    /* 级别与格式 */
    hplogc_level_t       level;            /* 全局过滤阈值，默认 INFO */
    const char*          default_format;   /* 未命中规则的兜底 format 名；NULL = "standard" */
    const char* const*   default_sinks;    /* 兜底 sink 实例名数组；NULL/空 = 未命中规则时丢弃 */
    size_t               default_sink_count;
    /* sink 与路由 */
    const hplogc_sink_config_t* sinks;  size_t sink_count;  /* 数组由调用方持有，init 期间拷贝所需元数据 */
    const hplogc_rule_t*        rules;  size_t rule_count;
    /* 全局运行时键（替代 v0.1 中"仅配置文件可设"的键） */
    const hplogc_kv_t*   global_options;
    size_t               global_option_count;  /* ≤ HPLOGC_MAX_OPTIONS */
    /* 缓冲区 */
    size_t                       buffer_size;       /* 字节，默认 1MB，范围 4KB~1GB；< 2×max_log_length 时 init 自动提升 + 警告 */
    hplogc_overflow_policy_t     overflow_policy;   /* 默认 discard */
    /* 异步（构建 ASYNC=OFF 时以下三项忽略） */
    uint32_t             batch_size;          /* 默认 64 */
    uint32_t             flush_interval_ms;   /* 默认 100 */
    uint32_t             shutdown_timeout_ms; /* 默认 5000 */
    /* 高级 */
    int                  escape_injection;    /* 默认 1 */
    size_t               max_log_length;      /* 默认 4096，范围 256~65536；字段占用同一预算 */
    const char*          truncation_marker;   /* 默认 "...[TRUNCATED]" */
    hplogc_crash_safety_t crash_safety;       /* 默认 shutdown */
    int                  signal_safe;         /* 默认 0 */
} hplogc_config_t;
```

> 各结构体字段布局实现时可微调，但**语义、默认值、取值范围以本节为准**；同 Major 版本内 POD 结构只允许尾部追加字段。
>
> 代码内配置值非法 → init 返回 `HPLOGC_ERR_INVALID_ARG`（不 clamp，区别于配置文件的 clamp + 警告，§10.4）。
>
> **本节所有符号与 `include/hplogc.h` 逐一对齐**；头文件中任何新增/更名的符号都必须先落到本节，反之亦然（避免二次漂移）。

### 7.7 v0.1 → v0.2 代码内配置迁移对照（新增）

| v0.1 写法 | v0.2 写法 |
|-----------|-----------|
| `hplogc_output_t o = {.type = HPLOGC_OUT_CONSOLE, .stream = 1, .color = 1};` | `hplogc_sink_config_t s = {.type = "console", .name = "console_err", .enabled = 1, .async = -1};` + `options = { {"stream","stderr"}, {"color","true"} }` |
| `hplogc_output_t o = {.type = HPLOGC_OUT_FILE, .path = ..., .rotate = HPLOGC_ROTATE_SIZE, .max_size = ...};` | `hplogc_sink_config_t s = {.type = "rollingfile"（或别名 "file"）, .name = ...};` + `options = { {"path",...}, {"rotate","size"}, {"max size",...}, {"max files",...}, {"fsync","true"}, {"file perms","0644"} }` |
| `cfg.outputs` / `cfg.output_count` | `cfg.sinks` / `cfg.sink_count` |
| `cfg.default_outputs` / `cfg.default_output_count` | `cfg.default_sinks` / `cfg.default_sink_count` |
| `rule.outputs` / `rule.output_count` | `rule.sinks` / `rule.sink_count` |
| （无） | `cfg.struct_size`（由 `hplogc_config_default()` 填写，**必须**先调用该函数） |
| （无） | `cfg.global_options` / `cfg.global_option_count`（承载时区、`time format`、`newline` 等原"文件专属键"） |

### 7.8 差距与改善项处置清单（新增，评审追责用）

> 下表是本次"rd_v0.1.md ↔ include/hplogc.h（**v0.1 版**）双向对齐"的完整台账。左侧为发现的问题，
> 右侧为 v0.2 的处置方式与在地条款。表中每一项的问题描述在 `rd_v0.1.md` / v0.1 版的
> `include/hplogc.h` 中均真实存在，其处置结论已被本版条款取代（两个原文件本身不被修改）。

| # | 问题 | 性质 | v0.2 处置 | 落点 |
|---|------|------|-----------|------|
| 1 | `hplogc_overflow_policy_t` 未定义，却被 `hplogc_config_t` 引用 | **编译阻塞** | 补齐定义 | §7.6 |
| 2 | `hplogc_crash_safety_t` 未定义，却被 `hplogc_config_t` 引用 | **编译阻塞** | 补齐定义 | §7.6 |
| 3 | 残留异构声明 `hpulogc_file_sync_policy` / `hpulogc_sink_t` / `hpulogc_file_sink_set_sync_policy()`（拼写错误且引用未定义类型） | **编译阻塞** + 前缀违规 | 删除；其"文件 write 同步策略"语义由 rollingfile sink 的 `HPLOGC_CAP_LINE_ATOMIC` 承接 | §4.10.3 |
| 4 | 编译期裁剪宏误用枚举符号名（`#if` 中被求值为 0 → 静默不裁剪） | 静默错误 | 仅接受数值 / `HPLOGC_CTL_*`；**CMake 白名单字符串映射是唯一有效防线**（头文件的范围 `#error` 无法拦截：枚举符号求值为 0 落在 `[0,6]` 内）；CMake 空值必须映射为 0，不得生成空宏定义 | §4.8、§7.3、§11 |
| 5 | per-category 级别清除用 `(hplogc_level_t)-1` 哨兵，规范从未定义 | 契约不一致 | 改为显式 `hplogc_clear_level_for_category()` | §7.3 |
| 6 | `HPLOGC_LEVEL_ALL` 无规范出处 | 规范缺失 | 纳入规范（等价于 `HPLOGC_LEVEL_TRACE`） | §7.6 |
| 7 | 缺少级别预检 API（附录 A.1-3 建议采纳却被搁置） | 能力缺口 | 新增 `hplogc_level_enabled()` 与 `HPLOGC_xxx_ENABLED()` 宏 | §7.3 |
| 8 | "只能通过配置文件设置"的键在 min 预设（`INI=OFF`）下不可达 | 结构性缺陷 | 统一由 `hplogc_kv_t` 全局键值表达 | §7.2、§7.6 |
| 9 | 公开 POD 无 ABI 尺寸校验手段 | 演进风险 | 首字段新增 `struct_size` | §7.6、§15 |
| 10 | 缺少级别名互转 API（配置文件解析与 `%level` 输出重复实现） | 能力缺口 | 新增 `hplogc_level_name()` / `hplogc_level_parse()` | §7.3 |
| 11 | socket 输出"骨架预留 + 定义即失败"，网络类输出又无处表达 | 语义混乱 | 删除 socket 类型；网络/检索型输出统一为**自定义 sink**（§4.10.5） | §4.7、§10.4 |
| 12 | 部分 API 的 Doxygen 未覆盖 errno 保持、未初始化行为、init/shutdown 并发免责、指针生命周期 | 文档缺口 | 全量补齐并纳入 Doxygen 门禁 | include/hplogc.h、§11 |
| 13 | `-pedantic` 下零可变参数调用告警（经 `HPLOGC_VA_ARGS` 中转所致） | 可移植性 | 便捷宏内部直接展开 `__VA_ARGS__`，`HPLOGC_VA_ARGS` 降级为兼容层 | §7.3 |
| 14 | 注释语言冲突：`AGENTS.md`（中文）vs `AG.md` §2.1 与本文 §15（英文） | **规范冲突（待仲裁）** | 本版沿用**中文**（与现存 `include/hplogc.h` 一致），列为待仲裁项 | 本节注 |

> **待仲裁项（第 14 项）**：现有 `include/hplogc.h` 全篇为中文 Doxygen，AGENTS.md 亦要求中文；
> 但 `AG.md` §2.1 与本文 §15 写的是"注释仅英文"。v0.2 头文件与新增章节按**中文**推进，
> 若仲裁结论为英文，需在评审通过后统一改写（纯注释变更，不影响 ABI）。

---

## 8. 性能目标

| 指标 | 目标值 | 条件 |
|------|--------|------|
| 单条延迟（无竞争） | < 100 ns | SPSC + 无锁 + 64B 消息 |
| P99 延迟（MPSC 竞争，无锁） | < 1 μs | MPSC + **无锁** + 8 生产者 |
| P99 延迟（MPSC 竞争，有锁） | < 10 μs | MPSC + **有锁** + 8 生产者 |
| 吞吐量（峰值） | > 500,000 logs/sec | 64B/条，异步模式，消费者线程批量同步写 |
| 基线内存 | < 32 KB（不含缓冲区） | `min` 版本 |
| 缓冲区范围 | 4 KB ~ 1 GB（可配置） | 默认 1 MB |

- CI 中纳入性能基准线，吞吐量下降 > 10% 触发告警。

---

## 9. 安全性与健壮性

| 类别 | 要求 |
|------|------|
| 日志注入防护 | 在格式化阶段仅转义**用户消息（`%msg`）**中的换行符和 ANSI 转义序列（可配置开关）；不影响库自身生成的颜色/格式序列；json 格式仅执行 JSON 转义、跳过注入转义（§12） |
| 格式化安全 | API 强制 `fmt` 与参数分离，禁止用户字符串作格式串 |
| 单条长度限制 | `max log length` 作用于**格式化后的完整日志行**（含换行），默认 4 KB、范围 256~65536 字节；超出截断并附加 `truncation marker`，截断后总长（含 marker）仍不超过 `max log length`。**生产者线程的消息体渲染上限同为 `max log length`**（超出部分静默丢弃、不加 marker；整行截断与 marker 在格式化阶段执行，§4.9④）。init 校验 `buffer size ≥ 2 × max log length`（含元数据余量），不足时自动提升并警告（§4.3），保证 wait 策略不存在"单条日志永不入队"的死锁边界 |
| 信号处理 | 普通 API 不可在 signal handler 中调用；`hplogc_log_signal_safe()` **始终为 async-signal-safe** 并保存/恢复 `errno`；`signal safe = false`（默认）时调用该函数静默丢弃（无未定义行为），`true` 时 init 预分配信号通道资源（见 §7.3），文档明确其受限子集 |
| fork() 安全 | `pthread_atfork` 处理（Windows 无 fork，该配置忽略），子进程行为由 `fork behavior` 配置：**reinit**（child handler 仅设置原子脏标记——async-signal-safe；子进程首次调用任意需要运行态的日志 API 时惰性重建：文件型配置重读配置文件，代码内配置使用 init 时保存的副本；重建失败该条丢弃并限频警告）/ **disable**（子进程中日志调用静默丢弃）/ **inherit**（继承父进程内部状态，不安全，仅调试）。**子进程不继承父进程的任何线程**（fork 语义）：§4.5 的热加载监视线程与异步消费者线程在子进程中均不存在——`reinit` 的惰性重建须按当前构建重建它们（`HPLOGC_ENABLE_HOT_RELOAD=OFF` 时不重建监视线程），`disable` / `inherit` 下子进程不再具备热加载能力 |
| 崩溃安全 | `crash safety` 为 fsync 策略：none / periodic（周期 = `flush interval`）/ entry（每条）/ shutdown；**fsync 严格度排序：none < shutdown < periodic < entry**；有效策略 = 全局 `crash_safety` 与各 sink 实例的 `fsync` 配置**取更严格者**（sink `fsync=true` ⇒ 该 sink 至少 entry 级）。仅声明 `HPLOGC_CAP_FSYNC` 的 sink 参与 fsync |
| errno 保持 | 正常路径与错误路径的日志写入 API（含 `hplogc_log_signal_safe`）**保存并恢复调用线程的 `errno`**；sink 回调须遵守同一约定 |
| 并发正确性 | 所有跨线程共享的计数与状态标志（`dropped`/`overwritten`/`throttled`/`accepted`/`written`/`fields_dropped`、per-sink 的 `written`/`dropped`/`failed`、periodic fsync 的周期计数、shutdown 状态等）必须通过 §2.2 原子后端实现，**禁止依赖普通整型的"事实原子性"** |
| sink 回调安全边界 | sink 回调禁止调用除 `hplogc_sink_*` 之外的库 API（防重入）；不得缓存 `hplogc_event_t` 内的指针（仅在回调期间有效）；除首次私有初始化外不得动态分配内存（见下"内存安全"）。回调异常不得传播到调用方线程 |
| 多进程边界 | 仅承诺**单进程内多线程**安全；不承诺多进程写同一日志文件及并发轮转安全（如需跨进程请外部协调，见附录 A.2-4） |
| 文件 I/O 失败 | **init 时**：任一 file 类 sink 打开失败 → 启动失败，返回 `HPLOGC_ERR_IO`（fail-fast）；**运行期**：写入失败时尝试重新打开文件一次，仍失败则该条**在该 sink 上**丢弃——计入该 sink 的 `failed`；仅当该条日志的**全部目标 sink 均失败**时才计入全局 `dropped`（多 sink 记账以 §12.4 为准），并输出限频 stderr 警告（每 5 秒至多 1 条），后续每条日志重试重开。磁盘满/只读遵循同一策略 |
| 内存安全 | 初始化时预分配；稳态（日志写入热路径）不调用 `malloc`/`free`；允许的例外仅限：首次登记新 category、per-thread 渲染缓冲（含字段区）首次使用及按需增长（上限 `max log length`）、sink 首次私有初始化。`emit`/`emit_batch`/`flush`/`destroy` 回调内**不得**分配内存 |
| 字符串安全 | 所有操作使用 `snprintf` 等长度受限函数 |

---

## 10. 配置文件规范

### 10.1 设计原则

1. **zlog 词法基线（规范性）**：配置文件词法与解析器实现参考 zlog（HardySimpson/zlog）`conf.c` 的实现：**自研行式解析器**（无外部 INI 库）、仅 `#` 注释、引号感知、反斜杠续行、解析错误必须带文件名与行号。节集合与 rules 语义为 hplogc 扩展；与 zlog 的其余差异记录于附录 A。
2. **词法规则（规范性）**：
   - 键值以**第一个 `=`** 分割；value 为 `=` 之后至行尾的内容（去首尾空白）；若 value 以成对双引号包裹则去引号，**引号内的 `#`、`,`、`=` 均不具特殊含义**（不作注释、不作行内分隔符，允许含空格的值，如 `time format`、`truncation marker`）；
   - 行首（首个非空白字符）为 `#` 的行是注释行；行尾 `#` 及其后内容视为注释（引号外）；`;` **不是**注释字符；
   - 行尾独立反斜杠 `\` 为续行符，下一行内容拼接至当前行后再解析（错误行号按首行计）；
   - **键名区分大小写**，采用 zlog 风格空格分词（如 `strict init`、`buffer size`）；级别名、布尔值、枚举**值**大小写不敏感；
   - 尺寸后缀（不区分大小写，zlog 语义）：`1k`=1000、`1kb`=1024、`1m`=10⁶、`1mb`=2²⁰、`1g`=10⁹、`1gb`=2³⁰；无后缀为字节数；
   - `[outputs]` 行内参数为逗号分隔的 `key=value`，值可用双引号包裹（含空格、`,`、`=` 的值）；
   - 物理行长度 ≤ 1024 字节（续行各段分别计），超出启动失败。
   - 配置文件键名与 §7.6 代码内配置字段一一对应（如 `buffer size` ↔ `buffer_size`）；代码内配置不受配置文件词法约束。
3. **配置文件中只包含运行时可配置项**。
4. **编译期决策通过 `[build]` 节只读展示**，用户不可通过配置文件更改。
5. 被裁剪功能的配置节在解析时**静默跳过**；裁剪节内部的单个键亦静默忽略。
6. 配置项值**超出合法范围**时，裁剪到最近有效值 + stderr 警告。
7. 配置项值**合法但当前构建不支持**时（fail-fast），**启动失败**并返回 `HPLOGC_ERR_UNSUPPORTED`，不做静默降级（见 §10.4）。典型场景：引用了未注册 / 被裁剪的 sink 类型、当前构建不支持的溢出策略。
8. 长度与数量限制：配置字符串值 ≤ `HPLOGC_MAX_FMT_LEN`（256 字节，**含结尾 NUL**），路径 ≤ `HPLOGC_MAX_PATH_LEN`（512 字节，**含结尾 NUL**），sink 实例数量 ≤ `HPLOGC_MAX_SINKS`（16），rules 条数 ≤ `HPLOGC_MAX_RULES`（64），超出启动失败（`HPLOGC_ERR_CONFIG`）。（`HPLOGC_MAX_OUTPUTS` 仅为 v0.1 兼容别名，规范性条款一律使用 `HPLOGC_MAX_SINKS`，见 §7.6）

### 10.2 节结构总览

> **节出现顺序（规范性，zlog 顺序校验语义）**：**出现的节**必须遵循下表自上而下的相对顺序（`[build]` → `[global]` → `[formats]` → `[outputs]` → `[buffer]` → `[async]` → `[throttle]` → `[rules]` → `[advanced]`）；顺序回退或出现未知节名 → 启动失败（`HPLOGC_ERR_CONFIG`），报行号。
>
> **所有节均可省略**（省略 = 使用该节全部键的默认值）；空文件或仅含注释/空行的配置文件合法。`[build]` 为可选节（存在时忽略其值，见 §10.4）。
>
> 配置文件未定义任何 sink 实例且未设置 `default sinks`（配置文件键名仍为 `default outputs`，见 §10.3）时，输出一次 stderr 提示（避免全静默误配）。

| 节名 | 可读性 | 内容 | 裁剪行为 |
|------|--------|------|----------|
| `[build]` | 只读 | 构建版本、并发模式、功能可用性 | 可选；始终解析（值被忽略） |
| `[global]` | 读写 | 语法严格性、级别、兜底 format/outputs、时间、编码、换行、信号触发 | 始终解析 |
| `[formats]` | 读写 | 命名格式模板 | 始终解析 |
| `[outputs]` | 读写 | **sink 实例定义**（一行 = 一个 sink 实例 + 其私有配置） | 始终解析；被裁剪的**参数**静默忽略，被引用的 **type 未注册 / 被裁剪**则启动失败（`HPLOGC_ERR_UNSUPPORTED`，§4.7.1） |
| `[buffer]` | 读写 | 缓冲区大小、溢出策略 | 始终解析 |
| `[async]` | 读写 | 批量/刷新参数 | `HPLOGC_ENABLE_ASYNC=OFF` 时跳过 |
| `[throttle]` | 读写 | 限流/采样参数 | `HPLOGC_ENABLE_THROTTLE=OFF` 时跳过 |
| `[rules]` | 读写 | 路由规则 | 始终解析 |
| `[advanced]` | 读写 | 安全/健壮性/统计 | 始终解析 |

### 10.3 完整配置模板

```ini
# ============================================================================
# hplogc 日志配置文件
# ============================================================================
# 语法：INI 风格，词法与 zlog conf 一致（§10.1）。
#   - 仅 # 为注释字符：# 起始的行为注释行；行尾 # 后为注释（双引号内除外）。
#   - 行尾独立反斜杠 \ 为续行符。
#   - key = value；value 为 = 后整行（可含空格），可用成对双引号包裹；
#     引号内 # 、 , 、= 均不具特殊含义。
#   - 键名区分大小写（空格分词）；级别名/布尔/枚举值大小写不敏感。
#   - 尺寸后缀：1k=1000, 1kb=1024, 1m=10^6, 1mb=2^20, 1g=10^9, 1gb=2^30。
#   - 出现的节须保持相对顺序（§10.2）；所有节均可省略（省略 = 默认值）。
# ============================================================================


# ============================================================================
# [build] 构建信息（可选；只读展示，值与实际构建不一致时忽略并警告）
# ============================================================================
[build]

build version = full
concurrency = mpsc
lockfree = false
has async = true
has color = true
has rotate = true
has hot reload = true
has category = true
has throttle = false
has ini = true


# ============================================================================
# [global] 全局运行时配置
# ============================================================================
[global]

# 语法严格性：true 时未知键启动失败；false 时未知键降级为 stderr 警告并忽略
# （其余错误——语法错误、缺值、重复定义、引用未定义名称——无论取值均启动失败）
# 仅在 init 时生效；热加载解析时未知键按当前生效值处理（§10.5）
strict init = true

# 过滤级别，低于此级别的日志被丢弃：TRACE ~ FATAL
level = INFO

# 未命中任何规则时的兜底 format 名（引用 [formats] 中已定义的名称）
default format = standard

# 未命中任何规则时的兜底 sinks（逗号分隔，引用 [outputs] 中已定义的 sink 实例名）
# 键名保持 `default outputs` 以兼容 v0.1；新增别名 `default sinks`，二者同义
# 未命中规则且该值为空时，该条日志被丢弃
default outputs = console_out

# 时区：local（本地时区）或 utc
timezone = local

# 时间戳源：
#   realtime  - CLOCK_REALTIME，真实时间，受 time format 控制显示格式
#   monotonic - 单调时钟，%time 固定输出相对 init 的 "秒.微秒"，time format 忽略
timestamp source = realtime

# 时间格式（strftime 风格，扩展占位符：%f 微秒、%F3 毫秒）
time format = %Y-%m-%d %H:%M:%S.%f

# 字符编码（当前仅支持 utf-8）
encoding = utf-8

# 运行时是否捕获源码位置（编译期 HPLOGC_ENABLE_SOURCE_LOC=OFF 时此键忽略）
capture source loc = true

# 换行符：auto（跟随平台，Unix=\n / Windows=\r\n）、lf（统一 \n）、crlf（统一 \r\n）
newline = auto

# 进程/线程 ID 显示格式：decimal（十进制）、hex（十六进制）、none（不显示）
pid format = decimal
tid format = decimal

# 配置文件变更检查间隔（秒），0 表示禁用热加载
# Linux 优先 inotify，此值为 inotify 不可用时的回退轮询间隔
hot reload interval = 5

# 是否由库注册 SIGHUP 处理器触发配置热加载检查（默认关闭，避免与宿主应用信号处理冲突）
# 仅 HPLOGC_ENABLE_HOT_RELOAD=ON 时有效；handler 内仅设置原子标志，实际检查由库后台执行；
# 与 `hot reload interval = 0`（禁用轮询）独立：信号为显式触发，仍生效。
# 注册失败（如宿主已安装 SIGHUP handler）时输出警告并继续（热加载仅靠 inotify/轮询）。
signal reload = false


# ============================================================================
# [formats] 日志格式模板
# ============================================================================
# 模板值为 = 后整行，建议用双引号包裹。
# 可用占位符：%level %time %pid %tid %file %line %func %msg %category %n %%
# ============================================================================
[formats]

minimal     = "%level: %msg%n"
standard    = "%time [%level] %msg%n"
categorized = "%time [%level] [%category] %msg%n"
detailed    = "%time [%level] [pid:%pid tid:%tid] [%file:%line %func] [%category] %msg%n"
json        = "{\"time\":\"%time\",\"level\":\"%level\",\"category\":\"%category\",\"pid\":%pid,\"tid\":%tid,\"file\":\"%file\",\"line\":%line,\"msg\":\"%msg\"}%n"


# ============================================================================
# [outputs] sink 实例定义（v0.2：一个条目 = 一个 sink 实例）
# ============================================================================
# 语法（与 v0.1 一致，仅把首个参数位置的类型名从受限枚举放开为注册 sink 名）：
#   <实例名> = <type>, key=value, ...          # 首个参数是 sink type
#   <实例名> = type=<type>, key=value, ...     # 等价显式写法
#
# 行内参数为逗号分隔的 key=value，值可用双引号包裹（含空格、逗号、等号的值）。
#
# 通用键（核心消费）：type、enabled、async
#   enabled=false  → 条目保留但不创建实例（便于用注释之外的方式临时下线）
#   async          → true/false；仅对带 HPLOGC_CAP_ASYNC 的 sink 生效，否则告警忽略
#
# console:      stream=stdout|stderr, color=true|false
# rollingfile:  path, rotate=none|size|time|both, max size, time unit=hour|day|week|month,
#               max files, fsync, symlink latest, rotate naming, file perms, dir perms
#               （类型别名 "file" 仅把 rotate 的默认值设为 none，显式 rotate 以显式值为准，
#                §4.7.1；保留该别名用于兼容 v0.1 配置）
#
# 自定义 sink（经 hplogc_sink_register 注册）示例：
#   audit_sink = mykafka, brokers=127.0.0.1:9092, topic=audit
# 未注册 / 被裁剪的类型 → 启动失败 HPLOGC_ERR_UNSUPPORTED（fail-fast，不静默降级）。
# 注意：依赖第三方客户端的 sink 不得内置进库（§4.10.5），只能由应用侧注册。
# ============================================================================
[outputs]

console_out  = console, stream=stdout, color=true
console_err  = console, stream=stderr, color=true

main_log     = file, path=/var/log/myapp/app.log, rotate=size, max size=100mb, max files=10, file perms=0644, dir perms=0755, symlink latest=true
error_log    = file, path=/var/log/myapp/error.log, rotate=time, time unit=day, max files=30, file perms=0644
debug_log    = file, path=/var/log/myapp/debug.log, rotate=both, max size=50mb, time unit=day, max files=7
audit_log    = file, path=/var/log/myapp/audit.log, rotate=none, fsync=true, file perms=0600

# 自定义 sink 示例（须已在代码中注册该类型；enabled=false 表示暂不上线）：
# audit_sink  = mykafka, enabled=false, brokers=127.0.0.1:9092, topic=audit

# Windows 示例：
# main_log_win = file, path="C:\Logs\MyApp\app.log", rotate=size, max size=100mb, max files=10


# ============================================================================
# [buffer] 环形缓冲区运行时参数（仅 init 时生效，热加载忽略变更）
# ============================================================================
# 注意：concurrency 与 lockfree 由编译期构建决定，不可在此设置
# ============================================================================
[buffer]

buffer size = 1mb
overflow policy = discard


# ============================================================================
# [async] 异步写入参数（HPLOGC_ENABLE_ASYNC=OFF 时此节被跳过）
# ============================================================================
# batch size        批量提交条数（1~65535）
# flush interval    未攒满 batch 时的强制提交间隔（毫秒，1~60000）
# shutdown timeout  shutdown 等待异步写入完成的超时（毫秒，0 = 无限等待）
# ============================================================================
[async]

batch size = 64
flush interval = 100
shutdown timeout = 5000


# ============================================================================
# [throttle] 限流与采样（HPLOGC_ENABLE_THROTTLE=OFF 时此节被跳过）
# ============================================================================
# 执行顺序：先限流，后采样（§4.9③）。
# global rate limit    全局令牌桶速率（条/秒），0 表示不限流；桶容量 = burst size
# per category rate limit  每个 category 独立令牌桶速率（条/秒），0 表示不限流；
#                      与全局限流独立检查，任一超限即丢弃
# sampling rate        采样率（0.0 ~ 1.0），确定性 1/N 采样：
#                      N = round(1/采样率)，全局共享原子计数器，每 N 条放行 1 条；
#                      0.0 = 全部丢弃，1.0 = 全部放行
# burst size           令牌桶容量（条），全局桶与分类桶共用
# ============================================================================
[throttle]

global rate limit = 0
per category rate limit = 0
sampling rate = 1.0
burst size = 100


# ============================================================================
# [rules] 日志路由规则
# ============================================================================
# 语法：category.level = format_name, sink_name1, sink_name2, ...
#       （第二项起为 [outputs] 中定义的 sink 实例名，一条日志可同时投递到多个 sink）
#
# selector 解析（规范性）：以最后一个 '.' 分割为 category 与 level；
# level 部分不得包含 '.'（否则报错 + 行号）。category 层级以 '.' 分隔。
#
# category 匹配：
#   *          匹配所有分类
#   name       精确匹配分类名
#   name.*     匹配 name 及其子分类（以 '.' 分隔的层级前缀，含 name 自身）
#
# level 匹配：
#   *          所有级别
#   LEVEL      精确匹配（如 TRACE）
#   A~B        范围匹配，含端点（如 ERROR~FATAL 表示 ERROR 及以上）；
#              min > max 为配置错误（§10.4）
#
# 匹配语义（规范性，zlog 风格，纯顺序）：
#   - 从上到下扫描，第一条 category 匹配且 level 落在范围内的规则生效
#   - 仅生效一条规则；该条日志按此规则的 outputs 各输出一份
#   - 兜底规则（category 为 *）必须放在具体规则之后，否则其后的规则永远不会命中
#   - 无任何命中规则时，按 [global] 的 default format + default outputs 兜底；
#     default outputs 为空时该日志被丢弃
#
# format/output 仅允许引用 [formats]/[outputs] 中已定义的名称，不支持内联定义
# ============================================================================
[rules]

app.debug.* = detailed, debug_log
security.* = json, audit_log, console_err
db.WARN~FATAL = standard, main_log
net.ERROR~FATAL = detailed, error_log, console_err
*.ERROR~FATAL = detailed, error_log
app.* = categorized, main_log
*.* = standard, console_out


# ============================================================================
# [advanced] 高级选项
# ============================================================================
[advanced]

# 日志注入防护：转义用户消息中的换行符和 ANSI 序列（不影响库自身的颜色序列；
# json 格式下不生效，见 §12）
escape injection = true

# 单条日志最大长度（字节，256~65536），超出后按 truncation marker 截断
# （同时约束生产者线程的消息体渲染上限，§9）
max log length = 4096
truncation marker = ...[TRUNCATED]

# fork() 后子进程行为：reinit / disable / inherit（含义见 §9）
fork behavior = reinit

# 是否允许 hplogc_log_signal_safe() 实际输出（见 §7.3/§9）
signal safe = false

# fsync 策略：none / periodic / entry / shutdown（含义见 §9）
crash safety = shutdown

# 统计信息输出间隔（秒），0 表示禁用（min 构建下忽略）
stats interval = 0

# 统计输出目标：stderr 或 file（使用 stats file 路径）
stats output = stderr
# stats file = /var/log/myapp/stats.log
```

#### 10.3.1 rules 命中示例（以上方模板规则顺序为准）

| 日志（category, level） | 命中规则 | 输出 |
|--------------------------|----------|------|
| (app.debug.conn, INFO) | `app.debug.*` | debug_log |
| (app.http, INFO) | `app.*` | main_log |
| (app.http, ERROR) | `*.ERROR~FATAL` | error_log |
| (db.query, WARN) | `db.WARN~FATAL` | main_log |
| (db.query, ERROR) | `db.WARN~FATAL` | main_log（db 规则在前，优先命中） |
| (net.sock, ERROR) | `net.ERROR~FATAL` | error_log + console_err |
| (security.login, TRACE) | `security.*` | audit_log + console_err |
| (ui.render, INFO) | `*.*` | console_out |

### 10.4 配置解析器行为约束

| 场景 | 行为 |
|------|------|
| 配置文件不存在 | 返回 `HPLOGC_ERR_CONFIG`，不 fallback |
| 配置文件为非普通文件（目录/FIFO/设备等） | 启动失败，返回 `HPLOGC_ERR_CONFIG` |
| 语法错误（节头格式、缺 `=`、缺值、物理行超 1024 字节等） | 返回错误 + 行号，不 fallback |
| 节出现顺序违反 §10.2 规范顺序，或出现未知节名 | 启动失败，返回 `HPLOGC_ERR_CONFIG`，报行号 |
| 未知键 | `strict init = true`：启动失败（`HPLOGC_ERR_CONFIG`，报行号）；`false`：stderr 警告并忽略该键 |
| 引用未定义 format/sink（含 default format、default outputs/default sinks、stats file 引用） | 启动失败，报错 |
| 引用未注册 / 被当前构建裁剪的 sink 类型 | 启动失败，返回 `HPLOGC_ERR_UNSUPPORTED`（fail-fast，不静默降级；§4.7.1） |
| `enabled = false` 的 sink 条目 | 合法：保留条目但不创建实例（便于在不删除条目的前提下临时下线） |
| `async = true` 但目标 sink 不具备 `HPLOGC_CAP_ASYNC` | 非错误：告警并忽略该键 |
| 单个 sink 的私有参数数量 > `HPLOGC_MAX_OPTIONS` | 启动失败，返回 `HPLOGC_ERR_CONFIG` |
| 在 rules 中内联定义 format/sink | 不支持；视同引用未定义名称，启动失败 |
| `[formats]`/`[outputs]` 中重复定义同名键 | 启动失败（`HPLOGC_ERR_CONFIG`），报重复行号 |
| `[rules]` 中的同名键 | 允许（即多条规则），按出现顺序生效 |
| `[global]`/`[buffer]`/`[async]`/`[throttle]`/`[advanced]` 中重复键 | 后值覆盖前值 + stderr 警告 |
| `default outputs` 与 `default sinks` 同时出现 | 二者同义（§10.3）；同义键**不视为冲突错误**——按"后出现的覆盖先出现的"处理（沿用本表重复键规则）+ stderr 警告。二者均出现且仅其一为空时，以最后出现者为准（空 = 未命中规则时丢弃） |
| rule 的 min level > max level | 启动失败，返回 `HPLOGC_ERR_CONFIG` |
| 两个及以上 file 类 sink 的路径（规范化后）相同 | 启动失败，返回 `HPLOGC_ERR_CONFIG` |
| `rotate naming` 模板既不含 `{index}` 也不含 `{timestamp}` | 启动失败，返回 `HPLOGC_ERR_CONFIG` |
| sink 实例数量 > `HPLOGC_MAX_SINKS` 或 rules 条数 > `HPLOGC_MAX_RULES` | 启动失败，返回 `HPLOGC_ERR_CONFIG` |
| `buffer size < 2 × max log length` | **非错误**：自动提升 buffer size（下限 4KB）+ stderr 警告（§4.3） |
| 配置值超出合法范围 | 裁剪到最近有效值 + stderr 警告 |
| 配置值合法但当前构建不支持 | **启动失败**，返回 `HPLOGC_ERR_UNSUPPORTED`（如：无锁构建 `overflow policy = wait`、无锁 MPSC 构建 `overflow policy = overwrite`、引用未注册/被裁剪的 sink 类型、**同步构建下引用了不具备 `HPLOGC_CAP_SYNC` 的 sink 类型**（§4.10.3 能力位矩阵）） |
| `[formats]` 模板含未知占位符或行尾孤立 `%` | 启动失败，返回 `HPLOGC_ERR_CONFIG`，报出格式名与行号（§12.1） |
| 配置项属于已裁剪功能 | 静默跳过整个配置节（节内的单个键亦静默忽略） |
| 热加载时新配置非法 | 保留旧配置，错误详情输出至 stderr |
| `[build]` 节 | 可选：缺失合法；存在时其键值与实际构建信息比对，不一致 → 忽略写入值，输出警告 |

> `strict init` 仅影响"未知键"的处理级别（与 zlog 的软/硬失败分级对齐，但范围更收敛）；语法错误、缺值、节顺序错误、重复定义、引用未定义名称无论 `strict init` 取值均启动失败。

### 10.5 热加载行为明细（规范性）

| 配置项 | 热加载行为 |
|--------|-----------|
| `global.level`、`timezone`、`time format`、`newline`、`pid format`/`tid format` | 立即生效 |
| `global.default format` / `global.default outputs` | 立即生效（引用校验失败则整体回滚） |
| `[formats]` | 立即生效（整表原子替换） |
| `[rules]` | 立即生效（整表原子替换） |
| `[outputs]` | 原子替换（sink 实例维度）：**type 与全部私有键值参数均未变更**的 file 类 sink **复用已打开 fd，不关闭重开**；新增实例按 `create → configure* → start` 创建，任一失败则整体回滚旧配置；被移除的实例先 `flush` 再 `destroy`；console 类实例无持久 fd，随原子替换直接生效。**不得**让 emit 回调与被销毁的实例并发（§4.10.2） |
| `[throttle]` | 立即生效 |
| `async.batch size` / `async.flush interval` | 立即生效 |
| `async.shutdown timeout` | 立即生效 |
| `advanced.escape injection` / `max log length` / `truncation marker` / `crash safety` | 立即生效 |
| `advanced.fork behavior` | 立即生效（仅影响其后的 fork） |
| `advanced.signal safe` | 立即生效（影响其后的 `hplogc_log_signal_safe` 调用是否输出） |
| `advanced.stats interval` / `stats output` / `stats file` | 立即生效 |
| `[buffer]`（buffer size / overflow policy） | **忽略变更**（重建缓冲区代价大），输出一次提示日志 |
| `global.hot reload interval` | 立即生效（下一次检查按新间隔） |
| `global.capture source loc`、`timestamp source` | 立即生效 |
| `global.strict init` | 仅 init 时生效；热加载解析新配置时，未知键按当前生效的 `strict init` 值处理 |
| `global.signal reload` | 立即生效（true 时注册 SIGHUP handler，注册失败输出警告） |

> 热加载的替换必须是原子的：任一环节失败（如新 sink 实例 start 失败、引用未定义名称）则整体回滚到旧配置，保证不存在半新半旧状态。
>
> **自定义 sink 类型不在热加载范围内**：类型的注册/注销只在 init 之前生效（§7.3），热加载只能替换**实例**，不能引入新的 type——引用新 type 的配置在热加载时同样触发整体回滚。

---

## 11. 构建与分发

| 项目 | 要求 |
|------|------|
| 语言标准 | `HPLOGC_C_STANDARD`（99 / 11，默认 99）驱动 `CMAKE_C_STANDARD`，并联动原子后端自动探测（§4.3） |
| 原子后端 | `HPLOGC_ATOMIC_BACKEND = auto \| stdatomic \| gcc-atomic \| gcc-sync \| msvc-interlocked`（默认 auto；`gcc-atomic`/`gcc-sync` 仅 Linux/macOS，`msvc-interlocked` 仅 Windows，交叉指定为 CMake 配置错误） |
| 环形缓冲实现 | `HPLOGC_LOCKFREE`（OFF/ON）二选一编译 `ringbuf_locked.c` / `ringbuf_lockfree.c`（§3.3） |
| 内置 sink 集合 | `HPLOGC_SINKS`（CMake 列表，**默认 `console;rollingfile;null`**，§4.8）。`null` 为契约参考实现与测试替身，默认注册以保证 §13.1/§16.1 的测试与 §7.3 的审计下线方案可用；`min` 预设覆盖为 `console;rollingfile`（§5），`syslog` 需显式追加（P2，§4.7.1/§17）。未列入的内置类型不注册 → 运行时等同未注册；列表中出现无法识别的类型视为 CMake 配置错误 |
| 编译期裁剪级别 | CMake 接收 `HPLOGC_COMPILE_TIME_LEVEL = TRACE\|DEBUG\|INFO\|WARN\|ERROR\|FATAL\|OFF` 字符串，**映射为数值**后定义该宏；空值（不裁剪）映射为 `0`，**不得生成空宏定义**（空宏会使头文件的 `#if` 报预处理语法错误）。CMake 侧须拒绝白名单外的输入——头文件侧**无法**拦截枚举符号名（§7.3） |
| 警告级别 | GCC/Clang: `-Wall -Wextra`；MSVC: `/W4` |
| Sanitizer | `HPLOGC_SANITIZER = none \| address \| thread \| undefined`（默认 none；address 与 undefined 可叠加，thread 与 address 互斥——同时指定为 CMake 配置错误，替代原一键式选项） |
| Doxygen | 公共 API 全量 Doxygen 文档化；CI 以 `WARN_AS_ERROR = YES` 运行 doxygen 作为门禁 |
| 静态分析 | 集成 clang-tidy / cppcheck |
| 代码格式化 | `.clang-format` 统一风格 |
| 交叉编译 | 支持 CMake Toolchain File |
| 安装 | `make install` 安装头文件（`include/hplogc.h`，v0.2 起为正式公共 API，已随库安装）、库、`hplogcConfig.cmake` |
| 包管理 | 提供 vcpkg / Conan recipe（P2） |

---

## 12. 日志内容规范

| 属性 | 说明 |
|------|------|
| 时间戳源 | `CLOCK_REALTIME`（默认）或 `CLOCK_MONOTONIC`（`%time` 固定输出相对 init 的"秒.微秒"，`time format` 忽略） |
| 时区 | 本地（默认）或 UTC |
| 编码 | 统一 UTF-8；Windows 下 `wchar_t` → UTF-8 |
| 源码位置 | `__FILE__`/`__LINE__`/`__func__`；编译期 `HPLOGC_ENABLE_SOURCE_LOC=OFF` 或运行时 `capture source loc = false` 时：`%file`/`%func` 展开为空串，**`%line` 展开为 `0`**（保证 `json` 模板中的 `"line":%line` 始终产出合法 JSON，§12.1） |
| 换行符 | `%n` 默认跟随平台：`\n`（Unix）或 `\r\n`（Windows）；可通过 `global.newline` 强制 `lf` / `crlf` / `auto` |
| JSON 转义 | `json` 格式对 `%msg`/`%file`/`%category`/`%func` 的展开结果自动做 JSON 字符串转义（`"` `\` 与控制字符），**仅执行 JSON 转义、跳过 `escape injection` 注入转义**（避免双重转义），且不受该开关影响；`json` 输出中 pid/tid **强制十进制**（忽略 `pid format`/`tid format` 的 hex/none 设置，保证 JSON 合法） |
| 结构化字段 | 内置文本格式（含 `json` 字符串模板）**不渲染** fields，`%msg` 与占位符集合不因携带字段而发生变化；**只有声明 `HPLOGC_CAP_STRUCT` 的 sink 才会消费** `hplogc_event_t::fields`（§4.11.1） |

#### 12.1 占位符解析规则（规范性）

**占位符集合**固定为 `%level %time %pid %tid %file %line %func %msg %category %n %%`（§10.3），不做扩展（附录 A.1-7 的格式占位符扩展仍为待决策项）。

| 规则 | 定义 |
|------|------|
| `%%` | 展开为字面 `%` |
| **未知占位符**（`%` 后跟集合外字符，含行尾孤立 `%`） | **格式模板非法**：init 时校验即失败，返回 `HPLOGC_ERR_CONFIG` 并报出 `[formats]` 中的格式名与行号。**不**做"原样输出"降级——静默输出 `%x` 属配置错误被掩盖，违背 fail-fast（§10.1） |
| `%file` / `%line` / `%func` | 编译期 `HPLOGC_ENABLE_SOURCE_LOC=OFF` 或运行时 `capture source loc = false` 时展开为空串（`%line` 展开为 `0`） |
| `%pid` / `%tid` | 受 `pid format` / `tid format` 控制（见下） |

**`pid format` / `tid format` 的展开语义（规范性）**：

| 取值 | `%pid` / `%tid` 展开 |
|------|----------------------|
| `decimal` | 十进制数字（默认） |
| `hex` | 十六进制，带 `0x` 前缀 |
| `none` | **空串**——仅替换占位符自身，**不删除**模板中的相邻字面文本（模板 `[pid:%pid]` 在 `none` 时输出 `[pid:]`） |

> json 格式下 pid/tid **强制十进制**（忽略 hex / none，保证 JSON 合法，见本节 JSON 转义行）。

#### 12.2 `time format` 的扩展占位符语法（规范性）

在 `strftime` 语义之外，hplogc 定义两个扩展占位符：

| 占位符 | 语义 | 合法性 |
|--------|------|--------|
| `%f` | 微秒，**固定 6 位**，零填充 | 合法 |
| `%F<n>` | 秒的小数部分，`<n>` 为小数位宽度，**取 1~6**，零填充（`%F3` = 毫秒 3 位） | `n` 为 1~6 时合法 |
| `%F`（不带数字）、`%F0`、`%F7` 及以上 | — | **非法**：配置错误，启动失败并报行号 |

> 其余 `%X` 一律按 `strftime` 语义解释；`strftime` 亦不识别者按 §12.1 的未知占位符处理。
> `timestamp source = monotonic` 时 `%time` 固定输出"秒.微秒"，`time format` 整体忽略（§10.3）。

#### 12.3 "json 格式"的判定（规范性）

- 判定依据是 `[formats]` 中的**格式名**，而非模板内容：仅当格式名**恰为 `json`**（键名区分大小写，§10.1）时，该格式启用以下三项行为：
  1. 对 `%msg` / `%file` / `%category` / `%func` 的展开结果执行 **JSON 字符串转义**；
  2. **跳过** `escape injection` 注入转义（避免双重转义），且不受该开关影响；
  3. `%pid` / `%tid` **强制十进制**。
- 用户自定义的其它格式名（如 `myjson`）**不**享有上述行为，仍按普通文本格式处理（受 `escape injection` 约束）。
- 理由：按名字判定可静态确定、零运行时开销且实现唯一；按模板内容嗅探会在不同实现间产生分叉。

#### 12.4 多 sink 投递的记账规则（规范性）

一条日志命中规则后投递到 N 个 sink（N ≥ 1）。**全局计数器按条计（同一条日志至多 +1）**，**per-sink 计数器按 sink 各计**：

| 情形 | 全局 `written` | 全局 `dropped` | 成功 sink 的 `written` | 失败 sink 的 `failed` |
|------|----------------|----------------|------------------------|----------------------|
| 至少 1 个 sink 成功（含部分成功） | +1 | — | 各 +1 | 失败者各 +1 |
| N 个 sink 全部写失败 | — | +1 | — | 各 +1 |
| 投递**之前**即被丢弃（队列溢出 §4.3、限流/采样 §4.9③、未命中规则且无兜底） | — | +1（限流计 `throttled`） | — | — |
| 投递阶段被 sink 背压丢弃（§4.10.4） | 视其余 sink 结果 | 若因此全部未写出则 +1 | — | 该 sink 的 `dropped` +1 |

- **不重复计数**：全局 `dropped` 与 per-sink `dropped` / `failed` 分属不同层级，**不要求**二者数值相加等于任何总量；唯一硬约束是**同一条日志在全局层面至多 +1**。
- 队列溢出与限流发生在投递之前，**不产生任何 per-sink 计数**（该条从未到达 sink）。

---

## 13. 测试要求

### 13.1 单元测试

- 覆盖：环形缓冲区（有锁/无锁两套实现分别覆盖）、原子后端（每个后端的行为等价性测试）、格式化器、配置解析器、文件写入器、轮转器、**sink 层**（注册表查重/ABI 版本校验、`console`/`rollingfile`/`null` 各类型等价性、**自定义 sink 的全生命周期顺序断言**）、**字段序列化与预算裁剪**。
- **sink 契约测试（新增，必测）**：以 `init` 晚于全部 `configure` 为断言点——先用私有键把字段置为非默认值再 `start`，验证 `init` 未覆盖已配置值（防止 §4.10.2 的"默认值覆盖配置"回归）。
- 框架：CTest 集成或自实现 ASSERT 宏。
- 覆盖率：模块行覆盖率 ≥ 90%（平台相关 glue 代码与 `#ifdef` 独占分支可豁免，豁免范围在覆盖率报告中显式列出）。

### 13.2 集成测试

- 覆盖所有功能组合和边界场景：
  - 缓冲区满/空、配置错误/缺失、磁盘满/只读
  - 溢出策略（discard/overwrite/wait）行为与 `dropped`/`overwritten`/`throttled` 计数正确性
  - 高频并发、fork() 子进程（reinit 惰性重建/disable/inherit）、热加载非法配置、热加载 output 增删、SIGHUP 触发热加载、json 格式输出合法性校验
  - **多 sink 场景**：一条日志按规则命中多个 sink 的并发投递、`enabled=false` 条目、未知 type 的 fail-fast（`HPLOGC_ERR_UNSUPPORTED`）、per-sink `written`/`dropped`/`failed` 计数正确性、sink 背压不得阻塞管线
  - **热加载回滚**：新 sink 实例 start 失败时整体回滚（不得出现半新半旧）、未变更实例 fd 复用、**引用新 type 的热加载必须回滚**
  - **结构化字段**：字段超量（> `HPLOGC_MAX_FIELDS`）与超长（> `HPLOGC_MAX_FIELD_STR_LEN`）丢弃计数、字段占用 `max_log_length` 预算、`hplogc_log_signal_safe()` 不携带字段
  - **自定义 sink**：注册/查询/注销、ABI 版本不匹配被拒、`emit` 与 `emit_batch` 至少一个缺失被拒、ABI 主/次版本抽取正确性（§4.10.5）
  - **裁剪与深拷贝**：`HPLOGC_AUDIT()` / `HPLOGC_METRIC()` 在任意 `HPLOGC_COMPILE_TIME_LEVEL` 取值下均保留（§7.3）；字段名 `key` 与字符串值传入栈上缓冲后，sink 侧仍能读到正确内容（§4.11.2）；`fields_dropped` 对三种成因（超数量 / 超长度截断 / 超预算）均计数（§4.11.2）

### 13.3 性能测试

- 基准测试：延迟（ns）、吞吐量（logs/sec）。
- **sink 间接层回归门禁（新增）**：vtable 调用相对 v0.1 直接调用，在同等配置下**吞吐回退不得 > 5%**；超过则该项视为回归，须通过 §4.10.4 的"按 sink 分组攒批"优化到阈值内。
- 压力测试：≥ 24h 高并发，验证无泄漏/无竞争/无死锁（**发布前执行**：夜间/手动触发，不进常规 CI）。
- 对比报告：hplogc vs printf vs zlog vs spdlog（C++ 参照）。

### 13.4 模糊测试

- INI 解析器 Fuzz 测试（AFL++ / libFuzzer），CI 中运行。

### 13.5 自动化

- CMake + CTest 一键运行。
- CI 矩阵（各平台维度对齐）：
  - Ubuntu（GCC/Clang）× {x86_64, ARM64} × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC}
  - macOS（Apple Clang）× {x86_64, ARM64} × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC}（Phase 3）
  - Windows：MSVC × x64 × C99（C11 任务于 VS2019 16.8+ 镜像上启用）× {有锁, 无锁} × {SPSC, MPSC}；MinGW-w64 × x64 × {C99, C11} × {有锁, 无锁} × {SPSC, MPSC}
  - Sanitizer：Linux 每次 CI 运行 ASan + UBSan（代表性构建组合）；TSan 夜间任务
- CI 按平台分阶段启用（§16）；Phase 2/3 验收含"公共代码零修改"检查（§3.3）。
- Valgrind / Dr. Memory 零泄漏验证。
- gcov / lcov 覆盖率报告 + 阈值门禁。

---

## 14. 文档与示例

| 文档 | 内容 |
|------|------|
| `README.md` | 简介、快速开始、构建说明 |
| `API.md` | 完整 API 参考（Doxygen） |
| `BUILD.md` | 各平台编译指南 |
| `PORTING.md` | 新平台移植 checklist（以 §3.3 平台接口契约为准） |
| `examples/` | 最小示例、异步示例、多 Category 示例、自定义格式示例 |

---

## 15. 工程约束汇总

| 类别 | 约束 |
|------|------|
| ABI 稳定性 | 同 Major 版本内 ABI 兼容；`hplogc_config_t`、`hplogc_build_info_t`、`hplogc_stats_t` 等公开 POD 结构**只允许尾部追加字段**；运行时内部句柄/实现对象使用 opaque pointer |
| 向后兼容 | 配置文件格式同 Major 版本内向后兼容 |
| 编码与命名 | 源文件 UTF-8 无 BOM（带 `/* -*- coding: utf-8 -*- */` 声明头，当含非 ASCII 时）；注释**中文**（AGENTS.md 与现存 `hplogc.h`；与 `AG.md` §2.1 "注释仅英文"冲突，**待仲裁，§7.8 第 14 项**）；标识符 snake_case、常量 `SCREAMING_SNAKE_CASE`（AGENTS.md）；公共 API 全量 Doxygen，CI 门禁见 §11 |
| 自定义 sink 开发约束 | 新增 sink 必须遵守 §4.10 的 ops 契约与调用顺序（`init` 晚于全部 `configure`）、ABI 版本填写与 `reserved` 清零、回调内禁止分配内存与调用非 `hplogc_sink_*` API；详见 `PORTING.md` checklist |
| Git 提交 | 建议 Conventional Commits |
| 版本管理 | 语义化版本 2.0 |
| 代码风格 | clang-format 统一 |

---

## 16. 实施阶段计划（规范性）

实现严格按以下三个阶段推进，阶段顺序固定，不得跳过或并行（文档、CI 脚本等辅助工作除外）。所有平台差异必须收敛到 §3.3 的平台层与原子后端；后续阶段对公共代码（`include/` 与 `src/` 非平台目录）**零修改**是各阶段验收条件。

### 16.1 Phase 1 — Linux（全功能）

范围：在 Linux 上完成本规范的全部功能实现，并为 Windows/macOS 预留全部扩展接口。

1. **冻结平台接口契约**：定义并评审 §3.3 所列平台接口头（同步原语、线程、时间、文件/目录、路径、watcher、tid、导出宏），作为 win32/darwin 的移植依据；契约一经冻结，Phase 2/3 不得要求修改（如需修改视为 Phase 1 缺陷）。
2. **实现 POSIX/Linux 平台层**：`src/platform/posix/`（与 macOS 共享）与 `src/platform/linux/`（inotify watcher 等）全部实现。
3. **原子层**：实现 `stdatomic`、`gcc-atomic`、`gcc-sync` 三个后端及行为等价性单元测试；CI 以 `-std=c99` 与 `-std=c11` 双标准分别构建，验证后端自动选择正确。
4. **环形缓冲**：有锁与无锁两套实现全部完成；Linux 上验证 SPSC/MPSC × 有锁/无锁 全矩阵（含各溢出策略可用性约束，§4.3）。
5. **CMake 骨架**：平台检测（`WIN32`/`APPLE`/其余）、`HPLOGC_LOCKFREE` 选择 ringbuf 源文件（§3.3）、`HPLOGC_ATOMIC_BACKEND`、`HPLOGC_C_STANDARD`、`HPLOGC_ENABLE_*` 裁剪、四版本预设（§5）。
6. **预留**：`src/platform/win32/`、`src/platform/darwin/` 仅含接口头文件与占位说明，不参与编译。
7. **sink 层**：实现 `src/sink/sink_registry.c`（注册表、内置注册、类型查找，init 后只读）+ `sink_console.c` + `sink_rollingfile.c`（文件写、轮转、备份清理、命名模板、`.latest` 软链、fsync）+ `sink_null.c`（契约参考实现与测试替身）；由 `HPLOGC_SINKS` CMake 列表决定参与编译的类型集合。
8. **结构化字段通道**：完成生产者线程的字段深拷贝入队、预算裁剪与计数（§4.11.2），以及 §7.6 的 `hplogc_field_t` / `hplogc_event_t` 载荷布局。

**完成标准（DoD）**：§13 全部测试在 Linux 矩阵（GCC/Clang × x86_64/ARM64 × C99/C11 × 有锁/无锁 × SPSC/MPSC）通过；**sink 契约测试全绿（含"init 晚于全部 configure"断言、热加载回滚、per-sink 计数正确性）**；ASan/TSan/UBSan 零报告；§8 性能目标、§13.3 的 sink 间接层回归门禁与 §5 min 体积验证达标。

### 16.2 Phase 2 — Windows

1. **实现 `src/platform/win32/`**：`SRWLOCK`/`CONDITION_VARIABLE`（有锁原语契约实现）、高精度时钟、`\` 路径与 UTF-8↔UTF-16 转换、文件 API 差异（`_commit` 对应 fsync；轮转清理用 `FindFirstFile` 系实现目录扫描语义）、无 inotify → 按 `hot reload interval` 轮询 mtime/大小。
2. **实现 `src/atomic/atomic_msvc.h`**：C99+ 全部语言标准统一使用 MSVC `Interlocked*`（MinGW-w64 亦通过 `intrin.h` 使用同一族，保证 Windows 后端唯一，见 §4.3）。
3. **MSVC 适配**：`/W4` 零警告、`##__VA_ARGS__` 兼容策略落地（§7.3）、rollingfile sink 的 `file perms`/`dir perms`/`symlink latest` 忽略并输出警告（§4.7.2）、默认换行 `\r\n`（§12）、控制台启用 VT 处理（`ENABLE_VIRTUAL_TERMINAL_PROCESSING`）以支持 ANSI 彩色（§4.7.1）。
4. **sink 层的 Windows 适配**：`rollingfile` sink 通过 §3.3 平台接口契约使用 Windows 文件 API（`_commit` 对应 fsync、`FindFirstFile` 目录扫描语义），sink 层本身不得出现 `#ifdef _WIN32`。
5. **完成 Windows CI**（MSVC/MinGW × x64）。

**DoD**：Windows 矩阵测试全绿；内存检查（MSVC ASan / Dr. Memory）零泄漏；相对 Phase 1，公共代码零修改（diff 验收）。

### 16.3 Phase 3 — macOS

1. **实现/验证 `src/platform/darwin/`**：复用 POSIX 共享层，补充 darwin 专属（`pthread_threadid_np`、kqueue watcher 或轮询回退、时钟兼容）。
2. **Apple Clang 双标准验证**：C11 `<stdatomic.h>` 与 C99 `__atomic_*` 两条原子路径均通过测试（禁止 `OSAtomic*`，§4.3）。
3. **双架构**：x86_64 + ARM64（Apple Silicon），universal binary 可选。
4. **完成 macOS CI**。

**DoD**：macOS 矩阵测试全绿、sanitizer 零报告；相对 Phase 1/2，公共代码零修改。

### 16.4 阶段与优先级映射

P0/P1 功能项在 Phase 1（Linux）完成；Windows/macOS 专属适配分别随 Phase 2/3 交付；P2 增强项中，彩色输出的 Linux 部分随 Phase 1 交付、Windows 控制台（VT）适配随 Phase 2 交付，其余 P2 项在 Phase 1 完成接口预留，其平台差异行为随所在阶段交付。映射总览见 §17。

---

## 17. 需求优先级总览

> 优先级仅表示实现排期顺序，不改变本规范中已定义的行为与默认值（如彩色输出的 Linux 部分随 Phase 1 交付、Windows VT 适配随 Phase 2 交付，已实现部分受 `HPLOGC_ENABLE_COLOR` 裁剪控制，选项默认 ON）。实现阶段划分见 §16：P0/P1 项于 Phase 1（Linux）交付；P2 项的平台差异随 Phase 2/3 交付。

| 优先级 | 项目 |
|--------|------|
| **P0（必须）** | 纯 C 实现、跨平台、线程安全、6 级日志 + `OFF` 阈值（共 7 个枚举值）、环形缓冲（有锁）、**内置 console / rollingfile sink 输出**、INI 配置、单元测试、CMake 构建、API 签名、导出符号、错误处理、编译警告、Sanitizer、安装规则、示例、配置解析器行为约束 |
| **P1（重要）** | 无锁队列、MPSC 模式、批量提交、日志轮转、溢出策略、集成测试、性能测试、性能量化目标、信号处理/fork 安全、配置热加载、多 Category、时间戳/时区、模糊测试、ABI 稳定性、C99/C11 双标准原子实现、**编译期裁剪体系（`HPLOGC_ENABLE_*` 等）、四版本构建预设**、**多 sink 体系（注册表 + 自定义注册 ABI + 内置 sink 清单）**、**结构化字段通道**、**级别预检 API（`hplogc_level_enabled`）与 `hplogc_clear_level_for_category`** |
| **P2（增强）** | 彩色输出（Linux 部分随 Phase 1，Windows VT 适配随 Phase 2）、格式定制、包管理器、静态分析、文档生成、移植指南、`hplogc_strerror` 错误描述 API、**内置 `syslog` sink（可选，`HPLOGC_SINKS` 控制）** |

> 依赖第三方客户端的 sink（kafka / loki / elasticsearch / clickhouse / s3 / mysql 等）**不在本项目的任何优先级内**：
> 按 §4.10.5 一律由应用侧以自定义 sink 形式实现。

---

## 附录 A：zlog 实现对比与待决策项

> **调研基线**：HardySimpson/zlog master 分支（截至 2026-09-18 提交，`src/version.h` 标识 1.2.18；最新 release 亦为 1.2.18，2024-07-03 发布，修复 CVE-2024-22857）。master 含未发布改动：可选后台消费者线程（`use_writer_thread`/`fifo_size`）、rwlock 写者饥饿让路修复、`zlog_init_from_string` 等。
>
> **性质**：本附录为决策辅助材料，**不构成规范性要求**（正文已明确写入的 zlog 词法采纳项除外，见 §10.1）。
>
> **使用方式**：A.1 各项由文档维护者逐项决策——采纳项转入正文规范性条款，不采纳项移入 A.2，状态列同步更新。

### A.1 待决策项（zlog 做法可能更优）

| # | 主题 | zlog（master）做法 | 本文档现行方案 | 对比与建议 | 状态 |
|---|------|--------------------|----------------|------------|------|
| 1 | 时间戳按秒缓存 | 每个时间占位符独立缓存槽，strftime 结果在同一秒内复用（每秒每占位符至多一次 `localtime_r`+`strftime`），热路径时间格式化近乎零成本 | 未规定（`%time` 每条重新渲染） | **建议采纳**为实现要求；代价仅每占位符一个缓存槽 + 每秒一次刷新，对 §8 延迟目标收益明显 | 待决策 |
| 2 | per-category 预计算路由 + 级别位图 | category 登记时预计算命中规则列表与级别位图；每条日志先 O(1) 位测试短路，再遍历命中规则；热加载用双缓冲 update/commit/rollback | §4.9 步骤②⑥ 仅定义语义，未规定实现结构 | **建议采纳**为实现要求；与本文 category 登记表、热加载原子替换天然契合 | 待决策 |
| 3 | level_enabled 检查 API | `zlog_level_enabled(cat, level)` + `zlog_fatal_enabled(cat)` 等宏 + printf format 属性（编译期检查格式串） | 无对应 API | **已采纳（v0.2）**：`hplogc_level_enabled(level, category)`、`HPLOGC_xxx_ENABLED(cat)` 宏、`hplogc_log*` 已带 printf format 属性 | ✅ 已落地 §7.3、P1 |
| 4 | 配置校验 CLI | 附带 `zlog-chk-conf` 独立校验工具（CI 可用） | 无 | **建议采纳**（P2）：`hplogc_chk_conf`，退出码报告错误行号 | 待决策 |
| 5 | 外部轮转检测 | 静态文件输出每条 `stat` 比对 inode/dev，检测外部 logrotate 换文件后重开（WatchedFileHandler 语义） | 仅"运行期写失败时重开一次"（§9） | zlog 每条 `stat` 有可测开销（与 §4.6 O(1) 检查要求冲突）；建议**节流采纳**（每 N 条 + 写失败时比对 inode）或保持现状 | 待决策 |
| 6 | `!LEVEL` 取反匹配 | 规则级别支持裸 `LEVEL`（≥ 语义）、`=LEVEL`（精确）、`!LEVEL`（除该级别外）、`*` | `*` / `LEVEL`（精确）/ `A~B`（范围） | `!LEVEL` 语法成本低、表达力补充，**建议采纳**；注意 zlog 裸 LEVEL 为 ≥ 语义，与本文"精确"不同，若采纳需保持本文语义并标注 | 待决策 |
| 7 | 格式占位符扩展 | `%d`/`%g`（本地/UTC 时间）、`%ms`/`%us`、`%k`（系统级 tid）、`%H`（主机名）、`%F`/`%f`（全/短文件名）、printf 宽度/精度修饰符（`%-20.30c`） | 9 个占位符 + `%f` 微秒扩展（§10.3/§12） | **建议至少采纳** `%g`（UTC 时间）与宽度/精度修饰符；`%k`/`%H`/短文件名低优先；本文 `%f` 与 zlog `%ms`/`%us` 语义重叠，若引入别名须在 §12 定义共存规则 | 待决策 |
| 8 | 用户自定义级别 | `[levels]` 节自定义级别（`NAME = int[, syslog_level]`），内置级别含 NOTICE | 固定 7 级枚举（§4.1/§7.6，ABI 冻结） | **建议不引入**（级别集稳定性优先、API 简单）；如确需再评估运行时注册制 | 待决策 |
| 9 | MDC | 每线程 MDC（put/get/remove + `%M(key)` 占位符） | 无 | **建议不引入**（异步模式下 MDC 属生产者线程上下文，跨线程传递语义复杂；请求级上下文建议调用方自行并入 `%msg`） | 待决策 |
| 10 | record / syslog / pipe 输出 | `$record` 回调、`>syslog[,facility]`（级别映射）、`\|pipe`（popen） | 仅 console/file；socket 骨架预留（定义即启动失败） | **已裁决（v0.2）**：三者在新的 sink 体系下统一形态——① `$record` 回调等价**自定义 sink 的 `emit`**，通过公开注册 ABI 达成，采纳；② syslog 采纳为**可选内置 sink**（P2，`HPLOGC_SINKS` 控制，仅 POSIX）；③ pipe **不采纳**（零依赖定位、popen 子进程管理复杂），有需要者以自定义 sink 实现 | ✅ 已裁决 §4.7.1、§4.10.5 |
| 11 | 轮转归档命名 | `#r` 滚动重编号（logrotate 风格级联）/ `#s` 序号递增，支持零填充宽度（`#2s`），归档路径可含时间占位符 | `{base}`/`{timestamp}`/`{index}` 模板 + `max files` 清理 + `.latest` 软链（§4.6） | 本文模板更灵活可控；`rotate naming` 仅用 `{index}`（不含 `{timestamp}`）即等效 `#s` 序号风格，§4.6 已说明该等效性，无需新增机制 | 待决策 |
| 12 | 双映射环形缓冲 | 异步消费者用 `memfd_create` + 两次 `MAP_FIXED` 双映射环形页，跨边界记录单次连续 memcpy；per-record RESERVED/COMMITTED 原子标志（Linux 专属，zlog 因 memfd 未支持 Windows） | 未规定无锁实现细节 | 可作为 Phase 1 Linux 无锁 SPSC 的**可选优化**；必须保留 macOS/Windows 通用回退（尾部分段拷贝）；因跨平台实现分叉，列为可选优化而非规范 | 待决策 |
| 13 | shutdown/热加载线程协同 | flush/退出经队列内命令记录 + 消费者完成握手；生产者 per-thread 状态引用计数延迟释放（消费者处理完最后一条后才释放）；注：其退出路径存在忙等缺陷，近期多个修复围绕这些缝隙 | §7.5/§10.5 定义了行为，未规定机制 | **建议采纳**为实现要求（命令记录 + 条件变量握手 + 引用计数延迟释放；用条件变量而非 zlog 式忙等） | 待决策 |
| 14 | per-thread 缓冲增长策略 | 每线程缓冲 1KB 起步、按增量增长（全局可配 `buffer min`/`buffer max`，默认上限 2MB），不缩减，稳态零 malloc；`buffer max=0` 为无上限 | `max log length`（默认 4KB）硬截断（§9）；本节所指 v0.2 为原 rd 修订记录中的次版本编号 | **建议组合**：预分配 + 按需增长至 `max log length` 上限即截断（与 §9 渲染上限条款方向一致，兼得稳态零 malloc 与硬上限；避免 zlog 无上限模式的内存风险）；采纳与否仅影响 per-thread 缓冲增长策略实现 | 待决策 |
| 15 | 可组合 sink：vtable + 注册表（源自 clog 骨架） | `clog_sink_ops_t` + `clog_sink_register()`；一个配置 section = 一个 sink 实例 + 一条路由 | v0.1：固定 `hplogc_output_t` 枚举 + 扁平结构体 | **已采纳（本文档 v0.2）**：引入 `hplogc_sink_ops_t` 并纳入公开 ABI；配置端保留 `[outputs]` 行内语法，仅把首个参数的 type 从受限枚举放开为注册 sink 名，从而兼顾可扩展性与配置向后兼容 | ✅ 已落地 §4.7、§4.10 |
| 16 | 结构化字段通道（源自 clog `clog_field_t`） | `CLOG_FIELD_*` 构造宏 + `%M(key)` 占位符；**借用调用方指针**，异步路径要求字面量或长生命周期内存 | v0.1 无字段通道 | **已采纳（本文档 v0.2）**，但生命周期策略**刻意反向**：环形缓冲跨线程，字段**必须深拷贝**入队，调用方无需保证指针生命周期（理由见 A.2-6） | ✅ 已落地 §4.11 |

### A.2 明确不采纳项（本文方案更优，记录结论）

| # | 主题 | zlog 做法 | 本文档方案（保持） | 不采纳理由 |
|---|------|-----------|--------------------|------------|
| 1 | 全局 rwlock 热路径 | 每条日志持有全局 env 读锁（含格式化与 `write()`）；为修写者饥饿引入让路协议（作者自测损耗 ~1.5% 吞吐，仍有 `sched_yield` 自旋） | 生产者热路径无全局锁（环形缓冲 + 消费者线程，§4.2/§4.4） | zlog 同步模型的扩展性天花板；本文架构天然规避该锁 |
| 2 | 值校验宽松 | 数值不可解析/越界静默变 0；布尔"非 false 即 true"（拼写错误静默生效）；重复键静默 last-wins | 超范围 clamp + 警告；布尔严格校验；重复键检测（`[formats]`/`[outputs]` 报错、其余警告覆盖，§10.4） | 静默错误配置违背 fail-fast 原则（§10.1） |
| 3 | reload 全量重建 | reload 重建全部 rule 并关闭重开所有 fd（未变更文件也重开，存在窗口） | 未变更 output 复用 fd，仅增删/参数变更项重建（§10.5） | 减少重开窗口与开销；语义已定义 |
| 4 | POSIX 跨进程"锁文件" | rotater 以 `open`/`close` 充当跨进程锁（POSIX 上未实际发出任何锁系统调用，仅 Windows `CreateFile` 独占有效） | 不承诺多进程安全（§9） | 定位单进程多线程；zlog 该机制在 POSIX 上实为无效保护，不值得效仿 |
| 5 | 通用键 value 截断 | 通用键 value 以 `sscanf %s` 截断于首个空白（仅 `default format` 特例取整行） | value 统一取 `=` 后整行（引号可包裹，§10.1） | 本文为严格超集：允许含空格的值（`time format`、`truncation marker`、路径）；zlog 风格配置在本文规则下解析结果一致 |
| 6 | 字段借用调用方指针（源自 clog） | `CLOG_FIELD_STR` 不深拷贝：异步路径下必须指向字面量或长生命周期内存，绑错即悬垂 | 字段在生产者线程**深拷贝**入环形缓冲，调用方无需保证指针生命周期（§4.11.2） | 借用指针与"环形缓冲 + 消费者线程"模型根本冲突：调用方无法得知事件何时被消费完成。改用深拷贝 + `max log length` 预算约束换取安全性与易用性，代价是可预期的入队拷贝量与必须定义丢弃/截断规则 |

---

## 修订记录

> **版本号说明**：`rd_v0.1.md` 内部的修订记录把"基于 v0.1 全文评审的 30 项修订"标为
> **v0.2（2026-09-19）**。为免与本文档版本混淆，`rd_v0.1.md` 中的该条目在本文改标为
> **"v0.1 评审条目（2026-09-19）"**；下文的 **v0.2（2026-09-28）** 才指本文档版本。

### v0.2 四次评审修订（2026-09-28）—— 消除第三轮引入的二次矛盾

对第三轮新增条款做回归比对，修复其**与既有条款打架**的 2 处矛盾，并补齐 5 处遗漏登记。

1. **§9 文件 I/O 失败**（原与 §12.4 相反）：运行期写失败改为"计入**该 sink 的 `failed`**；
   仅当该条日志的**全部目标 sink 均失败**时才计入全局 `dropped`"。此前 §9 写的是
   "计入全局 dropped + 该 sink failed"，与 §12.4"部分成功即计全局 written"直接冲突。
2. **§12 源码位置行**（原与 §12.1 相反）：明确 `%file`/`%func` 展开为空串、**`%line` 展开为 `0`**。
   此前写"三者均展开为空串"，会使 json 模板的 `"line":%line` 产出非法 JSON，且违背 §12.1。
3. §10.4 补登记两个新增失败场景：同步构建下引用无 `HPLOGC_CAP_SYNC` 的 sink 类型
   → `HPLOGC_ERR_UNSUPPORTED`（§4.10.3）；`[formats]` 模板含未知占位符 → `HPLOGC_ERR_CONFIG`（§12.1）。
4. §9 fork 安全补：**子进程不继承任何线程**，§4.5 的监视线程与异步消费者线程在子进程中不存在，
   `reinit` 须按构建重建（`HOT_RELOAD=OFF` 时不重建监视线程）。
5. §7.5 shutdown 补顺序：须**先停止热加载监视线程**，再 flush / 销毁 sink，避免二者竞争。
6. §0.1 / §7.8：加"命名约定"注——文中 `include/hplogc.h` 自 v0.2 起指转正后的正式头文件，
   涉及 v0.1 原文件处标注"（v0.1 版）"，消除重命名造成的指代漂移。
7. `include/hplogc.h` 第 17-18 行：整理删除提案注记后残留的不自然断行。

---

### v0.2 三次评审修订（2026-09-28）—— 消除实现分叉

针对"能否无歧义开发"的评估，补齐 9 处**会导致不同实现不兼容**的行为分叉点；
同时完成头文件转正重命名（`include/hplogc-0.2.h` → `include/hplogc.h`，含包含守卫
`HPLOGC_0_2_H` → `HPLOGC_H`、提案注记改写，以及 rd 中全部路径引用与 §11 安装条款同步）。

1. §12.1：占位符解析规则——`%%` 展开为字面 `%`；**未知占位符（含行尾孤立 `%`）视为
   格式模板非法，init 失败并报格式名与行号**（不做"原样输出"降级，遵循 fail-fast）；
   `pid format`/`tid format = none` 展开为**空串**（不删除相邻字面文本）。
2. §12.2：`time format` 扩展占位符语法——`%f` 固定 6 位微秒；`%F<n>` 中 `n` 取 1~6；
   `%F`（不带数字）、`%F0`、`%F7` 及以上为配置错误。
3. §12.3：**"json 格式"按格式名判定**（恰为 `json`，键名区分大小写），自定义名如 `myjson`
   不享有 JSON 转义 / 跳过注入转义 / pid-tid 强制十进制三项行为；理由为可静态确定、零开销、
   避免按内容嗅探导致实现分叉。
4. §7.2：**内置 5 个格式名始终可用**（无 `[formats]` 节亦不触发"引用未定义 format"失败）；
   `[formats]` 中同名条目**覆盖**内置默认模板（合法，显式优先）。
5. §4.5：热加载检查由**独立监视线程**执行（不参与日志管线，init 创建 / shutdown 退出）；
   三种触发源（inotify、轮询、SIGHUP 原子标志）统一由该线程处理；**与 `HPLOGC_ENABLE_ASYNC`
   无关**，故 `sync_thread` 预设同样拥有该线程。
6. §4.9⑧ 注 + §4.10.3：**不存在"异步模式的同步旁路"**——异步构建下输出一律由消费者线程
   执行，仅 `CAP_SYNC` 的 sink 由消费者逐条 `emit`；`CAP_SYNC` 只在同步构建下决定线程归属。
7. §4.10.3：**能力位 × 构建模式矩阵**——补齐 4 种组合及"同步构建下 sink 无 `CAP_SYNC`
   → init 返回 `HPLOGC_ERR_UNSUPPORTED`"这一 fail-fast 边界；并定义 per-sink `async` 键
   的作用域（仅在矩阵内显式选择，不满足能力位时告警忽略，非错误）。
8. §12.4 + §7.3：**多 sink 投递的记账规则**——全局 `written` 在"至少 1 个 sink 成功"时 +1，
   全部失败则改计 `dropped`；全局计数器按条计（至多 +1），per-sink 按 sink 各计；
   投递前发生的溢出/限流不产生 per-sink 计数。
9. §10.4：`default outputs` 与 `default sinks` 同时出现时**不视为冲突**，按重复键规则
   "后出现者覆盖先出现者"+ 警告。

---

### v0.2 二次评审修订（2026-09-28）—— 可移植性与口径收口

基于第一轮修订后的再次比对（10 项落差）修订。**规范侧（rd）**：

1. §4.1：修正与 §4.8/§7.3/§0.2-B7 的直接矛盾——`HPLOGC_COMPILE_TIME_LEVEL` **严禁**使用
   `HPLOGC_LEVEL_xxx` 枚举符号名（原 §4.1 仍写"允许取值为枚举符号名"，正是该缺陷的源头描述）；
   并补全裁剪点含 `HPLOGC_xxx_F`、AUDIT/METRIC 不裁剪。
2. §4.8 / §11 / §4.7.1：`HPLOGC_SINKS` 默认改为 `console;rollingfile;null`——`null` 须默认注册，
   否则 §13.1 的类型等价性测试、§16.1 的实现要求与 §7.3"把 audit/metric 指向 null sink"的建议
   在默认构建下均无法落地；`min` 预设仍覆盖为 `console;rollingfile`。
3. §4.11.2：统一 `fields_dropped` 口径——**三种成因（超数量 / 超长度截断 / 超预算）均计入**，
   消除与头文件"截断计入统计"的相反表述。
4. §10.3：`file` 别名注释同步为 §4.7.1 的"仅提供 rotate 默认值"语义。
5. §7.6：ABI 常量统一为 `1u`/`0u`；补字段构造宏的 C99 / C++20 语言要求；`HPLOGC_API` /
   `HPLOGC_PRINTF` 标注展开式见头文件。
6. §13.2 补测试项（AUDIT/METRIC 不裁剪、`key` 深拷贝、`fields_dropped` 三成因、ABI 抽取）；
   §17 P0 改为"6 级日志 + `OFF` 阈值"；§1 文档版本标注含评审修订。

**头文件侧（`include/hplogc.h`，原名 `hplogc-0.2.h`）**：

7. 文件头声明收敛：`HPLOGC_FIELD_*` 依赖 C99 指定初始化器，**C++20 之前需编译器扩展**
   （`-pedantic-errors` / MSVC `/permissive-` 下会报错，已实测）；宏定义处同步加注。
8. `emit_batch` 的 `@return` 补"部分成功"记账并引用 §4.10.3；修正 `emit` 注释语病。
9. `hplogc_sink_flush` / `hplogc_sink_find` 补 NULL 参数与未初始化行为（对齐 §7.5）。
10. 两处 `fields_dropped` 注释统一为三成因；`hplogc_log_fields` 注释补"含字段名 `key`"
    并修正"既可以"语病。

---

### v0.2 评审修订（2026-09-28）—— rd ↔ 头文件一致性收口

基于 `docs/rd_v0.2.md` 与 `include/hplogc.h`（当时名为 `hplogc-0.2.h`）的逐符号比对（18 项落差）修订。

**规范侧（rd）**

1. §6 最小体积改为 `.text` ≤ 8 KB，与 §5 一致（原 32 KB 与 §5 冲突，§8 的 32 KB 为内存口径，已标注区分）。
2. §11 `HPLOGC_SINKS` 默认值改为 `console;rollingfile`（原写法含 P2 的 `syslog`，与 §4.8 冲突）。
3. §3.3 目录树补 `src/sink/sink_null.c`（§4.7.1/§13.1/§16.1 均已要求该 sink）。
4. §7.6 补入头文件已有的公共符号：`hplogc_error_t`（含 `HPLOGC_OK`）、`HPLOGC_VERSION_STRING`、
   `HPLOGC_PRINTF`、`HPLOGC_API` 与 DLL 宏、`HPLOGC_SINK_ABI_VERSION` 系列（落实 §7.6 的"逐一对齐"）。
5. §7.3/§7.8-4/§11：明确 **CMake 是编译期裁剪的唯一有效防线**（头文件 `#error` 拦截不到枚举符号名），
   且空值必须映射为 `0` 而非空宏定义。
6. §7.3：明确 `HPLOGC_AUDIT()` / `HPLOGC_METRIC()` **不受** `HPLOGC_COMPILE_TIME_LEVEL` 裁剪及理由。
7. §7.3 便捷宏签名统一为 `(cat, ...)` 形态并补全 6 个 `HPLOGC_xxx_ENABLED` 宏；补充 `level_name` /
   `level_parse` 的返回值与取值细节。
8. §7.4 补错误码类型名 `hplogc_error_t` 与 `HPLOGC_OK`；§7.6 长度宏标注"含结尾 NUL"；
   §10.1/§10.4 的 `HPLOGC_MAX_OUTPUTS` 改为 `HPLOGC_MAX_SINKS`（前者降为迁移别名）。
9. §4.7.1 澄清 `file` 别名仅提供 `rotate` 默认值、与显式 `rotate` 可共存；§4.10.3 明确
   `HPLOGC_CAP_STRUCT` 下核心始终传递 `fields`；§4.10.3 补 `emit_batch` 返回值契约。
10. §4.10.5 补 ABI 版本号的主/次编码规则与 `ops` 指针所有权；§7.5 补 per-sink API 的
    未初始化 / 未支持 / NULL 参数行为与热加载后句柄失效警告。
11. §4.11.2 / §7.6：明确字段深拷贝**含 `key`**，调用方无需保证任何指针生命周期。

**头文件侧（`include/hplogc.h`，原名 `hplogc-0.2.h`）**

12. 修正 `hplogc_sink_ops_t` 文档中的生命周期顺序为 `configure* → init → start`（原写反）。
13. 修正 `hplogc_level_enabled` 的串入错误文本；修正统计字段名笔误 `dropped_fields` → `fields_dropped`。
14. 补齐 `hplogc_level_name` / `hplogc_level_parse` 的 `@param` / `@return`（§11 Doxygen 门禁）。
15. `HPLOGC_VA_ARGS` 增加 MSVC `/Zc:preprocessor`（`_MSVC_TRADITIONAL == 0`）的 `__VA_OPT__` 分支。
16. `hplogc_field_t::key` 注释改为"入队时深拷贝"，与 §4.11.2 的深拷贝承诺一致。
17. 删除 `configure` 注释中 rd 未定义的通用键 `level`；`hplogc_kv_t::value` 的 NULL 语义改为
    "不得为 NULL"。
18. `HPLOGC_SINK_ABI_VERSION` 改为 `(主版本 << 16) | 次版本` 并新增抽取宏；
    `set_level_for_category` / `clear_level_for_category` 补 `HPLOGC_ENABLE_CATEGORY=OFF` 的 stub 行为；
    `HPLOGC_COMPILE_TIME_LEVEL` 处补空宏风险注记。

---

### v0.2（2026-09-28）—— API 双向对齐与多 sink 体系

基于 `rd_v0.1.md` 与 `include/hplogc.h` 的双向比对（台账见 §7.8），并吸收
`tmp/sink_desin.md`、`tmp/clog.h` 的多 sink 设计。

**一、编译阻塞修复**

1. 补齐 `hplogc_overflow_policy_t` 定义——v0.1 头文件的 `hplogc_config_t` 引用了该类型却从未定义（§7.6）。
2. 补齐 `hplogc_crash_safety_t` 定义——同上第二类缺失（§7.6）。
3. 删除残留异构声明 `hpulogc_file_sync_policy` / `hpulogc_sink_t` / `hpulogc_file_sink_set_sync_policy()`
   （拼写错误、前缀违规、引用未定义类型）；其"文件 write 同步策略"语义由 `HPLOGC_CAP_LINE_ATOMIC` 承接（§4.10.3）。

**二、破坏性变更**（迁移对照见 §0.2 与 §7.7）

4. 废弃 `hplogc_output_t` 与 `hplogc_output_type_t`，输出目标统一由 sink 表达（§4.7、§7.6）。
5. 字段更名：`hplogc_rule_t::outputs` → `sinks`、`hplogc_config_t::outputs` → `sinks`、
   `default_outputs` → `default_sinks`（配置键名 `default outputs` 保留兼容并新增别名 `default sinks`）。
6. `hplogc_rotate_t` / `hplogc_time_unit_t` 不再作为公共结构体字段类型，改为 rollingfile sink 的配置值词表（§4.7.2）。
7. `hplogc_config_t` 新增首字段 `struct_size`（ABI 自检），与核心尺寸不符时 init 返回 `HPLOGC_ERR_CONFIG`（§7.6）。
8. per-category 级别覆盖的清除方式由 `(hplogc_level_t)-1` 哨兵改为显式 `hplogc_clear_level_for_category()`（§7.3）。
9. `HPLOGC_COMPILE_TIME_LEVEL` 只接受数值 0~6 或 `HPLOGC_CTL_*`（修复"想裁剪却全部保留"的静默缺陷），
   CMake 负责把 `TRACE..OFF` 字符串映射为数值，头文件侧越界即 `#error`（§4.8、§7.3）。
10. 新增错误码 `HPLOGC_ERR_UNSUPPORTED`（-6），用于"值合法但当前构建不支持 / 未注册 sink 类型"（§7.4、§10.4）。

**三、多 sink 体系（新增规范性章节）**

11. §4.10：三层结构（ops / 实例 / 路由）、生命周期顺序 `create → configure* → start → init → emit → flush → destroy`、
    **`init` 必须晚于全部 `configure`** 的默认值填充约束、五种能力位与线程模型、按 sink 分组攒批、背压策略。
12. §4.7：内置 sink 清单（`console` / `rollingfile`（别名 `file`）/ `syslog` / `null`）与各自的配置键表。
13. 公开自定义 sink 注册 ABI：ops 字段顺序冻结、尾部 `reserved[4]`、`abi_version` 校验（§4.10.5、§7.6）。
14. **零依赖裁决**：kafka / loki / ES / clickhouse / s3 / mysql 等第三方客户端 sink 不进仓库，
    只能由应用侧注册（§4.10.5、§17）。
15. 删除 v0.1 的 socket 输出骨架；未知 / 被裁剪类型统一 fail-fast 返回 `HPLOGC_ERR_UNSUPPORTED`（§4.7、§10.4）。
16. 热加载细化到 sink 实例维度（未变更实例复用 fd、整体回滚、不得类型变动）（§10.5）。

**四、结构化字段通道（新增规范性章节）**

17. §4.11：`hplogc_field_t` + `hplogc_log_fields()` / `vlog_fields()` + `HPLOGC_xxx_F` 宏 +
    `HPLOGC_AUDIT()` / `HPLOGC_METRIC()` 语义子流。
18. 生命周期裁决：字段在生产者线程**深拷贝**入环（刻意区别于 clog 的借用指针），
    受 `HPLOGC_MAX_FIELDS`、`HPLOGC_MAX_FIELD_STR_LEN` 与 `max log length` 预算约束，超限丢弃并计数（§4.11.2、A.2-6）。
19. `hplogc_log_signal_safe()` 不携带字段（async-signal-safe 约束）（§4.11.2）。

**五、能力补充与其他一致性**

20. 新增 `hplogc_level_enabled()` 与 `HPLOGC_xxx_ENABLED()` 预检（原附录 A.1-3，采纳并转入正文）（§7.3、附录 A.1-3）。
21. 新增 `hplogc_level_name()` / `hplogc_level_parse()`（此前缺有时会各自重复实现）（§7.3）。
22. `hplogc_kv_t` 承载全局运行时键，修复 min 预设（`INI=OFF`）下时区 / `time format` / `newline` /
    `pid format` / 限流等键不可达的结构性缺陷（§7.2、§7.6）。
23. `hplogc_stats_t` 尾部追加 `fields_dropped`；per-sink 统计由新增 `hplogc_sink_get_stats()` 单独暴露（§7.3、§7.6）。
24. `hplogc_build_info_t` 尾部追加 `has_fields` 与 `sinks`（§7.6）。
25. CMake 新增 `HPLOGC_SINKS` 列表与 "编译期级别由 CMake 映射" 规则；安装规则把提案头文件列为不安装例外（§11）。
26. §3.3 源码组织新增 `src/sink/`；明确 sink 层为平台无关公共层、须经平台接口契约访问 OS（§3.3、§16.2）。
27. 测试与验收增补：sink 契约测试（含 `init` 晚于 `configure` 断言）、热加载回滚、per-sink 计数、
    字段预算裁剪、自定义 sink 注册；新增 "sink 间接层吞吐回退 ≤ 5%" 与 "min 增量 ≤ 0.5 KB" 门禁（§5、§13）。
28. 待仲裁项登记：注释语言（中文 vs 英文）冲突（§7.8 第 14 项、§15）。

---

### v0.1 评审条目（2026-09-19，原文标题为 "v0.2"）

基于 v0.1 全文评审（30 项）修订。**矛盾修复**：

1. per-category 级别语义统一为"覆盖"（§4.1/§4.9②/§7.3）。
2. `default format`/`default outputs` 兜底语义补全：未命中规则时按兜底 format + outputs 输出，为空则丢弃（§4.9⑥/§7.2/§7.6/§10.2/§10.3/§10.4/§10.5）。
3. §16/§17 优先级映射对齐：编译期裁剪体系与四版本预设上调 P1（§16.4/§17）。
4. 编译器矩阵与 C11 要求对齐：GCC ≥ 4.9；MSVC C11 构建需 2019 16.8+（§2.1）。
5. 定义生产者渲染上限 = `max log length`，init 校验 `buffer size ≥ 2 × max log length`，消除 wait 策略死锁边界（§4.3/§4.9④/§9/§10.4）。
6. 缓冲区"仅 init 时指定、热加载忽略"表述修正（§4.3/§10.5）。

**行为补充**：

7. `HPLOGC_ENABLE_CATEGORY=OFF` 时规则按 `*` 匹配、`%category` 展开空串（§4.8）。
8. 限流/采样语义定死：先限流后采样、全局/分类独立令牌桶、确定性 1/N 采样、stats 新增 `throttled` 字段（§4.9③/§7.3/§10.3）。
9. SIGHUP 触发增加 `signal reload` 开关与 handler 所有权定义（§4.5/§10.3/§10.5）。
10. category 为 NULL/空串等价 `"*"`；不做字符集校验（§4.9）。
11. per-output `fsync` 与全局 `crash_safety` 取更严格者；严格度 none < shutdown < periodic < entry（§7.6/§9）。
12. 热加载表补全 default format/outputs、`[advanced]` 全部键、shutdown timeout、console 增删、strict init（§10.5）。
13. `hplogc_log_signal_safe` 始终 async-signal-safe；`signal_safe=false` 时静默丢弃（§7.3/§9）。
14. `fork behavior=reinit` 惰性重建语义（child handler 仅置脏标记）（§9）。
15. 配置节均可省略、`[build]` 可选；全静默误配提示（§10.2/§10.4）。
16. 新增 `hplogc_config_default()` 强制初始化约定；`hplogc_init(NULL)` = `init_default()`；代码内配置非法值 fail-fast（不 clamp）；声明代码内配置为配置文件子集并列出专属键（§7.2/§7.3/§7.6）。
17. 校验补全：rule min>max、file output 重复路径、stats file 引用、`written` 按条计数、outputs/rules 数量上限（§7.3/§10.4）。
18. json 格式仅 JSON 转义（跳过注入转义）+ pid/tid 强制十进制（§12）。

**改进与一致性**：

19. 级别枚举更名 `HPLOGC_LEVEL_*`，消除与便捷宏 `HPLOGC_xxx` 的同名隐患（§4.1/§7.3/§7.6）。
20. min 预设 `INI=OFF`（定位嵌入式、保护 8KB 体积预算；需要配置文件者自行组合选项）（§5）。
21. `##__VA_ARGS__` 可移植性方案改写为可执行的检测序列（§7.3）。
22. CI 矩阵补齐 SPSC/MPSC、C99/C11 维度，sanitizer 任务显式化（§13.5）。
23. `HPLOGC_SANITIZER`（address/thread/undefined）替代一键式 `HPLOGC_ENABLE_SANITIZER`（§11）。
24. 许可证定为 MIT（§1）。
25. 新增 errno 保持要求（§9）。
26. 诊断信息统一 stderr；`hplogc_strerror` 列入 P2（§7.4/§17）。
27. §4.8 "HPLOGC_ENABLE_*" 措辞修正（§4.8）。
28. AGENTS.md 工程约束纳入（Doxygen 门禁、UTF-8 无 BOM、注释英文、snake_case）（§11/§15）。
29. 配置词法补充：引号内 `,`/`=` 不作分隔；outputs/rules 数量上限（§10.1/§10.4）。
30. 无锁 MPSC 构建下 `overwrite` 不可用（fail-fast，与 wait 同类）（§4.3/§5/§10.4）。

---

*（全文完）*
