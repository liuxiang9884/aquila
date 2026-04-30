# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读整个历史对话的前提下，快速理解 `aquila` 当前状态、重要文档、代码入口和下一步应该怎么接手。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 当前重点：WebSocket 内核已经完成 P0/P1/P2/P3 主体；Gate futures SBE BBO 行情、`BookTicker`、market data client、market data session、benchmark 和 live probe 已落地；下一阶段继续 Gate 交易 submit/update 链路设计。
- 构建：CMake + `build.sh`。
- 核心原则：正确性、确定性、最低延迟、尾延迟可控、固定容量、少动态分配、性能结论必须有 benchmark / profile / live probe 证据。
- 当前建议分支入口：`main`。

## 新对话第一步

先运行：

```bash
git -C /home/liuxiang/dev/aquila status --short
git -C /home/liuxiang/dev/aquila log --oneline -8
```

再读：

```text
AGENTS.md
README.md
doc/project_onboarding_guide.md
doc/agent-handoff-gate-trade-architecture.md
doc/websocket_read_write_benchmark_comparison.md
```

如果继续 Gate 交易架构，优先读 `doc/agent-handoff-gate-trade-architecture.md`。如果继续 WebSocket 性能优化，优先读 `doc/websocket_client_future_optimizations.md`。

## 文档索引

| 文档 | 什么时候读 | 关键内容 |
| --- | --- | --- |
| `AGENTS.md` | 每次新会话最先读 | 中文/英文约定、低延迟原则、测试/benchmark/提交规则。 |
| `README.md` | 了解构建和工具入口 | build、ctest、benchmark、probe、latency compare 的运行方式。 |
| `doc/websocket_frame_codec_receive_strategies.md` | 理解 FrameCodec decode 为什么这样设计 | mirrored ring、direct delivery、fast path、QueuedFrameCodec、decode 收口结论。 |
| `doc/websocket_read_write_benchmark_comparison.md` | 快速看 read/write benchmark 对比 | `aquila`、Drogon-style、`third_party/websocket` read/write 差异和数值。 |
| `doc/websocket_client_future_optimizations.md` | 继续 WebSocket 优化时读 | read/write/active spin/network 的未来优化 backlog。 |
| `doc/agent-handoff-gate-trade-architecture.md` | 继续 Gate 交易架构或 Gate SBE 行情时读 | Gate 文档结论、SBE BBO 当前落地状态、Sirius 旧实现、双 WS login 测试、三种线程模型。 |

## 代码入口

### Core 基础类型

| 文件 | 职责 |
| --- | --- |
| `core/common/types.h` | 项目通用枚举，当前包含 `aquila::Exchange`。 |
| `core/common/constants.h` | 项目通用常量，当前包含缓存行大小等基础常量。 |
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

### Gate 交易准备代码

| 文件 | 职责 |
| --- | --- |
| `exchange/gate/trading/submit_response_parser.h` | Gate submit WS JSON response parser，生产路径使用 `simdjson::ondemand`。 |
| `test/exchange/gate/trading/submit_response_parser_test.cpp` | submit response parser 回归测试。 |

### 工具

| 文件 | 用途 |
| --- | --- |
| `tools/websocket_probe.cpp` | 单连接 live probe，支持 graceful stop 后输出最终 metrics。 |
| `tools/websocket_latency_compare.cpp` | public/private 或多连接 latency compare / warmup selection。 |
| `tools/gate_futures_book_ticker_probe.cpp` | Gate futures SBE `futures.book_ticker` live probe，默认 BTC_USDT。 |
| `scripts/gate/test_gate_ws_connect.py` | Gate WS 连接 / login smoke。 |
| `scripts/gate/test_gate_ws_dual_login.py` | 同账号双 WebSocket login 验证。 |

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
| `benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp` | Gate submit response JSON parse benchmark；yyjson 只在这里作为 simdjson 对照。 |

## 当前重要结论

### WebSocket decode

`FrameCodec` decode 主体已经收口：

- 默认生产路径使用 mirrored receive ring。
- 单帧和 repeated `Poll()` 多帧 drain 走 direct delivery。
- ready metadata ring 已移到 `QueuedFrameCodec` 对照路径。
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
StrategyThread + GateOrderSubmitWsSession
GateOrderUpdateThread + GateOrderUpdateWsSession
feedback SPSC -> StrategyThread
```

原因：

- 下单路径最短。
- 行情 burst 时策略线程仍以行情为最高优先级。
- `futures.orders` / `futures.usertrades` / SBE decode 不污染下单路径。
- Gate 已通过脚本验证允许同一账号两个 WS 同时 login。

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
- session 处理 subscribe/unsubscribe ack/error text frame；binary SBE final frame 进入 client 快路径。
- text JSON 使用 `simdjson::ondemand`；如果 `MessageView::readable_tail_bytes` 满足 `simdjson::SIMDJSON_PADDING`，直接用 padded view，否则 fallback 到 `simdjson::padded_string`。
- symbol -> symbol_id 使用 `absl::flat_hash_map`，初始化时按 symbol 数 `reserve()`，单 symbol 也走同一路径。
- `BookTicker` 中不保存字符串 symbol，只保存内部 `symbol_id`。
- BBO 的 `mantissa + exponent` 在 decode 阶段转成 `double`，便于策略和因子计算。
- `ExtractBookTickerSymbol()` 不再重复校验 SBE header；完整 header 校验保留在 `DecodeBookTickerWithHeader()`。
- 如需引用 BTC_USDT live probe 稳定性或真实延迟结论，重新运行并保留原始输出。

2026-04-30 Gate market data release benchmark：

| case | time |
| --- | ---: |
| `decode_book_ticker_with_header` | 2.85ns |
| `client_on_binary_payload/1` | 6.42ns |
| `client_on_binary_payload/8` | 7.21ns |
| `client_on_binary_payload/32` | 7.17ns |
| `session_handle_binary/1` | 37.9ns |
| `session_handle_binary/8` | 49.7ns |
| `session_handle_binary/32` | 48.3ns |
| `session_handle_subscribe_ack` | 135ns |
| `session_handle_subscribe_ack_padded_view` | 115ns |

`session_handle_binary` 包含 session 计数和 `NowNs()`，不等同于纯 decode 成本。

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

Gate BBO live probe：

```bash
./build/debug/tools/gate_futures_book_ticker_probe --contract BTC_USDT --symbol-id 1 --duration-ms 10000
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
2. 确认是否采用 `GateOrderSubmitWsSession` + `GateOrderUpdateWsSession`。
3. 设计 `RequestIdCodec`、`OrderTextCodec`、`OrderFeedback` 固定结构。
4. 继续补 Gate SBE 私有回报 decode：`orders`、`usertrades`、`positions`。
5. 写最小 benchmark：submit send、ack parse、update decode、feedback SPSC。
6. 如需 live 证据，重跑 BTC_USDT probe 并把原始输出记录到 handoff。
7. 再开始 C++ 实现。
