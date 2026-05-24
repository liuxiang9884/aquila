# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读历史对话的前提下，快速确认 `aquila` 当前状态、事实源、代码入口、验证命令和下一步。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 构建：CMake + `build.sh`，依赖通过本机 `$HOME/vcpkg`。
- 当前重点：WebSocket 内核、Gate / Binance 行情、data session / SHM、strategy `DataReader`、Gate submit/cancel、order feedback SHM、Gate private `futures.orders` feedback、`OrderManager`、`TradingRuntime`、Gate runtime adapter、`demo` 策略 live smoke、LeadLag replay / live-orders 链路，以及 TUI Symbol Workbench / market data monitor demo 均已落地。
- 当前边界：LeadLag strategy 层生产订单闭环已完成；`lead_lag_strategy --execute` 已接到真实 live-orders runtime，并在 `ContinuityLost` 后停止、返回 handoff exit code。V1 flat-account、tiny-position、continuity-lost stop-and-flat、ZEC 小额 filled open / close、unfilled-cancel smoke 和本地端到端 benchmark 已完成；外围 `run_live_with_guard.py` 已负责 preflight、final REST check 和异常 stop-and-flat。2026-05-22 release 11-symbol live-orders guarded run 只完成 1 组完整 strategy open / close，并暴露 Gate IOC partial-fill terminal feedback 缺失和 decimal-size REST flat 判断不足；当前 C++ order / feedback / Gate encoder / LeadLag sizing 已支持 decimal quantity，Gate `futures.orders` parser 已补高精度 fill price 的 IOC partial-fill terminal 单元测试，REST final check / emergency flatten 已支持 decimal size 与 `value` / `margin` residual 判断，但两项仍需小额 live smoke 复核；复核前不要继续无人值守真实订单长跑。当前版本不新增独立 `AccountPositionFeedbackSession`；account / position realtime feedback 作为 V2 可选能力。TUI 当前仍是只读 monitor demo：market data 可从现有 Gate / Binance `BookTicker` SHM 读取并降级显示 `NA`，订单、仓位、PnL 和 health 还未接真实账户数据。
- 当前建议分支入口：`main`。
- 核心原则：正确性、确定性、最低延迟、尾延迟可控、固定容量、少动态分配；性能结论必须有 benchmark / profile / live probe 证据。

## 新对话第一步

先运行：

```bash
git -C /home/liuxiang/dev/aquila status --short --branch
git -C /home/liuxiang/dev/aquila log --oneline -8
```

再读：

```text
AGENTS.md
README.md
doc/project_onboarding_guide.md
doc/evaluation_support.md
```

按方向继续读：

| 方向 | 优先文档 |
| --- | --- |
| Gate 交易架构 | `doc/agent-handoff-gate-trade-architecture.md`、Gate order / feedback specs |
| TUI / account monitor | `doc/tui_onboarding_guide.md`、`doc/tui_gate_account_monitor_design.md` |
| Binance 行情 | `doc/agent-handoff-binance-market-data.md` |
| data session / config | `doc/data_session_config.md`、`doc/data_reader_config.md`、`doc/data_session_shm_communication_design.md` |
| 交易组件边界 | `doc/strategy_order_component_model.md`、`doc/trading_component_architecture_discussion.md` |
| LeadLag fixed 策略 | `strategy/lead_lag/README.md`、LeadLag reconstruction / design / audit docs |
| LeadLag 实盘长跑 / 测试 | `doc/lead_lag_live_runtime_plan.md` |
| LeadLag live / replay 测试 runbook | `doc/lead_lag_live_replay_testing.md` |
| ORDI replay / 对账 | `doc/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` |
| WebSocket 性能 | `doc/websocket_client_future_optimizations.md`、`doc/websocket_read_write_benchmark_comparison.md` |

## 当前事实源

以本节、`git status`、`git log` 和当前代码为准；旧执行计划文档已清理，不再作为事实源。

截至 2026-05-24：

