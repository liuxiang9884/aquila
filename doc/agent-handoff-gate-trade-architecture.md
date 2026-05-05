# Agent 接手说明：Gate 交易 WebSocket 架构讨论

## 这份文件是给谁的

这份 handoff 给下一轮模型或开发者使用，用来承接 2026-04-28 关于 Gate futures 交易 WebSocket 架构的讨论。

本文不是已批准的实现计划；它记录已经确认的协议事实、实验结果、Sirius 旧实现结论，以及当前推荐的线程 / session 划分方向。

## 当前仓库状态

- 当前主线分支：`main`
- 截至 2026-05-02，最近相关功能提交包括以下内容；新接手时以 `git log --oneline -8` 为准：
  - `aff3408 docs: document futures contract metadata fields`
  - `ca1a9cc scripts: unify futures contract metadata fields`
  - `a3c444a scripts: add Binance futures contract query`
  - `e502bd1 scripts: add Gate futures contract query`
  - `7c0fb70 docs: add evaluation support rules`
  - `9548bd2 build: add evaluation support target`
  - `33fab19 docs: document benchmark helper locations`
  - `63ac1d9 core: move comparison helpers to tests`
- 新接手时先执行：

```bash
git -C /home/liuxiang/dev/aquila status --short
git -C /home/liuxiang/dev/aquila log --oneline -8
```

## Gate 文档结论

参考文档：

- Gate Futures WebSocket v4：https://www.gate.com/docs/developers/futures/ws/zh_CN/
- 订单频道：https://www.gate.com/docs/developers/futures/ws/zh_CN/#%E8%AE%A2%E5%8D%95%E9%A2%91%E9%81%93
- 用户私有成交频道：https://www.gate.com/docs/developers/futures/ws/zh_CN/#%E7%94%A8%E6%88%B7%E7%A7%81%E6%9C%89%E6%88%90%E4%BA%A4%E9%A2%91%E9%81%93
- SBE 数据推送：https://www.gate.com/docs/developers/futures/ws/zh_CN/#sbe-%E6%95%B0%E6%8D%AE%E6%8E%A8%E9%80%81

已经确认：

1. Gate futures WebSocket 交易 API 是 JSON request / response。
2. `futures.login` 用于 WebSocket 交易 API 登录。
3. `futures.order_place` / `futures.order_batch_place` 会出现 `ack=true` 的请求接收确认，以及 `ack=false` 的实际结果。
4. 撤单、改单、查询类 API 返回对应 JSON result / error。
5. `futures.orders`、`futures.usertrades`、`futures.positions` 属于私有推送频道，不是 order API response。
6. SBE endpoint 使用 JSON 做请求和首次响应，使用 SBE 做数据推送；同一条连接上可能混合 JSON text frame 和 SBE binary frame。
7. SBE 文档明确覆盖 `futures.orders`、`futures.usertrades`、`futures.positions`；不要默认假设 `futures.balances` 已覆盖 SBE。

## Gate SBE 行情当前落地状态

当前已经先以 Gate futures `bbo` / `futures.book_ticker` 为样例完成行情接入骨架。这部分不是交易回报实现，但它验证了 SBE schema、生成代码、message dispatch、BBO decode、统一 `BookTicker` 数据结构、market data client、market data session 和 WebSocket typed handler 之间的边界。

2026-05-05 当前收口：

1. `DataSession` 已落地，负责连接生命周期、订阅控制 text frame、SBE binary frame 分流和统计。
2. WebSocket 内核新增模板化 typed message handler path；`MessageCallback` 保留给工具和旧测试。
3. Gate 行情热路径使用 `absl::flat_hash_map` 做 symbol -> symbol_id 查询，初始化时 `reserve()`，单 symbol 也走同一路径。
4. production / 系统内部 JSON parser 统一为 `simdjson::ondemand`；`yyjson` 只保留在 submit response benchmark 中做对照。
5. Gate BBO 行情 client 热路径使用 trusted symbol extract / trusted decode；schema/template、payload shape、symbol config 和 BBO `event=Update` 作为协议不变量，debug 下 assert，release 主路径不再保留 unknown-symbol / decode-failure 诊断分支。
6. client/session 构造期使用 symbol span 构建 lookup / subscription views，构造后不保存无用 `symbols_` span。
7. Gate data session TOML parser 已在启动冷路径加载 `instrument_catalog` 和 `subscribe_symbols`，生成 `DataSessionConfig`、WebSocket target、exchange symbol 列表和 symbol id 列表；tool 根据 `enable_tls` 选择 TLS 或 plain WebSocket policy。

### Core 数据类型

已新增统一交易所枚举和行情数据结构：

```text
core/common/types.h
core/common/constants.h
core/market_data/types.h
```

当前 `aquila::Exchange` 包含：

```text
kBinance, kOkx, kGate, kBybit, kBitget, kCoinbase
```

当前 `aquila::BookTicker` 字段顺序按缓存行和计算热度整理：

```cpp
std::int64_t id;
std::int32_t symbol_id;
Exchange exchange;
std::int64_t exchange_ns;
std::int64_t local_ns;
double bid_price;
double bid_volume;
double ask_price;
double ask_volume;
```

