# Aquila TUI Monitor

本文是 `monitor/` 方向的当前事实源。`gate_account_tui` 是独立、只读的 C++20/FTXUI terminal monitor；
它不参与策略执行、不修改账户状态、不读取交易系统进程内存。

## 当前范围

已实现 Symbol Workbench 与 market-data live path：

- 静态 demo snapshot、symbol/order/position/PnL/health layout。
- 显式 `--live-market-data` attach Gate/Binance `BookTicker` SHM。
- Interactive 与 bounded `--dump` snapshot。
- Optional SHM source 降级、changed-row batch、overrun/drop diagnostics。
- UI thread 与 `MarketDataThread` 间固定容量 SPSC queue。

订单、仓位、PnL 和 account health 仍是 demo 数据；当前不连接 Gate private order WebSocket、不查询 REST、不展示真实账户。
第一版 monitor 始终只读，不提供 cancel、flatten、pause strategy、cancel-all 或 close 操作。

## 运行方式

```bash
./build/debug/monitor/gate_account_tui
./build/debug/monitor/gate_account_tui --live-market-data
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
./build/debug/monitor/gate_account_tui --dump --view health --width 160 --height 40
./build/debug/monitor/gate_account_tui --dump --live-market-data \
  --market-data-config config/monitors/gate_account_tui_market_data.toml \
  --width 260 --height 60
```

`--live-market-data` 不启动 producer。Optional source 缺失时对应行显示 `NA` 并产生 alert；required/all source unavailable 时
live market data 标记 unavailable，但进程保留 UI。

## 依赖与所有权

依赖方向固定为：

```text
monitor/* -> core/* / exchange/* / config/*
```

`core/`、`exchange/`、`strategy/`、`tools/` 不反向依赖 monitor。TUI 不复用 `TradingRuntime` 或 `OrderManager`，也不抢读
交易系统 `OrderFeedbackShmReader` lane。全账户 monitor 必须保留 Aquila、external 和 manual/unknown order，不能只显示
`text=t-<local_order_id>`。

线程模型：

```text
UIThread
  FTXUI render/input
  own visible model
  drain SPSC updates

MarketDataThread
  drain BookTicker SHM
  coalesce latest rows
  push changed batch every 100ms

AccountMonitorThread (future)
  REST baseline/drift
  independent Gate private orders WS

LogBackendThread
```

Worker 不持 UI model mutex，不直接调用 FTXUI。Market-data queue full 可丢 latest batch并计数；account/order event 不能静默丢，
必须进入 stale/degraded。

## Market data contract

Interactive 使用 config 的 `latest + drain`；dump 强制 `earliest_visible + drain` 读取 visible window。按
`(exchange,symbol_id)` 和严格递增 `BookTicker.id` coalesce，100ms 只推 changed rows。

当前 `BookTicker` 提供 id、exchange/event/local timestamp 和 bid/ask price/volume。没有 last trade、24h volume 或 turnover；
这些字段必须显示 `NA`，不能用 bid/ask 伪造。后续应接 Trade/ticker SHM 或低频 REST 数据源。

## 代码与配置入口

| 入口 | 职责 |
| --- | --- |
| `monitor/tui/gate_account_tui.cpp` | CLI、dump/interactive、thread 接线 |
| `monitor/tui/symbol_workbench_view.h` | Workbench view |
| `monitor/tui/runtime_health_view.h` | Health view |
| `monitor/model/account_monitor_snapshot.h` | UI-owned snapshot |
| `monitor/model/monitor_spsc_queue.h` | Thread boundary |
| `monitor/market_data/market_data_thread.*` | SHM reader/coalescer |
| `monitor/market_data/market_data_store.h` | Latest BBO store |
| `config/monitors/gate_account_tui_market_data.toml` | Source config |

## Account monitor 后续设计

启动时用 read-only REST 建立 open orders、positions 和 account summary baseline；独立 private WS 订阅
`futures.orders` 增量。Monitor raw parser 必须保留原始 order scope 和 source：

| Source | 判定 |
| --- | --- |
| Aquila | `text=t-<local_order_id>` 可解析 |
| External | text 存在但非 Aquila |
| Manual/Unknown | text 为空或无法识别 |

目标模型：`MonitorOrderBook`、`PositionLedger`、`PnlLedger`、`AccountMonitorModel`。REST drift 只标红，不自动修正交易系统：
unknown order、missing terminal、position mismatch、stale snapshot 或 query failure 都保持可见。

PnL 权威口径、`futures.orders` 是否足够重建 fee/fill delta、是否补 `futures.usertrades`、metadata 来源和 Trade/ticker feed
仍需在实现前确认。任何账户操作需要独立安全设计与用户批准。

## 验证

```bash
cmake --build build/debug --target \
  gate_account_tui \
  monitor_symbol_workbench_demo_data_test \
  monitor_symbol_workbench_view_test \
  monitor_market_data_view_model_test \
  monitor_market_data_store_test \
  monitor_spsc_queue_test \
  monitor_market_data_thread_test -j8
ctest --test-dir build/debug -R monitor_ --output-on-failure
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
```

下一步依次是 raw order parser fixture、REST snapshot、account ledgers、独立 private session 和只读 live smoke。
