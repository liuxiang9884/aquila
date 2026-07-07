# Market Data Fusion 目录迁移设计

## 目标

把 `core/market_data` 下的 fusion 相关文件集中到 `core/market_data/fusion/`，同时合并只包含别名或轻量
traits 的小文件，减少一级目录噪音并让 BookTicker / Trade fusion 的公共实现边界更清楚。

本轮采用激进迁移：不保留旧 include forwarding headers；active code、测试、benchmark 和 active docs 全部
改到新路径。保留现有 public C++ 类型名，不做语义 rename。

## 当前事实

- `core/market_data` 一级目录同时放了 data reader、SHM、typed binary format 和 fusion 文件；fusion 文件已经
  占据约 19 个 header。
- `book_ticker_fusion_config.h` / `trade_fusion_config.h`、`*_metadata.h`、`*_thread.h` 多数只是十几行 alias。
- `book_ticker_fusion_metadata_policy.h` / `trade_fusion_metadata_policy.h` 是轻量 metadata traits + policy alias。
- `book_ticker_fusion.h` / `trade_fusion.h` 包含 feed-specific core facade；`*_runner.h` 包含 feed-specific runner
  traits / alias。
- `fastest_route_fusion*.h`、`fusion_metadata_writer.h`、`fusion_metadata_policy.h` 是公共实现；最终按
  `fastest_route.h`、`metadata.h`、`thread.h` 聚合，不塞进 feed facade。
- 当前测试和 benchmark 也散在 `test/core/market_data/` 与 `benchmark/core/market_data/` 一级目录。

## Locked Decisions

- 不保留旧 include 路径。迁移后 `#include "core/market_data/book_ticker_fusion.h"` 这类路径在 active code 中
  不再存在。
- public C++ 类型名保持不变，例如 `BookTickerFusionCore`、`BookTickerFusionRunner`、
  `FastestRouteFusionDecision` 不改名。
- TOML schema、CLI 参数、SHM ABI、metadata ABI、runner hot path 语义不改变。
- 目录内文件名去掉重复 `fusion_` 前缀；`fastest_route` 保留，因为它是 fusion 策略语义。
- 合并纯 alias / 轻量 traits，并进一步按类型合并公共 runner loop、thread wrapper、metadata writer / policy；
  不改变 runtime 语义。
- 测试和 benchmark 同步迁移到 `fusion/` 子目录。
- 测试 / benchmark target 名也同步改为新目录语义，例如 `core_market_data_fusion_fastest_route_test` 和
  `market_data_fusion_fastest_route_benchmark`。

## 目标生产目录

目标结构：

```text
core/market_data/fusion/
  book_ticker.h
  config.h
  fastest_route.h
  metadata.h
  thread.h
  trade.h
```

合并规则：

- `fusion/config.h`
  - `FusionSourceConfig`
  - `FusionOutputConfig`
  - `BasicFusionConfig`
  - `BookTickerFusionSourceConfig`
  - `BookTickerFusionOutputConfig`
  - `BookTickerFusionConfig`
  - `TradeFusionSourceConfig`
  - `TradeFusionOutputConfig`
  - `TradeFusionConfig`
- `fusion/metadata.h`
  - `FusionMetadataRecord`
  - `FusionMetadataWriter`
  - `BasicFusionMetadataWriter`
  - `FileFusionMetadataPolicy`
  - `NoopFusionMetadataPolicy`
  - `BookTickerFusionMetadataRecord`
  - `BookTickerFusionMetadataWriter`
  - `TradeFusionMetadataRecord`
  - `TradeFusionMetadataWriter`
- `fusion/thread.h`
  - `BookTickerFusionThreadStats`
  - `BookTickerFusionThread`
  - `TradeFusionThreadStats`
  - `TradeFusionThread`