约束：

1. `BookTicker` 是 standard-layout、trivially-copyable。
2. 当前 size 为 `kCacheLineBytes`，字段 offset 已由 `test/core/market_data/types_test.cpp` 固化。
3. `symbol_id` 是系统内部唯一标的 ID；`symbol` 字符串不放入热路径结构。
4. `exchange_ns` 来自交易所事件时间；`local_ns` 是本机收到 / 解码时刻。

### SBE 代码生成和 BBO wire 结构

Gate SBE schema 放在：

```text
exchange/gate/sbe/schema/gate_fex_ws_latest.xml
```

已使用本地 `third_party/sbepp` 构建出的 `sbeppc` 生成 C++ header，生成代码放在：

```text
exchange/gate/sbe/generated/
```

BBO payload 在 wire 上的结构：

```text
8 bytes messageHeader
59 bytes fixed block
varString8 channel
varString8 s
```

其中：

1. `messageHeader` 包括 `blockLength`、`templateId`、`schemaId`、`version`，每个字段都是 little-endian `uint16`。
2. `varString8` 是 1 字节长度前缀 + 对应长度字符串内容。
3. BBO 数值字段使用 `mantissa + exponent`，当前在 decode 时转成 `double`，用于对接策略和因子计算。
4. `sbepp` decode 本身是 view/wrapper 风格，不复制 payload；payload 生命周期必须覆盖 decode 使用过程。

### SBE dispatch 和 BookTicker decode

已新增：

```text
exchange/gate/sbe/message_header.h
exchange/gate/sbe/message_dispatcher.h
exchange/gate/sbe/book_ticker_decoder.h
test/exchange/gate/sbe/message_dispatcher_test.cpp
test/exchange/gate/sbe/book_ticker_decoder_test.cpp
```

当前 binary message 处理流程：

```text
DataSession::Handle(binary MessageView)
  -> capture local_ns
  -> FuturesMarketDataClient::OnBinaryPayload
  -> DispatchSbeMessage
  -> schemaId / version check
  -> templateId dispatch
  -> ExtractTrustedBookTickerSymbol(payload, header)
  -> flat_hash_map symbol -> symbol_id
  -> DecodeTrustedBookTickerWithHeader(payload, header)
  -> aquila::BookTicker
  -> Consumer::OnBookTicker
```

生产路径只保留 trusted BBO helper：`ExtractTrustedBookTickerSymbol()` 和 `DecodeTrustedBookTickerWithHeader()`。保守的 `ExtractBookTickerSymbolForTest()` / `DecodeBookTickerForTest()` 已移到 `test/exchange/gate/sbe/book_ticker_decoder_test.cpp`；benchmark 对照的 `DecodeBookTickerWithHeaderBenchmark()` 已移到 `benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp`。`DispatchSbeMessage` 已为 Gate 当前 schema 中的 10 个 template 建立枚举映射，包括 `BookTicker`、`PublicTrade`、`Obu`、`OrderBook`、`OrderBookUpdate`、`UserTrade`、`Position`、`Candlestick`、`FuturesTicker`、`Orders`。

十进制转换生产函数是 `DecimalExponentScale()`，使用预计算 scale 表；主路径负指数分支带 `[[likely]]`，越界只在 debug assert 中检查。`DecimalMantissaToDoubleForTest()` 只留在 `test/exchange/gate/sbe/book_ticker_decoder_test.cpp` 中验证转换结果。这里是基于 Gate futures 行情常见精度做的低延迟取舍，后续如果接入极端精度字段，需要先补测试再扩大表。

BBO 额外约束：

1. Gate BBO binary market data frame 的 `event` 视为 `Update`，trusted decode 只用 debug assert 固化该合约；其他 SBE template 不能复用这个事件假设。
2. Decimal scale 表固定支持 0 到 10 位小数；如果用户需要更宽精度，由调用方在进入 decoder 前判断。
3. 保守 BBO decode / symbol extract 只保留在 test 或 benchmark 对照文件中；client 热路径只走 trusted 版本。

### Gate futures market data client

已新增：

```text
exchange/gate/market_data/client.h
exchange/gate/market_data/subscription.h
test/exchange/gate/market_data/futures_market_data_client_test.cpp
```

当前 client 命名为：

```cpp
aquila::gate::FuturesMarketDataClient<Consumer>
```

设计取舍：

1. 使用纯模板组合，不引入 `MarketDataSink` 虚接口，也不在热路径使用 `std::function`。
2. `Consumer` 只需要提供 `OnBookTicker(const aquila::BookTicker&) noexcept`。
3. `SymbolBinding` 使用 `{symbol, symbol_id}`；初始化后构建 `absl::flat_hash_map<std::string_view, std::int32_t>`，并按 symbol 数预留容量，避免热路径 rehash。
4. WebSocket text frame 在 session 层处理；client 热路径只处理 binary SBE payload。
5. unknown template 和非 binary frame 不会中断 client，当前以接受并丢弃为主；unknown symbol 是启动 symbol config 错误，debug assert，release 热路径不记录 unknown-symbol counter。
6. BBO decode 成功后同步调用 consumer，避免额外队列和动态分配。

订阅请求构造：

