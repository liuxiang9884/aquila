# Gate Trade Data Session 设计

## 目标

在现有 Gate `BookTicker` data session 机制上加入 public trade 行情：同一个 Gate data session 进程可以按配置订阅多个 symbol 的 BBO 和 trade，decode 后分别写入同一个 SHM object 内的两个 typed `SPBroadcastQueue` channel。

第一版只实现 Gate SBE trade 主路径。Gate JSON trade 的字段映射和后续边界在本文记录，但不作为第一版代码范围。原因是当前 Gate data session 代码只支持 SBE `book_ticker`；JSON text update 在现有实现里只是计数为 unsupported，没有已经落地的 JSON BBO 主路径可复用。

## 当前代码事实

- `core/market_data/types.h` 只有 `BookTicker`，大小为 64 bytes。
- `core/market_data/data_shm.h` 只实现 `BookTickerShmChannel` / `DataShmPublisher` / `BookTickerShmReader`，一个 SHM channel 只有一个 producer，reader 本地维护 cursor。
- `exchange/gate/sbe/message_dispatcher.h` 已识别 `GateSbeMessageType::kPublicTrade`，但 `exchange/gate/market_data/client.h` 只处理 `kBookTicker`。
- `exchange/gate/sbe/schema/gate_fex_ws_latest.xml` 中 `bbo` 和 `publicTrade` 都已存在。
- `exchange/gate/market_data/data_session_config.cpp` 当前硬约束 `wire_format == "sbe"` 且 `feed == "book_ticker"`。
- `exchange/gate/market_data/data_session.h` 当前只构造 `futures.book_ticker` subscribe / unsubscribe 请求，text envelope parser 只识别 `futures.book_ticker` 的 control ack。

## 协议字段对齐

### BBO

现有 `BookTicker`：

```cpp
struct BookTicker {
  std::int64_t id;
  std::int32_t symbol_id;
  Exchange exchange;
  std::int64_t exchange_ns;
  std::int64_t local_ns;

  double bid_price;
  double bid_volume;
  double ask_price;
  double ask_volume;
};
```

Gate SBE `bbo` 关键字段：

| Gate 字段 | 当前映射 | 含义 |
| --- | --- | --- |
| `u` | `BookTicker.id` | orderbook / BBO update id |
| `time` | `BookTicker.exchange_ns = time * 1000` | WebSocket server send timestamp，us |
| `t` | 当前不写入 struct | orderbook engine update timestamp，us |
| `bidMantissaPrice` / `pxExponent` | `bid_price` | best bid price |
| `bidMantissaSize` / `szExponent` | `bid_volume` | best bid size |
| `askMantissaPrice` / `pxExponent` | `ask_price` | best ask price |
| `askMantissaSize` / `szExponent` | `ask_volume` | best ask size |
| `s` | symbol lookup | Gate contract symbol |

如果后续要把 `bbo.t` 加入 hot-path struct，同时保持 64-byte ABI，推荐复用 `exchange` 后的 padding 存 signed int24 `engine_to_ws_send_us = bbo.time - bbo.t`。该字段只作为诊断 delta，超出 `+-8.388s` 时 saturate 并计数；不要为它把 `BookTicker` 主 ABI 扩到 72 bytes。

### Trade

第一版新增 `Trade`：

```cpp
struct Trade {
  std::int64_t id;
  std::int32_t symbol_id;
  Exchange exchange;
  OrderSide side;
  std::uint16_t reserved;

  std::int64_t exchange_ns;
  std::int64_t trade_ns;
  std::int64_t local_ns;

  double price;
  double volume;

  std::uint32_t batch_index;
  std::uint32_t batch_count;
};
```

`Trade` 仍为 64 bytes、standard-layout、trivially-copyable，适合直接写入 `SPBroadcastQueue`。

Gate SBE `publicTrade` 映射：

| Gate 字段 | `Trade` 字段 | 含义 |
| --- | --- | --- |
| `trades[].id` | `id` | trade id |
| `contract` | `symbol_id` lookup | Gate contract symbol |
| 固定值 | `exchange = Exchange::kGate` | 交易所 |
| `trades[].size > 0` | `side = OrderSide::kBuy` | taker buy |
| `trades[].size < 0` | `side = OrderSide::kSell` | taker sell |
| `publicTrade.time` | `exchange_ns = time * 1000` | WebSocket server send timestamp，us |
| `trades[].t` | `trade_ns = t * 1000` | trade creation timestamp，us |
| 本机 receive-before-decode | `local_ns` | 与 `BookTicker.local_ns` 同语义 |
| `trades[].price` / `pxExponent` | `price` | 成交价 |
| `abs(trades[].size)` / `szExponent` | `volume` | 成交量，非负 |
| `trades` group 内位置 | `batch_index` | 同一 Gate message 内从 0 开始的位置 |
| `trades` group 总数 | `batch_count` | 同一 Gate message 内 trade 数量 |

