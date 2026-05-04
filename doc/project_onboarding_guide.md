# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读整个历史对话的前提下，快速理解 `aquila` 当前状态、重要文档、代码入口和下一步应该怎么接手。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 当前重点：WebSocket 内核已经完成 P0/P1/P2/P3 主体；Gate futures SBE BBO 行情与 Binance USD-M futures JSON bookTicker 行情、`BookTicker`、market data client、market data session、benchmark 和 live probe 已落地；Gate / Binance 期货合约元数据脚本已输出统一一类下单前字段；行情热路径已按协议不变量收口，下一阶段继续 symbol metadata 接入策略 / 下单链路和 Gate 交易 submit/update 设计。
- 构建：CMake + `build.sh`。
- 核心原则：正确性、确定性、最低延迟、尾延迟可控、固定容量、少动态分配、性能结论必须有 benchmark / profile / live probe 证据。
- 当前建议分支入口：`main`。

## 最近已完成

截至 2026-05-02，`main` 已完成并准备同步到 `origin/main` 的主要内容：

1. Gate / Binance market data 热路径防御性分支收口。
2. Gate BBO 生产 decoder 只保留 trusted 路径，保守 decode 和 benchmark wrapper 已移出生产 header。
3. `QueuedFrameCodec` 与 Gate BBO payload builder 已迁移到 `evaluation/`。
4. 新增 header-only `aquila_evaluation` target，只允许 test / benchmark 使用。
5. Gate / Binance 期货合约元数据脚本已按统一一类下单前字段输出 `pandas.DataFrame`。
6. `doc/futures_contract_metadata_fields.md` 已记录字段含义、交易所映射和当前空值语义。
7. `README.md`、本 onboarding、Gate / Binance handoff 和 evaluation 文档已按当前实现边界对齐。

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
doc/websocket_read_write_benchmark_comparison.md
```

如果继续 Gate 交易架构，优先读 `doc/agent-handoff-gate-trade-architecture.md`。如果继续 Binance 行情，优先读 `doc/agent-handoff-binance-market-data.md`。如果继续 WebSocket 性能优化，优先读 `doc/websocket_client_future_optimizations.md`。

## 文档索引

| 文档 | 什么时候读 | 关键内容 |
| --- | --- | --- |
| `AGENTS.md` | 每次新会话最先读 | 中文/英文约定、低延迟原则、测试/benchmark/提交规则。 |
| `README.md` | 了解构建和工具入口 | build、ctest、benchmark、probe、latency compare 的运行方式。 |
| `doc/websocket_frame_codec_receive_strategies.md` | 理解 FrameCodec decode 为什么这样设计 | mirrored ring、direct delivery、fast path、QueuedFrameCodec、decode 收口结论。 |
| `doc/websocket_read_write_benchmark_comparison.md` | 快速看 read/write benchmark 对比 | `aquila`、Drogon-style、`third_party/websocket` read/write 差异和数值。 |
| `doc/websocket_client_future_optimizations.md` | 继续 WebSocket 优化时读 | read/write/active spin/network 的未来优化 backlog。 |
| `doc/websocket_prepared_write_options.md` | 调整 WebSocket 写路径预分配容量时读 | `DefaultWebSocketOptions`、`MakeConnectionConfig<OptionsT>()`、prepared write slots/bytes 的含义和使用边界。 |
| `doc/data_session_config.md` | 修改 `config/data_sessions/*.toml` 或新增 data session 配置时读 | 每进程一份 data session config、`instrument_catalog`、`data_session.subscribe_symbols`、symbol pool 生成、WebSocket endpoint / execution_policy / read_path / heartbeat / reconnect 字段和默认值，以及 TOML / CSV / log 依赖边界。 |
| `doc/evaluation_support.md` | 增加 test / benchmark 共享辅助代码时读 | `evaluation/` 目录、`aquila_evaluation` target、生产路径禁止依赖 evaluation 的边界。 |
| `doc/futures_contract_metadata_fields.md` | 处理 Gate / Binance 合约基础信息和下单前校验字段时读 | 统一 DataFrame 字段、Gate/Binance 字段映射、quantity 单位差异和当前空值语义。 |
| `doc/agent-handoff-gate-trade-architecture.md` | 继续 Gate 交易架构或 Gate SBE 行情时读 | Gate 文档结论、SBE BBO 当前落地状态、Sirius 旧实现、双 WS login 测试、三种线程模型。 |
| `doc/agent-handoff-binance-market-data.md` | 继续 Binance USD-M futures bookTicker 行情时读 | raw stream URL、JSON parser、client/session、benchmark 和 probe 入口。 |

## 代码入口

### Core 基础类型

| 文件 | 职责 |
| --- | --- |
| `core/common/types.h` | 项目通用枚举，当前包含 `aquila::Exchange`。 |
| `core/common/constants.h` | 项目通用常量，当前包含缓存行大小等基础常量。 |
| `core/utils/numeric.h` | 基于 `fast_float::from_chars` 的 `ToNumeric<T>` / `ToDouble` / `ToUint64` 等热路径数字转换 helper，失败只在 debug assert。 |
| `core/market_data/types.h` | 统一行情数据结构，当前包含 `aquila::BookTicker`。 |

### WebSocket 内核

| 文件 | 职责 |
| --- | --- |
| `core/websocket/types.h` | 配置、状态、错误码、flush mode 等基础类型。 |
| `core/websocket/frame_codec.h` | WebSocket frame encode/decode、mirrored ring、mask key pool、8B XOR。 |
| `core/websocket/critical_session.h` | read/write pump、pending write、control slot、heartbeat、`kTryFlushOne`。 |
| `core/websocket/websocket_client.h` | plain/TLS client 生命周期、reconnect/backoff、runtime loop 集成。 |
| `core/websocket/active_spin_loop.h` | active spin loop 调度。 |
| `core/websocket/prepared_write.h` | 预分配 write slot / arena。 |
| `config/websocket_config.h` | 冷路径 WebSocket TOML 配置结构、默认值和到 `websocket::ConnectionConfig` 的转换；TOML 解析使用 `toml++`，诊断日志走 Nova 封装。 |

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
| `exchange/gate/market_data/session.h` | `FuturesMarketDataSession<Consumer, TransportSocketT>`，负责 WS 生命周期、subscribe/unsubscribe text 控制消息和 binary SBE 分流。 |

### Binance USD-M futures 行情

| 文件 | 职责 |
| --- | --- |
| `exchange/binance/market_data/stream.h` | 构造 `/public/ws/<symbol>@bookTicker` raw stream target，并限制单连接 stream 数上限。 |
| `exchange/binance/market_data/book_ticker_parser.h` | Binance JSON bookTicker -> 中间 `BookTickerUpdate`，生产路径使用 `simdjson::ondemand` 和 `fast_float`。 |
| `exchange/binance/market_data/client.h` | 模板化 `FuturesMarketDataClient<Consumer>`，从 JSON text payload 产出 `BookTicker`。 |
| `exchange/binance/market_data/session.h` | raw stream target session，负责 WS 生命周期和 text JSON 分流；active 后不发送 runtime subscribe。 |

### Gate 交易准备代码

| 文件 | 职责 |
| --- | --- |
| `exchange/gate/trading/submit_response_parser.h` | Gate submit WS JSON response parser，生产路径使用 `simdjson::ondemand`。 |
| `test/exchange/gate/trading/submit_response_parser_test.cpp` | submit response parser 回归测试。 |

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
| `tools/websocket_probe.cpp` | 单连接 live probe，支持 graceful stop 后输出最终 metrics。 |
| `tools/websocket_latency_compare.cpp` | public/private 或多连接 latency compare / warmup selection。 |
| `tools/gate_futures_book_ticker_probe.cpp` | Gate futures SBE `futures.book_ticker` live probe，默认 BTC_USDT。 |
| `tools/binance_futures_book_ticker_probe.cpp` | Binance USD-M futures JSON `bookTicker` live probe，默认 BTCUSDT。 |
| `scripts/gate/test_gate_ws_connect.py` | Gate WS 连接 / login smoke。 |
| `scripts/gate/test_gate_ws_dual_login.py` | 同账号双 WebSocket login 验证。 |
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

### Gate 交易架构

当前推荐方向：

```text
StrategyThread + GateOrderSession
GateOrderFeedbackThread + GateOrderFeedbackSession
feedback SPSC -> StrategyThread
```

原因：

- 下单路径最短。
- 行情 burst 时策略线程仍以行情为最高优先级。
- `futures.orders` / `futures.usertrades` / SBE decode 不污染下单路径。
- Gate 已通过脚本验证允许同一账号两个 WS 同时 login。

扩展到 Gate + Binance 行情后的系统线程命名：

```text
GateFutureMarketDataThread
  - GateFutureMarketDataSession

BinanceFutureMarketDataThread
  - BinanceFutureMarketDataSession

StrategyThread
  - Strategy
  - GateOrderSession
  - BinanceOrderSession

GateOrderFeedbackThread
  - GateOrderFeedbackSession

BinanceOrderFeedbackThread
  - BinanceOrderFeedbackSession
```

其中 `risk control`、`order management` 和 `order execution` 归属于 `Strategy` 模块；`OrderSession` 是上行交易指令和轻量 API response 通道，`OrderFeedbackSession` 是下行私有回报通道。

### Gate SBE 行情当前状态

当前已经以 Gate futures BBO 为样例打通：

```text
FuturesMarketDataSession::Handle(binary MessageView)
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
- `FuturesMarketDataSession<Consumer, TransportSocketT>` 不在内部创建线程；调用方决定在哪个线程运行 `Start()`。
- core WebSocket 层负责 frame/message 完整性；exchange session 不再为外部 non-final `MessageView` 做兼容统计。
- session 处理 subscribe/unsubscribe ack/error text frame；binary SBE frame 进入 client 快路径。
- text JSON 使用 `simdjson::ondemand`；如果 `MessageView::readable_tail_bytes` 满足 `simdjson::SIMDJSON_PADDING`，直接用 padded view，否则 fallback 到 `simdjson::padded_string`。
- symbol -> symbol_id 使用 `absl::flat_hash_map`，初始化时按 symbol 数 `reserve()`，单 symbol 也走同一路径。
- symbol config 是启动期不变量；行情 payload 中出现未配置 symbol 时 debug assert，release 主路径不保留 unknown-symbol 诊断分支。
- `BookTicker` 中不保存字符串 symbol，只保存内部 `symbol_id`。
- BBO 的 `mantissa + exponent` 在 decode 阶段转成 `double`，便于策略和因子计算。
- Gate BBO binary market data 的 `event` 合约是 `Update`；其他 SBE template 不能复用这个假设。
- Gate decimal scale 当前最多支持 10 位小数；超出范围只在 debug assert，用户如需更宽精度应在进入 decoder 前自行判断。
- `ExtractTrustedBookTickerSymbol()` / `DecodeTrustedBookTickerWithHeader()` 是 client 热路径；保守的 `ExtractBookTickerSymbolForTest()` / `DecodeBookTickerForTest()` 已移到 `test/exchange/gate/sbe/book_ticker_decoder_test.cpp`，共享 payload fixture 位于 `evaluation/exchange/gate/sbe/book_ticker_payload_builder.h`，`DecodeBookTickerWithHeaderBenchmark()` 已移到 `benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp`。
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
- symbol config 是启动期不变量；unknown symbol 使用 debug assert，不在 release 主路径记录 unknown-symbol 计数。
- client/session 构造期使用 symbol span 构建 stream target 和 lookup，构造后不再保存无用 `symbols_` span。

2026-05-02 Binance release selected benchmark：

| case | time |
| --- | ---: |
| `parse_book_ticker_padded_view` | 158ns |
| `client_on_text_payload/1` | 183ns |
| `session_handle_text_padded_view` | 195ns |

`session_handle_text_padded_view` 是当前 Binance bookTicker 已实现路径的主要 microbenchmark；公网链路延迟需要重新跑 live probe。

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
./build/debug/test/exchange/gate/market_data/gate_futures_market_data_session_test
./build/release/test/exchange/gate/sbe/gate_sbe_message_dispatcher_test
./build/release/test/exchange/gate/sbe/gate_sbe_book_ticker_decoder_test
./build/release/test/exchange/gate/market_data/gate_futures_market_data_client_test
./build/release/test/exchange/gate/market_data/gate_futures_market_data_session_test
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

Gate submit response parser benchmark：

```bash
taskset -c 2 ./build/release/benchmark/exchange/gate/trading/gate_submit_response_parse_benchmark --benchmark_filter='gate_submit_response_parse_order_place_ack_echo_simdjson_ack_minimal_padded_view/' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

Gate BBO live probe：

```bash
./build/debug/tools/gate_futures_book_ticker_probe --contract BTC_USDT --symbol-id 1 --duration-ms 10000
```

Binance USD-M futures bookTicker 测试和 benchmark：

```bash
./build/debug/test/exchange/binance/market_data/binance_book_ticker_parser_test
./build/debug/test/exchange/binance/market_data/binance_futures_market_data_client_test
./build/debug/test/exchange/binance/market_data/binance_futures_market_data_session_test
./build/release/benchmark/exchange/binance/market_data/binance_futures_market_data_benchmark --benchmark_filter='binance_market_data/(parse_book_ticker|parse_book_ticker_padded_view|client_on_text_payload|session_handle_text|session_handle_text_padded_view)'
```

Binance USD-M futures bookTicker live probe：

```bash
./build/debug/tools/binance_futures_book_ticker_probe --contract BTCUSDT --symbol-id 1 --duration-ms 10000
```

Gate / Binance 期货合约元数据脚本：

```bash
scripts/gate/query_futures_contracts_test.py
scripts/binance/query_um_futures_contracts_test.py
/home/liuxiang/dev/pyenv/lx/bin/python -m py_compile \
  scripts/gate/query_futures_contracts.py \
  scripts/gate/query_futures_contracts_test.py \
  scripts/binance/query_um_futures_contracts.py \
  scripts/binance/query_um_futures_contracts_test.py
```

REST 查询 smoke 需要外网：

```bash
scripts/gate/query_futures_contracts.py BTC_USDT ETH_USDT --format csv
scripts/binance/query_um_futures_contracts.py BTCUSDT ETHUSDT --format csv
```

## 接手注意事项

- 不要 push，除非用户明确要求。
- 修改完成后默认提交到当前 branch，commit message 用英文。
- 文档中文；代码注释英文。
- 性能结论必须写明 benchmark / live probe / profile 证据。
- `third_party/` 中的 Drogon、Sirius、MengRao websocket 是参考代码，通常不提交改动。
- 如果继续 Gate 交易实现，先补设计 / benchmark 边界，不要直接把 Sirius 的 Drogon 架构搬进主线。

## 下一步建议

如果新对话从 Gate 交易继续，建议顺序：

1. 读取 `doc/agent-handoff-gate-trade-architecture.md`。
2. 确认统一 symbol metadata 如何在 strategy、risk check 和 exchange adapter 中缓存并进入下单前校验。
3. 确认是否采用 `GateOrderSession` + `GateOrderFeedbackSession`。
4. 设计 `RequestIdCodec`、`OrderTextCodec`、`OrderFeedback` 固定结构。
5. 继续补 Gate SBE 私有回报 decode：`orders`、`usertrades`、`positions`。
6. 写最小 benchmark：submit send、ack parse、feedback decode、feedback SPSC。
7. 如需 live 证据，重跑 BTC_USDT probe 并把原始输出记录到 handoff。
8. 再开始 C++ 实现。