```cpp
BuildFuturesBookTickerSubscribeRequest(symbols, epoch_seconds)
BuildFuturesBookTickerUnsubscribeRequest(symbols, epoch_seconds)
```

输出 JSON 形如：

```json
{"time":123,"channel":"futures.book_ticker","event":"subscribe","payload":["BTC_USDT"]}
```

### Gate futures market data session

已新增：

```text
exchange/gate/market_data/data_session.h
test/exchange/gate/market_data/data_session_test.cpp
benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp
```

当前 session 命名为：

```cpp
aquila::gate::DataSession<
    Consumer, WebSocketPolicy, DiagnosticsPolicy>
```

功能边界：

1. session 不在内部创建线程；调用方决定在哪个线程调用 `Start()` / `Run()` / `Stop()`，其中 `Run()` 在 session 内部安装 SIGINT / SIGTERM stop handler。
2. active 后自动发送 `futures.book_ticker` subscribe；支持显式 `RequestUnsubscribe()`。
3. core WebSocket 层负责 frame/message 完整性；exchange session 不再处理外部 non-final `MessageView`。
4. binary frame 走 BBO/SBE 快路径。
5. text frame 只处理 subscribe/unsubscribe ack/error，以及 JSON market data update 计数；当前不把 JSON market data 转成 `BookTicker`。
6. text control parser 使用复用的 `simdjson::ondemand::parser text_parser_`。
7. 如果 `MessageView::readable_tail_bytes >= simdjson::SIMDJSON_PADDING`，text parser 直接使用 zero-copy padded view；否则 fallback 到 `simdjson::padded_string` scratch copy。
8. session 内部通过 WebSocket state hook 处理 active 后订阅和 `ever_active` 状态；外部不再设置 state/error handler。
9. `DataSessionStats` 是低频诊断计数，不属于行情热路径输出结构；默认 `DiagnosticsPolicy = NoopDataSessionDiagnosticsPolicy`，不会在 binary 热路径做计数，probe / test / text-control benchmark 需要显式启用 `SessionOnlyDiagnosticsPolicy` 或 `DataSessionDiagnosticsPolicy`。
10. text control envelope parser 已拆到 `text_envelope_parser.h`，订阅状态机已拆到 `subscription_controller.h`，session 只保留收包分流、订阅请求发送和 client 组合。

注意：

1. `local_ns` 在 session 收到 binary frame 后立即采集，再传入 client，避免在更深层反复取时钟，也方便未来由外层 runtime 统一传入本地时间。
2. `DecodeBookTickerWithHeaderBenchmark()` 在 benchmark 中保留完整 header / event 校验；client 热路径删除的是重复防御分支。

后续 TODO（暂不实现）：

1. Symbol lookup ownership：考虑引入稳定的 `GateSymbolLookup` / `SymbolTableView`，显式表达 `std::string_view` 的生命周期和 ownership 边界，避免未来多链路共享 symbol 表时重复构造或生命周期误用。
2. Trading submit types/session split：后续下单链路再把 submit response 类型、parser 和 order session 拆开，保持 ack 热路径最小化，同时避免 market-data 与 trading 结构混在同一层。

### 合约元数据和 symbol 配置

已新增 Gate / Binance 期货合约基础信息脚本：

```text
scripts/gate/query_futures_contracts.py
scripts/binance/query_um_futures_contracts.py
doc/futures_contract_metadata_fields.md
```

这两个脚本按 symbol 顺序查询 REST 基础信息并生成统一 `pandas.DataFrame` 字段。当前只覆盖一类下单前必需元数据：价格 tick、价格小数位、数量步进、数量上下限、市价数量上限、最小名义价值、合约乘数、价格偏离限制和市价偏离限制。

对 Gate 交易链路最重要的差异：

1. Gate `quantity` 默认是合约张数，脚本用 `quanto_multiplier` 输出 `notional_multiplier`；Binance USD-M futures `quantity` 是 base asset 数量，`notional_multiplier=1.0`。
2. Gate `price_tick` 来自 `order_price_round`；Binance `price_tick` 来自 `PRICE_FILTER.tickSize`。
3. Gate 未提供 `min_notional`，当前输出空值；Binance 从 `MIN_NOTIONAL` / `NOTIONAL` filter 映射。
4. Gate 在 `enable_decimal=false` 时 `quantity_step=1.0`、`quantity_decimal_places=0`；如果 `enable_decimal=true`，当前脚本暂不推导 decimal contract size，`quantity_step` 和 `quantity_decimal_places` 输出空值，避免把未验证规则带入下单热路径。
5. Gate 的 `order_price_deviate` 是相对标记价的订单价格偏离比例；Binance 的 `PERCENT_PRICE` 是 bidirectional multiplier，脚本统一为相对偏离比例。

这组 metadata 应在启动期构建并缓存，供 strategy、risk check 和 exchange adapter 共享；不要在行情或下单热路径里反复查询 REST 或重复解析交易所 JSON。

### Gate futures market data benchmark

当前 benchmark 命令：

