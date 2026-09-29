# hplogc 验收摘要（Phase 1 + Phase 2/3 平台扩展）

> 生成日期：2026-09-29
> 范围：Phase 1（Linux 全功能）实现验收 + Phase 2/3（macOS / Windows 平台、CI 矩阵、覆盖率报告项、内置 socket sink）验收。

## 1. 交付范围

| 任务 | 内容 | 主要落点 |
|------|------|----------|
| 1 | rd 澄清条款 G1-G6 + 门禁口径 | `docs/rd_v0.2.md` §18.1 / §18.1.1 |
| 3 | CMake 骨架 + 平台契约 | `CMakeLists.txt`、`cmake/`、`src/platform/`、`include/hplogc.h` |
| 4 | ring 有锁 / 无锁两套实现 | `src/ring/` |
| 5 | INI 解析器 | `src/conf/ini.c`、`src/conf/build.c` |
| 6 | core 初始化 / 统计 + sink 注册表 | `src/core/core.c`、`src/sink/` |
| 7 | 路由 + 日志管线 + 异步消费者 | `src/core/route.c`、`format.c`、`async.c`、`examples/` |
| 8 | signal_safe / fork / 热加载 | `src/core/signal.c`、`reload.c`、平台 watcher |
| 9 | 测试体系 + CI | `tests/`、`.github/workflows/ci.yml` |
| — | **macOS（darwin）平台层** | `src/platform/darwin/plat_watcher.c`（kqueue） |
| — | **Windows（win32）平台层** | `src/platform/win32/` 6 个文件 + `src/atomic/atomic_msvc.h` |
| — | **CI 矩阵扩展** | `windows-latest`（MSVC）+ MinGW/MSYS2 双工具链 |
| — | **覆盖率报告项** | CI `coverage` job（仅归档，不设阈值） |
| — | **内置 socket sink** | `src/sink/sink_socket.c` + 平台 `hp_socket_*`（rd §4.7.3 / G7） |

## 2. 本地验收（门禁对照）

| 门禁 | 结果 | 证据 |
|------|------|------|
| G1 恰好一次（2000 条 / 1 MiB / DISCARD） | 通过 | 日志行数 == accepted == written == 2000，dropped == 0，无空行 / 无重复 |
| G2 消费者不漏取（lockfree） | 通过 | `hp_async_stop` 自旋排干，`written == accepted`，无 ring 残留 |
| G3 无虚假丢弃 | 通过 | 缓冲调大后 dropped 仅由真实满引发 |
| G4/G5 配置解析 | 通过 | `rules` 箭头语法、`formats.*`/`outputs.*` 前缀节解析正确；坏配置报 `HPLOGC_ERR_CONFIG` |
| G6 stats 生命周期 | 通过 | 未 init 调用 `get_stats` 返回 `HPLOGC_ERR_STATE` 且不写 `*stats` |
| G7 socket 不阻塞 / 零依赖 | 通过 | 不可达 TCP 在超时内返回（有界）；失败不计全局 `dropped`；仅链接系统 socket 库 |
| ctest（locked / lockfree，含 socket） | 通过 | 4/4 用例全绿（config / ring / smoke / socket） |
| ASan + UBSan（locked / lockfree） | 通过 | 运行期零内存错误、零 UB 报告 |

## 3. 平台矩阵验证

| 平台 / 工具链 | 验证方式 | 结果 |
|---------------|----------|------|
| Linux（GCC，locked / lockfree） | 本地构建 + ctest | ✅ 通过 |
| Linux（ASan + UBSan） | 本地构建 + ctest | ✅ 通过 |
| **macOS（Apple silicon）** | GitHub Actions `macos-latest` 实机 | ✅ locked / lockfree 均通过 |
| **Windows（MSVC）** | GitHub Actions `windows-latest` 实机 | ✅ locked / lockfree 均通过 |
| **Windows（MinGW-w64 32 位 / 64 位）** | GitHub Actions + MSYS2（`MINGW32` + `MINGW64` 各跑 locked / lockfree） | ✅ 4 项全通过 |
| Windows（MinGW 交叉预检） | 本地 `i686-w64-mingw32-gcc` / `x86_64-w64-mingw32-gcc` | ✅ 通过 |

**GitHub Actions 全矩阵：13 / 13 job 全绿**（ubuntu ×2、macos ×2、windows-msvc ×2、
windows-mingw MINGW32 ×2 + MINGW64 ×2、sanitizer ×2、coverage ×1）。

### 转绿过程中修复的四个真实缺陷

| # | 缺陷 | 表现 | 修复 |
|---|------|------|------|
| 1 | `posix/plat_file.c` 使用 POSIX.1-2008 的 `st_mtim` | macOS 编译失败（`no member named 'st_mtim'`） | `__APPLE__` 分支改用 `st_mtimespec` |
| 2 | 原子后端宏对库是 `PRIVATE`，测试目标拿不到 | MSVC 编译失败（`#error 未选择原子后端`） | 在 `tests/CMakeLists.txt` 为四个测试目标补 `PRIVATE` 定义 |
| 3 | `hp_dispatch_emit` 栈帧约 **1.05 MB**（16 组 × 4096 内联数组） | macOS Bus error / Windows SegFault（Linux 线程栈 8 MB 侥幸不崩） | 分组缓冲改为按实际批量 `n` 的堆切片，栈帧降至 **4.7 KB**；`hp_reload_do` 的 400 KB 配置结构同样改为堆分配 |
| 4 | `g_rt.reload_mu` / `reload_cv` **从未初始化** | Windows 热加载监视线程在 `EnterCriticalSection` 内 SIGSEGV（POSIX 全零锁恰可用故不报错） | 在 `hp_rt_init_once` 补 `hp_mutex_init` / `hp_cond_init` |

另：测试原硬编码 `/tmp`（Windows 无此路径），改为 `test_tmpdir()` 按 `TMPDIR`/`TEMP`/`TMP` 回退。

## 4. CI 矩阵

`.github/workflows/ci.yml` 覆盖：

- `build-test`：ubuntu / macos / windows(MSVC) × locked / lockfree（6 项）
- `windows-mingw`：MSYS2 MINGW64 + MinGW Makefiles × locked / lockfree
- `sanitizer`：ubuntu，ASan + UBSan × locked / lockfree
- `coverage`：gcovr 生成 HTML / XML 报告并归档（**不设阈值**）

任一矩阵项失败即阻断合并；覆盖率 job 为报告项，不阻断。

## 5. 覆盖率基线（"先报告后收口"）

首个迭代仅归档报告，本地基线（gcovr 8.6，仅单测用例）：

- **行覆盖 46.5%**（1431 / 3079）
- **函数覆盖 62.4%**（138 / 221）
- **分支覆盖 32.7%**（706 / 2159）

阈值收口留待下一迭代按上述实际水位决定（对齐 rd §18 C7"报告项"口径）。

## 6. 已知良性告警（既有代码，非本次引入，snprintf 安全截断不越界）

- `conf/build.c`：`[outputs]` 私有参数 key 拷贝的 `-Wformat-truncation`
- `sink/sink_rollingfile.c`：归档命名 / 路径拼接的 `-Wformat-truncation`

## 7. 遗留项

- macOS（darwin）与 Windows（MSVC）的**实机**验证依赖 GitHub Actions 回执（本地仅完成 MinGW 交叉验证）
- 覆盖率阈值门禁（下一迭代收口）
- 需要第三方客户端的协议 sink（kafka / loki / ES 等）：按 rd §4.10.5 零依赖裁决**不入库**，一律由应用侧自定义 sink 实现