- `fusion/book_ticker.h`
  - `BookTickerFusionTraits`
  - `BookTickerFusionCore`
  - `BookTickerFusionDecision`
  - `BookTickerFusionRunnerTraits`
  - `BasicBookTickerFusionRunner`
  - `BookTickerFusionPollStats`
  - `BookTickerFusionRunner`
  - `BookTickerFusionMetadataTraits`
  - `FileBookTickerFusionMetadataPolicy`
  - `NoopBookTickerFusionMetadataPolicy`
  - `DefaultBookTickerFusionMetadataPolicy`
- `fusion/trade.h`
  - `TradeFusionTraits`
  - `TradeFusionCore`
  - `TradeFusionDecision`
  - `TradeFusionRunnerTraits`
  - `BasicTradeFusionRunner`
  - `TradeFusionPollStats`
  - `TradeFusionRunner`
  - `TradeFusionMetadataTraits`
  - `FileTradeFusionMetadataPolicy`
  - `NoopTradeFusionMetadataPolicy`
  - `DefaultTradeFusionMetadataPolicy`

公共实现文件：

- `fusion/fastest_route.h`
- `fusion/fastest_route_runner.h`
- `fusion/fastest_route_thread.h`
- `fusion/metadata_policy.h`
- `fusion/metadata_writer.h`

删除的旧生产 header：

```text
core/market_data/book_ticker_fusion.h
core/market_data/book_ticker_fusion_config.h
core/market_data/book_ticker_fusion_metadata.h
core/market_data/book_ticker_fusion_metadata_policy.h
core/market_data/book_ticker_fusion_runner.h
core/market_data/book_ticker_fusion_thread.h
core/market_data/fastest_route_fusion.h
core/market_data/fastest_route_fusion_runner.h
core/market_data/fastest_route_fusion_thread.h
core/market_data/fusion_config.h
core/market_data/fusion_metadata.h
core/market_data/fusion_metadata_policy.h
core/market_data/fusion_metadata_writer.h
core/market_data/trade_fusion.h
core/market_data/trade_fusion_config.h
core/market_data/trade_fusion_metadata.h
core/market_data/trade_fusion_metadata_policy.h
core/market_data/trade_fusion_runner.h
core/market_data/trade_fusion_thread.h
```

## 测试和 Benchmark

测试迁移：

```text
test/core/market_data/fusion/
  book_ticker_metadata_test.cpp
  book_ticker_runner_test.cpp
  book_ticker_test.cpp
  book_ticker_thread_test.cpp
  fastest_route_test.cpp
  metadata_policy_test.cpp
  trade_metadata_test.cpp
  trade_runner_test.cpp
  trade_test.cpp
  trade_thread_test.cpp
```

测试 target rename：

```text
core_market_data_fastest_route_fusion_test
  -> core_market_data_fusion_fastest_route_test
core_market_data_book_ticker_fusion_test
  -> core_market_data_fusion_book_ticker_test
core_market_data_book_ticker_fusion_metadata_test
  -> core_market_data_fusion_book_ticker_metadata_test
core_market_data_fusion_metadata_policy_test
  -> core_market_data_fusion_metadata_policy_test
core_market_data_trade_fusion_metadata_test
  -> core_market_data_fusion_trade_metadata_test
core_market_data_trade_fusion_test
  -> core_market_data_fusion_trade_test
core_market_data_book_ticker_fusion_runner_test
  -> core_market_data_fusion_book_ticker_runner_test
core_market_data_trade_fusion_runner_test
  -> core_market_data_fusion_trade_runner_test
core_market_data_book_ticker_fusion_static_review
  -> core_market_data_fusion_book_ticker_static_review
core_market_data_book_ticker_fusion_thread_test
  -> core_market_data_fusion_book_ticker_thread_test
core_market_data_trade_fusion_thread_test
  -> core_market_data_fusion_trade_thread_test
```

Benchmark 迁移：

```text
benchmark/core/market_data/fastest_route_fusion_benchmark.cpp
  -> benchmark/core/market_data/fusion/fastest_route_benchmark.cpp
```