```bash
taskset -c 2 ./build/release/benchmark/exchange/gate/market_data/gate_futures_market_data_benchmark --benchmark_filter='gate_market_data/(decode_book_ticker_with_header|client_on_binary_payload|client_on_message_binary|session_handle_binary|session_handle_subscribe_ack|session_handle_subscribe_ack_padded_view)' --benchmark_repetitions=10
```

2026-05-02 当前 selected mean 结果：

| case | time |
| --- | ---: |
| `decode_book_ticker_with_header` | 2.85ns |
| `client_on_binary_payload/1` | 2.39ns |
| `session_handle_binary/1` | 32.1ns |

解释：

1. `client_on_binary_payload` 是已知 binary payload 的 client 快路径。
2. `client_on_message_binary` 包含 `MessageView` kind 分流和时钟外传。
3. `session_handle_binary` 默认关闭 session 统计，主要包含 session 分流、`NowNs()` 和 client binary 处理，因此数值仍受时钟成本影响。
4. padded text view 比 scratch copy text parse 更快，但 subscribe/unsubscribe ack 属于低频控制路径。

2026-05-02 当前验证：

```text
cmake --build build/debug --target binance_book_ticker_parser_test binance_futures_market_data_client_test binance_data_session_test gate_futures_market_data_client_test gate_data_session_test gate_sbe_book_ticker_decoder_test gate_sbe_message_dispatcher_test gate_submit_response_parser_test -j 32
ctest --test-dir build/debug -R "(binance_book_ticker_parser_test|binance_futures_market_data_client_test|binance_data_session_test|gate_futures_market_data_client_test|gate_data_session_test|gate_sbe_book_ticker_decoder_test|gate_sbe_message_dispatcher_test|gate_submit_response_parser_test)" --output-on-failure # 8/8 passed
cmake --build build/release --target binance_book_ticker_parser_test binance_futures_market_data_client_test binance_data_session_test gate_futures_market_data_client_test gate_data_session_test gate_sbe_book_ticker_decoder_test gate_sbe_message_dispatcher_test gate_submit_response_parser_test -j 32
ctest --test-dir build/release -R "(binance_book_ticker_parser_test|binance_futures_market_data_client_test|binance_data_session_test|gate_futures_market_data_client_test|gate_data_session_test|gate_sbe_book_ticker_decoder_test|gate_sbe_message_dispatcher_test|gate_submit_response_parser_test)" --output-on-failure # 8/8 passed
```

### WebSocket typed message handler

已新增模板化 handler path：

```text
core/websocket/message_view.h
core/websocket/critical_session.h
core/websocket/websocket_client.h
benchmark/websocket/message_handler_dispatch_benchmark.cpp
```

当前接口：

```cpp
template <typename MessageHandlerT>
struct MessageHandlerRef {
  MessageHandlerT* handler{nullptr};
  DeliveryResult Handle(const MessageView& view) const noexcept;
};

template <typename MessageHandlerT>
MessageHandlerRef<MessageHandlerT> MakeMessageHandler(MessageHandlerT& handler) noexcept;
```

`BasicWebSocketClient<TransportSocketT, MessageHandlerT>` 和 `CriticalSession<TlsSocketT, MessageHandlerT>` 支持 typed handler。`MessageCallback` 仍保留为默认类型，供工具、旧测试和 C 风格回调路径使用。

2026-04-30 当前 microbenchmark：

| case | time |
| --- | ---: |
| `message_callback` | 2.11ns |
| `typed_handler_ref` | 2.37ns |
| `typed_handler_value` | 2.30ns |

结论：这个 microbenchmark 没证明 typed handler 自身比函数指针更快；保留 typed path 的主要理由是让 Gate session 直接作为 handler 接入 `BasicWebSocketClient`，避免 `AsMessageCallback()` 适配层，并为编译期组合保留空间。不要基于这组数值宣称 dispatch 已有性能收益。

### Gate futures book ticker probe

已新增 live probe：

```text
tools/gate/futures_book_ticker_probe.cpp
```

默认参数：

```text
host=fx-ws.gateio.ws
port=443
target=/v4/ws/usdt/sbe?sbe_schema_id=1
contract=BTC_USDT
symbol_id=1
duration_ms=1800000
tls=true
```

上述默认参数是 Gate 公网 `wss://` 路径。若使用 Gate private link / plain WS，应使用 private
host / service，并显式关闭 TLS；不要把公网 `fx-ws.gateio.ws:443` 与 plain WS 混用。

probe 行为：

1. 建立 Gate futures SBE WebSocket 连接。
2. 通过 `DataSession` 在 active 后自动订阅 `futures.book_ticker`。
3. text frame 由 session 统计 subscribe/unsubscribe ack/error。
4. binary frame 由 session/client 解码为 `BookTicker` 后同步进入 probe consumer。
5. 输出总消息数、text/binary 数、session 统计、BBO decode 成功数、处理耗时、到达间隔、第一条和最后一条 `BookTicker`，以及 `id_delta`。

如果新对话需要引用 live 稳定性或真实延迟结论，应重新运行并保留输出：

```bash
./build/debug/tools/gate_futures_book_ticker_probe --contract BTC_USDT --symbol-id 1 --duration-ms 10000
```

如需正式引用性能或稳定性结论，必须保留 probe 输出或 benchmark 结果。

## 当前建议命名

