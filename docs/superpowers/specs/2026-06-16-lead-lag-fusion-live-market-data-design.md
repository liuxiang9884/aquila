# LeadLag fusion 实盘行情设计

## 背景

当前 LeadLag 30-symbol live strategy 通过 `strategy.data_reader` 直接读取 Gate / Binance 原始 `BookTicker` SHM。Gate / Binance fastest-route fusion 已实现 threaded bundle，canonical output 仍保持现有 `BookTicker` ABI，`exchange` 字段保留原交易所，`local_ns` 表示 fusion publish time。

2026-06-16 的 32-symbol shadow 准备过程中，Gate private plain `futures.book_ticker` 对 `TON_USDT` 返回 `unknown currency pair TON_USDT`，导致整批订阅失败。去掉 `TON_USDT` 后，31-symbol 合并订阅在 Gate private plain 上返回 `success` 并能收到二进制 BBO。因此本轮实盘 fusion 配置以 31 个有效 symbol 为边界：原 30-symbol 集合删除 `TON_USDT`，加入 `BTC_USDT` 和 `ETH_USDT`。

## 目标

新增一套可用于 LeadLag 真实订单 smoke 的 fusion market-data 配置，使策略消费 Gate / Binance canonical fusion SHM，而不是单路原始 data session SHM。

## 非目标

- 不修改 LeadLag strategy C++ 热路径。
- 不把 strategy 直接改成多 source reader。
- 不在本轮实现 fusion process health 自动 stop-and-flat 代码；先在 live operations pipeline 中明确人工 / guard 编排要求。
- 不继续尝试交易或订阅 `TON_USDT`。

## 设计

新增 `31symbols_no_ton_fusion` 配置族：

- Gate / Binance data fusion launch config 各启动 `N=4` source data session thread、1 个 fusion thread 和一个 log backend。
- Fusion source SHM 使用独立命名，canonical SHM 使用稳定命名供 strategy data reader 使用。
- Fusion metadata mode 仍要求 release build 使用 `AQUILA_BOOK_TICKER_FUSION_METADATA_MODE=file`。
- Data session build 使用 `AQUILA_DATA_SESSION_DIAG_LEVEL=0`，避免 L4 diagnostics 干扰实盘 hot path。
- Strategy runtime config 仅把 `[strategy.data_reader].config` 切到 fusion data reader，其它 order session / feedback / execution 配置保持现有 30-symbol private plain 路径。
- Nested LeadLag pair config 删除 `TON_USDT`，新增 `BTC_USDT` / `ETH_USDT` pair。新增 pair 使用和现有 30-symbol 配置一致的 trigger、freshness、capacity 字段，`open_notional=20.0`、`open_slippage=2`、`close_slippage=2` 作为保守 smoke 默认。

## 启动顺序

真实订单 smoke 前必须先启动并检查 fusion：

1. 启动 `gate_data_fusion` 和 `binance_data_fusion`。
2. 确认 Gate / Binance canonical fusion SHM 都有数据，metadata 文件增长。
3. 用 recorder 或 probe 验证 canonical stream 的 `skipped` / `overruns` 为 0，且 `TON_USDT` 不在本轮 symbol 集合中。
4. 再启动 `lead_lag_strategy` guarded run，策略读取 fusion data reader。

## 验证

配置落地后必须至少运行：

```bash
./build/release/tools/gate_data_fusion --config config/market_data_fusion/gate_data_fusion_31symbols_no_ton_book_ticker_4sources.toml
./build/release/tools/binance_data_fusion --config config/market_data_fusion/binance_data_fusion_31symbols_no_ton_book_ticker_4sources.toml
./build/release/tools/lead_lag_strategy --config config/strategies/lead_lag_31symbols_no_ton_fusion_live_strategy_20260616.toml
ctest --test-dir build/debug -R '(book_ticker_fusion|data_session_config|data_reader_config|lead_lag_config|live_strategy)' --output-on-failure
git diff --check
```

dry-run 的 `lead_lag_strategy` 只解析配置，不连接 WebSocket、不打开 SHM、不提交订单；真实订单仍必须走 `scripts/lead_lag/run_live_with_guard.py` pipeline。
