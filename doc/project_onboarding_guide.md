# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读整个历史对话的前提下，快速理解 `aquila` 当前状态、重要文档、代码入口和下一步应该怎么接手。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 当前重点：WebSocket 内核已经完成 P0/P1/P2/P3 主体；Gate futures SBE BBO 行情与 Binance USD-M futures JSON bookTicker 行情、`BookTicker`、market data client、data session、SHM sink、strategy `DataReader`、benchmark、live probe 和每进程 config / log config 已落地；Gate / Binance 期货合约元数据脚本已输出统一一类下单前字段；行情热路径已按协议不变量收口；Gate `OrderSession` 第一版 submit/cancel C++ 主路径、strategy `OrderManager` 第一版订单框架、Task1 order feedback SHM transport、Task2 Gate private `futures.orders` parser / `OrderFeedbackSession` / `OrderManager` feedback apply，以及 strategy runtime production loop / Gate adapter / demo 策略 dry-run 工具已落地；LeadLag fixed 策略到 `aquila` 的分层设计 spec 已开始，配置 / instrument metadata、raw market state、BBO extrema、MoveQueue、noise 和 spread 统计语义已按 fixed Go 源码补齐。
- 构建：CMake + `build.sh`。
- 核心原则：正确性、确定性、最低延迟、尾延迟可控、固定容量、少动态分配、性能结论必须有 benchmark / profile / live probe 证据。
- 当前建议分支入口：`main`。

## 最近已完成

截至 2026-05-09，`main` 已完成的主要内容：