不要继续使用过于抽象的 `OrderEntry` / `PrivateReport`。建议命名为：

```text
GateOrderSession
GateOrderFeedbackSession

BinanceOrderSession
BinanceOrderFeedbackSession
```

完整系统中，行情 session 的架构命名为：

```text
GateDataSession
BinanceDataSession
```

含义：

| 名称 | 主要职责 | 消息格式 |
| --- | --- | --- |
| `GateOrderSession` | 登录、下单、撤单、改单、订单查询；读取轻量 API response / ack / error | JSON |
| `GateOrderFeedbackSession` | 登录或订阅鉴权，订阅订单 / 成交 / 持仓更新；读取持续回报流 | JSON 控制消息 + SBE binary push |
| `BinanceOrderSession` | Binance 下单、撤单、改单、订单查询；具体协议形态后续确认 | 后续确认 |
| `BinanceOrderFeedbackSession` | Binance 订单、成交、账户 / 持仓私有更新；转成固定 feedback event | 后续确认 |

说明：

1. `OrderSession` 是上行交易指令通道，同时读取同一 API WebSocket 上的轻量响应，不只表示 place order。
2. `OrderFeedbackSession` 是下行交易回报通道，服务于策略内的 order management、risk state 和 execution feedback。
3. `GateDataSession` / `BinanceDataSession` 是系统架构命名；当前 C++ 落地类已统一为命名空间内的 `DataSession` 模板实现。

## 双 WebSocket 登录测试

新增脚本：

```text
scripts/gate/test_gate_ws_dual_login.py
```

运行方式：

```bash
TEST_KEY=... TEST_SECRET=... scripts/gate/test_gate_ws_dual_login.py --timeout 8
```

脚本行为：

```text
1. 建立两个 Gate futures WebSocket 连接
2. 两个连接都发送 futures.login
3. 输出 request_id、uid、conn_id、conn_trace_id
4. 判断是否 both_logged_in_same_account
```

已验证结果：

```text
[OK] ws=A uid=14446887 ... detail=login succeeded
[OK] ws=B uid=14446887 ... detail=login succeeded
result=both_logged_in_same_account
```

结论：Gate futures WebSocket 允许同一账号同时登录两个物理 WebSocket 连接。这支持 `GateOrderSession` + `GateOrderFeedbackSession` 的双连接设计。

## Submit WS JSON response 解析结论

已新增 Gate submit response parser 与 benchmark：

```text
exchange/gate/trading/submit_response_parser.h
test/exchange/gate/trading/submit_response_parser_test.cpp
benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp
```

当前实现区分两类解析路径：

| 路径 | 含义 | 提取字段 |
| --- | --- | --- |
| full business parse | 当前业务完整解析，不等于提取 JSON 全字段 | `request_id`、`ack`、`header.status`、`header.channel`、`data.errs.label`、`data.result.req_id/id/text` |
| minimal ack parse | Submit WS ACK 快路径 | `request_id`、`ack` |

`minimal ack parse` 的定位：

1. 用于 `ack=true` 的轻量请求接收确认。
2. 不读取 `header`、`data.result.req_id`、`data.errs`。
3. 不能替代 result / error 的完整解析。
4. 如果业务需要校验 status、channel、error label 或最终 order id，应走 full business parse 或后续设计分层解析。

当前实现约束：

1. 数字字符串转换统一走 `core/utils/numeric.h` 中基于 `fast_float::from_chars` 的 `ToNumeric<T>` / `ToUint64` 等 helper。
2. `ReadSimdjsonUint64()` 的输出指针是内部调用合约，debug 下 assert，不在 release 热路径做空指针防御。
3. `RequestIdCodec` / `OrderTextCodec` 尚未落地，当前 submit parser 仍只保留 hash 字段；等下单 session 设计时再替换。

### yyjson 与 simdjson 取舍

当前依赖状态：

```text
production / 系统内部 JSON: simdjson::ondemand
benchmark 对照: yyjson, simdjson 4.6.2
```

结论：

1. `yyjson` default parse 不要求调用方提供 padding，也不修改 payload；但它会构建 DOM，所以 minimal 只减少字段访问 / hash / 转换，不能跳过整包 JSON parse。
2. `YYJSON_READ_INSITU` 需要 `YYJSON_PADDING_SIZE`，当前通常为 4 字节；它会修改 payload buffer，不适合直接用于共享的 WebSocket frame buffer。
3. `simdjson::ondemand` 需要 `simdjson::SIMDJSON_PADDING`，当前通常为 64 字节；padding 只要求可读，不要求清零。
4. `simdjson::ondemand` 的 `string_view` / document / value 依赖原始 buffer 生命周期，热路径应立即转成 hash、整数、bool 等自有值，不要跨 buffer 复用保存 view。
5. production 代码里不要写死 padding 数值，应使用 `simdjson::SIMDJSON_PADDING`；benchmark 对照 yyjson 时使用 `YYJSON_PADDING_SIZE`。

### Submit response parse benchmark

运行命令：

```bash
taskset -c 2 ./build/release/benchmark/exchange/gate/trading/gate_submit_response_parse_benchmark --benchmark_filter='ack_minimal_padded_view|simdjson_ondemand_padded_view|yyjson_default_padded_view'
```

