# hplogc

**跨平台纯 C 语言高性能日志库**。支持 gcc / clang / msvc / mingw-w64，兼容 C99 / C11 及更高标准，头文件以 `extern "C"` 包裹，**C++ 安全**。

设计要点：**核心不认识任何具体输出目标**——输出统一由 sink（类型 + 实例 + vtable）表达，新增一种输出 = 增加一个 `.c` 文件 + 注册一行，核心 / 路由 / 异步队列无需改动。零第三方依赖。

- 需求与规范：[`docs/rd_v0.2.md`](docs/rd_v0.2.md)
- 对外 API 契约：[`include/hplogc.h`](include/hplogc.h)
- 验收结论：[`docs/phase1_acceptance_summary.md`](docs/phase1_acceptance_summary.md)

---

## 目录

- [1. 已实现功能总览](#1-已实现功能总览)
- [2. 平台与编译器支持矩阵](#2-平台与编译器支持矩阵)
- [3. 构建手册](#3-构建手册)
- [4. 安装手册](#4-安装手册)
- [5. 使用手册：代码内配置](#5-使用手册代码内配置)
- [6. 使用手册：配置文件](#6-使用手册配置文件)
- [7. 内置 sink 手册](#7-内置-sink-手册)
- [8. 异步模型与背压](#8-异步模型与背压)
- [9. 自定义 sink 扩展](#9-自定义-sink-扩展)
- [10. 热加载 / SIGHUP / fork](#10-热加载--sighup--fork)
- [11. 线程安全与性能](#11-线程安全与性能)
- [12. 测试与 CI](#12-测试与-ci)
- [13. 目录结构](#13-目录结构)
- [14. 已知限制与注意事项](#14-已知限制与注意事项)

---

## 1. 已实现功能总览

| 功能 | 说明 | 编译开关（默认） |
|------|------|------------------|
| 多 sink 体系 | 一个 sink = 类型（`hplogc_sink_ops_t`）+ 实例（名字 + 私有配置 + 私有状态）；注册表 + 自定义注册 ABI | 始终 |
| 内置 sink | `console`、`rollingfile`（别名 `file`）、`null`、`syslog`、`socket`（UDP/TCP） | `HPLOGC_SINKS` 列表 |
| 同步写入 | 调用者线程直接 emit | `HPLOGC_ENABLE_ASYNC=ON` 时仍可逐 sink 关闭 |
| 异步写入 | 单一消费者线程 + 环形缓冲 + 批量提交；按 sink 分组攒批 | `HPLOGC_ENABLE_ASYNC`（ON） |
| 环形缓冲两种实现 | 有锁（mutex/condvar）与无锁（SPSC，原子 head/tail） | `HPLOGC_LOCKFREE`（OFF=有锁） |
| 溢出策略 | `discard`（丢弃新日志）/ `overwrite`（覆盖最旧）/ `wait`（阻塞生产者） | 运行时配置 |
| 日志轮转 | `none` / `size` / `time` / `both`；备份数、命名模板、`.latest` 软链、fsync | `HPLOGC_ENABLE_ROTATE`（ON） |
| INI 配置文件 | 行式 INI：节、键值对、`#` 注释、双引号、行尾续行；前缀匹配节 | `HPLOGC_ENABLE_INI`（ON） |
| 配置热加载 | 三种触发源：文件监视器（Linux inotify / macOS kqueue）、轮询（mtime+size）、SIGHUP | `HPLOGC_ENABLE_HOT_RELOAD`（ON） |
| 多 Category 路由 | `cat.level` 选择器 + per-category 级别覆盖 | `HPLOGC_ENABLE_CATEGORY`（ON） |
| 结构化字段通道 | `hplogc_field_t` + `_F` 宏；字段随消息深拷贝入队 | 始终（`HPLOGC_MAX_FIELDS`=16） |
| 级别控制 | 全局阈值、per-category 覆盖、`*_ENABLED` 预检、编译期级别裁剪 | `HPLOGC_COMPILE_TIME_LEVEL` |
| ANSI 彩色输出 | 仅终端设备启用；Windows 走 VT 处理 | `HPLOGC_ENABLE_COLOR`（ON） |
| 源码位置捕获 | `__FILE__` / `__LINE__` / `__func__` | `HPLOGC_ENABLE_SOURCE_LOC`（ON） |
| signal-safe 写入 | `hplogc_log_signal_safe()`：不加锁 / 不格式化 / 不分配，直接 `write(2)` | `signal safe = true` 启用 |
| crash safety | `none` / `periodic` / `entry` / `shutdown`（按 sink fsync 能力） | 始终 |
| 统计与自省 | 全局 `hplogc_get_stats()`、per-sink `hplogc_sink_get_stats()`、`hplogc_get_build_info()` | 始终 |
| 限流与采样 | 全局/per-category 速率、采样率、突发额度 | `HPLOGC_ENABLE_THROTTLE`（**OFF**） |
| 注入转义 | 转义消息中的控制字符，防御日志注入 | `escape injection`（默认 true） |
| fork 行为 | `reinit` / `disable` / `inherit` | 始终（Windows 无 fork） |
| 编译期裁剪预设 | `full` / `min` / `sync_thread` / `async_single` | `HPLOGC_BUILD_PRESET` |
| 零第三方依赖 | 仅用系统库（Windows 网络输出另需系统库 `ws2_32`） | — |

**未内置**：kafka / loki / elasticsearch / clickhouse / s3 / mysql 等需要第三方客户端的 sink——按零依赖裁决**不进入本仓库**，需以自定义 sink 形态在应用侧实现（见 [§9](#9-自定义-sink-扩展)）。

---

## 2. 平台与编译器支持矩阵

平台差异全部收敛在 `src/platform/`（`hplogc_platform.h` 契约 + `posix/` `linux/` `darwin/` `win32/` 实现），核心与 sink 层不出现任何 `#ifdef _WIN32`。

| 平台 | 编译器 | 平台层实现 | 验证状态 |
|------|--------|-----------|----------|
| Linux | GCC / Clang | `posix/` + `linux/`（inotify 监视器） | ✅ CI：locked / lockfree / ASan+UBSan / coverage |
| macOS | AppleClang（arm64、x86_64） | `posix/` + `darwin/`（kqueue 监视器） | ✅ CI：locked / lockfree |
| Windows | MSVC | `win32/`（CRITICAL_SECTION、Win32 线程/文件、Winsock2） | ✅ CI：locked / lockfree |
| Windows | MinGW-w64（i686 + x86_64） | 同上 | ✅ CI：MINGW32 / MINGW64 × locked / lockfree |

GitHub Actions 全矩阵 **13 / 13 job 全绿**（ubuntu ×2、macos ×2、windows-msvc ×2、windows-mingw ×4、sanitizer ×2、coverage ×1）。

原子操作后端自动选择：C11 `<stdatomic.h>` / GCC-Clang `__atomic_*` / `__sync_*` / MSVC-MinGW `Interlocked*`。

---

## 3. 构建手册

### 3.1 依赖

- CMake ≥ 3.16
- C99 或 C11 编译器（GCC / Clang / MSVC / MinGW-w64）
- POSIX 下需 pthread（CMake 自动 `find_package(Threads)`）与 `libm`
- Windows 网络输出（`socket` sink）链接系统库 `ws2_32`，CMake 自动处理
- 无任何第三方依赖

### 3.2 常规构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # 跑测试（4 个用例）
./build/examples/hplogc_example_basic          # 跑示例
```

构建产物：`build/src/libhplogc.a`（静态库，默认）、`build/examples/hplogc_example_basic`、`build/examples/hplogc_example_async`。

### 3.3 构建选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `CMAKE_BUILD_TYPE` | — | `Debug` / `Release` / `RelWithDebInfo` |
| `HPLOGC_ENABLE_ASYNC` | ON | 异步写入（消费者线程 + 批量提交） |
| `HPLOGC_ENABLE_COLOR` | ON | ANSI 彩色输出 |
| `HPLOGC_ENABLE_ROTATE` | ON | 日志轮转 |
| `HPLOGC_ENABLE_INI` | ON | INI 配置解析器 |
| `HPLOGC_ENABLE_HOT_RELOAD` | ON | 配置热加载 |
| `HPLOGC_ENABLE_CATEGORY` | ON | 多 Category 路由与 per-category 级别 |
| `HPLOGC_ENABLE_THROTTLE` | OFF | 限流与采样 |
| `HPLOGC_ENABLE_SOURCE_LOC` | ON | 捕获 `__FILE__` / `__LINE__` |
| `HPLOGC_LOCKFREE` | OFF | ON=无锁 ring（SPSC）；OFF=有锁 ring |
| `HPLOGC_CONCURRENCY_UPPER` | SPSC | `SPSC` / `MPSC` |
| `HPLOGC_SINKS` | `console;rollingfile;null` | 编译进库的 sink 类型；已知类型：`console` `rollingfile` `syslog` `null` `socket` |
| `HPLOGC_BUILD_TESTS` | ON | 构建测试 |
| `HPLOGC_BUILD_EXAMPLES` | ON | 构建示例 |
| `HPLOGC_ENABLE_COVERAGE` | OFF | 覆盖率插桩（`--coverage -O0 -g`，不支持 MSVC） |
| `HPLOGC_SANITIZER` | none | sanitizer 预设 |
| `HPLOGC_COMPILE_TIME_LEVEL` | 空（放行全部） | 编译期级别裁剪，**必须填数值**（0=TRACE … 6=OFF），填枚举名会静默失效 |
| `HPLOGC_ATOMIC_BACKEND` | 自动 | `stdatomic` / `gcc-atomic` / `gcc-sync` / `msvc-interlocked` |
| `HPLOGC_BUILD_PRESET` | 空 | 见 3.4 |
| `HPLOGC_BUILD_SHARED_LIBS` | OFF | 构建动态库 |

### 3.4 构建预设

`HPLOGC_BUILD_PRESET` 一次性设定一组开关（可用 `hplogc_get_build_info()` 回读 `build_version`）：

| 预设 | 含义 |
|------|------|
| 空（默认） | `custom`，等价于 `full` |
| `full` | 全功能：异步 + 彩色 + 轮转 + INI + 热加载 + category + 源码位置 |
| `min` | 最小体积：同步、无轮转/无彩色/无 INI/无热加载/category，sink 仅 `console;rollingfile` |
| `sync_thread` | 同步 + 多线程安全 |
| `async_single` | 异步 + 单消费者 |

```bash
cmake -S . -B build-min -DHPLOGC_BUILD_PRESET=min
```

### 3.5 无锁 ring 与 sanitizer

```bash
# 无锁 SPSC ring
cmake -S . -B build-lf -DCMAKE_BUILD_TYPE=Release -DHPLOGC_LOCKFREE=ON

# ASan + UBSan
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

### 3.6 覆盖率

```bash
cmake -S . -B build-cov -DCMAKE_BUILD_TYPE=Debug -DHPLOGC_ENABLE_COVERAGE=ON
cmake --build build-cov -j && ctest --test-dir build-cov
gcovr --root . build-cov --exclude '.*/tests/.*' --exclude '.*/examples/.*' --print-summary
```

### 3.7 Windows 交叉编译（Linux 上产出 Windows 库）

```bash
cat > /tmp/mingw64.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
EOF
cmake -S . -B build-win64 -DCMAKE_TOOLCHAIN_FILE=/tmp/mingw64.cmake \
  -DCMAKE_BUILD_TYPE=Release -DHPLOGC_SINKS="console;rollingfile;null;socket"
cmake --build build-win64 -j     # 产出 build-win64/src/libhplogc.a
```

32 位把编译器换成 `i686-w64-mingw32-gcc` 即可。

---

## 4. 安装手册

### 4.1 安装

```bash
cmake --install build                                   # 默认前缀（/usr/local）
cmake --install build --prefix /path/to/prefix          # 自定义前缀
```

安装布局（相对前缀）：

```
include/hplogc.h                          # 唯一公共头文件
lib/libhplogc.a                           # 静态库（或动态库）
lib/cmake/hplogc/hplogcConfig.cmake       # find_package 包配置
lib/cmake/hplogc/hplogcTargets.cmake      # 导出目标
```

### 4.2 在自己的工程中使用

**方式一：`find_package`（推荐）**

```cmake
cmake_minimum_required(VERSION 3.16)
project(myapp C)

find_package(hplogc REQUIRED)

add_executable(myapp main.c)
target_link_libraries(myapp PRIVATE hplogc::hplogc)
```

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/prefix
```

**方式二：直接链接**

```bash
cc -std=c11 main.c -I/path/to/prefix/include \
   /path/to/prefix/lib/libhplogc.a -lpthread -lm -o myapp
```

（POSIX 需 `-lpthread -lm`；Windows 若启用 `socket` sink 需 `-lws2_32`。）

---

## 5. 使用手册：代码内配置

### 5.1 最小示例

```c
#include <hplogc.h>

int main(void)
{
    hplogc_config_t cfg;
    const char*  sinks[] = { "console" };
    hplogc_kv_t  opts[]  = { { "stream", "stdout" }, { "color", "true" } };
    hplogc_sink_config_t out = { "console", "console", 1, -1, opts, 2 };
    hplogc_rule_t rules[] = {
        { "*", HPLOGC_LEVEL_TRACE, HPLOGC_LEVEL_OFF, "standard", sinks, 1 }
    };

    hplogc_config_default(&cfg);          /* 必须：填充默认值与 struct_size */
    cfg.sinks = &out;
    cfg.sink_count = 1;
    cfg.rules = rules;
    cfg.rule_count = 1;
    cfg.default_sinks = sinks;            /* 规则未命中时的落点 */
    cfg.default_sink_count = 1;

    if (hplogc_init(&cfg) != HPLOGC_OK) {
        return 1;
    }

    HPLOGC_INFO("app", "started version=%s", "1.0");
    HPLOGC_ERROR("app.db", "connect failed code=%d", 7);

    hplogc_shutdown();                    /* 冲刷并排空后释放 */
    return 0;
}
```

一键上手也可用内置默认：`hplogc_init_default()`（INFO 级、`standard` 格式、一个 stderr console sink、1 MB 缓冲、丢弃式溢出），或从文件加载：`hplogc_init_from_file("hplogc.conf")`。

### 5.2 日志宏

| 宏 | 形式 |
|----|------|
| `HPLOGC_TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `FATAL` | `HPLOGC_INFO(cat, fmt, ...)` |
| 结构化变体 | `HPLOGC_INFO_F(cat, fields, nfields, fmt, ...)` |
| 级别预检 | `HPLOGC_INFO_ENABLED(cat)` 等（非零表示会处理） |
| 语义子流 | `HPLOGC_AUDIT(fields, nfields, ...)`、`HPLOGC_METRIC(fields, nfields, ...)`（category 固定为 `audit` / `metric`，INFO 级） |

宏自动注入 `__FILE__` / `__LINE__` / `__func__`；低于编译期级别（`HPLOGC_COMPILE_TIME_LEVEL`）的宏在预处理期退化为 `((void)0)`，**运行时零开销**。

### 5.3 结构化字段

```c
hplogc_field_t f[] = {
    HPLOGC_FIELD_STR("actor", "alice"),
    HPLOGC_FIELD_INT("amount", 5000),
    HPLOGC_FIELD_DOUBLE("ratio", 0.75),
    HPLOGC_FIELD_BOOL("retry", 1),
};
HPLOGC_INFO_F("audit", f, 4, "transfer done");
```

字段类型：`HPLOGC_V_INT` / `V_UINT` / `V_DOUBLE` / `V_STR` / `V_BOOL`（`hplogc_field_t.type`）。
上限：`HPLOGC_MAX_FIELDS`=16（可 `-D` 覆盖），字符串字段长度上限 `HPLOGC_MAX_FIELD_STR_LEN`=128。超出或整体超 `max log length` 预算的字段会被丢弃并计入 `fields_dropped`。

### 5.4 级别控制

```c
hplogc_set_level(HPLOGC_LEVEL_WARN);                  /* 全局阈值 */
hplogc_set_level_for_category("app.db", HPLOGC_LEVEL_TRACE);
hplogc_clear_level_for_category("app.db");
if (hplogc_level_enabled(HPLOGC_LEVEL_TRACE, "app.db")) { /* 昂贵参数的预检 */ }
```

### 5.5 signal-safe 与统计

```c
/* 信号处理器中唯一安全的写入方式：需先启用 signal safe = true */
hplogc_log_signal_safe(HPLOGC_LEVEL_ERROR, "crash imminent");

hplogc_stats_t st;
if (hplogc_get_stats(&st) == HPLOGC_OK) {
    printf("accepted=%llu written=%llu dropped=%llu\n",
           (unsigned long long)st.accepted,
           (unsigned long long)st.written,
           (unsigned long long)st.dropped);
}

hplogc_build_info_t bi;
hplogc_get_build_info(&bi);        /* 构建特性开关、ring 类型、已编译 sink 列表 */
```

`hplogc_stats_t` 字段：`accepted` `dropped` `overwritten` `throttled` `written` `buffer_used` `buffer_size` `fields_dropped`。

> 注意：`hplogc_get_stats()` 在未初始化或 `hplogc_shutdown()` 之后返回 `HPLOGC_ERR_STATE` 且**不写**入参——请在 shutdown **之前**读取统计。

### 5.6 冲刷与关闭

```c
hplogc_flush();   /* 把队列刷到各 sink（不 fsync） */
hplogc_sync();    /* 同上，并对支持 fsync 的 sink 额外 fsync */
hplogc_shutdown();/* 冲刷 + 排空（受 shutdown timeout 约束）→ flush + destroy 全部 sink；幂等 */
```

### 5.7 常用返回值

| 常量 | 值 | 含义 |
|------|----|------|
| `HPLOGC_OK` | 0 | 成功 |
| `HPLOGC_ERR_INVALID_ARG` | -1 | 非法参数 |
| `HPLOGC_ERR_NO_MEM` | -2 | 内存不足 |
| `HPLOGC_ERR_IO` | -3 | I/O 失败（如输出目标打不开） |
| `HPLOGC_ERR_CONFIG` | -4 | 配置缺失或非法 |
| `HPLOGC_ERR_STATE` | -5 | 状态错误（未初始化 / 重复初始化 / 已 shutdown） |
| `HPLOGC_ERR_UNSUPPORTED` | -6 | 合法但当前构建不支持（未注册 / 被裁剪的 sink 类型等，fail-fast） |

---

## 6. 使用手册：配置文件

```bash
hplogc_init_from_file("hplogc.conf");
```

### 6.1 完整示例

```ini
[global]
level = INFO
default format = standard
default sinks = console,app_file
time format = %Y-%m-%d %H:%M:%S.%f
timezone = local
timestamp source = realtime
newline = auto
capture source loc = true
hot reload interval = 5
signal reload = false
strict init = false

[formats]
brief = %time %level [%category] %msg%n

[outputs]
console  = console,stream=stdout,color=true,enabled=true
app_file = rollingfile,path=/var/log/app.log,rotate=size,max size=10mb,max files=5

[buffer]
buffer size = 1mb
overflow policy = discard

[async]
batch size = 64
flush interval = 100
shutdown timeout = 5000

[rules]
app.*.* = brief -> console,app_file
*.*     = standard -> console

[advanced]
max log length = 4096
crash safety = shutdown
fork behavior = reinit
signal safe = false
```

### 6.2 节与顺序

节必须**按下列编号递增**出现，否则报 `HPLOGC_ERR_CONFIG`（"节顺序违反 §10.2"）：

| 序 | 节 | 前缀匹配 | 内容 |
|----|----|----------|------|
| 0 | `[build]` | 否 | 只读展示，全部键被忽略 |
| 1 | `[global]` | 否 | 全局设置（见表 6.3） |
| 2 | `[formats]` / `[formats.xxx]` | **是** | `格式名 = 模板` |
| 3 | `[outputs]` / `[outputs.xxx]` | **是** | `实例名 = 类型, 键=值, ...` |
| 4 | `[buffer]` | 否 | 环形缓冲大小与溢出策略 |
| 5 | `[async]` | 否 | 批量 / 冲刷 / 关闭超时 |
| 6 | `[throttle]` | 否 | 限流与采样（`HPLOGC_ENABLE_THROTTLE=ON` 才有意义） |
| 7 | `[rules]` | 否 | 路由规则 |
| 8 | `[advanced]` | 否 | 高级项（见表 6.4） |

未知节或写在节外的键 → 报错。未知键：`strict init = true` 时失败，否则告警忽略。

### 6.3 `[global]` 键

| 键 | 取值 | 默认 |
|----|------|------|
| `level` | `TRACE`..`FATAL`、`OFF`（大小写不敏感） | `INFO` |
| `default format` | 内置 5 名或 `[formats]` 自定义名 | `standard` |
| `default sinks`（或 `default outputs`） | 逗号分隔实例名 | 空（未命中即丢弃） |
| `time format` | strftime 风格，额外支持 `%f`（6 位微秒）与 `%F<n>` | `%Y-%m-%d %H:%M:%S.%f` |
| `timezone` | `utc` / `local`（其他值报错） | `local` |
| `timestamp source` | `realtime` / `monotonic`（其他值报错） | `realtime` |
| `newline` | `auto` / `lf` / `crlf` | `auto` |
| `pid format` / `tid format` | `decimal` / `hex` / `none` | `decimal` |
| `capture source loc` | 布尔 | `true` |
| `hot reload interval` | 秒（0=禁用轮询） | `0` |
| `signal reload` | 布尔（注册 SIGHUP） | `false` |
| `strict init` | 布尔 | `false` |

### 6.4 `[advanced]` 键

| 键 | 取值 | 默认 |
|----|------|------|
| `max log length` | 256~65536（钳制） | `4096` |
| `truncation marker` | 字符串 | `...[TRUNCATED]` |
| `escape injection` | 布尔 | `true` |
| `crash safety` | `none` / `periodic` / `entry` / `shutdown` | `shutdown` |
| `fork behavior` | `reinit` / `disable` / `inherit` | `reinit` |
| `signal safe` | 布尔 | `false` |
| `stats interval` | 秒 | `0` |
| `stats output` | `file` / `stderr` | `stderr` |
| `stats file` | 路径；`stats output = file` 时必填 | 空 |

> `crash safety` / `max log length` / `fork behavior` 属于 `[advanced]`，**不是** `[global]`。

### 6.5 `[buffer]` 与 `[async]`

| 节 | 键 | 取值 | 默认 |
|----|----|------|------|
| `[buffer]` | `buffer size` | 尺寸串：`4kb`~`1gb`（后缀 `k`/`kb`/`m`/`mb`/`g`/`gb`），**字节数** | `1mb` |
| `[buffer]` | `overflow policy` | `discard` / `overwrite` / `wait` | `discard` |
| `[async]` | `batch size` | 1~65535 | `64` |
| `[async]` | `flush interval` | 毫秒，1~60000 | `100` |
| `[async]` | `shutdown timeout` | 毫秒，0=一直等待 | `5000` |
| `[throttle]` | `global rate limit` / `per category rate limit` / `sampling rate` / `burst size` | — | `0` / `0` / `1.0` / `100`（仅在 `HPLOGC_ENABLE_THROTTLE=ON` 时有运行时效果，默认 OFF） |

`batch size` 属于 `[async]`，**不是** `[buffer]`。

### 6.6 `[outputs]` 行内语法

```
实例名 = <类型>[, 键=值, 键=值, ...]
```

- 第一段是类型：裸写（`rollingfile`）或 `type=rollingfile` 均可；`"file"` 是 `"rollingfile"` 的别名。
- 其后每段必须是 `key=value`，缺 `=` 直接报错。
- **逗号后不要加空格**：私有键名前的空格不会被裁剪，`, stream=...` 会变成键名 `' stream'` 而无法识别（`[rules]` 右侧的 sink 列表则允许空格）。键名内部的空格（如 `max size`）不受影响。
- **通用键只有 `enabled` 与 `async`**；其余全部作为该 sink 的私有配置。
- **`level` 不是 sink 键**——写在 `[outputs]` 里会被当作私有键交给 sink，sink 不识别即失败/告警。
- `async = -1`（默认）表示由 sink 的 `HPLOGC_CAP_ASYNC` 能力位决定。
- 实例上限 16（`HPLOGC_MAX_SINKS`），重名报错；私有键上限 64（`HPLOGC_MAX_OPTIONS`）。

### 6.7 `[rules]` 语法

```
<category 选择器>[.<级别>] = <格式名> -> <sink1>,<sink2>,...
```

- 级别写法：`*`（全级别）、`ERROR`（单级）、`WARN~FATAL`（区间），大小写不敏感。
- 值中必须有 `->`，左侧为格式名，右侧为逗号分隔的 sink 实例名。
- 匹配：自上而下取**第一条**命中；`*` 匹配全部，`app.*` 匹配 `app` 自身及其子级；未命中则用 `default format` + `default sinks`。
- 规则上限 64。

> **易踩坑**：键中**不带 `.级别`** 时只匹配 `TRACE`（如 `app = fmt -> s1`）。请一律写成 `app.*` 或 `app.*.*`。

### 6.8 `[formats]` 与占位符

```ini
[formats]
brief = %time %level [%category] %msg%n
```

占位符全集：`%level` `%time` `%pid` `%tid` `%file` `%line` `%func` `%msg` `%category` `%n`（换行）。

内置 5 个格式：`minimal`、`standard`、`categorized`、`detailed`、`json`。
**换行由模板的 `%n` 负责**，sink 不再追加。名为 `json` 的格式按名判定走 JSON 转义。

---

## 7. 内置 sink 手册

| 类型 | 别名 | 能力 | 说明 | 是否在默认 `HPLOGC_SINKS` |
|------|------|------|------|---------------------------|
| `console` | — | SYNC / ASYNC | stdout/stderr，终端下 ANSI 彩色 | ✅ |
| `rollingfile` | `file` | SYNC / ASYNC / LINE_ATOMIC / FSYNC | 文件写入 + 轮转 | ✅ |
| `null` | — | SYNC / ASYNC | 丢弃一切（测试基线 / 下线落点） | ✅ |
| `syslog` | — | SYNC | POSIX `openlog`/`syslog` | ❌（仅 POSIX；需加入列表） |
| `socket` | — | SYNC / ASYNC | UDP / TCP 网络输出，零第三方依赖 | ❌（需加入列表） |

启用非默认 sink：

```bash
cmake -S . -B build -DHPLOGC_SINKS="console;rollingfile;null;syslog;socket"
```

### 7.1 `console`

| 键 | 取值 | 默认 |
|----|------|------|
| `stream` | `1`=stdout / `2`=stderr | `stdout` |
| `color` | 布尔 | 由 `HPLOGC_ENABLE_COLOR` 决定 |

### 7.2 `rollingfile`（别名 `file`）

| 键 | 取值 | 默认 |
|----|------|------|
| `path` | 路径 | 必填 |
| `rotate` | `none` / `size` / `time` / `both` | `none` |
| `max size` | 尺寸串（`rotate` 含 size 时必填） | — |
| `time unit` | `hour` / `day` / `week` / `month`（`rotate` 含 time 时必填） | — |
| `max files` | 备份数，0=不限 | `0` |
| `fsync` | 布尔（与 `crash safety` 取更严格者） | `false` |
| `symlink latest` | 布尔 | `false`（**Windows 忽略并告警**） |
| `rotate naming` | 归档命名模板 | `{base}.{timestamp}.{index}.log` |
| `file perms` | 八进制 | `0644`（**Windows 忽略并告警**） |
| `dir perms` | 八进制 | `0755`（**Windows 忽略并告警**） |

### 7.3 `syslog`

| 键 | 取值 | 默认 |
|----|------|------|
| `facility` | syslog facility | `user` |

仅 POSIX；Windows 上该类型不可注册。

### 7.4 `socket`（UDP / TCP）

| 键 | 取值 | 默认 |
|----|------|------|
| `host` | IP 或域名 | 必填 |
| `port` | 1~65535 | 必填 |
| `protocol` | `udp` / `tcp` | `udp` |
| `connect timeout` | 毫秒，0=系统默认 | `0` |
| `reconnect` | 布尔（TCP 断线后退避重连） | `true` |

语义：目标不可达**不使 init 失败**（best-effort）；发送失败只计入该 sink 的 `failed`；TCP 重连采用有上限的指数退避（100 ms ~ 5 s）；**任何路径都不会无限阻塞调用线程**。UDP 为 best-effort，不保证送达、不重传。

### 7.5 `null`

无私有键；未识别任何键（返回非 0）。

---

## 8. 异步模型与背压

```
调用线程 ──► 环形缓冲 ──► 唯一消费者线程 ──► 按 sink 分组攒批 ──► sink.emit_batch / emit
```

- 队列实现二选一：有锁（mutex/condvar）或无锁 SPSC ring（`HPLOGC_LOCKFREE=ON`）。
- 消费者线程由库创建；**不存在"异步模式下的同步旁路"**——即使 sink 只有 `HPLOGC_CAP_SYNC`，也由消费者线程逐条调用其 `emit`，绝不回落到调用者线程。
- 溢出策略：`discard`（丢弃新日志，计入 `dropped`）/ `overwrite`（覆盖最旧，计入 `overwritten`）/ `wait`（阻塞生产者）。
- `overwrite` 不会覆盖已被取出但未 release 的条目；队首"在飞行中"时退化为丢弃新日志并计 `dropped`。
- 记账（§12.4）：至少 1 个目标 sink 成功 → 全局 `written++`；**全部目标 sink 均失败**才计全局 `dropped`；per-sink 统计见 `hplogc_sink_get_stats()`。
- `hplogc_flush()` 阻塞直到消费者排空；`hplogc_shutdown()` 先 flush 再销毁，受 `shutdown timeout` 约束。
- 同步构建（`HPLOGC_ENABLE_ASYNC=OFF`）不创建队列，`hplogc_stats_t::buffer_used` 与 `buffer_size` 恒为 `0`。

---

## 9. 自定义 sink 扩展

新增一种输出 = 实现 `hplogc_sink_ops_t` + 注册一行，核心 / 路由 / 异步队列无需改动。

**生命周期顺序（规范性）**：

```
create → configure* → init → start → emit / emit_batch* → flush → destroy
```

- `init` 在**全部** `configure` 之后执行，因此 `init` 内只能写 `if (p->field == 0) p->field = default;` 形式的默认值填充，**不得无条件赋值**（会清零已读入的配置）。
- `emit` 与 `emit_batch` 至少实现其一；网络型 / 聚合型 sink 必须实现 `emit_batch`。
- `hplogc_event_t` 的指针仅在回调期间有效；需要留存请自行深拷贝。
- `emit_batch` 返回成功写出的条数（部分成功按该值记账）；负值表示整批失败。
- 回调中禁止调用除 `hplogc_sink_*` 之外的库 API（避免重入）；稳态不得动态分配内存。

```c
static int my_configure(hplogc_sink_t* s, const char* key, const char* val) { /* ... */ return 0; }
static int my_init(hplogc_sink_t* s)      { /* 只填默认值 */ return 0; }
static int my_start(hplogc_sink_t* s)     { /* 打开资源 */ return 0; }
static void my_emit(hplogc_sink_t* s, const hplogc_event_t* ev) { /* 写一条 */ }
static int  my_emit_batch(hplogc_sink_t* s, const hplogc_event_t* const* evs, size_t n) { return (int)n; }
static void my_destroy(hplogc_sink_t* s)  { /* 释放 */ }

static const hplogc_sink_ops_t my_ops = {
    "mytype",                       /* type：配置里的类型名 */
    HPLOGC_SINK_ABI_VERSION,        /* 必须填 */
    HPLOGC_CAP_SYNC | HPLOGC_CAP_ASYNC,
    sizeof(my_priv_t),              /* priv_size：核心分配并清零 */
    my_configure, my_init, my_start,
    my_emit, my_emit_batch,
    NULL,                           /* flush：无缓冲语义可为 NULL */
    my_destroy,
    { NULL, NULL, NULL, NULL }      /* reserved[4] 必须全 NULL */
};

hplogc_sink_register(&my_ops);      /* 必须在 hplogc_init 之前 */
```

ABI 约束：字段顺序在 `HPLOGC_SINK_ABI_VERSION` 主版本内冻结；新增回调只能消耗 `reserved` 槽位；注册表要求主版本严格相等，否则返回 `HPLOGC_ERR_UNSUPPORTED`。库只保存 ops 指针，请提供静态常量。

---

## 10. 热加载 / SIGHUP / fork

热加载有三种触发源，任一命中即重新解析并**原子替换**配置（失败则整体回滚、保留旧配置）：

1. 文件监视器：Linux inotify、macOS kqueue（Windows 无原生机制，回退轮询）
2. 轮询：`hot reload interval > 0` 时按 mtime + size 比对
3. SIGHUP：`signal reload = true` 时安装处理器（仅置原子标志）

平台差异：**Windows 没有 POSIX 信号也没有 fork**，`hp_install_sighup()` 与 `hp_atfork_child()` 均返回 `HPLOGC_ERR_UNSUPPORTED`，库仅靠监视器 / 轮询工作，不会崩溃。

`fork behavior`：`reinit`（子进程需重新初始化）/ `disable`（子进程禁用日志）/ `inherit`（沿用父进程状态）。

---

## 11. 线程安全与性能

- 所有公共 API 可多线程并发调用；`hplogc_shutdown()` 与其他 API 并发是唯一不安全的组合。
- 跨线程计数走 `hp_atomic_*`（禁止依赖"事实原子性"）。
- 热路径：级别预检（`HPLOGC_xxx_ENABLED`）→ 编译期裁剪 → 格式化一次 → 入队；消费者侧按 sink 分组攒批，减少往返。
- 单条日志长度上限 `max log length`（默认 4096），超出截断并追加 `truncation marker`。
- 调用者 `errno` 不会被日志调用破坏。

---

## 12. 测试与 CI

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

测试用例：`test_ring`（环形缓冲）、`test_config`（INI + 配置装配）、`test_smoke`（端到端落地校验）、`test_socket`（网络抽象与不阻塞门禁）。

CI（`.github/workflows/ci.yml`）矩阵：

- `build-test`：ubuntu / macos / windows-msvc × locked / lockfree
- `windows-mingw`：MSYS2 `MINGW32`(i686) 与 `MINGW64`(x86_64) × locked / lockfree
- `sanitizer`：ubuntu，ASan + UBSan × locked / lockfree
- `coverage`：gcovr 生成报告并归档（**不设阈值**）

---

## 13. 目录结构

```
include/hplogc.h          公共 API（唯一对外头文件）
src/
  core/                   核心：初始化、路由、格式化、异步消费者、热加载、signal
  ring/                   环形缓冲（有锁 / 无锁两套）
  conf/                   INI 解析与配置装配
  sink/                   内置 sink + 注册表
  platform/               平台契约与 posix / linux / darwin / win32 实现
  atomic/                 原子后端（stdatomic / gcc / msvc）
tests/                    单元测试与冒烟测试
examples/                 示例（basic / async_demo）
docs/                     需求规范与验收摘要
cmake/                    find_package 包配置模板
```

---

## 14. 已知限制与注意事项

1. **配置节顺序强制**：节必须按 `[build]`→`[global]`→`[formats]`→`[outputs]`→`[buffer]`→`[async]`→`[throttle]`→`[rules]`→`[advanced]` 递增出现。
2. **`[rules]` 不带级别后缀只匹配 TRACE**，请写成 `app.*` / `app.*.*`。
3. ~~`[async]` 节的键在默认构建下不生效~~ **已修复**（原为配置解析分支的 `#ifndef` 极性错误，导致开启异步时 `[async]` 的 `batch size` / `flush interval` / `shutdown timeout` 被静默忽略、只能用编译期默认值）。`[throttle]` 的键始终可解析，但**限流仅在 `HPLOGC_ENABLE_THROTTLE=ON` 时才有运行时效果**（默认 OFF）。
4. **Windows 差异**：无 POSIX 信号与 fork（`signal reload` / `fork behavior` 不生效）；`file perms` / `dir perms` / `symlink latest` 被忽略并告警；`syslog` 不可注册。
5. **`socket` 与 `syslog` 默认不注册**，需显式加入 `HPLOGC_SINKS`。
6. **`level` 不是 sink 键**，写在 `[outputs]` 里会被当作私有键。
7. **统计读取时机**：`hplogc_get_stats()` 须在 `hplogc_shutdown()` 之前调用，之后返回 `HPLOGC_ERR_STATE` 且不写入参。
8. **字符串字段不做深拷贝的场景**：`HPLOGC_FIELD_STR` 的指针在同步路径下仅在调用期间使用；异步路径下字段会被深拷贝入队，调用方无需保活。
9. **`[outputs]` 行内逗号后不要加空格**（键名前导空格不裁剪，会导致私有键无法识别）；`[rules]` 右侧 sink 列表允许空格。
10. **良性编译告警**（既有代码，安全截断不越界，非缺陷）：`conf/build.c`、`sink/sink_rollingfile.c` 的 `-Wformat-truncation`。
11. `hplogc_init*()` 系列存在约 400 KB 栈帧，主线程调用无碍；若从**栈很小的辅助线程**调用 init 需留意（尤其 Windows 默认 1 MB 线程栈）。
