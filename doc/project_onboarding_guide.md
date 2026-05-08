# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读整个历史对话的前提下，快速理解 `aquila` 当前状态、重要文档、代码入口和下一步应该怎么接手。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 当前重点：WebSocket 内核已经完成 P0/P1/P2/P3 主体；Gate futures SBE BBO 行情与 Binance USD-M futures JSON bookTicker 行情、`BookTicker`、market data client、data session、SHM sink、strategy `DataReader`、benchmark、live probe 和每进程 config / log config 已落地；Gate / Binance 期货合约元数据脚本已输出统一一类下单前字段；行情热路径已按协议不变量收口；Gate `OrderSession` 第一版 submit/cancel C++ 主路径和 Strategy 第一版订单框架已落地，下一阶段转向 C++ WS `OrderSession` live smoke、私有回报和 reconcile。
- 构建：CMake + `build.sh`。
- 核心原则：正确性、确定性、最低延迟、尾延迟可控、固定容量、少动态分配、性能结论必须有 benchmark / profile / live probe 证据。
- 当前建议分支入口：`main`。

## 最近已完成

截至 2026-05-07，`main` 已完成的主要内容：

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
20. Gate `OrderSession` 当前边界已调整：Strategy 做风控、建单、订单池和状态机；`OrderSession` 直接接收订单 struct，在发送路径完成 Gate place/cancel JSON 序列化、`request_sequence -> local_order_id` correlation 和轻量同步 `OrderResponse` 回调。
21. `config/order_sessions/gate_order_session.toml` 和 Gate `OrderSessionConfig` parser 已落地，按 data session 风格复用通用 WebSocket config parser；TOML 只写 `[order_session]`、credentials env 名和 `[order_session.websocket.*]`，WS target 由 `settle` 生成 `/v4/ws/<settle>`。
22. `AGENTS.md` 已加入 subagent 规则：主会话派发 `spawn_agent` / subagent 时默认显式设置 `reasoning_effort = "xhigh"`，并默认不让 subagent 再派生下级 subagent。
23. Strategy 第一版订单框架已按 Sirius 风格重构：`core/trading/order_pool.h`、`strategy/order_types.h` 和 `strategy/strategy.h` 覆盖通用固定容量订单池、订单创建、状态推进和直接 session 发送；Strategy 不维护 exchange order id 索引，不再缓存 Gate wire fields，也不再暴露 `PrepareOrder()` / `SubmitOrder()` 两阶段接口。
24. `scripts/gate/run_futures_order_smoke.py` 和 `scripts/gate/run_futures_order_smoke_test.py` 已落地；2026-05-07 使用 Gate REST 对 `BTC_USDT`、1 手、5 轮真实 smoke，结果 `5/5 filled_and_closed`，最终 `position size=0`、`pending_orders=0`、`open orders=[]`。这只是 REST smoke，不是 C++ WS `OrderSession` live smoke。
25. Strategy / Gate 第一版边界已明确：Strategy 负责订单对象、风控位置、状态和执行流程；Gate `OrderSession` 负责从订单 struct 现场编码 place/cancel 请求、correlation 和轻量 response。Strategy benchmark 是 fake session direct-send baseline，不包含真实 WebSocket 或 socket 成本。

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
docs/superpowers/plans/2026-05-07-gate-order-session-implementation-plan.md
docs/superpowers/plans/2026-05-07-strategy-order-framework-implementation-plan.md
docs/superpowers/plans/2026-05-07-order-session-struct-flow-implementation-plan.md
doc/websocket_read_write_benchmark_comparison.md
doc/data_reader_config.md
```

如果继续 Gate 交易架构，优先读 `doc/agent-handoff-gate-trade-architecture.md`、`docs/superpowers/specs/2026-05-07-gate-order-session-design.md`、`docs/superpowers/plans/2026-05-07-gate-order-session-implementation-plan.md`、`docs/superpowers/plans/2026-05-07-strategy-order-framework-implementation-plan.md` 和 `docs/superpowers/plans/2026-05-07-order-session-struct-flow-implementation-plan.md`。如果继续 Binance 行情，优先读 `doc/agent-handoff-binance-market-data.md`。如果继续 WebSocket 性能优化，优先读 `doc/websocket_client_future_optimizations.md`。

## 给下一个对话的 onboarding 提示

请先在 `/home/liuxiang/dev/aquila` 运行 `git status --short --branch` 和 `git log --oneline -8`，
然后依次阅读 `AGENTS.md`、`README.md`、`doc/project_onboarding_guide.md`、`doc/evaluation_support.md`。
以 onboarding 的“最近已完成”“代码入口”“当前重要结论”和“下一步建议”为事实源；如果继续 Gate 交易架构，
再读 `doc/agent-handoff-gate-trade-architecture.md`、`docs/superpowers/specs/2026-05-07-gate-order-session-design.md`、
`docs/superpowers/plans/2026-05-07-gate-order-session-implementation-plan.md`
、`docs/superpowers/plans/2026-05-07-strategy-order-framework-implementation-plan.md` 和 `docs/superpowers/plans/2026-05-07-order-session-struct-flow-implementation-plan.md`，如果继续 Binance 行情，再读
`doc/agent-handoff-binance-market-data.md`，如果继续 data session / config，再读
`doc/data_session_config.md`；如果继续 strategy data reader，再读 `doc/data_reader_config.md`。修改后按项目规则跑对应验证并自动提交；如果用户输入“结束对话”，
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
| `doc/agent-handoff-gate-trade-architecture.md` | 继续 Gate 交易架构或 Gate SBE 行情时读 | Gate 文档结论、SBE BBO 当前落地状态、Sirius 旧实现、双 WS login 测试、三种线程模型。 |
| `docs/superpowers/specs/2026-05-07-gate-order-session-design.md` | 继续 Gate 交易架构或审查 submit/cancel 边界时读 | `aquila::gate::OrderSession` 第一版范围、Strategy / OrderSession / OrderFeedbackSession 边界、直接 struct 发单输入、`RequestIdCodec` / `OrderTextCodec` / response correlation 语义。 |
| `docs/superpowers/plans/2026-05-07-gate-order-session-implementation-plan.md` | 审查 Gate `OrderSession` 第一版实现或追溯任务拆分时读 | TDD 实现任务：types/codecs、login signature/request encoder、submit parser correlation、session、benchmark、handoff/onboarding 更新。当前 Task 1-6 已完成。 |
| `docs/superpowers/plans/2026-05-07-strategy-order-framework-implementation-plan.md` | 追溯 Strategy 订单框架第一版历史边界时读 | Strategy 订单对象、core `OrderPool`、state machine、direct-send fake session benchmark、REST smoke 和文档同步任务拆分；当前实现已被 struct-flow plan 调整。 |
| `docs/superpowers/plans/2026-05-07-order-session-struct-flow-implementation-plan.md` | 继续 Strategy / Gate OrderSession 直接 struct 发单边界时读 | Strategy 直接建单入 `core/trading` 的 `OrderPool` 并发送订单 struct；Gate `OrderSession` 在发送路径完成 place/cancel JSON 序列化；移除 Strategy 侧 Gate wire cache、exchange id 索引、`PrepareOrder()` 和 `SubmitOrder()`。 |
| `doc/agent-handoff-binance-market-data.md` | 继续 Binance USD-M futures bookTicker 行情时读 | raw stream URL、JSON parser、client/session、benchmark 和 probe 入口。 |

## 代码入口

### Core 基础类型

| 文件 | 职责 |
| --- | --- |
| `core/common/result.h` | 通用 `Result<T>`，用于启动期 parser / loader 这类显式返回成功值或错误字符串的场景。 |
| `core/common/types.h` | 项目通用枚举，当前包含 `aquila::Exchange`。 |
| `core/common/constants.h` | 项目通用常量，当前包含缓存行大小等基础常量。 |
| `core/trading/order_pool.h` | 交易通用固定容量订单池；slot vector 固定为 max live 的 2 倍，local id 查找走 `absl::flat_hash_map`；map reserve hint 在 max live 小于 1024 时为 16x，否则为 8x；不维护 exchange order id 索引；构造期拒绝超过 `uint32_t` slot index 范围的容量。 |
| `core/utils/numeric.h` | 基于 `fast_float::from_chars` 的 `ToNumeric<T>` / `ToDouble` / `ToUint64` 等热路径数字转换 helper，失败只在 debug assert。 |
| `core/market_data/types.h` | 统一行情数据结构，当前包含 `aquila::BookTicker`。 |

### 配置实现

| 文件 | 职责 |
| --- | --- |
| `core/config/websocket_config.h` | 冷路径 WebSocket TOML 配置结构、默认值和到 `websocket::ConnectionConfig` 的转换；由 `aquila_config` target 暴露，TOML 解析使用 `toml++`，诊断日志走 Nova 封装，parser 只保留必填项和枚举映射约束。 |
| `core/config/instrument_catalog.h` | 启动期 instrument CSV catalog，加载 `aquila.instrument.v1` 的完整字段；当前 data session 只消费 `symbol_id`、`exchange`、`symbol`、`exchange_symbol`，lookup 使用 `absl::flat_hash_map`。 |
| `core/config/data_reader_config.h` | Strategy data reader TOML parser / loader，加载 instrument catalog，并生成 `DataReader` 可直接消费的多 source config。 |
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
| `strategy/order_types.h` | Strategy 订单创建请求、`StrategyOrder`、place/cancel/result event 类型；订单对象保存 symbol、price_text、数量、TIF 和状态，不保存 Gate wire cache。 |
| `strategy/strategy.h` | 模板化 `Strategy<OrderSessionT>`，提供 `PlaceLimitOrder()`、`CancelOrder()` 和 response apply；发单时直接把订单 struct 交给 session。 |
| `test/core/trading/order_pool_test.cpp` | 通用 `OrderPool` 本地订单 ID、容量限制、slot 复用、指针稳定和 zero capacity 测试。 |
| `test/strategy/strategy_test.cpp` | Strategy place/cancel/response 状态推进测试。 |
| `benchmark/strategy/order_gateway_benchmark.cpp` | Strategy direct-send fake session baseline；不包含真实 WebSocket 或 socket。 |

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
| `exchange/gate/trading/order_signature.h` / `exchange/gate/trading/order_signature.cpp` | Gate WS API login HMAC-SHA512 lowercase hex 签名。 |
| `exchange/gate/trading/order_request_encoder.h` | `futures.login`、`futures.order_place`、`futures.order_cancel` JSON request 固定缓冲区编码。 |
| `exchange/gate/trading/submit_response_parser.h` | Gate submit WS JSON response parser，生产路径使用 `simdjson::ondemand`，保留 hash 字段并新增 decoded request id / req_id / local order id correlation 字段。 |
| `exchange/gate/trading/order_session.h` | 模板化 `OrderSession<ResponseHandler, WebSocketPolicy, Diagnostics>`，持有 `BasicWebSocketClient`，处理 WS login、place/cancel 发送、response correlation 和同步回调。 |
| `exchange/gate/trading/order_session_config.h` / `exchange/gate/trading/order_session_config.cpp` | Gate `OrderSession` 启动期 TOML parser；复用通用 WebSocket config，生成 `/v4/ws/<settle>` target，保留 credentials env 名和 `request_map_capacity`。 |
| `config/order_sessions/gate_order_session.toml` | Gate order session 示例配置；包含 `[log]`、`[order_session]`、`[order_session.credentials]` 和 `[order_session.websocket.*]`。 |
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
| `benchmark/core/trading/order_pool_benchmark.cpp` | 通用 `OrderPool` create-until-capacity、live find 和 create/find/erase recycle microbenchmark。 |
| `benchmark/strategy/order_gateway_benchmark.cpp` | Strategy direct-send fake session baseline，不包含真实 `OrderSession` 编码、WebSocket 或 socket。 |

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
feedback SPSC -> StrategyThread
```