样本 payload 为 Gate `futures.order_place` 的 `ack=true` 订单回声，payload 约 839B；benchmark 使用 padded view，避免把每次 copy 成本混入解析差异。

2026-04-28 parser 对照结果：

| case | p50 | p99 | p99.9 |
| --- | ---: | ---: | ---: |
| `yyjson_default_padded_view` | 391ns | 663ns | 766ns |
| `yyjson_ack_minimal_padded_view` | 359ns | 605ns | 678ns |
| `simdjson_ondemand_padded_view` | 254ns | 363ns | 583ns |
| `simdjson_ack_minimal_padded_view` | 123ns | 129ns | 444ns |

2026-05-02 selected mean：

| case | mean |
| --- | ---: |
| `simdjson_ack_minimal_padded_view` | 115ns |

推荐：

```text
Submit WS ack=true 快路径优先使用 simdjson minimal ack parse。
result / error / 低频异常路径可使用 full business parse。
padding 或生命周期条件不满足时，fallback 到 simdjson padded scratch copy。
```

注意：`yyjson` 现在只用于 benchmark 对照，不作为生产或系统内部 JSON parser。

## FrameCodec readable tail padding 契约

为支持 simdjson zero-copy padded view，`MessageView` 已新增：

```cpp
std::uint32_t readable_tail_bytes{0};
```

语义：

1. `MessageView::payload` 仍然只表示真实 WebSocket payload，不包含 padding。
2. `readable_tail_bytes` 表示 payload 末尾之后，在当前 `MessageView` 生命周期内可安全读取的额外字节数。
3. 这些 tail bytes 不是 payload，内容可能是 ring buffer 中的任意历史或后续数据。
4. 业务层只能用它判断 parser padding 是否满足，例如 `readable_tail_bytes >= simdjson::SIMDJSON_PADDING`。
5. 不允许把 tail bytes 当业务数据读取，不允许写 tail bytes，也不允许把 parser 返回的 view 保存到 frame buffer 生命周期之外。

生产 `FrameCodec` 和 evaluation 对照 `QueuedFrameCodec` 都会填充该字段。当前 mirrored ring 使跨 ring boundary 的 payload 后仍有连续可读虚拟地址空间，因此可以把这个能力显式暴露给上层，而不是把 padding 混进 payload size。

### FrameCodec benchmark 对比

修改前后使用同一条命令：

```bash
taskset -c 2 ./build/release/benchmark/websocket/frame_codec_benchmark
```

关键 decode path 对比：

| case | 修改前 p50 / p99 | 修改后 p50 / p99 |
| --- | ---: | ---: |
| direct contiguous | 5ns / 6ns | 5ns / 6ns |
| queued contiguous | 15ns / 18ns | 15ns / 17ns |
| direct coalesced drain | 3ns / 3ns | 3ns / 3ns |
| queued coalesced drain | 9ns / 14ns | 9ns / 16ns |
| direct mirrored boundary | 50ns / 56ns | 50ns / 56ns |
| queued mirrored boundary | 71ns / 80ns | 72ns / 83ns |

结论：暴露 `readable_tail_bytes` 对 FrameCodec decode 主路径没有可见 p50 回归；p99 的小幅波动在当前 benchmark 噪声范围内。

验证命令：

```bash
cmake --build build/debug -j8
cmake --build build/release -j8
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/release --output-on-failure
```

2026-04-28 验证结果：debug / release 均为 `17/17` tests passed。

## Sirius 旧实现结论

旧实现位于：

```text
third_party/sirius/exchange/gate
```

它是用户早期设计的一版 Gate 行情和交易实现。该目录目前属于第三方 / 参考代码，不作为当前 `aquila` 主线实现。

Sirius 交易结构：

```text
WebSocketClient 基类
  ├── FutureDataEngine
  └── TradeEngine
```

`TradeEngine` 的交易 WebSocket 同时处理：

```text
futures.login
futures.order_place / futures.order_cancel
futures.orders 私有订单回报
```

也就是说，Sirius 是“单个 trade WS 同时处理 submit 和 update”的设计。

值得借鉴：

- `RequestIdCodec`：把请求类型编码进 `req_id`，方便 API response 分流。
- `OrderTextCodec`：通过 Gate `text = t-{order_id}` 或 `ao-{price_order_id}` 反查内部订单 ID。
- parser 和 engine 有一定分层。
- 回报转成固定事件结构后写入 feedback channel。

不适合直接搬进 `aquila` 热路径：

- 依赖 Drogon WebSocket，frame/read/write buffer 不可控。
- 下单路径使用 `std::string`、`fmt::format`、`std::format` 构造请求，存在动态分配。
- message callback 后还有虚函数和 hash map channel dispatch。
- submit/update 混在同一个 Drogon loop，update burst 可能污染下单路径。
- 心跳、重连、degraded、metrics 都比当前 `core/websocket` 简单。

## 三种交易线程模型

### 方案 A：Strategy + Order 同线程，Order Feedback 独立线程