Benchmark target rename：

```text
fastest_route_fusion_benchmark
  -> market_data_fusion_fastest_route_benchmark
```

## Include 迁移

active code 应统一使用新路径：

```cpp
#include "core/market_data/fusion/book_ticker.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/fastest_route.h"
#include "core/market_data/fusion/metadata.h"
#include "core/market_data/fusion/thread.h"
#include "core/market_data/fusion/trade.h"
```

调用方选择最窄可用 header：

- 只需要 config 类型时 include `fusion/config.h`。
- 只需要 thread alias 时 include `fusion/thread.h`。
- 需要 core / runner / metadata policy 时 include `fusion/book_ticker.h` 或 `fusion/trade.h`。
- 测试公共 core 时 include `fusion/fastest_route.h`。

## CMake 和文档

需要同步更新：

- `core/market_data/CMakeLists.txt`
- `test/core/market_data/CMakeLists.txt`
- `benchmark/core/market_data/CMakeLists.txt`
- `core/config/fusion_config_parser.h` 和 config wrapper include。
- `tools/market_data/fusion_cli.h`、`data_fusion_tool_support.h`、Gate / Binance data fusion tools。
- active docs 中的代码入口和 benchmark 命令，尤其是 `docs/project_onboarding_guide.md`。

历史 `docs/superpowers/**` 中记录旧路径的已提交计划 / spec 不作为 active cleanup 阻断。

## 不改变的行为

- 不改变 `BookTickerFusionCore::OnBookTicker()` / `TradeFusionCore::OnTrade()` 行为。
- 不改变 `BasicFastestRouteFusionRunner::PollOnce()`。
- 不改变 metadata record size、字段顺序或写入策略。
- 不改变 data fusion dry-run / summary log schema。
- 不改变 standalone fusion CLI binary 名称、CLI 参数或 config schema。
- 不改变 public C++ 类型名。

## 验证

最低功能验证：

```bash
cmake --build build/debug --target \
  core_market_data_fusion_fastest_route_test \
  core_market_data_fusion_book_ticker_test \
  core_market_data_fusion_book_ticker_metadata_test \
  core_market_data_fusion_metadata_policy_test \
  core_market_data_fusion_trade_metadata_test \
  core_market_data_fusion_trade_test \
  core_market_data_fusion_book_ticker_runner_test \
  core_market_data_fusion_trade_runner_test \
  core_market_data_fusion_book_ticker_thread_test \
  core_market_data_fusion_trade_thread_test \
  gate_data_fusion binance_data_fusion \
  gate_book_ticker_fusion binance_book_ticker_fusion \
  gate_trade_fusion binance_trade_fusion -j

ctest --test-dir build/debug -R 'core_market_data_fusion|fusion_config|data_fusion_tool_support|fusion_cli_traits|book_ticker_fusion_cli|trade_fusion_cli' --output-on-failure
```

旧路径清理检查：

```bash
rg '#include "core/market_data/(book_ticker_fusion|trade_fusion|fastest_route_fusion|fusion_)' core tools test benchmark
find core/market_data -maxdepth 1 -type f -name '*fusion*'
```

上述命令期望无 active code 命中；`core/market_data/fusion/` 下的新文件除外。

性能验证：

```bash
cmake --build build/release --target market_data_fusion_fastest_route_benchmark -j
build/release/benchmark/core/market_data/market_data_fusion_fastest_route_benchmark \
  --benchmark_repetitions=5 \
  --benchmark_format=json \
  --benchmark_out=/home/liuxiang/tmp/fusion_refactor_perf/directory_migration.json
```

和 `/home/liuxiang/tmp/fusion_refactor_perf/config_tooling_unification.json` 对比。因为本轮应是 include / 文件组织
迁移，任何 core / runner benchmark 可复现退化都需要先调查，不能直接宣称性能无损。