原因：

- 下单路径最短。
- 行情 burst 时策略线程仍以行情为最高优先级。
- `futures.orders` / `futures.usertrades` / SBE decode 不污染下单路径。
- Gate 已通过脚本验证允许同一账号两个 WS 同时 login。

扩展到 Gate + Binance 行情后的系统线程命名：

```text
GateDataSessionThread
  - GateDataSession

BinanceDataSessionThread
  - BinanceDataSession

StrategyThread
  - Strategy
  - Gate OrderSession
  - Binance OrderSession

GateOrderFeedbackThread
  - Gate OrderFeedbackSession

BinanceOrderFeedbackThread
  - Binance OrderFeedbackSession
```

其中 `risk control`、`order management` 和 `order execution` 归属于 `Strategy` 模块；`OrderSession` 是上行交易指令和轻量 API response 通道，`OrderFeedbackSession` 是下行私有回报通道。

2026-05-07 已确认 C++ 命名位于 `aquila::gate` namespace，交易 submit/cancel 类名直接使用
`OrderSession`，不带交易所名前缀。第一版 `OrderSession` 已实现，只覆盖 `futures.login`、
`futures.order_place`、`futures.order_cancel`、`ack=true`、final result/error、`request_sequence -> local_order_id`
轻量关联和同步 `OrderResponse` 回调；不做风控、订单状态机、私有订单/成交/仓位回报、REST reconcile、batch/amend/cancel all。