`batch_index` / `batch_count` 不参与交易决策，保留它们是为了诊断、replay 和对账：Gate 一条 `publicTrade` message 可以包含多笔 trade，而 SHM 按“一笔成交一条 `Trade`”发布。下游看到相同 `exchange_ns` / `local_ns` 的多条 `Trade` 时，可以用这两个字段还原同一推送批次。

`reserved` 第一版固定写 0，不暴露 `flags`。如果后续实现 Gate JSON trade 且需要承载 `is_internal`，可以把 `reserved` 拆成 `std::uint8_t flags` + `std::uint8_t reserved`，保持 ABI size 不变。

### 时间语义

BBO 和 trade 对齐后的时间模型：

```text
engine / trade event creation
  BBO:   bbo.t
  Trade: trades[].t / create_time_ms

Gate WebSocket server send
  BBO:   bbo.time
  Trade: publicTrade.time / time_ms

local data session receive-before-decode
  BBO:   BookTicker.local_ns
  Trade: Trade.local_ns
```

因此：

- `exchange_ns - trade_ns` 表示 Gate 内部从 trade creation 到 WS server send 的时间。
- `local_ns - exchange_ns` 跨 Gate / 本机时钟，只适合作同一环境下的诊断参考，不单独解释为真实单程网络延迟。
- `local_ns - trade_ns` 同样跨时钟，只适合趋势和 outlier 诊断。

## SHM 设计

第一版把 BBO 和 trade 放在同一个 SHM object 内的两个独立 typed channel：

```text
aquila_gate_market_data
  book_ticker_channel: SPBroadcastQueue<BookTicker, kBookTickerShmCapacity>
  trade_channel:       SPBroadcastQueue<Trade, kTradeShmCapacity>
```

约束：

- 两个 channel 都是 single-producer broadcast queue。
- 两个 queue 的 producer 同属一个 data session process。
- reader cursor 仍在 reader 本地，不写入 SHM。
- trade burst 不会覆盖 BBO ring，因为两个 channel 独立。
- header / heartbeat / producer pid / create / remove_existing 生命周期共用同一个 SHM object。

实现建议：

- 保留现有 `BookTickerShmReader` public API，避免一次性改动所有 reader。
- 在 `data_shm.h` 内部抽出 typed helper，例如 `MarketDataShmChannel<T, Capacity, Magic>` / `MarketDataShmReader<T, Capacity>`。
- 新增 `TradeShmReader`、`OnTrade()` 和 `EmplaceTradeWith()`。
- `DataShmPublisher` 同时持有两个 channel；如果只启用其中一个 feed，未启用 channel 不发布。

容量第一版继续固定在代码中，不放入 TOML。建议：

```cpp
inline constexpr std::uint64_t kBookTickerShmCapacity = 65536;
inline constexpr std::uint64_t kTradeShmCapacity = 65536;
```

后续如果实盘 trade burst 证明 `65536` 不够，再基于 dropped / overrun 证据调容量，不预先把 capacity 暴露给配置。

## Config 设计

新配置使用数组：

```toml
[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT", "ETH_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feeds = ["book_ticker", "trade"]

[data_shm_sink]
enabled = true
shm_name = "aquila_gate_market_data"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = false
```

parser 规则：

- 新 checked-in 配置统一使用 `feeds`。
- `feeds` 允许值：`book_ticker`、`trade`。
- `feeds` 不能为空，不能重复。
- 第一版 `wire_format` 只接受 `sbe`。
- 如果保留旧 `feed = "book_ticker"`，只作为单 feed alias；同一配置同时出现 `feed` 和 `feeds` 必须报错。
- 如果 `data_shm_sink.enabled = true` 且订阅 `trade`，`trade_channel_name` 必须非空。
- 如果只订阅 `book_ticker`，`trade_channel_name` 可以缺省。

## Data Session 链路

启动冷路径：

