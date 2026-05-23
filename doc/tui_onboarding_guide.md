# Aquila TUI Onboarding

## 目的

本文是接手 Aquila TUI / monitor 方向时的入口文档。它只记录当前已确认的范围、边界、文档索引、代码入口建议和下一步，不替代详细设计。

## 当前决策

- 第一版 TUI 是运行在 terminal 中的 C++20 程序，推荐使用 FTXUI。
- TUI 放在独立顶层目录 `monitor/`，不放进 `tools/gate/`、`strategy/` 或交易 runtime。
- TUI 可复用 `core/`、`exchange/gate/`、`config/` 中已有的 WebSocket、Gate 登录、SBE schema / dispatch、配置和日志代码。
- 依赖方向必须单向：`monitor/* -> core/exchange/config`；`core/`、`exchange/`、`strategy/`、`tools/` 不反向依赖 `monitor/`。
- 第一版是只读监控，不执行撤单、平仓、暂停策略或修改配置；但 UI 和模型保留后续操作入口位置。
- TUI 不接入 `TradingRuntime`，不读取 `OrderManager` 内存状态，不消费交易系统的 order feedback SHM lane。
- Market data 第一版直接读取现有 Gate / Binance data session 发布的 `BookTicker` SHM；这是 broadcast queue，多 reader 独立 cursor，不会抢 strategy 的行情数据。
- TUI 不自动启动 Gate / Binance data session；optional market data SHM 缺失时跳过对应 source，行情显示 `NA`，并在 alert 中提示对应 SHM unavailable；required source 缺失或全部 source 不可用时 live market data 标为 unavailable，但 TUI 进程继续运行。
- 第一版跨线程交互只用 SPSC queue，不用 mutex 共享 UI model；UI thread 是 visible model 的唯一 owner。
- 完整第一版目标线程拆分为 UI thread、MarketDataThread、AccountMonitorThread 和 Nova log backend thread；当前代码已落地 UI thread 和显式 `--live-market-data` 下的 MarketDataThread，order 和 health 后续暂放同一个 AccountMonitorThread。
- MarketDataThread 使用 `drain` 模式读 SHM，在本线程内按严格单调的 Gate / Binance `BookTicker.id` coalesce 最新值，每 100ms 只把 changed rows 推给 UI。
- 监控对象是单一 Gate futures 账户的全账户订单 / 仓位 / PnL，而不是只看 Aquila 自己创建的订单。
- Aquila 订单通过 `text = t-<local_order_id>` 识别并标记 strategy id / local order id；外部程序订单、手工订单、未知 text 订单必须保留可见。
- 首页布局采用 Symbol Workbench：左侧 symbol 列表，中间当前 symbol 订单表，右侧仓位和 PnL，底部或副页展示事件流和连接状态。
- TUI 以键盘优先设计，同时支持鼠标选择；鼠标不能成为唯一操作路径。

## 当前 Demo / Market Data 状态

截至 2026-05-22，Symbol Workbench demo 和 market data live path 已落地。默认无参数仍显示静态 layout demo；加 `--live-market-data` 后会 attach `config/monitors/gate_account_tui_market_data.toml` 中的 Gate / Binance `BookTicker` SHM；`--dump --live-market-data` 会从 SHM visible window 做一次 bounded live snapshot 并渲染一帧，适合 SSH 环境检查：

```bash
./build/debug/monitor/gate_account_tui
./build/debug/monitor/gate_account_tui --live-market-data
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
```

当前订单、仓位、PnL、health 仍是静态 demo 数据，不连接 Gate private order WebSocket、不查询 REST、不显示真实账户数据。market data 只读取既有 data session SHM；若 data session 未启动，Gate / Binance 行情行显示 `NA` 并在 alert 中提示 unavailable。optional source attach 失败不会让整个 TUI 退出；required source 或全部 source 不可用时 live market data 标记为 unavailable，但进程仍保留静态 UI。目标 11 个 symbol：

