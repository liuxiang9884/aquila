# Multi-Feed Data Fusion Design

## 目标

`gate_data_fusion` / `binance_data_fusion` 支持在一个进程内启动多个行情类型的 fusion。`source` 仍表示一个 data session 副本；每个 source worker 根据配置订阅启用的 feed，并发布到对应 source SHM channel。每个 fusion thread 只处理一个 feed type。

## 已确认决策

- `launch config` 是进程拓扑事实源，决定启用哪些 feed、启动多少 source worker、每个 feed 使用哪个 fusion config。
- `fusion config` 继续保持单 feed：只描述该 feed 的 source channel、canonical output、metadata 和 poll 参数。
- `source` 等于一个 data session/source worker，不等于一个 feed。
- 当前只实例化 `book_ticker` 和 `trade`，实现结构按 feed traits 组织，后续新增 feed 时新增 traits 和 thread alias。
- 单 feed 启动时 source SHM 只创建该 feed 对应 channel；多 feed 启动时 source SHM 创建多个启用 channel。
- 每个 enabled feed 启动一个 fusion thread；不把 `book_ticker` 和 `trade` 放到同一个 fusion loop。
- 任一 source worker 或 fusion thread 初始化/运行失败时，整个 data fusion 进程 fail-fast 停止并返回非 0。
- 多个低延迟线程的 CPU binding 默认不允许冲突，包括 source worker、fusion thread 和 log backend。

## 线程模型

`N` 个 source、`M` 个 feed 时，应用线程数为：

```text
N source worker threads
M fusion threads
1 backend log thread
```

例如 `N=4`、`feeds=["book_ticker", "trade"]`：

```text
4 data session threads + 2 fusion threads + 1 log thread = 7 threads
```

## 配置模型

新 launch schema：

```toml
[launch]
name = "gate_data_fusion_book_ticker_trade_4sources"
feeds = ["book_ticker", "trade"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/gate_book_ticker_fusion_4sources.toml"
trade = "config/market_data_fusion/gate_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_source_0"
data_shm_name = "aquila_gate_md_src_0"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
```

Single-feed launch 也使用统一 schema：`launch.feeds = ["book_ticker"]` 或 `["trade"]`，并在 `[launch.fusion_configs]` 中只配置对应 feed。旧 `feed` / `fusion_config` / `book_ticker_shm_name` / `trade_shm_name` launch schema 不再保留。

## 数据流

```text
source worker 0
  DataSession(feeds)
  DataShmPublisher(enabled_channels)
    -> source0.book_ticker_channel
    -> source0.trade_channel

BookTickerFusionThread
  -> read source*.book_ticker_channel
  -> publish canonical book_ticker SHM

TradeFusionThread
  -> read source*.trade_channel
  -> publish canonical trade SHM
```

`DataShmConfig` 增加 per-channel enabled 状态。`DataShmManager` 只构造 enabled channel；reader attach 未启用或不存在的 channel 应失败，避免误以为单 feed SHM 中存在另一类数据。

## 验证

- TDD 覆盖：
  - Gate / Binance launch config 解析 multi-feed schema。
  - duplicate feed、missing fusion config、CPU binding 冲突报错。
  - source override 根据 enabled feed 设置 `DataSessionConfig.feeds` 和 `DataShmConfig` channel enabled 状态。
  - `DataShmPublisher(DataShmConfig)` 单 feed 时只创建对应 channel。
- Focused build/test：

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_data_shm_test \
  gate_data_fusion_config_test \
  binance_data_fusion_config_test \
  data_fusion_tool_support_test \
  -j8
ctest --test-dir build/debug -R '(core_market_data_data_shm|gate_data_fusion_config|binance_data_fusion_config|data_fusion_tool_support)' --output-on-failure
```

- Fusion 性能回归：

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target market_data_fusion_fastest_route_benchmark -j8
taskset -c 16 ./build/release/benchmark/core/market_data/market_data_fusion_fastest_route_benchmark --benchmark_out=/home/liuxiang/tmp/fusion_refactor_perf/multi_feed_bundle.json --benchmark_out_format=json
```