```text
TOML + instrument catalog
  -> ParseDataSessionConfig()
  -> feeds + symbol bindings
  -> websocket target /v4/ws/<settle>/sbe?sbe_schema_id=<id>
  -> DataShmPublisher(book_ticker_channel, trade_channel)
  -> Gate DataSession
```

连接 active 后：

```text
if feeds contains book_ticker:
  send futures.book_ticker subscribe payload

if feeds contains trade:
  send futures.trades subscribe payload
```

Gate SBE 官方连接模型要求订阅请求和首个响应仍为 JSON，数据推送按 WebSocket opcode 区分 JSON / SBE。因此 text path 要继续处理 subscribe / unsubscribe ack，binary path 按 SBE header 分发 `bbo` / `publicTrade`。

binary message path：

```text
websocket binary frame
  -> DispatchSbeMessage()
  -> kBookTicker: DecodeBookTickerWithHeader() -> OnBookTicker()
  -> kPublicTrade: DecodePublicTradeWithHeader() -> OnTrade() per trade entry
```

text path：

- `TextEnvelope` 需要把 channel 从 `channel_is_book_ticker` 改为 typed feed channel。
- subscribe ack 对 `futures.book_ticker` 和 `futures.trades` 分别计数。
- 第一版可以保持一个 aggregate subscription state，但 diagnostics 应区分 book/trade ack 和 error。

## JSON trade 后续边界

Gate JSON trade 后续映射应复用同一个 `Trade` ABI：

| JSON 字段 | `Trade` 字段 |
| --- | --- |
| `id` | `id` |
| `contract` | `symbol_id` lookup |
| `size > 0` / `< 0` | `side` |
| `abs(size)` | `volume` |
| `price` | `price` |
| `time_ms` | `exchange_ns = time_ms * 1'000'000` |
| `create_time_ms` | `trade_ns = create_time_ms * 1'000'000` |
| `is_internal` | 后续可用 `flags` 承载 |

第一版不实现 JSON parser，不改现有 text update unsupported 行为。后续做 JSON BBO 时再同时补 JSON trade，避免 data session 先承担未使用的双格式状态机成本。

## 第一版非目标

- 不把 strategy runtime 改成消费 `Trade`。
- 不改 LeadLag trigger，不把 trade 纳入实盘策略逻辑。
- 不实现 Binance trade。
- 不实现 Gate JSON trade hot path。
- 不实现 trade fusion。
- 不把 SHM capacity 放入 TOML。
- 不在 hot path 做文件落盘或 CSV 输出。

## 测试与验证

第一版实现时需要覆盖：

- `Trade` ABI：`sizeof(Trade) == 64`、standard-layout、trivially-copyable。
- Gate SBE trade decoder：
  - 单笔 trade。
  - 多笔 trade，同一 message 拆成多条 `Trade`，`batch_index` / `batch_count` 正确。
  - signed size 映射 `side`，`volume` 取绝对值。
  - `publicTrade.time` / `trades[].t` 分别写入 `exchange_ns` / `trade_ns`。
- `DataShmPublisher` / `TradeShmReader`：
  - book/trade 两个 channel 独立读写。
  - trade overrun 不影响 book ticker reader。
- Gate data session config：
  - `feeds = ["book_ticker", "trade"]` 成功。
  - 只订阅 `trade` 成功。
  - 旧 `feed = "book_ticker"` 如保留 alias 则成功。
  - 同时配置 `feed` 和 `feeds` 失败。
  - unknown feed / duplicated feed 失败。
- Gate data session text control：
  - `futures.book_ticker` subscribe ack。
  - `futures.trades` subscribe ack。
  - 非订阅 feed 的 ack 不误标 ready。

建议 focused 验证命令：

```bash
ctest --test-dir build/debug -R '(core_market_data|gate_.*market_data|data_session_config|data_reader_config|data_reader_recorder)' --output-on-failure
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml
git diff --check
```

如果实现触碰 `evaluation/` 边界，提交前额外运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望无命中。

## 参考

- Gate Futures WebSocket 官方文档：SBE endpoint、`sbe_schema_id`、JSON request / first response and SBE data push 语义。
- 本仓库 Gate SBE schema：`exchange/gate/sbe/schema/gate_fex_ws_latest.xml`。
- 当前 BBO decoder：`exchange/gate/sbe/book_ticker_decoder.h`。
- 当前 data session SHM 设计：`docs/data_session_shm_communication_design.md`。