```text
PROVE_USDT, RAVE_USDT, ZEC_USDT, SIREN_USDT, ETC_USDT, DASH_USDT,
RIVER_USDT, SUI_USDT, INJ_USDT, ENA_USDT, BRETT_USDT
```

默认选中 `ZEC_USDT`，展示 Aquila / Manual / External 三类订单、仓位 / PnL、source mix、health 和事件流。

当前 market data 行为边界：

- interactive live path 使用 config 中的 `start_position = latest` 和 `read_mode = drain`。
- dump snapshot path 强制 `earliest_visible + drain`，用于读取当前 SHM visible window 中已经存在的行。
- Gate / Binance `BookTicker.id` 都按 source 严格单调；reader thread 只在 id 变化时更新 latest row。
- 每 100ms 只向 UI 推 changed rows；reader overrun、UI dropped batch 和 unavailable source 会进入 alert。
- `BookTicker` SHM 当前没有 `last_price`、最新成交量、24h volume、turnover / value，这些字段显示 `NA`，不要用 bid / ask 伪造。

## 文档索引

| 文档 | 什么时候读 | 内容 |
| --- | --- | --- |
| `doc/tui_gate_account_monitor_design.md` | 继续设计或开始实现 TUI | 第一版 Gate account TUI 的详细架构、组件、数据流、错误处理和测试建议 |
| `doc/project_onboarding_guide.md` | 新对话总体接手 | 项目当前事实源、代码入口和全局下一步 |
| `doc/agent-handoff-gate-trade-architecture.md` | 复用 Gate private WS / SBE / order feedback 代码前 | Gate 交易 WebSocket、private feedback、SHM 和线程模型边界 |
| `doc/strategy_order_component_model.md` | 判断 TUI 是否能复用交易组件时 | `OrderManager`、`OrderFeedbackSession`、`TradingRuntime` 的职责边界 |
| `doc/lead_lag_reconcile_design.md` | 处理 REST snapshot / drift / manual intervention 语义时 | read-only reconcile、REST 事实校验和恢复边界 |
| `doc/futures_contract_metadata_fields.md` | 计算合约数量、notional、PnL 单位时 | Gate / Binance futures metadata 字段和单位差异 |

## 当前实现可复用点

- `monitor/model/account_monitor_snapshot.h`：TUI 静态 view model。
- `monitor/demo/symbol_workbench_demo_data.*`：当前 Symbol Workbench fake data source。
- `monitor/tui/symbol_workbench_view.h`：FTXUI Symbol Workbench 布局。
- `monitor/tui/gate_account_tui.cpp`：当前 demo / live market data 入口，支持 interactive、`--dump`、`--live-market-data` 和 `--market-data-config`。
- `monitor/market_data/market_data_thread.*`：monitor 专用 market data reader thread，支持 optional SHM source 降级、100ms changed batch 和 one-shot snapshot。
- `monitor/market_data/market_data_store.h` / `monitor/market_data/market_data_update.h`：按 `(exchange, symbol_id)` coalesce latest BBO，输出固定容量 batch 和 diagnostics。
- `monitor/model/market_data_view_model.h`：将 `MarketDataBatch` 转成 UI-owned market rows；`last_price`、volume、turnover 暂显示 `NA`。
- `core/market_data/data_shm.h`：`BookTickerShmReader` / `DataShmPublisher`，market data SHM broadcast queue。
- `config/monitors/gate_account_tui_market_data.toml`：monitor 专用 Gate + Binance SHM reader 配置，读取当前 requested data session SHM。
- `config/data_sessions/gate_data_session_requested_20260521.toml` 和 `config/data_sessions/binance_data_session_requested_20260521.toml`：requested 12-symbol data session producer 配置，当前已追加 `ETH_USDT`。
- `core/websocket/*`：WebSocket cold / hot path、TLS socket、message view、runtime policy。
- `exchange/gate/trading/order_feedback_session.h`：Gate private WS login / subscribe / connection lifecycle 参考实现。
- `exchange/gate/sbe/message_dispatcher.h` 和 `exchange/gate/sbe/generated/`：Gate SBE schema dispatch 和生成代码。
- `exchange/gate/trading/order_feedback_parser.h`：交易系统窄 feedback parser 的字段位置和 decimal 转换参考。
- `exchange/gate/trading/order_codecs.h`：`OrderTextCodec::Parse()` 用于识别 Aquila `t-<local_order_id>`。
- `scripts/gate/query_gate_account.py`：REST account / order / position read-only 查询语义参考；第一版 TUI 可以先在 C++ 中实现等价查询或通过后续 REST helper 复用其签名规则。
- `core/config/*` 和已有 TOML parser：TUI config 应沿用 TOML + CLI override 风格。

