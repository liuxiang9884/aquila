# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读历史对话的前提下，快速确认 `aquila` 当前状态、事实源、代码入口、验证命令和下一步。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 构建：CMake + `build.sh`，依赖通过本机 `$HOME/vcpkg`。
- 当前重点：WebSocket 内核、Gate / Binance 行情、data session / SHM、strategy `DataReader`、Gate submit/cancel、order feedback SHM、Gate private `futures.orders` feedback、`OrderManager`、`TradingRuntime`、Gate runtime adapter、`demo` 策略 live smoke，以及 LeadLag replay 信号链路均已落地。
- 当前边界：LeadLag strategy 层生产订单闭环已完成；`lead_lag_strategy --execute` 已接到真实 live-orders runtime，并在 `ContinuityLost` 后停止、返回 handoff exit code。V1 flat-account、tiny-position、continuity-lost stop-and-flat、ZEC 小额 filled open / close 和 unfilled-cancel smoke 已完成；submit rejected 安全 live 探测未拿到最终 `kRejected`，不计入已完成 smoke。仍缺 failure protocol probe、account / position feedback 和端到端 benchmark。更复杂的 read-only reconcile / resume 作为后续 V2。
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
| ORDI replay / 对账 | `doc/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` |
| WebSocket 性能 | `doc/websocket_client_future_optimizations.md`、`doc/websocket_read_write_benchmark_comparison.md` |

## 当前事实源

以本节、`git status`、`git log` 和当前代码为准；旧执行计划文档已清理，不再作为事实源。

截至 2026-05-22：