```text
MarketDataThread
  -> market ring / latest cache
        |
        v
StrategyThread + GateOrderSession
  - poll 行情
  - 策略计算
  - 直接下单 / 撤单 / 改单
  - 轻量 read order API response
        ^
        |
feedback SPSC
        |
GateOrderFeedbackThread + GateOrderFeedbackSession
  - subscribe futures.orders / futures.usertrades / futures.positions
  - decode JSON control + SBE push
  - 写入 feedback SPSC
```

优点：

- 下单路径最短：`行情 -> 策略 -> encode -> send`。
- 不经过 `strategy -> order ring -> trading thread` 这一跳。
- feedback SBE decode / orders / usertrades burst 不进入策略线程。
- 最符合“行情 burst 时行情最高优先级”。

缺点：

- 策略线程里仍有 `send()` / `SSL_write()` syscall 和少量 submit read。
- 两个物理 trade WS 需要处理登录 epoch、连接健康、重复回报、REST reconcile。

当前低延迟优先推荐：**方案 A**。

### 方案 B：Strategy 独立线程，Order + Order Feedback 合并为 TradingSession 线程

```text
StrategyThread
  -> order SPSC
        |
        v
TradingSessionThread
  - GateOrderSession
  - GateOrderFeedbackSession
  - order state / dedup / report correlation
        |
        v
feedback SPSC -> StrategyThread
```

优点：

- StrategyThread 最干净，不碰 socket syscall。
- 交易状态、去重、request_id correlation 更集中。
- 工程实现更稳妥，调试更简单。

缺点：

- 下单多一跳 SPSC 和一次 trading thread poll。
- 如果 order/feedback 在同一 loop 里处理，feedback burst 仍可能推迟 order send。

适合作为正确性优先的工程基线，但不是最低下单延迟。

### 方案 C：Strategy + Order + Order Feedback 全部同线程

```text
StrategyThread
  - market data
  - strategy
  - order submit
  - order feedback read / parse
```

优点：

- 实现最简单。
- 订单状态天然单线程一致。

缺点：

- feedback SBE decode / 私有回报 burst 会污染策略线程。
- trade read syscall、JSON/SBE parse 都在策略 loop 里。
- 很难同时保证行情最高优先级和交易回报及时处理。

不建议作为 `aquila` 的最终低延迟架构。

## 当前推荐方向

如果按性能第一选择：

```text
方案 A：Strategy + GateOrderSession 同线程，
       GateOrderFeedbackSession 独立线程，
       feedback 通过 SPSC 回到 strategy。
```

关键约束：

1. `GateOrderSession` 只处理 JSON order API response，不订阅 `futures.orders` / `futures.usertrades`。
2. `GateOrderFeedbackSession` 可连 `/sbe` endpoint，处理 JSON control + SBE push。
3. 策略线程必须保持行情最高优先级；feedback ring 只在合适预算下 poll。
4. order session 的 read budget 必须很小，只处理 login、ack/result/error、pong/close。
5. feedback session 的回报解析结果必须变成固定结构事件，再写 SPSC，避免把 JSON/SBE buffer 生命周期暴露给策略。
6. order session read path 里的 `ack=true` 快路径优先走 `simdjson_ack_minimal`，只提取 `request_id` 与 `ack`。
7. `MessageView::payload` 不包含 padding；如果 `readable_tail_bytes >= simdjson::SIMDJSON_PADDING`，submit parser 可以构造 zero-copy padded view。
8. 如果 tail padding 不满足，必须 fallback 到 simdjson padded scratch copy，不允许越界读取或临时改写 frame buffer。
9. `YYJSON_READ_INSITU` 会修改 buffer，默认不要直接用于共享的 FrameCodec payload。

## Gate + Binance 完整线程划分

结合 Gate / Binance 行情 session 和方案 A，完整系统线程建议如下：

```text
GateDataSessionThread
  - GateDataSession
  - Gate futures public market data WebSocket

BinanceDataSessionThread
  - BinanceDataSession
  - Binance futures public market data WebSocket

StrategyThread
  - Strategy
  - GateOrderSession
  - BinanceOrderSession
  - risk control
  - order management
  - order execution

GateOrderFeedbackThread
  - GateOrderFeedbackSession
  - Gate private feedback WebSocket

BinanceOrderFeedbackThread
  - BinanceOrderFeedbackSession
  - Binance private feedback WebSocket
```

跨线程数据流：

```text
GateDataSession     \
                                  -> StrategyThread
BinanceDataSession  /

StrategyThread
  - Strategy
  - GateOrderSession
  - BinanceOrderSession
  -> order submit / cancel / amend

GateOrderFeedbackSession
  -> GateOrderFeedbackThread
  -> feedback SPSC
  -> StrategyThread

BinanceOrderFeedbackSession
  -> BinanceOrderFeedbackThread
  -> feedback SPSC
  -> StrategyThread
```

说明：

1. `GateOrderSession` / `BinanceOrderSession` 有独立物理 WebSocket，但默认不拥有独立线程，而是贴在 `StrategyThread` 上运行。
2. `GateOrderFeedbackSession` / `BinanceOrderFeedbackSession` 独立线程运行，避免私有回报 burst 污染策略主循环。
3. `risk control`、`order management` 和 `order execution` 属于 `Strategy` 模块，不下沉到 exchange session。