- `main` 已完成 Task1 order feedback SHM transport、Task2 Gate private `futures.orders` parser、`OrderFeedbackSession`、`OrderManager::OnOrderFeedback()`、trading runtime production loop、Gate adapter 和 `demo` 策略 3 轮 live smoke。
- 公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`；旧 `core/strategy/*` 和 `strategy/order_types.h` / `strategy/order_manager.h` 兼容头已删除。
- Gate runtime adapter 已迁到 `exchange/gate/trading/order_session_runtime_adapter.h` + `aquila::gate::OrderSessionRuntimeAdapter`，不再放在 `tools/`。
- C++ trading quantity contract 已改为 `double quantity` + `quantity_text`：`core::OrderCreateRequest` / `core::StrategyOrder`、`OrderFeedbackEvent` / feedback SHM、`OrderManager`、Gate private feedback parser、Gate order session encoder 和 `OrderSessionRuntimeAdapter` 已支持 decimal quantity；Gate order session 在带 `X-Gate-Size-Decimal: 1` 的连接上把 JSON `size` 编码为 string，并由 `quantity_text` 按 side 生成正负文本。`kOrderFeedbackShmVersion` 已 bump 到 2，旧 feedback SHM 需要重建。
- 2026-05-20 `gate_demo_strategy` 用临时 3 轮配置完成 BTC_USDT live smoke；feedback 发布 6 个 `kFilled` event，REST 复核 open orders 为空、`position size=0`、`pending_orders=0`。
- 2026-05-22 TUI / monitor 已完成 `monitor/` skeleton、FTXUI Symbol Workbench demo、health / alert / balance 静态布局、monitor 专用 market data SHM reader、optional source fallback、one-shot live dump snapshot 和 monitor smoke tests。当前 `gate_account_tui --live-market-data` 只读现有 Gate / Binance data session SHM，不自动启动 data session；缺失 SHM 时显示 `NA` 并产生 alert。
- 2026-05-23 requested 12-symbol / ETH_USDT 配置整理已验证：`data_session_config_test`、`strategy_config_test`、`lead_lag_config_test` 和 `ctest --test-dir build/debug -R lead_lag --output-on-failure` 均通过；未跑全量 ctest。
- 工作区状态以 `git status` 为准；如出现本地未提交或未跟踪文件，先确认用途和归属再处理。

## 已完成摘要

### WebSocket / 行情

- WebSocket P0/P1/P2/P3 主体已完成：DNS / TCP / TLS / WebSocket cold path、active spin hot path、read/write pump、prepared write、heartbeat、reconnect/backoff、probe 和 benchmark。
- Gate futures SBE BBO 行情已落地：SBE schema / generated headers、message dispatch、BBO decoder、market data client、data session、live probe 和 benchmark。
- Binance USD-M futures JSON bookTicker 行情已落地：raw stream target、`simdjson::ondemand` parser、client/session、live probe 和 benchmark。
- Gate / Binance data session TOML parser、instrument catalog、SHM sink、startup tools 和 log config 已落地。
- `BookTicker` 作为统一行情结构进入 strategy `DataReader`；生产热路径不保存字符串 symbol，只保存内部 `symbol_id`。
- Gate / Binance 合约元数据脚本已输出统一一类下单前字段，字段语义见 `doc/futures_contract_metadata_fields.md`。
- Gate decimal-size 合约的 catalog metadata 已从 `order_size_min` 推导 `quantity_step` / `quantity_decimal_places`；`RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 的 Gate 行当前为 `quantity_step=0.1`、`quantity_decimal_places=1`。C++ 下单 / 回报 / LeadLag sizing 已消费该 metadata。2026-05-24 RAVE 小额 order session probe 确认未 quote 的 `"size":0.1` 会被 Gate 以 `INVALID_REQUEST` / `Mismatch type int64 with value number` 拒绝；带 decimal header 时 quote 为 `"size":"0.1"` 后不再 400，REST 复核 open orders 为空、position `size=0` / `pending_orders=0`。REST final check / emergency flatten 已带 decimal-size header、解析 decimal position size 并检查 `value` / `margin` residual；完整 strategy 小额 live smoke 仍需复核。

### DataReader / SHM

- `RealtimeDataReader` 和 `HistoricalDataReader` 已按 concept 形式落地，不要求虚基类。
- `Poll(handler)` 是单事件接口，`Drain(handler, max_events)` 是批量接口；`HistoricalDataReader` 额外提供 `finished()`。
- `DataReader` 不做 merge：实时多路 merge 归上游 data session / producer，历史多路 merge 归离线预处理。
- `RealtimeDataReader` 构造期要求至少一个 realtime source；多 source round-robin 使用构造期双表扫描。
- `HistoricalDataReader` 构造期 mmap 非空 binary 文件，热路径不打开文件、不抛异常。
- `DataReaderConfig::max_events_per_drain` 是 finite / replay reader 的外层 `Drain()` budget；旧字段 `max_events_per_source` 已删除。
- reader stats 已聚焦数据流本身；`poll_calls` / `empty_polls` 归 runtime / scheduler diagnostics，当前可通过 `TradingRuntimeDiagnostics` 记录。
- 2026-05-06 live drain 验证中 Gate / Binance source 均未检测到 SHM ring overrun。
- `data_reader_recorder` 已落地：输入 data reader TOML，使用 `RealtimeDataReader::Drain()` 从 Gate / Binance `BookTicker` SHM 写出一个合并后的 replay binary 文件；输出是连续 `aquila::BookTicker` 结构体记录，不加 header，记录顺序沿用 reader 实际 handler 输出顺序。`data_reader_recorder_test` 已覆盖双 SHM source 到单 binary 的本地集成路径。
- 2026-05-24 live record smoke 已完成：Gate / Binance data session 写 SHM，临时 `drain` recorder 写出 `15,685` 条裸 `BookTicker` binary，Gate `1,825` 条、Binance `13,860` 条，两个 source 的 `skipped=0`、`overruns=0`；`data_reader_probe` 的 historical mode 通过 `HistoricalDataReader` 读完同一文件。
- `data_reader_recorder` 是只读 SHM consumer，可以和 LeadLag / demo 策略实盘交易并行运行；完整 replay dump 必须使用临时 `drain` 配置，默认 `latest` 配置只适合状态采样。

### TUI / Account Monitor

- `monitor/` 已作为独立顶层目录落地，依赖方向为 `monitor/* -> core/config/exchange`；生产交易链路不反向依赖 `monitor/`。
- `gate_account_tui` 当前支持 interactive TUI、`--dump`、`--view health`、`--live-market-data` 和 `--market-data-config`。默认无参数显示静态 Symbol Workbench demo；live market data 需要外部 Gate / Binance data session 已经发布 SHM。
- Symbol Workbench 当前覆盖 requested 11 symbols：`PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`、`DASH_USDT`、`RIVER_USDT`、`SUI_USDT`、`INJ_USDT`、`ENA_USDT`、`BRETT_USDT`；默认选中 `ZEC_USDT`。
- `MarketDataThread` 使用 monitor 专用 SHM reader，支持 optional source attach 失败后继续运行；按 Gate / Binance 严格单调 `BookTicker.id` coalesce 最新 BBO，每 100ms 通过 SPSC 只向 UI 推 changed rows。
- `--dump --live-market-data` 使用 one-shot snapshot，从 visible window 按 `earliest_visible + drain` 读取并渲染一帧，适合 SSH / 隧道环境检查。interactive live path 仍沿用 config 中的 `latest + drain`。
- market data diagnostics 已可见：SHM unavailable、reader overrun 和 UI dropped batch 会进入 alert；当前 `BookTicker` 不含 `last_price`、最新成交量、24h volume、turnover / value，这些字段在 TUI 中显示 `NA`。
- 订单、仓位、PnL 和 health 仍是 demo / 静态数据；后续需要实现 monitor 专用 order source、REST snapshot、account model 和真实 health sampler。

### Gate 交易

- Gate `OrderSession` 第一版 submit/cancel C++ 主路径已落地：login HMAC-SHA512、固定缓冲区 request encoder、place/cancel、response correlation、同步 `OrderResponse` 回调和 benchmark。
- `OrderManager` 负责订单对象、订单池、状态机和直接 session 发送；不缓存 Gate wire fields，不维护 exchange order id 索引，不暴露两阶段 `PrepareOrder()` / `SubmitOrder()`。
- 下单 RTT 观测字段已进入 `StrategyOrder`：`request_send_local_ns`、`ack_local_receive_ns`、`response_local_receive_ns` 使用本机 Unix epoch ns，其中 `request_send_local_ns` 是请求提交给 WebSocket 发送路径前的本地时间；`ack_exchange_ns` / `response_exchange_ns` 来自 Gate submit response header 的 `x_out_time` 或 `response_time`；`accepted_exchange_ns` / `finish_exchange_ns` 来自 private `futures.orders` feedback。旧 `exchange_update_ns` 仍保留为最后一次已应用 feedback 的兼容字段。
- Task1 order feedback SHM transport 已落地：固定 8 lane、Nova SPSC、宽结构 `OrderFeedbackEvent`、continuity lost control event、publisher / reader / config / tests / benchmark。
- Task2 Gate order feedback 已落地：private `futures.orders` parser、`OrderFeedbackSession`、SHM publish、disconnect continuity lost 和 `OrderManager::OnOrderFeedback()`。
- `OrderManager::OnOrderFeedback()` 已处理 accepted、partial filled、filled、cancelled / terminal、rejected 和 continuity lost；accepted 后保存 exchange order id 并更新 cancel cache，terminal 后清理 cache。
- `gate_strategy_order` 已作为 `OrderManager` + Gate WebSocket 单笔下单工具落地；`gate_order_session_failure_probe` 已作为独立 `OrderSession` failure response 诊断工具落地；`gate_demo_strategy` 已作为 runtime live smoke 工具落地。
- 尚未完成：REST reconcile、feedback WS 断线未知订单恢复、batch/amend/cancel-all；account / position realtime feedback 可作为 V2 风控状态能力评估，不是当前 LeadLag V1 实盘前置项。

### Trading Runtime

- `TradingRuntime<StrategyT, OrderSessionT, DataReaderT>` 已落地，生产 `Create()` 从已解析 config 构造 data reader、order session、order manager、context、strategy 和可选 feedback reader。
- Gate production 路径使用 `OrderSessionT::SetRuntimeHook()` 在 WebSocket active spin loop 同线程轮询 feedback SHM / data reader。
- `OnOrderResponse()` 和 `OnOrderFeedback()` 都先更新 `OrderManager`，再调用 strategy hook。
- Gate adapter 不创建后台线程、不维护 command queue；place / cancel 在 StrategyThread / Gate session 同线程直接调用。
- `Ready() == false` 是上行交易能力硬边界；feedback continuity lost 是下行订单事实流连续性信号，不由 runtime 统一禁止开仓。

### LeadLag

- LeadLag fixed 策略第 1-7 部分设计拆解、C++ 策略层模块和 `leadlag::Strategy::OnBookTicker()` replay 信号主链路已落地。
- 已实现 config / metadata、raw market state、recorder wrappers、drift / alignment、threshold、signal / execution state、feedback state / order retire。
- `tools/lead_lag/replay.cpp` 可从 `BookTicker` binary 生成 signal CSV；ORDI_USDT Tardis / HDF replay 对比和 PnL 结论已记录。
- fixed Go / C++ 静态语义审计已完成；高影响差异集中在时间口径、move quantile exact vs histogram、synthetic replay 不等价于真实 order/fill 回测。
- `doc/lead_lag_live_runtime_plan.md` 已记录 LeadLag signal-only 长时间实盘观察、strategy 层真实订单闭环、runner `--execute` 真实入口、`ContinuityLost` stop-and-flat 应急链路、异常 smoke、端到端 benchmark 和后续真实订单长跑顺序。
- `doc/lead_lag_live_replay_testing.md` 已作为 live / replay 测试 runbook 落地；标准测试名 `lead_lag_live_replay_signal_parity` 表示 signal-only live、并行 DataReader recorder、binary replay 和 live/replay signal CSV 对比。下次用户只给测试名和时长时，按该文档直接执行，所有输出写入 `/home/liuxiang/tmp/<run_id>`。
- LeadLag default production accounting 已可提交 IOC limit order intent；`tools/lead_lag/live_strategy.cpp::RunLiveOrders()` 已接到 Gate live-orders runtime，缺凭据时返回 exit code `2`，收到 `ContinuityLost` 时返回 handoff exit code `10`。2026-05-22 已完成 BTC_USDT flat-account、tiny-position、隔离 `ContinuityLost` stop-and-flat smoke，以及 ZEC_USDT `--smoke-open-close` 小额 filled open / close 和 `--smoke-unfilled-cancel` 小额挂单撤单 smoke；最终 REST 复核 open orders 为空、position `size=0`。本地端到端 benchmark 已覆盖 submit 路径和 feedback 回报路径，结果见 `doc/lead_lag_live_runtime_plan.md`。2026-05-22 release 11-symbol live-orders guarded run 不是通过项：RIVER_USDT 完成 1 组完整 strategy open / close；RAVE_USDT IOC partial fill 在 REST 上可见，但 private feedback / strategy terminal feedback 缺失，guard 停机后平仓。2026-05-23 已修复 LeadLag C++ sizing / execution state / risk reservation / live smoke 的 decimal quantity，并把 Gate order feedback parser 的 decimal exponent 支持扩到 `[-15, 15]`，覆盖 `finish_as=ioc`、partial fill、`fill_price=0.562399019608` 的 terminal feedback 单元测试；REST final check / emergency flatten 已带 `X-Gate-Size-Decimal: 1`，用 `Decimal` 解析 position size，检查 `value` / `margin` residual，并可提交 decimal reduce-only close。上述 feedback 与 REST residual 修复仍需小额 live smoke 复核。`--smoke-submit-reject` 和独立 `gate_order_session_failure_probe` 已有诊断入口和测试，但 ZEC_USDT 安全 IOC、BTC zero-size submit、nonexistent cancel live 探测均未收到最终 failure response；后续不要把 rejected / cancel-rejected 算作已完成 smoke。当前 V1 对齐 Sirius 边界：策略持仓由订单回报推导，停机后用 REST final check / emergency flatten 校验真实账户，不新增独立 account / position feedback session。
- `scripts/lead_lag/run_live_with_guard.py` 已作为外围 guard wrapper 落地：启动前 REST preflight，正常退出后 final REST check，异常退出或 final 非 flat 时调用 emergency flatten；该 wrapper 不改 `TradingRuntime` 热路径。
- `config/data_sessions/gate_data_session_requested_20260521.toml`、`config/data_sessions/binance_data_session_requested_20260521.toml`、`config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 和 `config/strategies/lead_lag_requested_11symbols_20260522.toml` 当前覆盖 12 个 requested symbol：`PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`、`DASH_USDT`、`RIVER_USDT`、`SUI_USDT`、`INJ_USDT`、`ENA_USDT`、`BRETT_USDT`、`ETH_USDT`。文件名保留历史 `11symbols`，但 runtime 内容已追加 `ETH_USDT`；runtime log sink 名称已更新为 `12symbols`。`RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 的 Gate decimal-size metadata 当前会走 0.1 张数量格式化，Gate order session 带 `X-Gate-Size-Decimal: 1` 时会把 JSON `size` quote 成 string。12-symbol 配置当前启用 `[lead_lag.risk]`：`max_gross_notional=2000.0`，限制 strategy 全局持仓和 pending open reservation 的总 notional，只拒绝新开仓，不阻止 reduce-only close；`max_holding_position` 未配置，暂不限制数量。`execute.open_slippage` / `execute.close_slippage` 已显式配置为按 `price_tick` 调整 IOC limit 价格：12 个 symbol 均为 `3` ticks；`lead_lag_signal_triggered` 日志输出触发行情 `trigger_ticker_id`，order intent / reject 日志输出同一个 `trigger_ticker_id` 以及 `raw_price` / `order_price` 便于复盘。
- 2026-05-23 LeadLag 信号输出边界已明确：replay / signal-only live 只有显式 `--signals-output` 才写 per-signal CSV，CSV 包含 `ticker_id`；真实订单模式不写 per-signal CSV，`lead_lag_signal_triggered` 与 `lead_lag_order_intent` / `lead_lag_order_intent_rejected` 通过同一个 `trigger_ticker_id` 关联。验证命令：`ctest --test-dir build/debug -R '(lead_lag|signal_csv_writer)' --output-on-failure` 通过 11/11。

### Evaluation

- `evaluation/` 已作为 test / benchmark 共享辅助代码目录落地，不是生产路径。
- `aquila_evaluation` 是 header-only target，只允许 `test/` 和 `benchmark/` target 链接。
- `core/`、`exchange/`、`tools/` 不允许 include `evaluation/`，也不允许链接 `aquila_evaluation`。

## 文档索引

| 文档 | 什么时候读 | 关键内容 |
| --- | --- | --- |
| `AGENTS.md` | 每次新会话最先读 | 中文/英文约定、低延迟原则、测试 / benchmark / 提交规则 |
| `README.md` | 了解构建和工具入口 | build、ctest、benchmark、probe、latency compare |
| `doc/evaluation_support.md` | 增加 test / benchmark 共享辅助代码 | `evaluation/` 边界和提交前检查 |
| `doc/futures_contract_metadata_fields.md` | 处理合约基础信息 | 统一 metadata 字段、Gate / Binance 映射、数量单位差异 |
| `doc/tui_onboarding_guide.md` | 接手 TUI / account monitor | 当前范围、运行命令、实现入口、未完成项 |
| `doc/tui_gate_account_monitor_design.md` | 继续 TUI 设计或实现 | Symbol Workbench、market data SHM、order / health 线程模型和测试建议 |
| `doc/strategy_order_component_model.md` | 细化交易组件边界 | DataReader、OrderSession、OrderFeedbackSession、OrderManager、Strategy |
| `doc/trading_component_architecture_discussion.md` | 继续组件架构讨论 | DataReader concept、Poll / Drain、no-merge、diagnostics 边界 |
| `doc/data_session_config.md` | 修改 data session 配置 | instrument catalog、subscribe symbols、WS / log / SHM 配置 |
| `doc/data_reader_config.md` | 修改 strategy reader 配置 | SHM source、read mode、Poll / Drain、diagnostics policy |
| `doc/data_session_shm_communication_design.md` | 维护行情 SHM | DataShmPublisher、BookTickerShmReader、overrun 边界 |
| `doc/agent-handoff-gate-trade-architecture.md` | 继续 Gate 交易架构 | Gate 文档结论、SBE BBO、双 WS login、线程模型 |
| Gate order / feedback specs | 继续 Gate submit / feedback | OrderSession、feedback event、SHM transport、strategy apply |
| `doc/agent-handoff-binance-market-data.md` | 继续 Binance 行情 | raw stream、JSON parser、client/session、benchmark |
| `strategy/lead_lag/README.md` | 快速理解 LeadLag 目录 | 模块职责、OnBookTicker 主流程、replay 输出、边界 |
| `doc/lead_lag_live_runtime_plan.md` | 准备 LeadLag 长时间实盘运行和测试 | signal-only runner、订单闭环、`ContinuityLost` 应急链路、live smoke、benchmark 顺序 |
| `doc/lead_lag_live_replay_testing.md` | 准备 LeadLag live / replay 对比测试 | 标准测试名、输出目录、临时 config、使用程序、signal parity 分析方法 |
| `doc/lead_lag_reconcile_design.md` | 准备 LeadLag `ContinuityLost` 应急处理 | stop-and-flat V1、Python REST 撤单 / reduce-only 市价平仓、V2 read-only reconcile 边界 |
| LeadLag reconstruction / design / audit docs | 继续 LeadLag 迁移 | fixed Go 语义、Aquila 映射、Go/C++ 差异 |
| `doc/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` | ORDI replay / 对账 | Tardis / HDF 输入差异、signal key、slip PnL |

## 代码入口

### Core / Config

| 文件 | 职责 |
| --- | --- |
| `core/common/result.h` | 通用 `Result<T>` |
| `core/common/types.h` | 项目通用枚举 |
| `core/common/constants.h` | 通用常量 |
| `core/trading/order_id.h` | `strategy_id` + `strategy_order_id` 编解码 `local_order_id` |
| `core/utils/numeric.h` | `fast_float::from_chars` 数字转换 helper |
| `core/utils/mapped_file.h` | read-only mmap RAII helper |
| `core/config/websocket_config.h` | WebSocket TOML 配置和 `ConnectionConfig` 转换 |
| `core/config/instrument_catalog.h` | 启动期 instrument CSV catalog |
| `core/config/data_reader_config.h` | Strategy data reader TOML parser / loader |
| `core/config/strategy_config.h` | Trading runtime `[strategy]` TOML parser / loader |

### Market Data

| 文件 | 职责 |
| --- | --- |
| `core/market_data/types.h` | 统一 `BookTicker` |
| `core/market_data/data_reader_concepts.h` | DataReader concept 约束 |
| `core/market_data/realtime_data_reader.h` | Strategy 侧 realtime SHM reader |
| `core/market_data/historical_data_reader.h` | Binary replay reader |
| `core/market_data/data_shm.h` | `DataShmPublisher` 和 `BookTickerShmReader` |
| `tools/market_data/data_reader_recorder.*` | Gate / Binance `BookTicker` SHM 到单个 replay binary recorder |
| `exchange/gate/market_data/*` | Gate SBE BBO client / session / config |
| `exchange/binance/market_data/*` | Binance bookTicker stream / parser / client / session / config |
| `tools/market_data/data_reader_probe.cpp` | 多 SHM source reader probe |

### Monitor / TUI

| 文件 | 职责 |
| --- | --- |
| `monitor/CMakeLists.txt` | `aquila_monitor` library 和 `gate_account_tui` executable |
| `monitor/tui/gate_account_tui.cpp` | TUI 入口，支持 static demo、live market data、dump 和 health view |
| `monitor/tui/symbol_workbench_view.h` | Symbol Workbench FTXUI 布局 |
| `monitor/tui/runtime_health_view.h` | health / alert view 布局 |
| `monitor/tui/quit_events.h` | `q` / `Esc` / Ctrl-C quit event 处理 |
| `monitor/model/account_monitor_snapshot.h` | TUI 可见 snapshot model |
| `monitor/model/market_data_view_model.h` | market data batch 到 UI 行的转换 |
| `monitor/model/monitor_spsc_queue.h` | monitor 内部固定容量 SPSC queue |
| `monitor/demo/symbol_workbench_demo_data.*` | 当前静态 demo 数据 |
| `monitor/market_data/market_data_thread.*` | monitor 专用 market data reader thread 和 one-shot snapshot |
| `monitor/market_data/market_data_store.h` | 按 `(exchange, symbol_id)` coalesce latest BBO |
| `monitor/market_data/market_data_update.h` | MarketData batch / row / diagnostics payload |
| `config/monitors/gate_account_tui_market_data.toml` | TUI market data SHM reader 配置 |

### Trading / Gate

| 文件 | 职责 |
| --- | --- |
| `core/trading/order_types.h` | 公共订单请求、订单对象、response / feedback event 类型 |
| `core/trading/order_pool.h` | 固定容量订单池 |
| `core/trading/order_manager.h` | 模板化 `OrderManager<OrderSessionT>` |
| `core/trading/strategy_context.h` | strategy 窄下单接口 |
| `core/trading/trading_runtime.h` | production runtime |
| `core/trading/order_feedback_event.h` | order feedback 宽结构 event ABI |
| `core/trading/order_feedback_shm.h` | 8 lane feedback SHM transport |
| `exchange/gate/trading/order_session.h` | Gate submit/cancel WebSocket session |
| `exchange/gate/trading/order_session_runtime_adapter.h` | Gate runtime adapter |
| `exchange/gate/trading/order_feedback_parser.h` | Gate private `futures.orders` parser |
| `exchange/gate/trading/order_feedback_session.h` | Gate feedback session |
| `tools/gate/strategy_order.cpp` | 单笔 Gate WS 下单工具 |
| `tools/gate/order_session_failure_probe.cpp` | 独立 Gate `OrderSession` failure response 诊断工具 |
| `tools/gate/demo_strategy.*` | `demo` strategy 和 live smoke 工具 |
| `tools/gate/order_feedback_session.cpp` | Gate feedback session 启动工具 |

### LeadLag

| 文件 | 职责 |
| --- | --- |
| `strategy/lead_lag/config.*` | LeadLag config parser / loader |
| `strategy/lead_lag/raw_market_state.h` | raw quote state 和 same-price 语义 |
| `strategy/lead_lag/window_stats.h` / `recorders.h` | rolling stats、noise、spread、move quantile |
| `strategy/lead_lag/alignment.h` | drift / alignment phase |
| `strategy/lead_lag/threshold.h` | threshold state |
| `strategy/lead_lag/cost_model.h` / `signal.h` / `execution_state.h` | signal / execution / feedback state |
| `strategy/lead_lag/strategy.h` | replay 信号主链路 |
| `tools/lead_lag/replay.cpp` | binary replay 输出 signal CSV |

### Tools / Scripts

| 文件 | 用途 |
| --- | --- |
| `tools/websocket/probe.cpp` | 单连接 live probe |
| `tools/websocket/latency_compare.cpp` | public/private latency compare |
| `tools/gate/futures_book_ticker_probe.cpp` | Gate futures BBO live probe |
| `tools/binance/futures_book_ticker_probe.cpp` | Binance bookTicker live probe |
| `tools/gate/data_session.cpp` | Gate data session 启动工具 |
| `tools/binance/data_session.cpp` | Binance data session 启动工具 |
| `scripts/gate/query_gate_account.py` | Gate read-only account / order / position 查询 |
| `scripts/gate/place_futures_order.py` | Gate REST futures 下单 / 撤单测试 |
| `scripts/gate/run_futures_order_smoke.py` | Gate REST 小额多轮 smoke |
| `scripts/gate/query_futures_contracts.py` | Gate futures 合约元数据 |
| `scripts/binance/query_um_futures_contracts.py` | Binance USD-M futures 合约元数据 |

## 当前重要结论

### WebSocket

- 生产 decode 默认使用 mirrored receive ring；单帧和 repeated `Poll()` 多帧 drain 走 direct delivery。
- ready metadata ring 已移到 `evaluation/websocket/queued_frame_codec.h` 作为对照路径。
- data frame fast path 覆盖 `FIN=1`、`RSV=0`、text/binary、server unmasked、payload length `<= 65535`。
- write path 已完成 mask key pool、8-byte chunk XOR、dedicated control write slot、prepared write 和 business write budget。
- `MessageCallback` 和 typed handler ref 同时保留；typed handler 价值主要是减少 Gate session 适配层和保留编译期组合空间，不要写成已验证性能收益。
- `BasicWebSocketClient::stop_requested_` 只作为停止位，使用 `memory_order_relaxed`；`Stop()` 中的 `Wakeup()` 用于打断阻塞等待，不承担内存同步语义。

### Gate 交易架构

当前推荐线程模型：

```text
StrategyThread + Gate OrderSession
GateOrderFeedbackThread + Gate OrderFeedbackSession
feedback SHM lane -> StrategyThread
```

关键边界：

- `OrderSession` 是上行交易指令和轻量 API response 通道，只覆盖 login、place、cancel、ack/result/error、request correlation 和同步 response 回调。
- `OrderFeedbackSession` 是下行私有订单事实通道，第一版只使用 private `futures.orders`，不接 `futures.usertrades`。
- `OrderManager` 是订单状态 owner，统一创建本地订单、分配 `local_order_id`、调用 session 发单 / 撤单，并消费 response / feedback 推进状态。
- strategy 只保存策略级 execution group 和自己关心的 `local_order_id`，通过 `StrategyContext` 下单 / 撤单 / 查询订单。
- `Ready() == false` 表示不应发起新的上行交易指令；断线时 `OrderSession` 清空 correlation，不构造假的 rejected/cancelled response，不直接改变订单状态。
- continuity lost 通过普通 feedback event 进入 `OrderManager`，不再通过 shared epoch atomic。

### Trading Runtime

- `TradingRuntime::Run()` 在支持 `SetRuntimeHook()` 的 order session 上使用同线程 hook mode。
- Gate WebSocket active spin loop 每轮先驱动 runtime hook，runtime 轮询 feedback SHM，并在 `OrderSessionT::Ready()` 为 true 后 poll data reader。
- `Ready() == false` 只 gate 行情驱动交易意图，不阻塞 order response 或 feedback drain。
- live reader 每轮调用 `Poll(runtime)`；finite replay reader 优先使用 `Drain(runtime, data_reader.max_events_per_drain)`。
- `gate_demo_strategy` 默认 dry-run；真实提交必须显式 `--execute`，并需要先启动 `gate_order_feedback_session --connect`。

### DataReader

- `DataReader` 是 Strategy-facing capability / concept，不要求运行时多态继承体系。
- `RealtimeDataReader` 无 EOF，不提供 `finished()`；`HistoricalDataReader` 通过 `finished()` 表达 EOF。
- 多 source round-robin 是实时 reader 的公平调度，不是全局时间排序。
- `Poll()` / `Drain()` 是 `noexcept` 热路径；配置校验、SHM attach、binary 文件检查和 mmap 失败保留在冷路径。
- `Diagnostics` 是记录器 / policy，`Stats` 是计数快照；不要在组件内部泛化命名为 `Metrics`。

### TUI / Account Monitor

- TUI 是独立只读 monitor，不是交易系统事实源；不能接入 `TradingRuntime`，也不应把 UI ledger 写回策略或订单状态机。
- 第一版跨线程边界是 worker thread 本地状态 + SPSC queue + UI thread owned visible model，不用 mutex 共享 UI model。
- market data 从既有 Gate / Binance `BookTicker` SHM 读取；SHM 缺失是可见降级状态，不由 TUI 自动启动 data session。
- 当前 `BookTicker` ABI 只有 BBO 和 id / timestamp 字段；`last_price`、成交量、turnover / value 显示 `NA` 是设计边界。
- 后续 order / position / PnL 需要 monitor 专用 raw event 和 REST snapshot；不要直接把交易系统 `OrderFeedbackEvent` 当作 account monitor 主事件。

### LeadLag

- `leadlag::Strategy::OnBookTicker()` 已串起 raw market state、alignment、recorder、threshold、signal engine 和 synthetic position accounting。
- 当前 replay 使用 `PositionAccountingMode::kSyntheticSignals`，不依赖真实订单 session，也不等价于真实 order/fill 回测。
- 时间口径、exact empirical quantile mode 和 synthetic replay 边界需要先和策略研发确认，再继续生产订单回报闭环。
- fixed Go 源码参考在 `third_party/strategy/wt-invariant-strategy-leadlag-must-fix/`，该目录被 git ignore。

## 常用验证命令

### 基础

```bash
./build.sh debug
./build.sh release
ctest --test-dir build/debug --output-on-failure
git diff --check
```

### WebSocket

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
ctest --test-dir build/release -R websocket_ --output-on-failure
taskset -c 2 ./build/release/benchmark/websocket/frame_codec_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/runtime_loopback_benchmark
```

### Gate / Binance 行情

```bash
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/gate_futures_book_ticker_probe --contract BTC_USDT --symbol-id 1 --duration-ms 10000
./build/debug/tools/binance_futures_book_ticker_probe --contract BTCUSDT --symbol-id 1 --duration-ms 10000
ctest --test-dir build/debug -R '(gate_.*market_data|binance_.*market_data|data_session_config)' --output-on-failure
```

### DataReader

```bash
./build/debug/test/config/data_reader_config_test
./build/debug/test/core/market_data/core_market_data_realtime_data_reader_test
./build/debug/test/core/market_data/core_market_data_historical_data_reader_test
./build/debug/test/core/market_data/core_market_data_shm_test
TMPDIR=/home/liuxiang/tmp ./build/debug/test/tools/market_data/data_reader_recorder_test
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
./build/debug/tools/data_reader_recorder --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml --output /home/liuxiang/tmp/live_merged_book_ticker.bin --mode truncate
```

Live drain / recorder 验证需要先启动 Gate / Binance data session 写 SHM，再用临时 drain 配置运行 probe 或 recorder；不要把仓库默认 `strategy_data_reader.toml` 改成 drain。实盘交易并行录制时观察 recorder 统计里的 per-source `overruns` / `skipped`，并避免抢占交易关键线程 CPU。

### TUI / Monitor

```bash
cmake --build build/debug --target gate_account_tui monitor_symbol_workbench_demo_data_test monitor_symbol_workbench_view_test monitor_market_data_view_model_test monitor_market_data_store_test monitor_spsc_queue_test monitor_market_data_thread_test -j 8
ctest --test-dir build/debug -R monitor_ --output-on-failure
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
```

未启动 Gate / Binance data session 时，dump smoke 期望显示 `market data unavailable` / SHM unavailable alert，并保留 Gate / Binance 行情 `NA` 行。如果已启动 data session，dump snapshot 应能从 visible SHM 读取 bid / ask；`last_price`、volume、turnover 仍应显示 `NA`。

### Gate Trading

```bash
ctest --test-dir build/debug -R '(core_order_pool|strategy|gate_order|gate_submit|order_session_config|order_feedback)' --output-on-failure
./build/debug/tools/gate_strategy_order --contract BTC_USDT --side buy --order-type limit --size 1 --price 81000 --tif gtc
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --duration-sec 0.1
./build/debug/tools/gate_demo_strategy --config config/strategies/demo_strategy.toml
```

真实 Gate live smoke 需要 `TEST_KEY` / `TEST_SECRET`、外网和显式 `--execute`；执行后必须用 REST 查询确认无残留 open orders / position。

### LeadLag

```bash
ctest --test-dir build/debug -R '(lead_lag|signal_csv_writer)' --output-on-failure
./build/debug/tools/lead_lag_replay --config config/strategies/lead_lag_ordi_replay.toml --signals-output /tmp/lead_lag_compare/tardis_signal.csv
scripts/lead_lag_replay_pnl.py /tmp/lead_lag_compare/tardis_signal.csv --slippage-ticks 0 --trades-output /tmp/lead_lag_compare/tardis_slip0.csv
```

### Evaluation 边界

修改 `evaluation/` 或相关 CMake 边界后运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望结果为空。

## 下一步建议

### Gate 交易

1. 读取 `doc/agent-handoff-gate-trade-architecture.md` 和 Gate order / feedback specs。
2. 从 `core/trading/trading_runtime.h`、`exchange/gate/trading/order_session_runtime_adapter.h`、`tools/gate/demo_strategy.h` 和 `tools/gate/demo_strategy.cpp` 接手。
3. failure protocol probe 已有独立工具，但安全 live 请求未拿到最终 failure response；继续前先基于 Gate 官方协议或 dedicated account 明确可返回最终 error 的请求形态。
4. V2 再评估 read-only reconcile / resume：未知订单状态、本地状态恢复、人工介入、新开仓暂停 / 恢复条件。
5. 接入 symbol metadata / risk check：启动期缓存合约元数据，submit 前校验 tick、quantity、notional、reduce-only 等。
6. 增加端到端 benchmark：覆盖 `TradingRuntime -> OrderSessionRuntimeAdapter -> OrderSession` 下单请求和 `OrderFeedbackSession -> SHM -> TradingRuntime` 回报消费。

### DataReader / 交易组件

1. 读取 `doc/strategy_order_component_model.md`。
2. 读取 `doc/trading_component_architecture_discussion.md`。
3. DataReader recorder live record smoke / replay 可读性验证已完成；若继续 recorder，下一步可做更长时间 guarded recording、录制脚本化或与 LeadLag / demo 实盘并行录制验证。继续使用临时 `drain` 配置，输出到 `/home/liuxiang/tmp`，观察 `overruns` / `skipped` 和 CPU 抢占，不要把仓库默认 `strategy_data_reader.toml` 改成 drain。
4. 若继续 DataReader feed 扩展，优先讨论 trade / order book 的 typed storage + unified scan table。
5. 如果生产工具需要导出 runtime loop diagnostics，再在具体 tool / strategy runner 中选择启用 `TradingRuntimeDiagnostics` 并低频打印 stats。
6. 若继续下一个组件，建议按 `OrderSession`、`OrderFeedbackSession`、`OrderManager`、`Strategy` 的顺序继续讨论接口边界。

### LeadLag

1. 读取 `doc/lead_lag_live_runtime_plan.md`、`doc/lead_lag_live_replay_testing.md`、`doc/lead_lag_reconcile_design.md` 和 `strategy/lead_lag/README.md`，确认当前 runner gating、live / replay 测试名、strategy 层订单闭环和 `ContinuityLost` handoff 边界。
2. 如继续 signal-only 长跑，可使用 `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 观察 12 个 requested symbol；文件名保留历史 `11symbols`，当前已追加 `ETH_USDT`。若用户说 `lead_lag_live_replay_signal_parity <duration>`，直接按 `doc/lead_lag_live_replay_testing.md` 执行 live signal、DataReader binary record、replay 和 signal CSV 对比，所有产物写入 `/home/liuxiang/tmp/<run_id>`。decimal-size 合约的 C++ 下单 / 回报 / LeadLag sizing 已支持 1 位小数，Gate WS 下单带 decimal header 时已 quote JSON `size`，REST final check / emergency flatten 已完成 decimal residual 代码级修复，但仍需完整 strategy 小额 live smoke 复核。
3. V1 emergency smoke、外围 guard wrapper、ZEC 小额 filled open / close 和 unfilled-cancel smoke 已完成；submit rejected / cancel-rejected 安全 live 探测未通过，不计入完成项。
4. 下一步优先处理 2026-05-22 release run 暴露的剩余 blocker：用小额 live smoke 复核 Gate private feedback 的 IOC partial-fill / partial-cancel terminal event 修复，以及 REST final check / emergency flatten 的 decimal residual 修复。复核前不要继续无人值守真实订单长跑。
5. 之后再做更长时间真实订单运行 guardrails 和继续补齐风控审查；当前已先加入 LeadLag strategy 全局 `max_gross_notional`，`max_holding_position` 可选但本版本暂不启用。account / position realtime feedback 作为 V2 可选能力，不作为当前 V1 前置项。failure response 继续前先确认 Gate 可返回最终 error 的请求形态。

### TUI / Account Monitor

1. 读取 `doc/tui_onboarding_guide.md` 和 `doc/tui_gate_account_monitor_design.md`，以当前 `monitor/` 实现为边界。
2. 下一步优先实现 monitor 专用 Gate orders raw parser 和 fixture tests；不要直接复用交易系统 `OrderFeedbackEvent` 作为 TUI 主事件。
3. 实现启动期 REST snapshot：open orders、positions、account summary；运行期先做 drift 标记，不自动修正或交易。
4. 实现 `MonitorOrderBook`、`PositionLedger`、`PnlLedger` 和真实 `AccountMonitorThread`，通过 SPSC 向 UI thread 发布 order / health batch。
5. 后续如果接 order pool SHM，应保留 `OrderSource` 可替换边界，使 UI model 不依赖具体来源。
6. market data 若要展示 `last_price`、成交量或 turnover，需要新增 trade / ticker SHM 或低频 REST ticker；补齐前继续显示 `NA`，不要用 bid / ask 伪造。

## 结束对话固定流程

用户输入“结束对话”时，只做收尾、同步和交接，不主动开启新功能：

1. 运行 `git status --short --branch` 和 `git log --oneline -8`。
2. 对照当前实现、配置和最近提交，更新相关文档，重点维护本 onboarding 的当前状态、入口、验证命令和下一步建议。
3. 如果触碰 evaluation、data session config、WebSocket、Gate / Binance handoff 或 README，同步对应文档。
4. 更新“给下一个对话的 onboarding 提示”。
5. 至少运行 `git diff --check`；如触碰 evaluation 边界，再运行 evaluation 边界检查。
6. 自动提交文档整理，commit message 使用英文；除非用户明确要求，不 push。
7. 最终回复给出提交哈希、验证结果，并贴出下一轮 onboarding 提示段落。

## 给下一个对话的 onboarding 提示

请先在 `/home/liuxiang/dev/aquila` 运行 `git status --short --branch` 和 `git log --oneline -8`，然后依次阅读 `AGENTS.md`、`README.md`、`doc/project_onboarding_guide.md`、`doc/evaluation_support.md`。以 onboarding 的“当前事实源”“代码入口”“当前重要结论”和“下一步建议”为事实源；当前分支、ahead/behind 和未提交状态以 `git status` 为准，不预设 `main` 与 `origin/main` 同步。当前公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`，Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h` + `aquila::gate::OrderSessionRuntimeAdapter`。当前 `main` 已完成 Task1 order feedback SHM transport、Task2 Gate private `futures.orders` parser、`OrderFeedbackSession`、`OrderManager::OnOrderFeedback()`、trading runtime production loop、Gate adapter 和 `demo` 策略 3 轮 live smoke；`DataReaderConfig::max_events_per_drain` 已替代旧 `max_events_per_source`，runtime loop diagnostics 已落在 `TradingRuntimeDiagnostics`。

如果继续 DataReader 小任务，先读 `doc/data_reader_config.md`；当前 `data_reader_recorder` 已能用 `RealtimeDataReader::Drain()` 从 Gate / Binance `BookTicker` SHM 写出单个 merged replay binary，输出格式是连续 `aquila::BookTicker` 结构体记录，并已有双 SHM source 本地集成测试。2026-05-24 live record smoke / replay 可读性验证已完成：临时 `drain` recorder 写出 `15,685` 条裸 `BookTicker` binary，Gate `1,825` 条、Binance `13,860` 条，两个 source 的 `skipped=0`、`overruns=0`，`data_reader_probe` historical mode 通过 `HistoricalDataReader` 读完同一文件。recorder 是只读 SHM consumer，可和 LeadLag / demo 策略实盘交易并行运行；完整 dump 需要临时 `drain` 配置，默认 `latest` 配置只适合状态采样，运行时要观察 `overruns` / `skipped` 和 CPU 抢占。下一步可做更长时间 guarded recording、录制脚本化，或继续 trade / order book feed 扩展。如果继续 TUI / account monitor，先读 `doc/tui_onboarding_guide.md` 和 `doc/tui_gate_account_monitor_design.md`；当前 `monitor/` 已完成 FTXUI Symbol Workbench demo、health / alert / balance 静态布局、monitor 专用 market data SHM reader、optional source fallback、one-shot dump snapshot 和 monitor tests，`gate_account_tui --live-market-data` 只读现有 Gate / Binance `BookTicker` SHM，订单、仓位、PnL 和 health 仍未接真实账户数据。下一步 TUI 优先做 monitor 专用 Gate orders raw parser、REST snapshot 和 account model。

如果继续 LeadLag 长时间实盘运行和测试，先读 `doc/lead_lag_live_runtime_plan.md`、`doc/lead_lag_live_replay_testing.md` 和 `doc/lead_lag_reconcile_design.md`；当前 strategy 层订单闭环、Python REST emergency flatten helper、`lead_lag_strategy --execute` live-orders handoff、flat-account / tiny-position emergency smoke、隔离 `ContinuityLost` stop-and-flat smoke、外围 `scripts/lead_lag/run_live_with_guard.py`、ZEC_USDT `--smoke-open-close` 小额 filled open / close、`--smoke-unfilled-cancel` 小额挂单撤单 smoke 和本地端到端 benchmark 已完成。标准测试名 `lead_lag_live_replay_signal_parity <duration>` 已定义：signal-only live、并行 DataReader recorder、binary replay、live/replay signal CSV 对比，产物写入 `/home/liuxiang/tmp/<run_id>`；该 runbook 只记录使用程序和测试后分析方法，每次具体结果留在对应 run directory 和最终回复中。2026-05-22 release 11-symbol live-orders guarded run 只完成 1 组完整 strategy open / close，并暴露 Gate IOC partial-fill terminal feedback 缺失和 decimal-size REST flat 判断不足；当前 C++ order / feedback / Gate encoder / LeadLag sizing 已支持 decimal quantity，Gate WS 下单带 `X-Gate-Size-Decimal: 1` 时会把 JSON `size` quote 为 string，Gate `futures.orders` parser 已补高精度 fill price 的 IOC partial-fill terminal 单元测试，REST final check / emergency flatten 已带 decimal-size header、解析 decimal position size、检查 `value` / `margin` residual 并可提交 decimal reduce-only close；这些修复仍需完整 strategy 小额 live smoke 复核，复核前不要继续无人值守真实订单长跑。当前 V1 对齐 Sirius 边界：策略持仓由订单回报推导，停机后用 REST final check / emergency flatten 校验真实账户，不新增独立 `AccountPositionFeedbackSession`；account / position realtime feedback 是 V2 可选能力。`--smoke-submit-reject` 和独立 `gate_order_session_failure_probe` 已有诊断入口和测试，但 ZEC_USDT 安全 IOC、BTC zero-size submit、nonexistent cancel live 探测均未收到最终 failure response，不计入已完成 smoke。`config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 当前已覆盖 12 个 requested symbols，文件名保留历史 `11symbols`，内容已追加 `ETH_USDT`；Gate decimal-size 合约 catalog metadata 已从 `order_size_min` 推导小数位，C++ 订单链路已用 `quantity_text` 下发小数 size。信号输出边界：replay / signal-only live 只有显式 `--signals-output` 才写 signal CSV；真实订单模式不写 per-signal CSV，信号和订单意图用日志中的 `trigger_ticker_id` 关联；`ctest --test-dir build/debug -R '(lead_lag|signal_csv_writer)' --output-on-failure` 已通过 11/11。修改后按项目规则验证并自动提交；不要 push，除非用户明确要求。