## 当前验证命令

```bash
cmake --build build/debug --target gate_account_tui monitor_symbol_workbench_demo_data_test monitor_symbol_workbench_view_test monitor_market_data_view_model_test monitor_market_data_store_test monitor_spsc_queue_test monitor_market_data_thread_test -j 8
ctest --test-dir build/debug -R monitor_ --output-on-failure
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
git diff --check
```

2026-05-22 已验证完整 debug `ctest --test-dir build/debug --output-on-failure` 为 80/80 passed。当前环境未启动真实 Gate / Binance data session 时，dump smoke 覆盖的是 missing-SHM fallback；present-SHM 路径由 fake SHM 测试覆盖。

## 不建议复用的点

- 不复用 `TradingRuntime`：TUI 不是策略执行链路，不应该进入交易系统 event loop。
- 不复用 `OrderManager` 作为账户订单事实源：它是策略订单状态 owner，不适合全账户 monitor。
- 不直接消费 `OrderFeedbackShmReader`：该 transport 是交易系统下行事实流，SPSC lane 语义不适合被 TUI 抢读。
- 不直接使用 `OrderFeedbackEvent` 作为 TUI 主事件：该结构依赖 Aquila `t-<local_order_id>`，并缺少 TUI 计算 PnL 需要的 contract、side / signed size、fee、完整 status 等字段。
- 不为第一版 market data 伪造 `last_price`、最新成交量、24h volume 或 turnover；现有 `BookTicker` SHM 缺这些字段时显示 `NA`。

## 下一步建议

1. 阅读 `doc/tui_gate_account_monitor_design.md`。
2. 后续实现 monitor 专用 Gate orders raw parser，覆盖全账户订单，不丢弃非 Aquila text；先用 fixture 固化 `futures.orders` 字段语义。
3. 增加启动期 REST snapshot：open orders、positions、account summary；运行中低频校验 drift。
4. 实现 monitor model：按 symbol 聚合 orders、position ledger、realized / unrealized PnL、source classification 和 stale 状态。
5. 实现独立 Gate account monitor session：只订阅 private `futures.orders`，输出 monitor raw update；order source 保持可替换，后续可以改为 order pool SHM。
6. 将 health / order thread 接入真实系统 API 和 monitor order source，保留 SPSC 到 UI thread 的边界。

## 当前开放问题

- PnL 口径需要最终确认：是否以 Gate REST account / position 字段为权威，还是 TUI ledger 为主、REST 只做 drift 标记。
- `futures.orders` 是否足够覆盖手续费和成交增量的精确口径，需要用 live sample 或 fixture 固化；如果不够，后续应补 `futures.usertrades` 或 REST fills。
- Market data 只读现有 `BookTicker` SHM 时，`last_price`、最新成交量、24h volume 和 turnover 暂不可用；是否通过 trade / ticker SHM 或 REST ticker 补齐留到后续。
- 合约 metadata 的启动期来源第一版优先读取现有 instrument CSV；是否新增 monitor 专用 metadata loader 留到实现时确认。
- 后续操作入口只预留布局和模型状态；真正 cancel / flatten / pause 需要单独设计和确认。