## 当前推荐进程划分

生产推荐把两个行情源和策略 / 交易拆成三个进程：

```text
gate-md-process
  GateDataSessionThread
    GateDataSession
    Gate futures public market data WebSocket

binance-md-process
  BinanceDataSessionThread
    BinanceDataSession
    Binance futures public market data WebSocket

strategy-trade-process
  StrategyThread
    Strategy
    GateOrderSession
    BinanceOrderSession
    risk control
    order management
    order execution

  GateOrderFeedbackThread
    GateOrderFeedbackSession
    Gate private feedback WebSocket

  BinanceOrderFeedbackThread
    BinanceOrderFeedbackSession
    Binance private feedback WebSocket
```

取舍：

1. Gate / Binance 行情拆进程，隔离 WebSocket 重连、decode 异常、private link 或公网链路抖动。
2. Strategy 与 `GateOrderSession` / `BinanceOrderSession` 同进程同线程，避免下单热路径引入 IPC 或跨线程队列。
3. `GateOrderFeedbackSession` / `BinanceOrderFeedbackSession` 与策略同进程但独立线程，回报通过固定结构事件写入 SPSC，再由 `StrategyThread` 按预算消费。
4. 开发期可以用单进程工具组合多份 config 简化调试；生产配置按进程拆分，Gate 行情进程和 Binance 行情进程分别加载自己的 data session TOML。

当前行情进程配置示例：

```text
config/data_sessions/gate_data_session.toml
config/data_sessions/binance_data_session.toml
```

当前仓库内 `config/data_sessions/gate_data_session.toml` 指向 Gate 公网行情 endpoint，因此
`enable_tls = true`。private link 部署应使用独立配置或部署覆盖，至少同时替换 host / service
和 `enable_tls`。

## 待讨论 / 待验证

下一轮建议按这个顺序继续：

1. 明确统一 symbol metadata 如何进入 strategy、risk check 和 exchange adapter，并确认 Gate decimal-size 合约的数量步进规则后再用于下单热路径。
2. 明确 `GateOrderSession` 需要支持哪些 WebSocket API：place、batch place、cancel、cancel ids、cancel cp、amend、status/list。
3. 设计 `RequestIdCodec`、`OrderTextCodec`、`OrderFeedback` 固定结构。
4. 继续补 Gate SBE 私有回报 decode：`orders`、`usertrades`、`positions`，并明确它们与 `OrderFeedback` 的字段映射。
5. 明确 feedback stream 断线时是否允许继续 submit。
6. 设计 REST reconcile：feedback WS 断线或 gap 后如何补齐未决订单状态。
7. 将 `MessageView::readable_tail_bytes` 接入未来 `GateOrderSession`，形成 submit read 快路径：`payload view -> padding check -> simdjson minimal ack parse -> request correlation`。
8. 为方案 A / B 写端到端最小 benchmark：`strategy -> submit send`、`submit ack parse -> request correlation`、`feedback decode -> feedback SPSC`、`feedback poll -> order state update`。
9. 如果需要引用 Gate live 稳定性证据，重新运行 `gate_futures_book_ticker_probe` 并把原始输出写入文档。

## 相关文件

- `core/common/types.h`
- `core/common/constants.h`
- `core/market_data/types.h`
- `exchange/gate/sbe/schema/gate_fex_ws_latest.xml`
- `exchange/gate/sbe/generated/`
- `exchange/gate/sbe/message_header.h`
- `exchange/gate/sbe/message_dispatcher.h`
- `exchange/gate/sbe/book_ticker_decoder.h`
- `exchange/gate/market_data/client.h`
- `exchange/gate/market_data/data_session.h`
- `exchange/gate/market_data/subscription.h`
- `tools/gate/futures_book_ticker_probe.cpp`
- `scripts/gate/test_gate_ws_dual_login.py`
- `scripts/gate/test_gate_ws_connect.py`
- `scripts/gate/query_futures_contracts.py`
- `scripts/gate/query_futures_contracts_test.py`
- `scripts/binance/query_um_futures_contracts.py`
- `scripts/binance/query_um_futures_contracts_test.py`
- `exchange/gate/trading/submit_response_parser.h`
- `test/exchange/gate/trading/submit_response_parser_test.cpp`
- `benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp`
- `test/core/common/exchange_test.cpp`
- `test/core/market_data/types_test.cpp`
- `test/exchange/gate/sbe/message_dispatcher_test.cpp`
- `test/exchange/gate/sbe/book_ticker_decoder_test.cpp`
- `test/exchange/gate/market_data/futures_market_data_client_test.cpp`
- `test/exchange/gate/market_data/data_session_test.cpp`
- `benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp`
- `core/websocket/message_view.h`
- `core/websocket/websocket_client.h`
- `core/websocket/critical_session.h`
- `core/websocket/frame_codec.h`
- `evaluation/websocket/queued_frame_codec.h`
- `test/websocket/frame_codec_test.cpp`
- `core/websocket/types.h`
- `doc/websocket_read_write_benchmark_comparison.md`
- `doc/websocket_client_future_optimizations.md`
- `doc/futures_contract_metadata_fields.md`
- `third_party/sirius/exchange/gate`