OrderSession 第一版关键结论：

- Strategy 负责风控、symbol metadata 校验、订单对象、订单状态机和订单执行逻辑，不缓存 Gate wire fields。
- `OrderSession` 接收订单 struct，不接收 Strategy 侧缓存的 wire-ready request，也不维护 pending order table。
- `OrderSession` 不做额外订单语义防御性检查；本地 id fallback text 编码失败时返回 `kInvalidLocalOrderId`，该路径会消耗 request sequence 但不进入 correlation map。
- place `ack=true` 只表示 Gate 收到请求，不建立交易所 order id 映射，也不清理 correlation。
- ack/result 成功形态必须是 HTTP 200；place final result 才在 `OrderSession` 内建立本地订单和交易所订单的匹配信息，并清理 correlation。
- cancel 优先使用 `OrderSession` 内部缓存的 exchange order id 编码；没有缓存时 fallback 到本地 `text="t-<local_order_id>"`。该缓存最多保留 `request_map_capacity` 条，可通过 `forget_exchange_order_id_for_local_order()` 显式清理。cancel response 使用 encoded request id 做主 correlation；如果 result 携带 `text`，必须匹配本地 id；仅 exchange-id 的进一步原始 cancel id 校验属于后续增强设计。
- 断线时清空 correlation，不构造假的 rejected/cancelled response；Strategy 后续通过 state/reconcile 处理未知状态。

