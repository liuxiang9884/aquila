# Gate Account TUI 设计

## 目标

`gate_account_tui` 是独立的只读 terminal monitor，用于观察 Gate futures 账户、行情、订单、仓位、PnL 和健康状态。它不参与策略执行，不修改账户状态，不读取交易系统内存状态。

当前已落地的是 Symbol Workbench / market data monitor demo；完整账户 monitor 仍未接真实 order / position / PnL 数据。

## 当前状态

已落地：

- 顶层目录 `monitor/`。
- `gate_account_tui` 入口。
- FTXUI Symbol Workbench / health view。
- demo snapshot model。
- monitor 专用 SPSC queue。
- Gate / Binance `BookTicker` SHM reader thread。
- optional SHM source fallback。
- `--dump` 和 interactive TUI。
- `--live-market-data` 只读已有 data session SHM。

未落地：

- monitor 专用 Gate `futures.orders` raw parser。
- REST snapshot client。
- account / position / PnL model。
- 真实 health sampler。
- 任何交易操作。

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

`--live-market-data` 不启动 data session。若 Gate / Binance SHM 不存在，界面显示 `NA` 并产生 alert。

## 代码入口

| 文件 | 职责 |
| --- | --- |
| `monitor/tui/gate_account_tui.cpp` | CLI、dump / interactive 入口、market data thread 接线。 |
| `monitor/tui/symbol_workbench_view.h` | Symbol Workbench 渲染。 |
| `monitor/tui/runtime_health_view.h` | health view 渲染。 |
| `monitor/model/account_monitor_snapshot.h` | UI snapshot 数据结构。 |
| `monitor/demo/symbol_workbench_demo_data.*` | demo snapshot。 |
| `monitor/model/monitor_spsc_queue.h` | monitor 线程间 SPSC queue。 |
| `monitor/market_data/market_data_thread.*` | monitor 专用 market data reader。 |
| `config/monitors/gate_account_tui_market_data.toml` | monitor market data reader config。 |

依赖方向：

```text
monitor/* -> core/* / exchange/* / config/*
```

禁止生产链路反向依赖 `monitor/`。

## Market Data

TUI 不直接连接行情 WebSocket；它 attach 已存在的 Gate / Binance `BookTicker` SHM。

当前真实字段来自 `BookTicker`：

```text
id, symbol_id, exchange, exchange_ns, local_ns,
bid_price, bid_volume, ask_price, ask_volume
```

因此下列字段当前显示 `NA`：

- `last_price`
- 最新成交量
- 24h volume
- turnover / value

不要用 bid / ask 伪造这些字段。后续如需要，应新增 trade / ticker feed SHM，或让 TUI 低频 REST ticker 补充。

`MarketDataThread` 行为：

- 构造时 attach source。
- optional source attach 失败时跳过并显示 warning。
- required source attach 失败时 live market data unavailable。
- interactive path 使用 config 中的 `latest + drain`。
- dump snapshot path 强制 `earliest_visible + drain`，读取 visible window 并 coalesce 一帧。
- 每 100ms 推送 changed rows 或 diagnostics-only batch。

## 未来 Account Monitor 数据源

### REST Snapshot

启动期需要 read-only REST snapshot：

- open orders。
- positions。
- account summary / balance。
- 可选 recent finished orders。

用途：

- 建立初始订单和仓位基线。
- 识别 TUI 启动前已存在的外部风险。
- 为 private WS 增量提供 drift 对账基线。

### Gate Orders WS

TUI 应建立独立 private WS 连接订阅 `futures.orders`。它不抢读交易系统 `OrderFeedbackShmReader` lane，也不把交易系统 `OrderFeedbackEvent` 当主事件。

建议新增 monitor 专用 update：

```text
Gate futures.orders raw payload
  -> monitor raw parser
  -> GateMonitorOrderUpdate
  -> MonitorOrderBook / PositionLedger / PnlLedger
  -> UI snapshot
```

必须保留 source classification：

| 类别 | 判定 |
| --- | --- |
| Aquila | `text` 可解析为 `t-<local_order_id>`。 |
| External | `text` 存在但不是 Aquila 格式。 |
| Manual / Unknown | `text` 为空或无法识别。 |

### REST Drift Check

低频 REST 校验只标红和展示原因，不自动修正交易系统：

- REST open orders 中存在 TUI 未见订单。
- TUI open order 在 REST 中不存在且无 terminal update。
- REST position 与 TUI ledger 不一致。
- snapshot stale 或查询失败。

## 状态模型

| 模型 | 职责 | 边界 |
| --- | --- | --- |
| `MonitorOrderBook` | 管理订单快照、source、per-symbol open count、最近订单。 | 不下单、不撤单、不推进交易系统 `OrderManager`。 |
| `PositionLedger` | 从 REST 初始化，从 WS fill delta 更新 signed position，并对 REST drift。 | 无可靠成交 delta 时只能标记 stale。 |
| `PnlLedger` | 展示 realized / unrealized / fee / total PnL。 | 缺 mark price 或 fill delta 时显示 incomplete / estimated。 |
| `AccountMonitorModel` | 汇总连接、snapshot、symbol model 和事件流，为 UI 提供只读快照。 | UI thread 是 visible model owner。 |

## 并发模型

```text
UIThread
  FTXUI render / input
  drain market_data_spsc / account_spsc
  own visible UiModel

MarketDataThread
  RealtimeDataReader drain BookTicker SHM
  coalesce latest rows
  push MarketDataBatch every 100ms

AccountMonitorThread (future)
  REST snapshot
  Gate futures.orders WS
  publish order / position / PnL deltas

LogBackendThread
  Nova logging backend
```

约束：

- worker thread 不持 UI model mutex，不直接操作 FTXUI。
- market data 是 latest-BBO 语义，queue full 时可丢 batch 并计数。
- account / order update 不能静默丢；queue full 应进入 degraded / stale。

## UI 范围

当前首页是 Symbol Workbench：

- symbol 列表。
- market data rows。
- order / position / PnL demo panes。
- events / health alerts。

未来页面：

- `Overview`
- `Orders`
- `Positions`
- `Events`
- `Health`

第一版不得启用 cancel、flatten、pause strategy、cancel-all 或 reduce-only close 等操作按钮。

## 测试

当前 smoke：

```bash
cmake --build build/debug --target gate_account_tui monitor_symbol_workbench_demo_data_test monitor_symbol_workbench_view_test monitor_market_data_view_model_test monitor_market_data_store_test monitor_spsc_queue_test monitor_market_data_thread_test
ctest --test-dir build/debug -R monitor_ --output-on-failure
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
```

后续 account monitor 测试：

- Gate order raw parser fixture。
- source classification。
- REST snapshot fixture。
- position / PnL ledger。
- WS + REST drift detector。
- queue overflow / stale semantics。

## 下一步

1. 实现 monitor 专用 Gate orders raw parser 和 fixture tests。
2. 实现 read-only REST snapshot client。
3. 实现 `MonitorOrderBook`、`PositionLedger`、`PnlLedger`。
4. 接入 independent Gate account monitor session。
5. 增加真实账户 live smoke runbook，只读验证，不做任何交易操作。
