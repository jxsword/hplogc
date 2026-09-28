# hplogc Phase 1 验收摘要

> 生成日期：2026-09-29
> 范围：Phase 1（Linux 全功能）实现验收，对应任务清单第 2 项（阶段1验收：矩阵 + tag + 推送 + 摘要）。

## 1. 交付范围

| 任务 | 内容 | 提交 |
|------|------|------|
| 1 | rd 澄清条款 G1-G6 + 门禁口径 | `docs/rd_v0.2.md` §18.1 |
| 3 | CMake 骨架 + 平台契约（公共头 / 平台抽象层） | `CMakeLists.txt`、`cmake/`、`src/platform/`、`include/hplogc.h` |
| 4 | ring 有锁 / 无锁两套实现 | `src/ring/` |
| 5 | INI 解析器 | `src/conf/ini.c` |
| 6 | core 初始化 / 统计 + sink 注册表 | `src/core/core.c`、`src/sink/` |
| 7 | 路由 + 日志管线 + 异步消费者 | `src/core/route.c`、`format.c`、`async.c`、`examples/` |
| 8 | signal_safe / fork / 热加载 | `src/core/signal.c`、`reload.c`、平台 watcher |
| 9 | 测试体系 + CI | `tests/`、` .github/workflows/ci.yml` |

## 2. 本地验收（门禁对照）

| 门禁 | 结果 | 证据 |
|------|------|------|
| G1 恰好一次（2000 条 / 1 MiB / DISCARD） | 通过 | 日志行数 == accepted == written == 2000，dropped == 0，无空行 / 无重复 |
| G2 消费者不漏取（lockfree） | 通过 | `hp_async_stop` 自旋排干，`written == accepted`，无 ring 残留 |
| G3 无虚假丢弃 | 通过 | 缓冲调大后 dropped 仅由真实满引发 |
| G4/G5 配置解析 | 通过 | `rules` 箭头语法、`formats.*`/`outputs.*` 前缀节解析正确；坏配置报 `HPLOGC_ERR_CONFIG` |
| G6 stats 生命周期 | 通过 | 未 init 调用 `get_stats` 返回 `HPLOGC_ERR_STATE` 且不写 `*stats` |
| ctest（locked / lockfree） | 通过 | 3/3 用例全绿（config / ring / smoke） |
| ASan + UBSan（locked / lockfree） | 通过 | 运行期零内存错误、零 UB 报告 |

## 3. CI 矩阵

`.github/workflows/ci.yml` 覆盖：

- 平台：`ubuntu-latest` × `macos-latest`
- ring：`locked` × `lockfree`
- sanitizer：`ubuntu-latest` 下 ASan + UBSan（locked / lockfree）

任一矩阵项失败即阻断合并。

## 4. 已知良性告警（既有代码，非本次引入，snprintf 安全截断不越界）

- `conf/build.c`：`[outputs]` 私有参数 key 拷贝的 `-Wformat-truncation`
- `sink/sink_rollingfile.c`：归档命名 / 路径拼接的 `-Wformat-truncation`

## 5. 遗留项（Phase 2/3）

- Windows / macOS 平台适配（Phase 2/3）
- 网络型 sink（kafka / loki / ES 等）骨架实现
- 覆盖率门禁接入（gcov / lcov）