### Strategy 第一版订单框架

Strategy 第一版订单框架已经把交易所无关订单对象、`core/trading/order_pool.h` 通用固定容量 pool、状态推进和 Gate `OrderSession` 直接发送接到一起：

- Strategy 负责订单对象生命周期、订单状态、place/cancel 执行流程和后续风控 / symbol metadata 接入位置。
- Strategy 不缓存 Gate wire fields，也不暴露 `PrepareOrder()` / `SubmitOrder()`；`PlaceLimitOrder()` 创建订单后立即调用 session。
- Gate `OrderSession` 边界不扩大：仍只做 WS login、place/cancel 编码发送、`request_sequence -> local_order_id` correlation 和轻量 `OrderResponse` 回调。
- 当前没有 private feedback、REST reconcile、batch/amend/cancel-all 或真实成交回报状态合并；断线后的未知订单状态仍需后续 reconcile / feedback 设计处理。
- `benchmark/strategy/order_gateway_benchmark.cpp` 只使用 fake order session，作为 Strategy direct-send 本机 smoke baseline；它不包含真实 `OrderSession` request encoding、WebSocket frame、TLS/plain socket 或交易所响应成本，不能写成端到端性能结论。

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

Strategy order framework tests：

```bash
cmake --build build/debug --target core_order_pool_test strategy_test -j8
./build/debug/test/core/trading/core_order_pool_test
./build/debug/test/strategy/strategy_test
ctest --test-dir build/debug -R 'core_order_pool|strategy' --output-on-failure
```

Gate submit/order benchmark：

```bash
taskset -c 2 ./build/release/benchmark/exchange/gate/trading/gate_submit_response_parse_benchmark --benchmark_filter='gate_submit_response_parse_order_place_ack_echo_simdjson_ack_minimal_padded_view/' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
./build/release/benchmark/exchange/gate/trading/gate_order_session_benchmark --benchmark_filter='BM_EncodePlaceOrder|BM_EncodeCancelOrder|BM_ParsePlaceResult|BM_ParsePlaceResultForOrderSession|BM_OrderSessionHandlePlaceAck|BM_OrderSessionHandlePlaceResult' --benchmark_min_time=0.01s
```

Strategy order gateway benchmark：

```bash
cmake --build build/release --target core_order_pool_benchmark strategy_order_gateway_benchmark -j8
./build/release/benchmark/core/trading/core_order_pool_benchmark --benchmark_min_time=0.01s
./build/release/benchmark/strategy/strategy_order_gateway_benchmark --benchmark_filter='BM_StrategyPlaceLimitOrder|BM_StrategyCancelAcceptedOrder' --benchmark_min_time=0.01s
```

这组 benchmark 是 core order pool 和 fake Strategy order session baseline，不包含真实 `OrderSession` 编码、WebSocket 或 socket。

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

如果新对话从 Gate 交易继续，建议顺序：

1. 读取 `doc/agent-handoff-gate-trade-architecture.md`。
2. 读取 `docs/superpowers/specs/2026-05-07-gate-order-session-design.md`。
3. 读取 `docs/superpowers/plans/2026-05-07-gate-order-session-implementation-plan.md`、`docs/superpowers/plans/2026-05-07-strategy-order-framework-implementation-plan.md` 和 `docs/superpowers/plans/2026-05-07-order-session-struct-flow-implementation-plan.md`，用于追溯已完成实现边界和验证命令。
4. 补 C++ WS `OrderSession` live smoke，使用极小数量和明确执行 / 撤单或平仓流程，保留原始输出；不要把 live 结果写成通用性能结论。
5. 设计并实现 `OrderFeedbackSession`：Gate SBE 私有 `orders`、`usertrades`、`positions` decode，固定 feedback event，feedback SPSC 到 StrategyThread。
6. 明确 REST reconcile 和 feedback WS 断线策略，覆盖未知订单状态、断线后本地状态恢复和人工介入边界。
7. 接入 symbol metadata / risk check：启动期缓存合约元数据，Strategy submit 前完成 tick、quantity、notional、reduce-only 等校验。
8. 增加端到端 benchmark：覆盖 `Strategy -> OrderSession` 下单请求构建 / 发送和 `OrderSession::Handle() -> Strategy` 回调消费；真实链路性能结论必须另跑 live probe 或 profile。