- `main` 已完成 Task1 order feedback SHM transport、Task2 Gate private `futures.orders` parser、`OrderFeedbackSession`、`OrderManager::OnOrderFeedback()`、trading runtime production loop、Gate adapter 和 `demo` 策略 3 轮 live smoke。
- 公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`；旧 `core/strategy/*` 和 `strategy/order_types.h` / `strategy/order_manager.h` 兼容头已删除。
- Gate runtime adapter 已迁到 `exchange/gate/trading/order_session_runtime_adapter.h` + `aquila::gate::OrderSessionRuntimeAdapter`，不再放在 `tools/`。
- 2026-05-20 `gate_demo_strategy` 用临时 3 轮配置完成 BTC_USDT live smoke；feedback 发布 6 个 `kFilled` event，REST 复核 open orders 为空、`position size=0`、`pending_orders=0`。
- 本轮已验证 `ctest --test-dir build/debug --output-on-failure` 为 68/68 passed。
- 工作区状态以 `git status` 为准；如出现本地未提交或未跟踪文件，先确认用途和归属再处理。

## 已完成摘要

### WebSocket / 行情

- WebSocket P0/P1/P2/P3 主体已完成：DNS / TCP / TLS / WebSocket cold path、active spin hot path、read/write pump、prepared write、heartbeat、reconnect/backoff、probe 和 benchmark。
- Gate futures SBE BBO 行情已落地：SBE schema / generated headers、message dispatch、BBO decoder、market data client、data session、live probe 和 benchmark。
- Binance USD-M futures JSON bookTicker 行情已落地：raw stream target、`simdjson::ondemand` parser、client/session、live probe 和 benchmark。
- Gate / Binance data session TOML parser、instrument catalog、SHM sink、startup tools 和 log config 已落地。
- `BookTicker` 作为统一行情结构进入 strategy `DataReader`；生产热路径不保存字符串 symbol，只保存内部 `symbol_id`。
- Gate / Binance 合约元数据脚本已输出统一一类下单前字段，字段语义见 `doc/futures_contract_metadata_fields.md`。
- Gate decimal-size 合约当前仍按整数 `size` 进入 runtime；metadata 查询保留真实 min / max，但 `quantity_step=1.0`、`quantity_decimal_places=0` 表示本版本只提交整数数量，完整 decimal quantity 支持留到后续版本。

### DataReader / SHM

- `RealtimeDataReader` 和 `HistoricalDataReader` 已按 concept 形式落地，不要求虚基类。
- `Poll(handler)` 是单事件接口，`Drain(handler, max_events)` 是批量接口；`HistoricalDataReader` 额外提供 `finished()`。
- `DataReader` 不做 merge：实时多路 merge 归上游 data session / producer，历史多路 merge 归离线预处理。
- `RealtimeDataReader` 构造期要求至少一个 realtime source；多 source round-robin 使用构造期双表扫描。
- `HistoricalDataReader` 构造期 mmap 非空 binary 文件，热路径不打开文件、不抛异常。
- `DataReaderConfig::max_events_per_drain` 是 finite / replay reader 的外层 `Drain()` budget；旧字段 `max_events_per_source` 已删除。
- reader stats 已聚焦数据流本身；`poll_calls` / `empty_polls` 归 runtime / scheduler diagnostics，当前可通过 `TradingRuntimeDiagnostics` 记录。
- 2026-05-06 live drain 验证中 Gate / Binance source 均未检测到 SHM ring overrun。

### Gate 交易

- Gate `OrderSession` 第一版 submit/cancel C++ 主路径已落地：login HMAC-SHA512、固定缓冲区 request encoder、place/cancel、response correlation、同步 `OrderResponse` 回调和 benchmark。
- `OrderManager` 负责订单对象、订单池、状态机和直接 session 发送；不缓存 Gate wire fields，不维护 exchange order id 索引，不暴露两阶段 `PrepareOrder()` / `SubmitOrder()`。
- Task1 order feedback SHM transport 已落地：固定 8 lane、Nova SPSC、宽结构 `OrderFeedbackEvent`、continuity lost control event、publisher / reader / config / tests / benchmark。
- Task2 Gate order feedback 已落地：private `futures.orders` parser、`OrderFeedbackSession`、SHM publish、disconnect continuity lost 和 `OrderManager::OnOrderFeedback()`。
- `OrderManager::OnOrderFeedback()` 已处理 accepted、partial filled、filled、cancelled / terminal、rejected 和 continuity lost；accepted 后保存 exchange order id 并更新 cancel cache，terminal 后清理 cache。
- `gate_strategy_order` 已作为 `OrderManager` + Gate WebSocket 单笔下单工具落地；`gate_demo_strategy` 已作为 runtime live smoke 工具落地。
- 尚未完成：REST reconcile、feedback WS 断线未知订单恢复、account / position feedback、batch/amend/cancel-all。

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
- `doc/lead_lag_live_runtime_plan.md` 已记录 LeadLag signal-only 长时间实盘观察、strategy 层真实订单闭环、runner `--execute` 真实入口、`ContinuityLost` stop-and-flat 应急链路、异常 smoke 和端到端 benchmark 的推进顺序。
- LeadLag default production accounting 已可提交 IOC limit order intent；`tools/lead_lag/live_strategy.cpp::RunLiveOrders()` 已接到 Gate live-orders runtime，缺凭据时返回 exit code `2`，收到 `ContinuityLost` 时返回 handoff exit code `10`。2026-05-22 已完成 BTC_USDT flat-account、tiny-position、隔离 `ContinuityLost` stop-and-flat smoke，以及 ZEC_USDT `--smoke-open-close` 小额 filled open / close 和 `--smoke-unfilled-cancel` 小额挂单撤单 smoke；最终 REST 复核 open orders 为空、position `size=0`。`--smoke-submit-reject` 已有诊断入口和测试，但 ZEC_USDT 安全 IOC live 探测只收到 `Ack`、未收到最终 `kRejected`；后续不要把它算作已完成 smoke，应改做独立 `OrderSession` failure protocol probe 或先推进 account / position feedback。
- `scripts/lead_lag/run_live_with_guard.py` 已作为外围 guard wrapper 落地：启动前 REST preflight，正常退出后 final REST check，异常退出或 final 非 flat 时调用 emergency flatten；该 wrapper 不改 `TradingRuntime` 热路径。
- `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 和 `config/strategies/lead_lag_requested_11symbols_20260522.toml` 已覆盖 `PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`、`DASH_USDT`、`RIVER_USDT`、`SUI_USDT`、`INJ_USDT`、`ENA_USDT`、`BRETT_USDT`；其中 `RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 仍只按整数下单。

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
| `exchange/gate/market_data/*` | Gate SBE BBO client / session / config |
| `exchange/binance/market_data/*` | Binance bookTicker stream / parser / client / session / config |
| `tools/market_data/data_reader_probe.cpp` | 多 SHM source reader probe |

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
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
```

Live drain 验证需要先启动 Gate / Binance data session 写 SHM，再用临时 drain 配置运行 probe；不要把仓库默认 `strategy_data_reader.toml` 改成 drain。

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
ctest --test-dir build/debug -R lead_lag --output-on-failure
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
3. 扩展 failure protocol probe：rejected / cancel-rejected 不要硬塞进 LeadLag runtime，优先用独立 `OrderSession` probe 验证交易所 response，并在后续 account / position feedback 接入后补对应 smoke。
4. V2 再评估 read-only reconcile / resume：未知订单状态、本地状态恢复、人工介入、新开仓暂停 / 恢复条件。
5. 接入 symbol metadata / risk check：启动期缓存合约元数据，submit 前校验 tick、quantity、notional、reduce-only 等。
6. 增加端到端 benchmark：覆盖 `TradingRuntime -> OrderSessionRuntimeAdapter -> OrderSession` 下单请求和 `OrderFeedbackSession -> SHM -> TradingRuntime` 回报消费。

### DataReader / 交易组件

1. 读取 `doc/strategy_order_component_model.md`。
2. 读取 `doc/trading_component_architecture_discussion.md`。
3. 若继续 DataReader feed 扩展，优先讨论 trade / order book 的 typed storage + unified scan table。
4. 如果生产工具需要导出 runtime loop diagnostics，再在具体 tool / strategy runner 中选择启用 `TradingRuntimeDiagnostics` 并低频打印 stats。
5. 若继续下一个组件，建议按 `OrderSession`、`OrderFeedbackSession`、`OrderManager`、`Strategy` 的顺序继续讨论接口边界。

### LeadLag

1. 读取 `doc/lead_lag_live_runtime_plan.md`、`doc/lead_lag_reconcile_design.md` 和 `strategy/lead_lag/README.md`，确认当前 runner gating、strategy 层订单闭环和 `ContinuityLost` handoff 边界。
2. 如继续 signal-only 长跑，可使用 `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 观察 11 个 requested symbol；decimal-size 合约当前只按整数下单，完整 decimal quantity 支持留到后续版本。
3. V1 emergency smoke、外围 guard wrapper、ZEC 小额 filled open / close 和 unfilled-cancel smoke 已完成；submit rejected 安全 live 探测未通过，不计入完成项。
4. 之后补独立 `OrderSession` failure protocol probe、account / position feedback、端到端 benchmark 和更长时间真实订单运行 guardrails。

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

请先在 `/home/liuxiang/dev/aquila` 运行 `git status --short --branch` 和 `git log --oneline -8`，然后依次阅读 `AGENTS.md`、`README.md`、`doc/project_onboarding_guide.md`、`doc/evaluation_support.md`。以 onboarding 的“当前事实源”“代码入口”“当前重要结论”和“下一步建议”为事实源；当前分支、ahead/behind 和未提交状态以 `git status` 为准，不预设 `main` 与 `origin/main` 同步。当前公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`，Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h` + `aquila::gate::OrderSessionRuntimeAdapter`。当前 `main` 已完成 Task1 order feedback SHM transport、Task2 Gate private `futures.orders` parser、`OrderFeedbackSession`、`OrderManager::OnOrderFeedback()`、trading runtime production loop、Gate adapter 和 `demo` 策略 3 轮 live smoke；`DataReaderConfig::max_events_per_drain` 已替代旧 `max_events_per_source`，runtime loop diagnostics 已落在 `TradingRuntimeDiagnostics`。如果继续 LeadLag 长时间实盘运行和测试，先读 `doc/lead_lag_live_runtime_plan.md` 和 `doc/lead_lag_reconcile_design.md`；当前 strategy 层订单闭环、Python REST emergency flatten helper、`lead_lag_strategy --execute` live-orders handoff、flat-account / tiny-position emergency smoke、隔离 `ContinuityLost` stop-and-flat smoke、外围 `scripts/lead_lag/run_live_with_guard.py`、ZEC_USDT `--smoke-open-close` 小额 filled open / close 和 `--smoke-unfilled-cancel` 小额挂单撤单 smoke 已完成。`--smoke-submit-reject` 已有诊断入口和测试，但 ZEC_USDT 安全 IOC live 探测只收到 `Ack`、未收到最终 `kRejected`，不计入已完成 smoke；后续不要硬凑 LeadLag rejected / cancel-rejected，应优先做独立 `OrderSession` failure protocol probe、account / position feedback 和端到端 benchmark。`config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 已覆盖 requested 11 symbols，Gate decimal-size 合约当前仍只按整数 `size` 下单。修改后按项目规则验证并自动提交；不要 push，除非用户明确要求。