1. Gate / Binance market data 热路径防御性分支收口。
2. Gate BBO 生产 decoder 只保留 trusted 路径，保守 decode 和 benchmark wrapper 已移出生产 header。
3. `QueuedFrameCodec` 与 Gate BBO payload builder 已迁移到 `evaluation/`。
4. 新增 header-only `aquila_evaluation` target，只允许 test / benchmark 使用。
5. Gate / Binance 期货合约元数据脚本已按统一一类下单前字段输出 `pandas.DataFrame`。
6. `doc/futures_contract_metadata_fields.md` 已记录字段含义、交易所映射和当前空值语义。
7. Gate / Binance data session TOML parser 和启动 tools 已落地，tools source 已按 `tools/gate/`、`tools/binance/`、`tools/websocket/` 归类。
8. 仓库内 Gate data session 示例配置使用公网 `wss://fx-ws.gateio.ws:443`，因此 `enable_tls = true`；private link / plain WS 部署需要替换 private endpoint 并设置 `enable_tls = false`。
9. `README.md`、本 onboarding、Gate / Binance handoff、data session config 和 evaluation 文档已按当前实现边界对齐。
10. Gate / Binance data session tools 启动时只 parse 一次 TOML：同一个 parsed table 用于 Nova log 初始化和 data session config 生成。
11. `config/data_sessions/*.toml` 的 log 文件默认写到 `/home/liuxiang/log/`，并用 data session 名称区分 file sink、console sink 和 backend log thread；Nova file sink 会追加启动时间生成实际日志文件名。
12. `AGENTS.md` 和本 onboarding 已加入“结束对话”收尾流程和新对话 onboarding 固定入口。
13. Strategy `DataReader` 已落地，按 `config/data_readers/strategy_data_reader.toml` attach Gate / Binance SHM book ticker source，支持 `latest` / `drain` 两种读取模式和编译期 diagnostics policy。
14. `tools/market_data/data_reader_probe.cpp` 已加入 `OnBookTicker()` 低频采样日志、`--log-every` 和 per-source final summary。
15. 2026-05-06 使用 Gate / Binance data session 实盘写 SHM，`data_reader_probe` 以 `drain` 模式运行 1800s：reader 共读 `4,635,362` 条，Gate `495,255` 条，Binance `4,140,107` 条，两个 source 的 `skipped=0`、`overruns=0`。
16. WebSocket client 的 `stop_requested_` 只作为停止位使用，已改为 `memory_order_relaxed`；probe/tool 的 signal stop flag 已统一为 `std::atomic<bool> signal_stop_requested`，不再使用 `volatile std::sig_atomic_t`。
17. `scripts/gate/query_gate_account.py` 已落地，按 Gate APIv4 read-only GET 查询当前 API key 可访问的账户总额、USDT futures 账户、个人费率、futures fee、futures orders 和 futures positions；命令行通过 `account` / `orders` / `positions` 子命令区分。
18. `scripts/gate/place_futures_order.py` 已落地，支持 Gate REST futures 常规下单测试和 `cancel` 命令行撤单；默认 dry-run，真实提交必须显式 `--execute`，提交后默认撤单，`--keep-open` 才保留挂单；脚本内置 `MAX_ORDER_SIZE = 5` 单次手数上限。
19. Gate submit/cancel `OrderSession` 第一版已落地：`order_types` / `order_codecs`、login HMAC-SHA512 signature、固定缓冲区 request encoder、submit response decoded correlation、`aquila::gate::OrderSession` 以及 encode / parse / `Handle()` dispatch benchmark 均已实现并验证；final result dispatch benchmark 已改为批量预置 inflight 口径，`OrderSession` 使用 no-hash parser profile 跳过成功路径不用的 request id / req_id / text hash。
20. Gate `OrderSession` 当前边界已调整：`OrderManager` 做建单、订单池和状态机；`OrderSession` 直接接收订单 struct，在发送路径完成 Gate place/cancel JSON 序列化、`request_sequence -> local_order_id` correlation 和轻量同步 `OrderResponse` 回调。
21. `config/order_sessions/gate_order_session.toml` 和 Gate `OrderSessionConfig` parser 已落地，按 data session 风格复用通用 WebSocket config parser；TOML 只写 `[order_session]`、credentials env 名和 `[order_session.websocket.*]`，WS target 由 `settle` 生成 `/v4/ws/<settle>`。
22. `AGENTS.md` 已加入 subagent 规则：主会话派发 `spawn_agent` / subagent 时默认显式设置 `reasoning_effort = "xhigh"`，并默认不让 subagent 再派生下级 subagent。
23. `OrderManager` 第一版订单框架已按 Sirius 风格重构：`core/trading/order_pool.h`、`core/strategy/order_types.h` 和 `core/strategy/order_manager.h` 覆盖通用固定容量订单池、订单创建、状态推进和直接 session 发送；旧 `strategy/order_types.h` / `strategy/order_manager.h` 仅保留为 forwarding compatibility header；`OrderManager` 不维护 exchange order id 索引，不再缓存 Gate wire fields，也不再暴露 `PrepareOrder()` / `SubmitOrder()` 两阶段接口。
24. `scripts/gate/run_futures_order_smoke.py` 和 `scripts/gate/run_futures_order_smoke_test.py` 已落地；2026-05-07 使用 Gate REST 对 `BTC_USDT`、1 手、5 轮真实 smoke，结果 `5/5 filled_and_closed`，最终 `position size=0`、`pending_orders=0`、`open orders=[]`。这只是 REST smoke，不是 C++ WS `OrderSession` live smoke。
25. `OrderManager` / Gate 第一版边界已明确：`OrderManager` 负责订单对象、风控位置、状态和执行流程；Gate `OrderSession` 负责从订单 struct 现场编码 place/cancel 请求、correlation 和轻量 response。`OrderManager` benchmark 是 fake session direct-send baseline，不包含真实 WebSocket 或 socket 成本。
26. `tools/gate/strategy_order.cpp` 已落地为 `OrderManager` + Gate WebSocket 下单工具：CLI 参数类似 REST 下单脚本，默认 dry-run，只有 `--execute` 才连接 `OrderSession` 并实盘发送；登录成功 callback 在 WebSocket 线程内调用 `OrderManager` 下单，避免跨线程直接调用 `OrderSession::PlaceOrder()`。2026-05-08 已用它做最小实盘提交验证：place API ack 可收到，真实订单状态仍需后续 `OrderFeedbackSession` 处理。
27. Gate `OrderFeedbackSession` 第一版 event 语义已收敛到文档：第一版只以 private `futures.orders` 为生命周期事实源，不接 `futures.usertrades`；已定义 accepted、partial filled、filled、cancelled/terminal finished 和 rejected 的边界。Task1 已实现宽结构 `OrderFeedbackEvent` 作为第一版 SHM ABI，并加入 `kGap` control event；tagged union 暂不进入第一版实现。
28. `local_order_id` 已升级为 `std::uint64_t`，编码为高 8 bit `strategy_id` 加低 56 bit `strategy_order_id`；`core/trading/order_id.h` 提供 `LocalOrderIdCodec`，`OrderPool` 可在构造时接收 `strategy_id`，Gate `text` 仍为 `t-<local_order_id>`。
29. Order feedback Task1 SHM transport 已实现：固定 8 lane、Nova SPSC、宽结构 `OrderFeedbackEvent`、`OrderFeedbackShmPublisher`、`OrderFeedbackShmReader`、TOML config parser、tests 和 `order_feedback_shm_benchmark` 均已落地；gap 以 `OrderFeedbackKind::kGap` event 投递到 lane，不再通过 shared gap epoch atomic 进入 Strategy。`OrderFeedbackShmManager` 初始化 / attach 通过 `Create()` / `Open()` / `OpenOrCreate()` 返回 `Result`，不向上层暴露 throwing constructor。
30. Task1 第一版不做 producer / reader heartbeat，不做 stale owner 自动判断或 pid alive probe；`consumer_run_id` 是唯一 ownership token，`consumer_pid` 只用于诊断，`Claim(..., force_claim=true)` 是显式恢复动作，`Release()` 只有 CAS 当前 run id 成功才清 pid。
31. Task2 Gate order feedback session 已实现：`exchange/gate/trading/order_feedback_parser.h` 解析 private `futures.orders` SBE orders payload，`OrderFeedbackSession` 登录后订阅 `futures.orders`，binary path 解析后调用 Task1 `OrderFeedbackShmPublisher::Publish()`，断线时使用 `PublishGlobalGap(...)` 向 8 lane fanout `kGap` event。
32. `OrderManager` 已实现 `OnOrderFeedback()`：accepted 保存 `exchange_order_id` 并通知同线程 `OrderSession` cache；partial fill 更新累计成交但在 `kCancelSent` 下保持撤单挂起状态；filled / cancelled terminal event 幂等终结订单并清理 cache；`kGap` event 设置 `feedback_gap_detected`。
33. `config/order_feedback/gate_order_feedback_session.toml`、`OrderFeedbackSessionConfig` parser、`tools/gate/order_feedback_session.cpp`、parser / session / strategy / SHM fake integration tests 和 `gate_order_feedback_parser_benchmark` 已落地。2026-05-08 release benchmark：parser one order mean `65.2ns`，session binary to counting publisher mean `95.3ns`，session binary to SHM publish + drain mean `105ns`；这些是本机 microbenchmark，不是公网端到端延迟结论。
34. Task2 已完成小额 live smoke：`gate_order_feedback_session` + `gate_strategy_order` 跑通过 1 手 BTC_USDT market buy + reduce-only sell 填平，以及 79000 buy limit accepted 后自动 cancel；REST 复核无残留 open orders / position。仍未实现 REST reconcile、account / position feedback 和 feedback WS 断线后的未知订单状态恢复。
35. `doc/leadlag-fixed-strategy-reconstruction-guide.md` 已保存 current fixed LeadLag 策略重建手册；`config/strategy.zip` 已解压到被 git ignore 的 `third_party/strategy/` 作为 fixed Go 源码参考；`doc/superpowers/specs/2026-05-08-leadlag-fixed-strategy-aquila-design.md` 已按 fixed 语义还原 + `aquila` 链路对齐方式开始拆解。LeadLag 第一部分配置已定：runtime `strategy.name = "lead_lag"`，user config 为 `config/strategy/lead_lag.toml`，root table `[lead_lag]`，`version = "1.0"`，pair 使用 `[[lead_lag.pairs]]` 数组。当前已确认：一个 `leadlag::Strategy` 可管理多个 pair、pair config 同时写 `symbol` / `symbol_id`、交易基础信息从 instrument catalog 压缩到运行期 metadata、`open_notional` 使用 `notional_multiplier` 转 lag 原生 quantity、raw market state 使用 `QuoteSnapshot{local_ns,bid,ask}` 和 `previous_quote`、same-price raw tick 不替换 quote 但仍推进 drift / alignment、Active seed 使用 previous quote、lag tick 触发 Active 后下一笔 lead tick 允许 resume。`BboExtremaWindow` 使用 vector-backed monotonic deque，允许 vector 扩容并用 `RecorderStats` 计数。fixed Go 源码确认：`MoveQueue` 是按 `stats_window` 切窗 roll 后清空，不是严格 rolling；`lead_noise` / `lag_noise` 是 rolling normalized std 的 rolling mean，不是指数 EMA；`lag_spread` 是 absolute spread 的 `StreamRecorder(stats_window)` mean。
36. Strategy runtime production loop 已落地：`core/strategy/strategy_runtime.h` 支持 `StrategyRuntime<UserStrategyT, OrderSessionT, DataReaderT>`，生产 `Create()` 从已解析 `StrategyConfig` / `DataReaderConfig` 构造 `DataReader`、`OrderSession`、`OrderManager`、`StrategyContext`、user strategy 和可选 feedback reader；Gate production 路径使用 `OrderSessionT::SetRuntimeHook()` 在 WebSocket active spin loop 同线程轮询 feedback SHM / data reader，`OnOrderResponse()` 和 `OnOrderFeedback()` 都先更新 `OrderManager` 再调用 user strategy hook，并支持 `OnStart` / `OnLoop` / `OnIdle` / `OnStop` / `ShouldStop`、`spin` / `yield` idle policy、`max_loop_seconds` 和 best-effort `bind_cpu_id`。
37. Gate strategy runtime adapter 和 `demo` 策略工具已落地：`tools/gate/strategy_runtime_adapter.h` 把 Gate `OrderSession` 包装为 runtime 可用的 `OrderSessionT`，通过 `BindRuntime()` 让 Gate response handler 同线程直接回调 `StrategyRuntime::OnOrderResponse()`，不再保留 production response queue、background order session thread 或 command queue；place / cancel 直接转发给同线程 Gate session。`tools/gate/demo_strategy.h` / `tools/gate/demo_strategy.cpp` 提供 `demo` user strategy 和 `gate_demo_strategy` 工具，默认 dry-run 只解析配置，不打开 WebSocket / SHM；显式 `--execute` 才进入实盘 runtime。本轮没有做实盘测试。
38. Nova upstream 已加入 `nova::LoggingGuard`（Nova commit `e40bbc5 Add logging guard`）；Aquila tool 直接使用该 RAII guard 初始化 / 停止 Nova log，本地 `tools/common/logging_guard.h` 已删除。Aquila 本地提交 `21b7740 Clean up completed planning docs` 已清理完成的执行计划文档，后续以 onboarding、handoff、design spec 和当前代码作为事实源。
39. LeadLag 第 3 部分底层数据结构已在 `core/base/` 落地：`MonotonicDeque<T>`、`RingQueue<T>`、`HeapBuffer<T>`、`DoubleHeap<T>` 和 `HistogramQuantile<T>` 均为 header-only template，实现启动期预分配、必要时 vector 扩容并保持计算准确性；`test/core/base/base_structures_test.cpp` 覆盖基本语义，`benchmark/core/base/base_structures_benchmark.cpp` 覆盖扩容 / 不扩容成本、`DoubleHeap` exact quantile、`HistogramQuantile` 近似 quantile 误差、value-only 查询和 value+reset 查询。当前生产低延迟 move quantile 方向是 fixed-bin histogram，dual heap 保留为单 quantile exact / replay 对照口径；histogram 使用 touched-bin 查询 / reset，当前本机 release benchmark 在 `10000 bins`、configured range `[900,1100]`、窗口样本 `[980,1015]`、`p=0.6` 下显示 AVX2 value-only 约从 `825 ns/query` 降到 `125 ns/query`，AVX2 value+reset 约从 `1410 ns` 降到 `454 ns`。这是本机 microbenchmark，不代表完整策略链路性能。

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
doc/futures_contract_metadata_fields.md
doc/agent-handoff-gate-trade-architecture.md
docs/superpowers/specs/2026-05-07-gate-order-session-design.md
docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md
docs/superpowers/specs/2026-05-08-order-feedback-shm-transport-design.md
docs/superpowers/specs/2026-05-08-gate-order-feedback-session-strategy-design.md
doc/websocket_read_write_benchmark_comparison.md
doc/data_reader_config.md
doc/leadlag-fixed-strategy-reconstruction-guide.md
doc/superpowers/specs/2026-05-08-leadlag-fixed-strategy-aquila-design.md
```

如果继续 Gate 交易架构，优先读 `doc/agent-handoff-gate-trade-architecture.md`、`docs/superpowers/specs/2026-05-07-gate-order-session-design.md`、`docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md`、`docs/superpowers/specs/2026-05-08-order-feedback-shm-transport-design.md` 和 `docs/superpowers/specs/2026-05-08-gate-order-feedback-session-strategy-design.md`。已完成的执行计划文档已删除，追溯实现边界和验证命令时以本 onboarding、Gate handoff 和当前代码为准。如果继续 Binance 行情，优先读 `doc/agent-handoff-binance-market-data.md`。如果继续 WebSocket 性能优化，优先读 `doc/websocket_client_future_optimizations.md`。
如果继续 LeadLag fixed 策略迁移，优先读 `doc/leadlag-fixed-strategy-reconstruction-guide.md` 和 `doc/superpowers/specs/2026-05-08-leadlag-fixed-strategy-aquila-design.md`；fixed Go 源码参考在 `third_party/strategy/wt-invariant-strategy-leadlag-must-fix/`，第一部分配置已落到 `config/strategy/lead_lag.toml`，第二部分 raw market state 已定为 `symbol_id` O(1) lookup、`Exchange` enum role、`local_ns` 时间口径、same-price 推进 alignment、previous quote seed 和 resume lead 语义；第 3 部分的底层数据结构已在 `core/base/` 实现。

## 给下一个对话的 onboarding 提示

请先在 `/home/liuxiang/dev/aquila` 运行 `git status --short --branch` 和 `git log --oneline -8`，
然后依次阅读 `AGENTS.md`、`README.md`、`doc/project_onboarding_guide.md`、`doc/evaluation_support.md`。
以 onboarding 的“最近已完成”“代码入口”“当前重要结论”和“下一步建议”为事实源；当前 `main` 至少包含 `81e1fd2 Add base data structures for lead lag recorders`，本轮收尾不 push；下一轮先用 `git status --short --branch` 和 `git log --oneline -8` 重新确认分支状态。当前 `main` 已完成 Task1 order feedback SHM transport、Task2 Gate private `futures.orders` parser、`OrderFeedbackSession`、`OrderManager::OnOrderFeedback()`、strategy runtime production loop、Gate runtime adapter 和 `demo` 策略 dry-run 工具。LeadLag fixed 策略迁移已完成：第一部分 config 落到 `config/strategy/lead_lag.toml`；第二部分 raw market state 语义已定；第三部分 recorder / queue / noise / spread / move quantile 设计已落到 `doc/superpowers/specs/2026-05-08-leadlag-fixed-strategy-aquila-design.md`，并已在 `core/base/` 实现 `MonotonicDeque<T>`、`RingQueue<T>`、`HeapBuffer<T>`、`DoubleHeap<T>`、`HistogramQuantile<T>` 五个底层抽象，配套测试和 benchmark 在 `test/core/base/`、`benchmark/core/base/`。后续如果继续 Gate 交易架构，再读 `doc/agent-handoff-gate-trade-architecture.md`、`docs/superpowers/specs/2026-05-07-gate-order-session-design.md`、`docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md`、`docs/superpowers/specs/2026-05-08-order-feedback-shm-transport-design.md` 和 `docs/superpowers/specs/2026-05-08-gate-order-feedback-session-strategy-design.md`；如果继续 Binance 行情，再读 `doc/agent-handoff-binance-market-data.md`；如果继续 data session / config，再读 `doc/data_session_config.md`；如果继续 strategy data reader，再读 `doc/data_reader_config.md`；如果继续 LeadLag fixed 策略迁移，下一步优先继续第 4 部分 drift / alignment phase 和第 5 部分 threshold engine 的 C++ 落地设计，然后用 `core/base` 组合实现 `BboExtremaWindow`、noise / spread recorder 和 `MoveQuantileWindow`。修改后按项目规则跑对应验证并自动提交；如果用户输入“结束对话”，
先整理相关文档和 onboarding，写好下一轮交接提示，验证后提交，除非用户明确要求不要提交或要求 push。

## 结束对话固定流程

用户输入“结束对话”时，当前对话需要先完成收尾，而不是继续开启新功能：

1. 运行 `git status --short --branch` 和 `git log --oneline -8`。
2. 对照当前实现、配置和最近提交，整理相关文档，并更新本 onboarding 的当前状态、入口、验证命令和下一步建议。
3. 如果本轮触碰 evaluation、data session config、WebSocket、Gate/Binance handoff 或 README，同步对应文档。
4. 更新上一节“给下一个对话的 onboarding 提示”，确保下一轮可以直接接手。
5. 至少运行 `git diff --check`；如触碰 evaluation 边界，再运行 `rg '#include "evaluation/' core exchange tools` 和 `rg 'aquila_evaluation' core exchange tools`。
6. 自动提交文档整理，commit message 使用英文；除非用户明确要求，不 push。
7. 最终回复给出提交哈希、验证结果，并贴出上一节 onboarding 提示段落。

## 文档索引

| 文档 | 什么时候读 | 关键内容 |
| --- | --- | --- |
| `AGENTS.md` | 每次新会话最先读 | 中文/英文约定、低延迟原则、测试/benchmark/提交规则。 |
| `README.md` | 了解构建和工具入口 | build、ctest、benchmark、probe、latency compare 的运行方式。 |
| `doc/websocket_frame_codec_receive_strategies.md` | 理解 FrameCodec decode 为什么这样设计 | mirrored ring、direct delivery、fast path、QueuedFrameCodec、decode 收口结论。 |
| `doc/websocket_read_write_benchmark_comparison.md` | 快速看 read/write benchmark 对比 | `aquila`、Drogon-style、`third_party/websocket` read/write 差异和数值。 |
| `doc/websocket_client_future_optimizations.md` | 继续 WebSocket 优化或调整写路径容量时读 | read/write/active spin/network 的未来优化 backlog，`DefaultWebSocketOptions`、`MakeConnectionConfig<OptionsT>()`、prepared write slots/bytes 的含义和使用边界。 |
| `doc/data_session_config.md` | 修改 `config/data_sessions/*.toml` 或新增 data session 配置时读 | 每进程一份 data session config、`instrument_catalog`、`data_session.subscribe_symbols`、symbol pool 生成、WebSocket endpoint / execution_policy / read_path / heartbeat / reconnect 字段和默认值，以及 TOML / CSV / log 依赖边界。 |
| `doc/data_reader_config.md` | 修改 `config/data_readers/*.toml` 或 strategy reader 行情入口时读 | Strategy `DataReader` 的多 SHM source 配置、`latest` / `drain` read mode、`Poll(handler)` 语义和 diagnostics policy。 |
| `doc/evaluation_support.md` | 增加 test / benchmark 共享辅助代码时读 | `evaluation/` 目录、`aquila_evaluation` target、生产路径禁止依赖 evaluation 的边界。 |
| `doc/futures_contract_metadata_fields.md` | 处理 Gate / Binance 合约基础信息和下单前校验字段时读 | 统一 DataFrame 字段、Gate/Binance 字段映射、quantity 单位差异和当前空值语义。 |
| `doc/leadlag-fixed-strategy-reconstruction-guide.md` | 继续 LeadLag fixed 策略拆解或对账时读 | current fixed 策略配置、OnRawBBO / OnLeadBBO / OnLagBBO 调用链、drift / alignment、UpdateMoveThreshold、open / close / stoploss 和订单状态机伪代码。 |
| `doc/superpowers/specs/2026-05-08-leadlag-fixed-strategy-aquila-design.md` | 继续把 LeadLag fixed 策略映射到 `aquila` 时读 | 按 7 层拆解 fixed 语义和 `aquila` 链路；已按 fixed Go 源码补齐 raw same-price、BBO extrema、MoveQueue、noise、spread、threshold 和 order state 关键语义。 |
| `doc/agent-handoff-gate-trade-architecture.md` | 继续 Gate 交易架构或 Gate SBE 行情时读 | Gate 文档结论、SBE BBO 当前落地状态、Sirius 旧实现、双 WS login 测试、三种线程模型。 |
| `docs/superpowers/specs/2026-05-07-gate-order-session-design.md` | 继续 Gate 交易架构或审查 submit/cancel 边界时读 | `aquila::gate::OrderSession` 第一版范围、Strategy / OrderSession / OrderFeedbackSession 边界、直接 struct 发单输入、`RequestIdCodec` / `OrderTextCodec` / response correlation 语义。 |
| `docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md` | 继续 Gate `OrderFeedbackSession` 架构和实现时读 | 第一版只使用 `futures.orders` 的订单生命周期 event、quantity/price/role/finish reason 语义、Strategy 状态推进和宽结构 event carrier。 |
| `docs/superpowers/specs/2026-05-08-order-feedback-shm-transport-design.md` | 审查或维护 Task1 订单 feedback SHM transport 时读 | 固定 8 lane、Nova SPSC、宽结构 event ABI、`kGap` event 化 gap、显式 reader ownership、无 heartbeat / stale owner 自动判断和 drain-only reader 语义。 |
| `docs/superpowers/specs/2026-05-08-gate-order-feedback-session-strategy-design.md` | 实现 Task2 Gate orders feedback parser / session / Strategy apply 前读 | 多进程共享 feedback session、Gate `futures.orders` 到 event 的映射、Strategy 状态机、OrderSession cache 更新和 gap handling。 |
| `doc/agent-handoff-binance-market-data.md` | 继续 Binance USD-M futures bookTicker 行情时读 | raw stream URL、JSON parser、client/session、benchmark 和 probe 入口。 |

## 代码入口

### Core 基础类型

| 文件 | 职责 |
| --- | --- |
| `core/common/result.h` | 通用 `Result<T>`，用于启动期 parser / loader 这类显式返回成功值或错误字符串的场景。 |
| `core/common/types.h` | 项目通用枚举，当前包含 `aquila::Exchange`。 |
| `core/trading/order_id.h` | `LocalOrderIdCodec`，按高 8 bit `strategy_id` + 低 56 bit `strategy_order_id` 编解码全局 `local_order_id`。 |
| `core/common/constants.h` | 项目通用常量，当前包含缓存行大小等基础常量。 |
| `core/trading/order_pool.h` | 交易通用固定容量订单池；slot vector 固定为 max live 的 2 倍，local id 查找走 `absl::flat_hash_map`；map reserve hint 在 max live 小于 1024 时为 16x，否则为 8x；不维护 exchange order id 索引；构造期拒绝超过 `uint32_t` slot index 范围的容量。 |
| `core/utils/numeric.h` | 基于 `fast_float::from_chars` 的 `ToNumeric<T>` / `ToDouble` / `ToUint64` 等热路径数字转换 helper，失败只在 debug assert。 |
| `core/base/monotonic_deque.h` | 通用 vector-backed `MonotonicDeque<T>`，用于 rolling extrema 候选队列；保留相等值 FIFO 语义，初始化可 reserve，必要时 vector 扩容。 |
| `core/base/ring_queue.h` | 通用 `RingQueue<T>`，capacity 归整到 2 的次幂，索引用 `& mask`，满时扩容并按 FIFO 顺序搬迁。 |
| `core/base/heap_buffer.h` | 通用 `HeapBuffer<T>`，封装 `std::vector<T>` + `std::push_heap` / `std::pop_heap`，避免 `std::priority_queue` 隐藏 capacity 控制。 |
| `core/base/double_heap.h` | `DoubleHeap<T>` exact empirical quantile，底层 lower max-heap / upper min-heap，空值返回 `T{}`；LeadLag 3-4 保留为单 quantile exact / replay 对照口径。 |
| `core/base/histogram_quantile.h` | `HistogramQuantile<T>` fixed-bin 近似 quantile，记录 underflow / overflow，支持 lower / midpoint / upper edge 返回模式，使用 touched-bin 查询 / reset；作为 LeadLag 生产低延迟 move quantile 默认方向。 |
| `core/market_data/types.h` | 统一行情数据结构，当前包含 `aquila::BookTicker`。 |

### 配置实现

| 文件 | 职责 |
| --- | --- |
| `core/config/websocket_config.h` | 冷路径 WebSocket TOML 配置结构、默认值和到 `websocket::ConnectionConfig` 的转换；由 `aquila_config` target 暴露，TOML 解析使用 `toml++`，诊断日志走 Nova 封装，parser 只保留必填项和枚举映射约束。 |
| `core/config/instrument_catalog.h` | 启动期 instrument CSV catalog，加载 `aquila.instrument.v1` 的完整字段；当前 data session 只消费 `symbol_id`、`exchange`、`symbol`、`exchange_symbol`，lookup 使用 `absl::flat_hash_map`。 |
| `core/config/data_reader_config.h` | Strategy data reader TOML parser / loader，加载 instrument catalog，并生成 `DataReader` 可直接消费的多 source config。 |
| `core/config/strategy_config.h` | Strategy runtime `[strategy]` TOML parser / loader，解析 strategy name/id、dry-run/live mode、order capacity、user strategy config path、data reader / order session config path 和 feedback SHM reader 参数。 |
| `exchange/gate/market_data/data_session_config.h` | Gate data session TOML parser / loader，加载 instrument catalog，并生成 `DataSession` 可直接消费的 Gate 专属 `DataSessionConfig`；target、`ConnectionConfig`、exchange symbol 列表和 symbol id 列表均在启动冷路径完成。 |
| `exchange/binance/market_data/data_session_config.h` | Binance data session TOML parser / loader，加载 instrument catalog，并生成 `DataSession` 可直接消费的 Binance 专属 `DataSessionConfig`；raw stream target、`ConnectionConfig`、exchange symbol 列表和 symbol id 列表均在启动冷路径完成。 |

### WebSocket 内核

| 文件 | 职责 |
| --- | --- |
| `core/websocket/types.h` | 配置、状态、错误码、flush mode 等基础类型。 |
| `core/websocket/frame_codec.h` | WebSocket frame encode/decode、mirrored ring、mask key pool、8B XOR。 |
| `core/websocket/critical_session.h` | read/write pump、pending write、control slot、heartbeat、`kTryFlushOne`。 |
| `core/websocket/websocket_client.h` | plain/TLS client 生命周期、reconnect/backoff、runtime loop 集成。 |
| `core/websocket/active_spin_loop.h` | active spin loop 调度。 |
| `core/websocket/prepared_write.h` | 预分配 write slot / arena。 |

### Strategy Data Reader

| 文件 | 职责 |
| --- | --- |
| `core/market_data/data_reader.h` | Strategy 侧 `DataReader`，从 Gate / Binance SHM book ticker source poll 行情；支持 `latest` 和 `drain` 两种 read mode，diagnostics 由编译期 policy 决定。 |
| `core/market_data/data_shm.h` | BookTicker SHM channel、`DataShmPublisher`、`BookTickerShmReader`；reader 支持 `TryReadOne()` 和 `TryReadLatest()`。 |
| `tools/market_data/data_reader_probe.cpp` | 独立 data reader probe，按 `config/data_readers/strategy_data_reader.toml` attach 多个 SHM source；支持 `--log-every` 低频 book ticker 采样日志和 per-source final summary。 |

### Strategy 订单框架

| 文件 | 职责 |
| --- | --- |
| `core/strategy/order_types.h` | Strategy 订单创建请求、`StrategyOrder`、place/cancel/result event 类型；订单对象保存 symbol、price_text、数量、TIF 和状态，不保存 Gate wire cache。旧 `strategy/order_types.h` 仅作 forwarding compatibility header。 |
| `core/strategy/order_manager.h` | 模板化 `OrderManager<OrderSessionT>`，提供 `PlaceLimitOrder()`、`CancelOrder()`、order response apply 和 feedback apply；发单时直接把订单 struct 交给 session。旧 `strategy/order_manager.h` 仅作 forwarding compatibility header。 |
| `core/strategy/strategy_context.h` | user strategy 的窄下单接口，封装 `OrderManager` 的 `PlaceLimitOrder()`、`CancelOrder()` 和 `FindOrder()`，不暴露 runtime 内部对象。 |
| `core/strategy/strategy_runtime.h` | `StrategyRuntime<UserStrategyT, OrderSessionT, DataReaderT>` production runtime；从已解析 config 构造 data reader / order session / order manager / context / user strategy / feedback reader，`Run()` 负责 ready gating、order response、feedback 和行情 poll，并在订单事件 hook 前先更新 `OrderManager`。 |
| `test/core/strategy/strategy_runtime_test.cpp` | runtime 回归测试，覆盖 non-copy/move、行情分发、order response 分发、order feedback 分发、feedback disabled、session 构造失败、正式 `Run()`、ready gating、`ShouldStop`、feedback reader poll 和 session stopped failure。 |
| `tools/gate/strategy_order.cpp` | `OrderManager` + Gate WebSocket 单笔下单工具；CLI 接收 contract / side / order-type / size / price / tif / reduce-only / keep-open，默认 dry-run，`--execute` 才实盘连接 WebSocket。 |
| `tools/gate/strategy_runtime_adapter.h` | Gate-specific runtime adapter；把 Gate `OrderSession` 包装为 `StrategyRuntime` 的 `OrderSessionT`，同线程 `BindRuntime()` 回调 `OnOrderResponse()`，`SetRuntimeHook()` 让 runtime 在 Gate WebSocket active loop 内轮询 feedback / data，不使用 production response queue、background order session thread 或 command queue。 |
| `tools/gate/demo_strategy.h` | `demo` user strategy：从 Gate BTC_USDT 行情 ask price 下 1 手 buy limit，等待 `wait_seconds`，成交则 `price=0` / IOC / reduce-only sell 平仓，未成交则撤单，循环 `rounds` 次；一轮 terminal 前不再开下一单。 |
| `tools/gate/demo_strategy.cpp` | `gate_demo_strategy` 工具；加载 strategy / demo / data reader / Gate order session 配置，默认 dry-run 只解析和打印，不打开 WebSocket / SHM；`--execute` 才创建 runtime 并进入实盘 loop。 |
| `config/strategies/demo_strategy.toml` / `config/strategies/demo.toml` | `demo` runtime 配置和 user strategy 参数配置。 |
| `config/strategies/lead_lag_btc_strategy.toml` / `config/strategy/lead_lag.toml` | `lead_lag` runtime 配置和第一版 user strategy 参数配置。 |
| `test/core/trading/order_pool_test.cpp` | 通用 `OrderPool` 本地订单 ID、容量限制、slot 复用、指针稳定和 zero capacity 测试。 |
| `test/strategy/strategy_test.cpp` | `OrderManager` place/cancel/response/feedback 状态推进测试。 |
| `benchmark/strategy/order_gateway_benchmark.cpp` | `OrderManager` direct-send fake session baseline；不包含真实 WebSocket 或 socket。 |

### Gate SBE 行情

| 文件 | 职责 |
| --- | --- |
| `exchange/gate/sbe/schema/gate_fex_ws_latest.xml` | Gate futures SBE schema 本地副本。 |
| `exchange/gate/sbe/generated/` | 使用 `third_party/sbepp` 生成的 Gate SBE C++ header。 |
| `exchange/gate/sbe/message_header.h` | 8 字节 SBE message header 解析。 |
| `exchange/gate/sbe/message_dispatcher.h` | Gate schema/version/template dispatch。 |
| `exchange/gate/sbe/book_ticker_decoder.h` | Gate BBO payload -> `aquila::BookTicker` decode。 |
| `exchange/gate/market_data/subscription.h` | `futures.book_ticker` subscribe/unsubscribe JSON 构造。 |
| `exchange/gate/market_data/client.h` | 模板化 `FuturesMarketDataClient<Consumer>`，从 SBE binary payload 产出 `BookTicker`。 |
| `exchange/gate/market_data/data_session.h` | `DataSession<Consumer, WebSocketPolicy, DiagnosticsPolicy>`，负责 WS 生命周期、subscribe/unsubscribe text 控制消息和 binary SBE 分流。 |
| `tools/gate/data_session.cpp` | Gate data session 启动工具；parse 一次 TOML 后初始化 Nova log 并生成 `DataSessionConfig`；默认 dry-run 打印配置生成结果，`--connect` 才实际连接。 |

### Binance USD-M futures 行情

| 文件 | 职责 |
| --- | --- |
| `exchange/binance/market_data/stream.h` | 构造 `/public/ws/<symbol>@bookTicker` raw stream target，并限制单连接 stream 数上限。 |
| `exchange/binance/market_data/book_ticker_parser.h` | Binance JSON bookTicker -> 中间 `BookTickerUpdate`，生产路径使用 `simdjson::ondemand` 和 `fast_float`。 |
| `exchange/binance/market_data/client.h` | 模板化 `FuturesMarketDataClient<Consumer>`，从 JSON text payload 产出 `BookTicker`。 |
| `exchange/binance/market_data/data_session.h` | `DataSession<Consumer, WebSocketPolicy, DiagnosticsPolicy>` raw stream target session，负责 WS 生命周期和 text JSON 分流；active 后不发送 runtime subscribe。 |
| `tools/binance/data_session.cpp` | Binance data session 启动工具；parse 一次 TOML 后初始化 Nova log 并生成 `DataSessionConfig`；默认 dry-run 打印配置生成结果，`--connect` 才实际连接。 |

### Gate 交易准备代码

| 文件 | 职责 |
| --- | --- |
| `exchange/gate/trading/order_types.h` | Gate submit/cancel 第一版轻量 request、response、send status 和 diagnostics stats 类型。 |
| `exchange/gate/trading/order_codecs.h` | `RequestIdCodec` 和 `OrderTextCodec`，用于 encoded request id 和 `text="t-<local_order_id>"` 编解码。 |
| `exchange/gate/trading/order_signature.h` / `exchange/gate/trading/order_signature.cpp` | Gate WS API login HMAC-SHA512 lowercase hex 签名，以及 private subscribe control message 使用的 `channel=...&event=subscribe&time=...` HMAC-SHA512 lowercase hex 签名。 |
| `exchange/gate/trading/order_request_encoder.h` | `futures.login`、`futures.order_place`、`futures.order_cancel` 和 `futures.orders` private subscribe JSON request 固定缓冲区编码。 |
| `exchange/gate/trading/submit_response_parser.h` | Gate submit WS JSON response parser，生产路径使用 `simdjson::ondemand`，保留 hash 字段并新增 decoded request id / req_id / local order id correlation 字段。 |
| `exchange/gate/trading/order_session.h` | 模板化 `OrderSession<ResponseHandler, WebSocketPolicy, Diagnostics>`，持有 `BasicWebSocketClient`，处理 WS login、place/cancel 发送、response correlation 和同步回调。 |
| `exchange/gate/trading/order_session_config.h` / `exchange/gate/trading/order_session_config.cpp` | Gate `OrderSession` 启动期 TOML parser；复用通用 WebSocket config，生成 `/v4/ws/<settle>` target，保留 credentials env 名和 `request_map_capacity`。 |
| `config/order_sessions/gate_order_session.toml` | Gate order session 示例配置；包含 `[log]`、`[order_session]`、`[order_session.credentials]` 和 `[order_session.websocket.*]`。 |
| `core/trading/order_feedback_event.h` | Task1 已实现：订单 feedback 宽结构 event ABI，承载 accepted / partial filled / filled / cancelled / rejected，并加入 `kGap` control event、gap scope / reason / sequence。 |
| `core/trading/order_feedback_shm.h` | Task1 已实现：固定 8 lane 的订单 feedback SHM channel、`Result` factory manager、publisher、8 lane global gap fanout、pending lane/global gap、reader claim / release 和 drain-only `Poll()`。 |
| `core/config/order_feedback_shm_config.h` / `core/config/order_feedback_shm_config.cpp` | Task1 已实现：订单 feedback SHM TOML parser，字段为 `shm_name`、`channel_name`、`max_strategy_count`、`queue_capacity`、`create`、`remove_existing`。 |
| `config/order_feedback/gate_order_feedback_shm.toml` | Task1 已实现：订单 feedback SHM 示例配置，固定 `max_strategy_count=8`、`queue_capacity=65536`，默认 `create=true`、`remove_existing=false`。 |
| `test/core/trading/order_feedback_event_test.cpp` | Task1 已实现：`OrderFeedbackEvent` ABI、`kGap` 和 route id 编码测试。 |
| `test/core/trading/order_feedback_shm_test.cpp` | Task1 已实现：SHM layout / manager、publisher route、queue full pending gap、global gap fanout、reader claim / release / poll 测试。 |
| `test/config/order_feedback_shm_config_test.cpp` | Task1 已实现：订单 feedback SHM config parser 和示例配置测试，明确不含 heartbeat / stale owner 字段。 |
| `benchmark/core/trading/order_feedback_shm_benchmark.cpp` | Task1 已实现：`PublishThenDrain`、`PollOneWithRefill`、`PublishPollLoop`、`PublishGlobalGapThenDrain` transport benchmark；单操作 case 包含 drain / refill 维护，不写成纯 publish / poll latency 或端到端性能结论。 |
| `exchange/gate/trading/order_feedback_parser.h` | Task2 已实现：Gate private `futures.orders` SBE orders message 到 `OrderFeedbackEvent` 的转换；支持 `_new`、`_update`、`filled`、terminal `finish_as`、role、quantity / price / update time 转换和 malformed diagnostics。 |
| `exchange/gate/trading/order_feedback_session.h` | Task2 已实现：Gate `OrderFeedbackSession`，负责 login / subscribe / JSON control parse / SBE binary parse / SHM publish；断线使用 `PublishGlobalGap(...)` 投递 global `kGap`，不访问 Strategy order。 |
| `exchange/gate/trading/order_feedback_session_config.h` / `exchange/gate/trading/order_feedback_session_config.cpp` | Task2 已实现：Gate order feedback session 启动期 TOML parser，复用通用 WebSocket config，并内嵌 Task1 SHM runtime config。 |
| `config/order_feedback/gate_order_feedback_session.toml` | Task2 已实现：Gate order feedback session 示例配置。 |
| `tools/gate/order_feedback_session.cpp` | Task2 已实现：默认 dry-run 打印 config；`--connect` 读取 env 凭证、创建 / 打开 SHM、运行 `OrderFeedbackSession` 并输出 session / parser / SHM stats。 |
| `test/exchange/gate/trading/order_feedback_parser_test.cpp` | Task2 parser 回归测试。 |
| `test/exchange/gate/trading/order_feedback_session_test.cpp` | Task2 session login / subscribe / binary publish / disconnect gap / publish failure 测试。 |
| `test/config/order_feedback_session_config_test.cpp` | Task2 order feedback session TOML parser 和示例配置测试。 |
| `test/strategy/strategy_order_feedback_shm_integration_test.cpp` | Task2 fake integration：Task1 SHM publisher / reader 路由 event 后调用 `OrderManager::OnOrderFeedback()`。 |
| `benchmark/exchange/gate/trading/order_feedback_parser_benchmark.cpp` | Task2 parser 和 session binary publish microbenchmark；包含 parser-only、session -> counting publisher、session -> SHM publish + drain 三个 case。 |
| `test/exchange/gate/trading/order_codecs_test.cpp` | order codecs 回归测试。 |
| `test/exchange/gate/trading/order_request_encoder_test.cpp` | login/place/cancel request encoder 回归测试。 |
| `test/exchange/gate/trading/submit_response_parser_test.cpp` | submit response parser decoded correlation 回归测试。 |
| `test/exchange/gate/trading/order_session_test.cpp` | fake handler + fake phase/message 驱动的 `OrderSession` 行为测试。 |
| `test/config/order_session_config_test.cpp` | Gate order session TOML / log / WebSocket target / request map capacity 配置测试。 |

### Evaluation 辅助代码

| 文件 | 职责 |
| --- | --- |
| `evaluation/CMakeLists.txt` | 定义 header-only `aquila_evaluation` target，只供 test / benchmark 使用。 |
| `evaluation/websocket/queued_frame_codec.h` | `FrameCodec` 的 ready queue / parse-ahead 对照实现。 |
| `evaluation/exchange/gate/sbe/book_ticker_payload_builder.h` | Gate BBO test / benchmark 共享 payload fixture。 |
| `doc/evaluation_support.md` | `evaluation/` 放置规则、CMake 依赖边界和提交前检查命令。 |

### 工具

| 文件 | 用途 |
| --- | --- |
| `tools/websocket/probe.cpp` | 单连接 live probe，支持 graceful stop 后输出最终 metrics。 |
| `tools/websocket/latency_compare.cpp` | public/private 或多连接 latency compare / warmup selection。 |
| `tools/gate/futures_book_ticker_probe.cpp` | Gate futures SBE `futures.book_ticker` live probe，默认 BTC_USDT。 |
| `tools/binance/futures_book_ticker_probe.cpp` | Binance USD-M futures JSON `bookTicker` live probe，默认 BTCUSDT。 |
| `scripts/gate/test_gate_ws_connect.py` | Gate WS 连接 / login smoke。 |
| `scripts/gate/test_gate_ws_dual_login.py` | 同账号双 WebSocket login 验证。 |
| `scripts/gate/query_gate_account.py` | Gate APIv4 read-only account / fee / order / position 查询脚本，默认读取 `TEST_KEY` / `TEST_SECRET` 环境变量。 |
| `scripts/gate/place_futures_order.py` | Gate APIv4 REST futures 下单 / 撤单测试脚本，默认 dry-run，`--execute` 后真实提交并默认撤单，单次最多 5 手。 |
| `scripts/gate/run_futures_order_smoke.py` | Gate APIv4 REST futures 小额多轮下单 smoke；真实提交必须显式 `--execute`，填单后用 reduce-only market close，未填单则撤单。 |
| `scripts/gate/run_futures_order_smoke_test.py` | REST futures order smoke runner 的本地单元测试。 |
| `scripts/gate/query_futures_contracts.py` | 查询 Gate USDT futures 合约基础信息，输出统一字段 DataFrame / CSV。 |
| `scripts/binance/query_um_futures_contracts.py` | 查询 Binance USD-M futures 合约基础信息，输出统一字段 DataFrame / CSV。 |

### Benchmark

| 文件 | 用途 |
| --- | --- |
| `benchmark/websocket/frame_codec_benchmark.cpp` | `FrameCodec` 单点 encode/decode。 |
| `benchmark/websocket/third_party_frame_codec_comparison_benchmark.cpp` | `aquila`、Drogon-style、third-party read codec 对比。 |
| `benchmark/websocket/session_write_path_benchmark.cpp` | write path、masking、Drogon-style、third-party write 对比。 |
| `benchmark/websocket/session_read_path_benchmark.cpp` | session read path socketpair 基线。 |
| `benchmark/websocket/session_mixed_path_benchmark.cpp` | read/write 混合、write budget、callback flush。 |
| `benchmark/websocket/session_tls_write_path_benchmark.cpp` | local TLS write path 基线。 |
| `benchmark/websocket/active_spin_benchmark.cpp` | active spin loop / stop check / clock 相关基线。 |
| `benchmark/websocket/message_handler_dispatch_benchmark.cpp` | `MessageCallback` 与 typed message handler dispatch 对照。 |
| `benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp` | Gate BBO decode、market data client/session binary path 和 text control parse benchmark。 |
| `benchmark/exchange/binance/market_data/futures_market_data_benchmark.cpp` | Binance JSON bookTicker parser、market data client/session text path benchmark；yyjson 和 ordered parser 只在此文件内做 benchmark 对照。 |
| `benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp` | Gate submit response JSON parse benchmark；yyjson 只在这里作为 simdjson 对照。 |
| `benchmark/exchange/gate/trading/order_session_benchmark.cpp` | Gate order place/cancel request encode 和 place result parse microbenchmark。 |
| `benchmark/exchange/gate/trading/order_feedback_parser_benchmark.cpp` | Gate private orders feedback parser 和 `OrderFeedbackSession` binary publish microbenchmark；不包含真实 WebSocket socket / TLS / 公网延迟。 |
| `benchmark/core/trading/order_pool_benchmark.cpp` | 通用 `OrderPool` create-until-capacity、live find 和 create/find/erase recycle microbenchmark。 |
| `benchmark/strategy/order_gateway_benchmark.cpp` | `OrderManager` direct-send fake session baseline，不包含真实 `OrderSession` 编码、WebSocket 或 socket。 |

## 当前重要结论

### WebSocket decode

`FrameCodec` decode 主体已经收口：

- 默认生产路径使用 mirrored receive ring。
- 单帧和 repeated `Poll()` 多帧 drain 走 direct delivery。
- ready metadata ring 已移到 `evaluation/websocket/queued_frame_codec.h` 中的 `QueuedFrameCodec` 对照路径。
- data frame fast path 覆盖 `FIN=1`、`RSV=0`、text/binary、server unmasked、payload length `<= 65535`。
- control frame、fragmentation、masked inbound、payload `>= 65536`、RSV / unknown opcode 走 generic path。

后续不要轻易为了几 ns 删除 `MessageView`、capacity/degraded、mirrored ring 或 payload 生命周期。

### WebSocket write

当前 write path 已完成：

- client mask key pool。
- 8-byte chunk XOR。
- dedicated control write slot。
- `CommitPreparedWrite(write, WriteFlushMode::kTryFlushOne)`。
- business write budget 默认 `1`。

下一步 write 优化重点不应是通用 frame encode，而是真实 Gate 订单 payload、签名、序列化、plain/TLS socket write 和交易所 ack。

### WebSocket message handler

当前同时保留两条 handler 路径：

- `MessageCallback`：C 风格函数指针 + context，仍是默认兼容路径，工具和旧测试继续可用。
- `MessageHandlerRef<T>`：typed handler ref，配合 `BasicWebSocketClient<TransportSocketT, MessageHandlerT>` 和 `CriticalSession<TlsSocketT, MessageHandlerT>`，让 Gate session 可直接作为 handler 接入。

2026-04-30 release microbenchmark：

| case | time |
| --- | ---: |
| `message_callback` | 2.11ns |
| `typed_handler_ref` | 2.37ns |
| `typed_handler_value` | 2.30ns |

结论：当前 microbenchmark 没证明 typed handler 比函数指针更快；它的价值主要是减少 Gate session 适配层和保留编译期组合空间，不应被写成已验证的性能收益。

### WebSocket stop flag

`BasicWebSocketClient::stop_requested_` 只作为停止位使用，不发布其他对象状态；当前使用
`memory_order_relaxed`。`Stop()` 里的 `Wakeup()` 仍然保留，它负责通过 `eventfd` 打断 cold path handshake
和 reconnect backoff 等阻塞等待，不承担内存同步语义。

各 probe / tool 中由 SIGINT / SIGTERM 修改的停止位统一命名为 `signal_stop_requested`，类型为
`std::atomic<bool>`，读写使用 `memory_order_relaxed`；不再使用 `volatile std::sig_atomic_t`。

### Gate 交易架构

当前推荐方向：

```text
StrategyThread + Gate OrderSession
GateOrderFeedbackThread + Gate OrderFeedbackSession
feedback SHM lane -> StrategyThread
```

原因：

- 下单路径最短。
- 行情 burst 时策略线程仍以行情为最高优先级。
- `futures.orders` SBE decode 不污染下单路径；第一版不接 `futures.usertrades`。
- Gate 已通过脚本验证允许同一账号两个 WS 同时 login。

扩展到 Gate + Binance 行情后的系统线程命名：

```text
GateDataSessionThread
  - GateDataSession

BinanceDataSessionThread
  - BinanceDataSession

StrategyThread
  - Strategy / OrderManager
  - Gate OrderSession
  - Binance OrderSession

GateOrderFeedbackThread
  - Gate OrderFeedbackSession

BinanceOrderFeedbackThread
  - Binance OrderFeedbackSession
```

其中 `risk control` 归属于用户策略，`order management` 和 `order execution` 归属于 `OrderManager` 模块；`OrderSession` 是上行交易指令和轻量 API response 通道，`OrderFeedbackSession` 是下行私有回报通道。

2026-05-07 已确认 C++ 命名位于 `aquila::gate` namespace，交易 submit/cancel 类名直接使用
`OrderSession`，不带交易所名前缀。第一版 `OrderSession` 已实现，只覆盖 `futures.login`、
`futures.order_place`、`futures.order_cancel`、`ack=true`、final result/error、`request_sequence -> local_order_id`
轻量关联和同步 `OrderResponse` 回调；不做风控、订单状态机、私有订单/成交/仓位回报、REST reconcile、batch/amend/cancel all。

OrderSession 第一版关键结论：

- `OrderManager` 负责 symbol metadata 校验接入位置、订单对象、订单状态机和订单执行逻辑，不缓存 Gate wire fields。
- `OrderSession` 接收订单 struct，不接收 `OrderManager` 侧缓存的 wire-ready request，也不维护 pending order table。
- `OrderSession` 不做额外订单语义防御性检查；本地 id fallback text 编码失败时返回 `kInvalidLocalOrderId`，该路径会消耗 request sequence 但不进入 correlation map。
- place `ack=true` 只表示 Gate 收到请求，不建立交易所 order id 映射，也不清理 correlation。
- ack/result 成功形态必须是 HTTP 200；place final result 才在 `OrderSession` 内建立本地订单和交易所订单的匹配信息，并清理 correlation。
- cancel 优先使用 `OrderSession` 内部缓存的 exchange order id 编码；没有缓存时 fallback 到本地 `text="t-<local_order_id>"`。该缓存最多保留 `request_map_capacity` 条，可通过 `forget_exchange_order_id_for_local_order()` 显式清理。cancel response 使用 encoded request id 做主 correlation；如果 result 携带 `text`，必须匹配本地 id；仅 exchange-id 的进一步原始 cancel id 校验属于后续增强设计。
- 断线时清空 correlation，不构造假的 rejected/cancelled response；`OrderManager` / 策略后续通过 state/reconcile 处理未知状态。

Order feedback Task1 / Task2 关键结论：

- Task1 通用订单 feedback SHM transport 已实现，不解析 Gate 报文，不更新 `StrategyOrder`；Task2 已在其上实现 Gate private `futures.orders` parser、`OrderFeedbackSession` 和 `OrderManager::OnOrderFeedback()`。
- SHM transport 使用一个 shared memory object、固定 8 lane、每 lane 一个 Nova SPSC queue；`local_order_id` 高 8 bit 直接作为 `strategy_id` 路由，不做 per-strategy channel name map。
- 第一版选择宽结构 `OrderFeedbackEvent` 作为 SHM ABI；它保持 trivial / standard-layout，承载 `exchange_update_ns`、本地诊断用 `local_receive_ns`，并用 `OrderFeedbackKind::kGap` 表达 transport control gap。
- gap 不再通过 `global_gap_epoch` / `lane_gap_epoch` atomic 进入 `OrderManager`；reader 只通过 `Poll()` 按 FIFO 消费 event，`kGap` 和普通 order feedback 一样交给 handler。
- `PublishGlobalGap()` 对 8 lane fanout `kGap`；某条 lane queue full 时 publisher 保留 pending gap，后续本地重试。
- 普通 publish 遇到当前 lane queue full 时不阻塞、不覆盖、不影响其他 lane；只更新当前 lane `queue_full_count` / `dropped_count` 异常计数，并产生 pending lane `kGap` event。
- reader ownership：`consumer_run_id` 是唯一 ownership token，0 表示 unclaimed；`consumer_pid` 仅诊断；`Claim(..., force_claim=true)` 是显式恢复动作；`Release()` CAS 当前 run id 成功才清 pid。第一版不做 producer / reader heartbeat，不做 stale owner 自动判断或 pid alive probe。
- 不做 shared successful `published_count` / `consumed_count`；成功计数是 publisher / reader 对象本地字段。
- Task1 config 字段只有 `shm_name`、`channel_name`、`max_strategy_count`、`queue_capacity`、`create`、`remove_existing`；没有 `heartbeat_interval_ms` / `stale_consumer_timeout_ms`。
- accepted event 到达 `OrderManager` 后，`OrderManager` 保存 `exchange_order_id` 并在同线程通知自己的 `OrderSession` 更新 cancel cache；filled / cancelled terminal event 后清理该 cache。`exchange_order_id` 不是 feedback route key。
- cancel 已发出后收到 partial fill 回报时，`OrderManager` 更新累计成交但保持 `kCancelSent`，避免重新开放重复撤单入口；filled / cancelled terminal event 仍可推进终态。
- Task2 release microbenchmark 只证明本机 parser / session binary publish 成本：`BM_GateOrderFeedbackParserOneOrder_mean 65.2ns`，`BM_GateOrderFeedbackSessionBinaryToCountingPublisher_mean 95.3ns`，`BM_GateOrderFeedbackSessionBinaryToShmPublisherThenDrain_mean 105ns`。真实链路延迟仍需 live probe / profile。

### Strategy 第一版订单框架

`OrderManager` 第一版订单框架已经把交易所无关订单对象、`core/trading/order_pool.h` 通用固定容量 pool、状态推进和 Gate `OrderSession` 直接发送接到一起：

- `OrderManager` 负责订单对象生命周期、订单状态、place/cancel 执行流程和后续风控 / symbol metadata 接入位置。
- `OrderManager` 不缓存 Gate wire fields，也不暴露 `PrepareOrder()` / `SubmitOrder()`；`PlaceLimitOrder()` 创建订单后立即调用 session。
- Gate `OrderSession` 边界不扩大：仍只做 WS login、place/cancel 编码发送、`request_sequence -> local_order_id` correlation 和轻量 `OrderResponse` 回调。
- 当前已有 private `futures.orders` feedback apply；仍没有 REST reconcile、batch/amend/cancel-all、account / position feedback 或断线后未知订单状态恢复。
- `benchmark/strategy/order_gateway_benchmark.cpp` 只使用 fake order session，作为 `OrderManager` direct-send 本机 smoke baseline；它不包含真实 `OrderSession` request encoding、WebSocket frame、TLS/plain socket 或交易所响应成本，不能写成端到端性能结论。

### Strategy Runtime

当前 `StrategyRuntime<UserStrategyT, OrderSessionT, DataReaderT>` 已经是可组合的 production runtime，但本轮没有做实盘测试：

- `[strategy]` 配置文件入口示例为 `config/strategies/demo_strategy.toml` 和 `config/strategies/lead_lag_btc_strategy.toml`，其中 `lead_lag_btc_strategy.toml` 的 `strategy.name = "lead_lag"`，`strategy.config = "config/strategy/lead_lag.toml"`。
- `core/config/strategy_config.h` 解析 strategy id、mode、order capacity、data reader config、order session config 和 feedback reader 参数；`strategy_id` 范围复用 order feedback SHM lane count。
- `StrategyContext<OrderSessionT>` 是 user strategy 的下单接口，只暴露 `PlaceLimitOrder()`、`CancelOrder()` 和 `FindOrder()`。
- `StrategyRuntime::Create()` 接收已解析的 `StrategyConfig` / `DataReaderConfig` 和 order session factory，构造 `DataReader`、`OrderSession`、`OrderManager`、`StrategyContext`、user strategy 和可选 `OrderFeedbackShmReader`。
- `Run()` 在支持 `SetRuntimeHook()` 的 order session 上使用同线程 hook mode：Gate WebSocket active spin loop 每轮先驱动 runtime hook，runtime 轮询 feedback SHM，并在 `OrderSessionT::Ready()` 为 true 后 poll data reader；`OrderResponse` 由 Gate response handler 通过 `BindRuntime()` 同线程直接进入 `OnOrderResponse()`。兼容测试 session 仍可走旧的 `PollOrderResponses()` fallback。
- user strategy 可实现 `OnStart(ContextT&)`、`OnBookTicker(const BookTicker&, ContextT&)`、`OnOrderResponse(const OrderResponseEvent&, ContextT&)`、`OnOrderFeedback(const OrderFeedbackEvent&, ContextT&)`、`OnLoop(ContextT&)`、`OnIdle(ContextT&)`、`OnStop(ContextT&)` 和 `ShouldStop()`。`OnOrderResponse()` / `OnOrderFeedback()` 都先调用 `OrderManager` apply，再调用 user strategy hook。
- Gate-specific 构造留在 `tools/gate/strategy_runtime_adapter.h` 和 tool 层，core runtime 不 include `exchange/`；Gate adapter 不创建后台线程、不维护 command queue，place/cancel 在 StrategyThread / Gate session 同线程直接调用。
- `gate_demo_strategy` 默认 dry-run，不打开 WebSocket / SHM；真实提交必须显式 `--execute`，并需要先启动 `gate_order_feedback_session --connect`。

### Gate SBE 行情当前状态

当前已经以 Gate futures BBO 为样例打通：

```text
DataSession::Handle(binary MessageView)
  -> capture local_ns
  -> FuturesMarketDataClient::OnBinaryPayload
  -> SBE message header parse / schema-template dispatch
  -> BBO symbol extract
  -> flat_hash_map symbol_id lookup
  -> BookTicker decode
  -> Consumer::OnBookTicker
```

关键约束：

- `FuturesMarketDataClient<Consumer>` 使用纯模板组合，热路径不引入虚函数或 `std::function`。
- `DataSession<Consumer, WebSocketPolicy, DiagnosticsPolicy>` 不在内部创建线程；调用方决定在哪个线程运行 `Start()` / `Run()`，其中 `Run()` 在 session 内部安装 SIGINT / SIGTERM stop handler。
- core WebSocket 层负责 frame/message 完整性；exchange session 不再为外部 non-final `MessageView` 做兼容统计。
- session 处理 subscribe/unsubscribe ack/error text frame；binary SBE frame 进入 client 快路径。
- text JSON 使用 `simdjson::ondemand`；如果 `MessageView::readable_tail_bytes` 满足 `simdjson::SIMDJSON_PADDING`，直接用 padded view，否则 fallback 到 `simdjson::padded_string`。
- symbol -> symbol_id 使用 `absl::flat_hash_map`，初始化时按 symbol 数 `reserve()`，单 symbol 也走同一路径。
- symbol config 是启动期不变量；行情 payload 中出现未配置 symbol 时 debug assert，release 主路径不保留 unknown-symbol 诊断分支。
- `BookTicker` 中不保存字符串 symbol，只保存内部 `symbol_id`。
- BBO 的 `mantissa + exponent` 在 decode 阶段转成 `double`，便于策略和因子计算。
- Gate BBO binary market data 的 `event` 合约是 `Update`；其他 SBE template 不能复用这个假设。
- Gate decimal scale 当前最多支持 10 位小数；超出范围只在 debug assert，用户如需更宽精度应在进入 decoder 前自行判断。
- `ExtractTrustedBookTickerSymbol()` / `DecodeBookTickerWithHeader()` 是 client 热路径；保守的 `ExtractBookTickerSymbolForTest()` / `DecodeBookTickerForTest()` 已移到 `test/exchange/gate/sbe/book_ticker_decoder_test.cpp`，共享 payload fixture 位于 `evaluation/exchange/gate/sbe/book_ticker_payload_builder.h`，`DecodeBookTickerWithHeaderBenchmark()` 已移到 `benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp`。
- 如需引用 BTC_USDT live probe 稳定性或真实延迟结论，重新运行并保留原始输出。

2026-05-02 Gate market data release selected benchmark：

| case | time |
| --- | ---: |
| `decode_book_ticker_with_header` | 2.85ns |
| `client_on_binary_payload/1` | 2.39ns |
| `session_handle_binary/1` | 32.1ns |

`session_handle_binary` 包含 session 分流和 `NowNs()`，不等同于纯 decode 成本；多 symbol 和 text-control 详细对照见 Gate handoff 文档。

### Binance USD-M futures 行情当前状态

- 生产路径使用 raw stream URL，例如 `/public/ws/btcusdt@bookTicker`，active 后不发送 runtime `SUBSCRIBE`。
- 生产 parser 使用 `simdjson::ondemand` + unordered field lookup；ordered parser 只保留在 benchmark 中，对当前 payload 收益太小，暂不切生产。
- yyjson 只保留在 benchmark 中做 parser 对照；`aquila_binance` 生产库不链接 yyjson。
- Binance `BookTickerUpdate::symbol` 固定 copy 到对象内 `symbol_storage`；不要假设 simdjson `get_string()` 返回的 `string_view` 一定指向原 payload。
- Binance client 已支持 `EmplaceBookTickerWith()` slot-writer sink；启用 `DataShmPublisher` 时会把
  `BookTickerUpdate` 字段直接映射到 shm `BookTicker` slot，普通 sink 仍走 `OnBookTicker()`。
- symbol config 是启动期不变量；unknown symbol 使用 debug assert，不在 release 主路径记录 unknown-symbol 计数。
- client/session 构造期使用 symbol span 构建 stream target 和 lookup，构造后不再保存无用 `symbols_` span。

2026-05-02 Binance release selected benchmark：

| case | time |
| --- | ---: |
| `parse_book_ticker_padded_view` | 158ns |
| `client_on_text_payload/1` | 183ns |
| `session_handle_text_padded_view` | 195ns |
| `parse_book_ticker_then_shm_push` | 184ns |
| `parse_book_ticker_into_shm_slot` | 184ns |

`session_handle_text_padded_view` 是当前 Binance bookTicker 已实现路径的主要 microbenchmark；公网链路延迟需要重新跑 live probe。

### Strategy DataReader / SHM reader

Strategy `DataReader` 当前从 Gate / Binance data session 创建的 SHM book ticker channel 读取行情。正式配置位于
`config/data_readers/strategy_data_reader.toml`，默认 `read_mode = "latest"`；验证完整事件流时可临时改成
`drain`，不要直接把仓库默认配置改成 drain。

2026-05-06 live drain 验证：

```text
Gate data session + Binance data session -> SHM
data_reader_probe --config /tmp/aquila_strategy_data_reader_drain.toml --log-every 10000
duration: 1800s
log: /home/liuxiang/log/strategy_data_reader_drain_live_20260506_133639.log
```

reader final summary：

```text
handler_book_tickers=4635362 diagnostics_book_tickers=4635362
gate_book_ticker    book_tickers=495255  skipped=0 overruns=0 last_book_ticker_id=111902051288
binance_book_ticker book_tickers=4140107 skipped=0 overruns=0 last_book_ticker_id=10485460945723
```

结论：本次 `drain` reader 运行窗口内两个 source 均未检测到 SHM ring overrun；`drain` 模式不主动
skip，因此 reader 统计中 `skipped=0`。data session producer 的 `published_count()` 从已有 SHM header
读取初始值，`remove_existing=false` 时不一定等于本次运行窗口内的生产条数；判断本次 reader 读取结果时以
`DataReaderDiagnostics` 的 per-source summary 为准。

### LeadLag fixed 策略设计当前状态

LeadLag fixed 策略迁移当前处于设计拆解 + 底层数据结构落地阶段；策略 recorder / signal / order 组合层尚未实现。事实源：

```text
doc/leadlag-fixed-strategy-reconstruction-guide.md
doc/superpowers/specs/2026-05-08-leadlag-fixed-strategy-aquila-design.md
```

当前已确认：

- `leadlag` 作为 namespace 名使用，内部类型不再加 `LeadLag` 前缀。
- 一个 `leadlag::Strategy` 可管理多个 pair；每个 `symbol_id` 最多一个 pair，热路径使用 `pairs_by_symbol_id[ticker.symbol_id]` 直接定位。
- pair config 同时写 `symbol` 和 `symbol_id`；启动期用 lead / lag 两边 `instrument_catalog` 记录校验一致性。
- 交易基础信息从 lag `InstrumentInfo` 压缩成运行期 metadata；策略运行期不保存完整 `InstrumentInfo*`。
- `open_notional` 表示每次开仓目标名义金额；Gate futures 下单 quantity 换算必须使用 `notional_multiplier`。
- raw market state 不保存完整 `BookTicker`，只保存 `QuoteSnapshot{local_ns,bid_price,ask_price}`、`previous_quote` 和对应 valid flag；策略窗口时间使用 `BookTicker.local_ns`。
- `price_changed` 只比较 bid / ask price，不比较 volume；same-price raw tick 不替换 quote，但两边 `has_quote` 后仍用已保存 quote 推进 drift / alignment。
- Active 切换时使用 previous quote seed，lag tick 触发 Active 后下一笔 lead tick 即使 same-price 也允许 resume 一次 lead handler。
- `BboExtremaWindow` 语义是 rolling `bbo_record.window` 内 bid / ask min/max；fixed Go 使用本地自定义 `MonotonicQueue`，正常 update 摊还 `O(1)`、min/max 查询 `O(1)`。Aquila 选择 vector-backed monotonic deque，启动期按 `extrema_window_capacity` reserve，允许 vector 自动扩容保证计算准确性；`RecorderStats` 只记录 `extrema_capacity_grow_count`，具体 symbol/exchange/vector/capacity 写 log。
- fixed Go 源码已解压到 `third_party/strategy/wt-invariant-strategy-leadlag-must-fix/`，该目录被 git ignore，仅作为源码参考。
- `MoveQueue` 是按 `stats_window` 时间边界切窗，`t > RollAt` 才 roll，roll 后清空旧 samples；不是严格 rolling 最近 `stats_window`。
- 3-4 move quantile：fixed Go 是 append-only `Up` / `Down` slice，roll 时 sort 后用 `gonum/stat.Quantile(..., stat.Empirical)`；`DoubleHeap<T>` 可作为单 quantile exact 对照，能避免 roll tick `O(n log n)` spike，但同一批样本若要算多组 quantile，需要维护多组 heap，更新和内存成本随 quantile 数线性增加。当前 LeadLag threshold engine 的生产方向改为 `HistogramQuantile<T>`：单次样本更新后可在同一组 bins 上读取多组 quantile，固定 range / bins 时热路径不分配，误差由 `bin_width` 和 underflow / overflow 统计约束；实现已支持 touched-bin 查询 / reset、AVX2 / AVX512 显式查询接口，benchmark 显示当前机器上 AVX2 更稳，exact replay 对账仍保留 exact 对照口径。
- `lead_noise` / `lag_noise` 不使用单调队列；fixed Go 使用 4 个 `StreamRecorder` / 8 个本地 FIFO queue。Aquila 设计为 4 个 `RingQueue<TimedValue>`：lead/lag 各一个 mid window 和 ratio window；`RingQueue<T>` 使用 vector、capacity 必须为 2 的次幂、索引用 `& mask`，扩容后 `RecorderStats.ring_queue_capacity_grow_count` 只记录次数，细节写 log。
- `lag_spread` 是 absolute spread 的 `StreamRecorder(stats_window)` mean；fixed Go 使用 1 个 `StreamRecorder` / 2 个 FIFO queue。Aquila 设计为 `SpreadState{MeanWindow}`，底层复用 3-2 的 `RingQueue<TimedValue>`、2 的次幂 capacity 和 `RecorderStats.ring_queue_capacity_grow_count`；`LagSpreadBuffer = max(current_spread - mean_spread, 0)`。
- `core/base/` 已实现第 3 部分所需的通用抽象数据结构：`MonotonicDeque<T>`、`RingQueue<T>`、`HeapBuffer<T>`、`DoubleHeap<T>`、`HistogramQuantile<T>`；测试入口是 `core_base_structures_test`，benchmark 入口是 `core_base_structures_benchmark`。

当前 pending：

- BBO extrema 在 fixed Go 中使用 `bbo.ServerTime` 做窗口淘汰，而 `aquila` 第一版设计倾向统一使用 `BookTicker.local_ns`；严格 fixed replay 对账前需要确认是否要给 extrema 注入 fixed-compatible server timestamp。
- 下一步讨论可以继续第 4 部分 drift / alignment phase 和第 5 部分 threshold roll 细节；进入实现计划前，还需要把 recorder 组合层、drift / alignment / threshold 设计转换成具体 C++ 结构和测试清单。

### 期货合约元数据

当前已有 Gate / Binance 两个启动期 REST 查询脚本：

```text
scripts/gate/query_futures_contracts.py
scripts/binance/query_um_futures_contracts.py
```

统一字段只覆盖一类下单前必需元数据，包括 `price_tick`、`quantity_step`、`min_quantity` / `max_quantity`、`min_notional`、`notional_multiplier` 和价格偏离限制。字段说明、Gate / Binance 映射和数量单位差异见 `doc/futures_contract_metadata_fields.md`。

关键边界：

- Gate 数量默认是合约张数，Binance USD-M futures 数量是 base asset 数量。
- Gate `enable_decimal=true` 时当前不推导 `quantity_step` / `quantity_decimal_places`，避免把未确认规则带入下单路径。
- 这组 metadata 应在启动期构建并缓存，供 strategy、risk check 和 exchange adapter 使用；不要在行情或下单热路径里查询 REST。

## 常用验证命令

构建：

```bash
./build.sh debug
./build.sh release
```

WebSocket 测试：

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
ctest --test-dir build/release -R websocket_ --output-on-failure
```

核心 benchmark：

```bash
taskset -c 2 ./build/release/benchmark/websocket/frame_codec_benchmark
taskset -c 2 ./build/release/benchmark/websocket/third_party_frame_codec_comparison_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_mixed_path_benchmark
```

Gate 双 WS login：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/test_gate_ws_dual_login.py --timeout 8
```

Gate SBE / market data 测试：

```bash
./build/debug/test/exchange/gate/sbe/gate_sbe_message_dispatcher_test
./build/debug/test/exchange/gate/sbe/gate_sbe_book_ticker_decoder_test
./build/debug/test/exchange/gate/market_data/gate_futures_market_data_client_test
./build/debug/test/exchange/gate/market_data/gate_data_session_test
./build/release/test/exchange/gate/sbe/gate_sbe_message_dispatcher_test
./build/release/test/exchange/gate/sbe/gate_sbe_book_ticker_decoder_test
./build/release/test/exchange/gate/market_data/gate_futures_market_data_client_test
./build/release/test/exchange/gate/market_data/gate_data_session_test
```

Gate market data benchmark：

```bash
./build/release/benchmark/exchange/gate/market_data/gate_futures_market_data_benchmark --benchmark_filter='gate_market_data/(decode_book_ticker_with_header|client_on_binary_payload|client_on_message_binary|session_handle_binary|session_handle_subscribe_ack|session_handle_subscribe_ack_padded_view)'
./build/release/benchmark/websocket/message_handler_dispatch_benchmark
```

Gate submit response parser 测试：

```bash
./build/debug/test/exchange/gate/trading/gate_submit_response_parser_test
./build/release/test/exchange/gate/trading/gate_submit_response_parser_test
```

Gate trading tests：

```bash
./build/debug/test/config/order_session_config_test
./build/debug/test/exchange/gate/trading/gate_order_codecs_test
./build/debug/test/exchange/gate/trading/gate_order_request_encoder_test
./build/debug/test/exchange/gate/trading/gate_submit_response_parser_test
./build/debug/test/exchange/gate/trading/gate_order_session_test
ctest --test-dir build/debug -R '(gate_(order|submit)|order_session_config)' --output-on-failure
```

Order feedback SHM Task1 tests / config / benchmark：

```bash
cmake --build build/debug --target core_order_feedback_event_test core_order_feedback_shm_test order_feedback_shm_config_test -j8
./build/debug/test/core/trading/core_order_feedback_event_test
./build/debug/test/core/trading/core_order_feedback_shm_test
./build/debug/test/config/order_feedback_shm_config_test
ctest --test-dir build/debug -R '(core_order_feedback|order_feedback_shm_config)' --output-on-failure
cmake --build build/release --target order_feedback_shm_benchmark -j8
./build/release/benchmark/core/trading/order_feedback_shm_benchmark --benchmark_filter='PublishThenDrain|PollOneWithRefill|PublishPollLoop|PublishGlobalGapThenDrain' --benchmark_min_time=0.01s
```

`order_feedback_shm_benchmark` 是 transport benchmark；`PublishThenDrain` / `PollOneWithRefill`
包含 drain / refill 维护，不能写成纯 publish / poll latency，也不能外推为 Gate parser、Strategy apply
或端到端性能结论。

Gate order feedback Task2 tests / tool / benchmark：

```bash
cmake --build build/debug --target gate_order_feedback_parser_test gate_order_feedback_session_test gate_order_request_encoder_test gate_order_session_test order_feedback_session_config_test strategy_test strategy_order_feedback_shm_integration_test gate_order_feedback_session -j8
./build/debug/test/exchange/gate/trading/gate_order_feedback_parser_test
./build/debug/test/exchange/gate/trading/gate_order_feedback_session_test
./build/debug/test/exchange/gate/trading/gate_order_request_encoder_test
./build/debug/test/exchange/gate/trading/gate_order_session_test
./build/debug/test/config/order_feedback_session_config_test
./build/debug/test/strategy/strategy_test
./build/debug/test/strategy/strategy_order_feedback_shm_integration_test
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --duration-sec 0.1
cmake --build build/release --target gate_order_feedback_parser_benchmark -j8
./build/release/benchmark/exchange/gate/trading/gate_order_feedback_parser_benchmark --benchmark_min_time=0.1s --benchmark_repetitions=3
```

OrderManager framework tests：

```bash
cmake --build build/debug --target core_order_pool_test strategy_test gate_strategy_order -j8
./build/debug/test/core/trading/core_order_pool_test
./build/debug/test/strategy/strategy_test
./build/debug/tools/gate_strategy_order --contract BTC_USDT --side buy --order-type limit --size 1 --price 81000 --tif gtc
ctest --test-dir build/debug -R 'core_order_pool|strategy' --output-on-failure
```

Gate submit/order benchmark：

```bash
taskset -c 2 ./build/release/benchmark/exchange/gate/trading/gate_submit_response_parse_benchmark --benchmark_filter='gate_submit_response_parse_order_place_ack_echo_simdjson_ack_minimal_padded_view/' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
./build/release/benchmark/exchange/gate/trading/gate_order_session_benchmark --benchmark_filter='BM_EncodePlaceOrder|BM_EncodeCancelOrder|BM_ParsePlaceResult|BM_ParsePlaceResultForOrderSession|BM_OrderSessionHandlePlaceAck|BM_OrderSessionHandlePlaceResult' --benchmark_min_time=0.01s
```

OrderManager order gateway benchmark：

```bash
cmake --build build/release --target core_order_pool_benchmark strategy_order_gateway_benchmark -j8
./build/release/benchmark/core/trading/core_order_pool_benchmark --benchmark_min_time=0.01s
./build/release/benchmark/strategy/strategy_order_gateway_benchmark --benchmark_filter='BM_OrderManagerPlaceLimitOrder|BM_OrderManagerCancelAcceptedOrder' --benchmark_min_time=0.01s
```

这组 benchmark 是 core order pool 和 `OrderManager` + fake order session baseline，不包含真实 `OrderSession` 编码、WebSocket 或 socket。

LeadLag base data structures tests / benchmark：

```bash
cmake --build build/debug --target core_base_structures_test -j8
./build/debug/test/core/base/core_base_structures_test
cmake --build build/release --target core_base_structures_test core_base_structures_benchmark -j8
ctest --test-dir build/release -R core_base_structures_test --output-on-failure
./build/release/benchmark/core/base/core_base_structures_benchmark --benchmark_min_time=0.05s
```

`core_base_structures_benchmark` 是 `core/base` 局部 microbenchmark，只比较数据结构扩容 / 不扩容、`DoubleHeap` exact quantile、`HistogramQuantile` 近似 quantile、value-only 查询和 value+reset 查询的局部成本与误差；不能外推为完整 LeadLag 策略链路时延。

Gate data session dry-run / BBO live probe：

```bash
./build/debug/tools/gate_data_session
./build/debug/tools/gate_futures_book_ticker_probe --contract BTC_USDT --symbol-id 1 --duration-ms 10000
```

Binance USD-M futures bookTicker 测试和 benchmark：

```bash
./build/debug/test/config/data_session_config_test
./build/debug/test/exchange/binance/market_data/binance_book_ticker_parser_test
./build/debug/test/exchange/binance/market_data/binance_futures_market_data_client_test
./build/debug/test/exchange/binance/market_data/binance_data_session_test
./build/release/benchmark/exchange/binance/market_data/binance_futures_market_data_benchmark --benchmark_filter='binance_market_data/(parse_book_ticker|parse_book_ticker_padded_view|parse_book_ticker_then_shm_push|parse_book_ticker_into_shm_slot|client_on_text_payload|session_handle_text|session_handle_text_padded_view)'
```

Binance USD-M futures data session dry-run / bookTicker live probe：

```bash
./build/debug/tools/binance_data_session
./build/debug/tools/binance_futures_book_ticker_probe --contract BTCUSDT --symbol-id 1 --duration-ms 10000
```

Data reader 配置 / runtime 测试：

```bash
./build/debug/test/config/data_reader_config_test
./build/debug/test/core/market_data/core_market_data_reader_test
./build/debug/test/core/market_data/core_market_data_shm_test
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
```

Data reader live drain 验证需要先启动 Gate / Binance data session 写 SHM，再用临时 drain 配置运行 probe；
不要把仓库默认 `strategy_data_reader.toml` 改成 drain：

```bash
/usr/bin/timeout --kill-after=10s 1860s ./build/debug/tools/gate_data_session --connect
/usr/bin/timeout --kill-after=10s 1860s ./build/debug/tools/binance_data_session --connect
/usr/bin/timeout --kill-after=10s 1800s ./build/debug/tools/data_reader_probe --config /tmp/aquila_strategy_data_reader_drain.toml --log-every 10000
```

Gate account / 期货合约元数据脚本：

```bash
scripts/gate/query_gate_account_test.py
scripts/gate/place_futures_order_test.py
scripts/gate/run_futures_order_smoke_test.py
scripts/gate/query_futures_contracts_test.py
scripts/binance/query_um_futures_contracts_test.py
/home/liuxiang/dev/pyenv/lx/bin/python -m py_compile \
  scripts/gate/query_gate_account.py \
  scripts/gate/query_gate_account_test.py \
  scripts/gate/place_futures_order.py \
  scripts/gate/place_futures_order_test.py \
  scripts/gate/run_futures_order_smoke.py \
  scripts/gate/run_futures_order_smoke_test.py \
  scripts/gate/query_futures_contracts.py \
  scripts/gate/query_futures_contracts_test.py \
  scripts/binance/query_um_futures_contracts.py \
  scripts/binance/query_um_futures_contracts_test.py
```

REST 查询 smoke 需要外网：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/query_gate_account.py --settle usdt --currency USDT --contract BTC_USDT --allow-partial
scripts/gate/query_futures_contracts.py BTC_USDT ETH_USDT --format csv
scripts/binance/query_um_futures_contracts.py BTCUSDT ETHUSDT --format csv
```

Gate REST futures 下单脚本默认 dry-run，不需要外网或凭证；真实提交必须显式 `--execute`，并应确认价格符合当前合约限制：

```bash
scripts/gate/place_futures_order.py --contract BTC_USDT --side buy --size 1 --price 1 --tif gtc
```

Gate REST futures 5 轮小额 order smoke 需要 `TEST_KEY` / `TEST_SECRET`、外网和显式 `--execute`；它会提交真实订单，填单后用 reduce-only market close，未填单则撤单：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/run_futures_order_smoke.py --contract BTC_USDT --iterations 5 --size 1 --max-open-size 2 --execute
```

2026-05-07 本机实盘 REST smoke 摘要：`BTC_USDT`、1 手、5 轮结果 `5/5 filled_and_closed`，最终
`position size=0`、`pending_orders=0`、`open orders=[]`。这不是 C++ WS `OrderSession` live smoke。

## 接手注意事项

- 不要 push，除非用户明确要求。
- 修改完成后默认提交到当前 branch，commit message 用英文。
- 文档中文；代码注释英文。
- 性能结论必须写明 benchmark / live probe / profile 证据。
- `third_party/` 中的 Drogon、Sirius、MengRao websocket 是参考代码，通常不提交改动。
- 如果继续 Gate 交易实现，先确认 `OrderSession` / `OrderFeedbackSession` / Strategy 边界，不要直接把 Sirius 的 Drogon 架构搬进主线。
- 如果用户输入“结束对话”，按本文档的固定流程先整理文档和 onboarding，再提交并给出下一轮 onboarding 提示。

## 下一步建议

如果新对话从 LeadLag fixed 策略迁移继续，建议顺序：

1. 读取 `doc/leadlag-fixed-strategy-reconstruction-guide.md`。
2. 读取 `doc/superpowers/specs/2026-05-08-leadlag-fixed-strategy-aquila-design.md`。
3. 参考 `third_party/strategy/wt-invariant-strategy-leadlag-must-fix/leadlag/algo/` 中的 fixed Go 源码，尤其是 `strategy.go`、`analysis.go`、`move.go`、`cost_model.go` 和 `execute_cache.go`。
4. 第 3 部分 recorder / queue / noise / spread / move quantile 设计已补齐，底层抽象数据结构已落地到 `core/base/`；下一步优先继续第 4 部分 drift / alignment phase 和第 5 部分 threshold engine 的 `aquila` C++ 落地设计。
5. 在实现计划前，先决定 BBO extrema 是否继续用 `BookTicker.local_ns`，还是为了 fixed replay 对账引入 server timestamp 输入。
6. 进入实现时，先用 `core/base` 组合 `BboExtremaWindow`、`MeanWindow` / `MeanStdWindow`、`NoiseState`、`SpreadState`、`MoveQuantileWindow`，并补针对 fixed Go 语义的单元测试和 replay 对账测试。

如果新对话从 Gate 交易继续，建议顺序：

1. 读取 `doc/agent-handoff-gate-trade-architecture.md`。
2. 读取 `docs/superpowers/specs/2026-05-07-gate-order-session-design.md`。
3. 读取 `docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md`、`docs/superpowers/specs/2026-05-08-order-feedback-shm-transport-design.md` 和 `docs/superpowers/specs/2026-05-08-gate-order-feedback-session-strategy-design.md`。
4. 直接从“代码入口”和 `doc/agent-handoff-gate-trade-architecture.md` 追溯已实现边界、验证命令和 benchmark 口径；已完成的执行计划文档不再保留。
5. 如果继续 strategy runtime，从 `core/strategy/strategy_runtime.h`、`tools/gate/strategy_runtime_adapter.h`、`tools/gate/demo_strategy.h` 和 `tools/gate/demo_strategy.cpp` 接手；当前 production `Run()` hook mode / Gate adapter / demo dry-run tool 已实现。
6. 准备最小 live smoke：先运行 `gate_order_feedback_session --connect`，再在用户明确允许后运行 `gate_demo_strategy --execute`，用 `wait_seconds` / `rounds` 小参数覆盖 filled-close 和 unfilled-cancel 分支，保留原始输出，并用 REST 查询确认无残留订单 / 仓位。
7. 明确 REST reconcile 和 feedback WS 断线策略，覆盖未知订单状态、断线后本地状态恢复、人工介入边界和新开仓暂停 / 恢复条件。
8. 接入 symbol metadata / risk check：启动期缓存合约元数据，Strategy submit 前完成 tick、quantity、notional、reduce-only 等校验；当前 `demo` 只按 ask price 字符串下单，尚未做 tick rounding。
9. 增加端到端 benchmark：覆盖 `StrategyRuntime -> GateOrderSessionAdapter -> OrderSession` 下单请求和 `OrderFeedbackSession -> SHM -> StrategyRuntime` 回报消费；真实链路性能结论必须另跑 live probe 或 profile。
