# Bitget BookTicker Market Data Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Bitget UTA public SBE `books1` BBO 接入 Aquila，达到 Gate `futures.book_ticker` 同等级能力：单路 `bitget_data_session`、多路 `bitget_data_fusion`、DataReader 和 recorder 全链路可用。

**Architecture:** 按 Gate 行情接入结构实现 Bitget 专用 adapter：Bitget SBE decoder 只负责 wire-to-`aquila::BookTicker`，Bitget market data client/session 负责订阅、收包、时间采集和 SHM 发布，Bitget data fusion 启动器复用现有 `BookTickerFusionRunner`。共享的 `BookTicker` ABI、SHM、RealtimeDataReader、HistoricalDataReader、recorder 和 fusion core 不做交易所特化改造。

**Tech Stack:** C++20、CMake、GTest、toml++、simdjson、fmt、Abseil `flat_hash_map`、Nova WebSocket/SHM/logging、现有 Aquila `BookTicker` typed binary format v1。

---

## Current Facts

- Bitget public SBE WebSocket URL: `wss://vip-ws-uta.bitget.com/v3/ws/public/sbe`。
- Bitget `books1` subscribe payload follows the observed script shape:

```json
{"op":"subscribe","args":[{"instType":"usdt-futures","topic":"books1","symbol":"BTCUSDT"}]}
```

- `scripts/bitget/market_data/probe_bbo_sbe.py` has observed `templateId=1002`, `schemaId=1`, `version=2`, and live `blockLength=64` while the minimum known fixed fields fit 56 bytes plus optional stream fields.
- Internal mapping is fixed for this implementation:

| Bitget `books1` field | `aquila::BookTicker` field |
|---|---|
| `seq` | `id` |
| instrument catalog lookup | `symbol_id` |
| constant `Exchange::kBitget` | `exchange` |
| `sts * 1000` | `exchange_ns` |
| `ts * 1000` | `event_ns` |
| data session ingress realtime clock | `local_ns` |
| `bid1Price * 10^priceExponent` | `bid_price` |
| `bid1Size * 10^sizeExponent` | `bid_volume` |
| `ask1Price * 10^priceExponent` | `ask_price` |
| `ask1Size * 10^sizeExponent` | `ask_volume` |

- If a decoded Bitget frame has no `sts` field, write `exchange_ns = event_ns` and cover that fallback in tests. Current live-observed frames should include `sts`.
- `aquila::BookTicker` is currently 72 bytes and already contains `exchange_ns`、`event_ns`、`local_ns`。Do not change the ABI for this task.
- `Exchange::kBitget` already exists in `core/common/types.h`; no enum addition is expected.

## File Structure

Create Bitget exchange adapter files:

- `exchange/bitget/CMakeLists.txt`: builds `aquila_bitget`.
- `exchange/bitget/sbe/message_header.h`: parses the 8-byte SBE header into `SbeMessageHeader`.
- `exchange/bitget/sbe/message_dispatcher.h`: validates Bitget schema/version/template and classifies `books1`.
- `exchange/bitget/sbe/book_ticker_decoder.h`: decodes `books1` binary payload into `aquila::BookTicker`.
- `exchange/bitget/market_data/subscription.h`: builds `books1` subscribe/unsubscribe JSON.
- `exchange/bitget/market_data/text_envelope_parser.h`: parses Bitget text ack/error envelopes.
- `exchange/bitget/market_data/subscription_controller.h`: tracks subscribe/unsubscribe state for `book_ticker`.
- `exchange/bitget/market_data/client.h`: dispatches binary SBE frames and publishes `BookTicker`.
- `exchange/bitget/market_data/data_session.h`: WebSocket session wrapper, local timestamp capture, subscription lifecycle.
- `exchange/bitget/market_data/data_session_config.h`
- `exchange/bitget/market_data/data_session_config.cpp`

Create tools and launch config support:

- `tools/bitget/bitget_data_session.cpp`: single Bitget data session CLI.
- `tools/bitget/bitget_data_fusion.cpp`: N-source Bitget data session + BookTicker fusion launcher.
- `tools/bitget/bitget_data_fusion_config.h`
- `tools/bitget/bitget_data_fusion_config.cpp`

Create test support:

- `evaluation/exchange/bitget/sbe/book_ticker_payload_builder.h`
- `test/exchange/bitget/sbe/CMakeLists.txt`
- `test/exchange/bitget/sbe/message_dispatcher_test.cpp`
- `test/exchange/bitget/sbe/book_ticker_decoder_test.cpp`
- `test/exchange/bitget/market_data/CMakeLists.txt`
- `test/exchange/bitget/market_data/futures_market_data_client_test.cpp`
- `test/exchange/bitget/market_data/data_session_test.cpp`
- `test/exchange/bitget/market_data/data_session_config_test.cpp`
- `test/tools/bitget/CMakeLists.txt`
- `test/tools/bitget/bitget_data_fusion_config_test.cpp`

Create configs:

- `config/data_sessions/bitget_data_session.toml`
- `config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml`
- `config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml`
- `config/data_readers/bitget_book_ticker_fusion_canonical_recorder.toml`
- `config/data_readers/bitget_book_ticker_fusion_4sources_source0_recorder.toml`
- `config/data_readers/bitget_book_ticker_fusion_4sources_source1_recorder.toml`
- `config/data_readers/bitget_book_ticker_fusion_4sources_source2_recorder.toml`
- `config/data_readers/bitget_book_ticker_fusion_4sources_source3_recorder.toml`

Modify build and docs:

- `CMakeLists.txt`: add `exchange/bitget` and Bitget test subdirectories if required by existing layout.
- `tools/CMakeLists.txt`: add `bitget_data_session` and `bitget_data_fusion`.
- `test/exchange/CMakeLists.txt`: include Bitget tests.
- `test/tools/CMakeLists.txt`: include Bitget tool tests.
- `docs/agent-handoff-bitget-market-data.md`: new handoff doc.
- `docs/data_session_config.md`: document Bitget data session config shape.
- `docs/data_session_shm_communication_design.md`: include Bitget timestamp mapping.
- `docs/diagnostic_fields.md`: include Bitget in BBO timestamp semantics where Gate/Binance are currently named.
- `docs/project_onboarding_guide.md`: add current Bitget entry points after implementation.

Do not modify these shared modules unless tests expose a real integration gap:

- `core/market_data/types.h`
- `core/market_data/data_shm.h`
- `core/market_data/realtime_data_reader.h`
- `core/market_data/historical_data_reader.h`
- `core/market_data/fusion/book_ticker.h`
- `tools/market_data/data_reader_recorder.cpp`
- `tools/market_data/data_reader_probe.cpp`

## Non-Goals

- No Bitget order gateway, private WS, account, order feedback, or trade feed in this plan.
- No change to `aquila::BookTicker` ABI.
- No change to fastest-route fusion algorithm.
- No shadow path inside LeadLag live strategy.
- No performance claims without benchmarks; this plan only defines the implementation and verification scope.

---

### Task 1: Bitget SBE Header, Dispatcher, and BookTicker Decoder

**Files:**
- Create: `exchange/bitget/sbe/message_header.h`
- Create: `exchange/bitget/sbe/message_dispatcher.h`
- Create: `exchange/bitget/sbe/book_ticker_decoder.h`
- Create: `evaluation/exchange/bitget/sbe/book_ticker_payload_builder.h`
- Create: `test/exchange/bitget/sbe/CMakeLists.txt`
- Create: `test/exchange/bitget/sbe/message_dispatcher_test.cpp`
- Create: `test/exchange/bitget/sbe/book_ticker_decoder_test.cpp`
- Modify: `exchange/bitget/CMakeLists.txt`
- Modify: `test/exchange/CMakeLists.txt`

- [ ] **Step 1: Write failing dispatcher tests**

Add tests covering:

```cpp
TEST(BitgetSbeMessageDispatcherTest, DispatchesBooks1Template) {
  const aquila::bitget::SbeMessageHeader header{
      .block_length = 59,
      .template_id = 1002,
      .schema_id = 1,
      .version = 2,
  };

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchBitgetSbeMessage(header);

  EXPECT_EQ(result.status, aquila::bitget::SbeDispatchStatus::kReady);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kBookTicker);
}

TEST(BitgetSbeMessageDispatcherTest, RejectsUnknownTemplate) {
  const aquila::bitget::SbeMessageHeader header{
      .block_length = 59,
      .template_id = 9999,
      .schema_id = 1,
      .version = 2,
  };

  const aquila::bitget::SbeDispatchResult result =
      aquila::bitget::DispatchBitgetSbeMessage(header);

  EXPECT_EQ(result.status,
            aquila::bitget::SbeDispatchStatus::kUnsupportedTemplate);
  EXPECT_EQ(result.message_type,
            aquila::bitget::BitgetSbeMessageType::kUnknown);
}
```

- [ ] **Step 2: Write failing decoder tests**

Use a builder that emits a binary payload equivalent to the Python probe fixture:

```cpp
TEST(BitgetSbeBookTickerDecoderTest, MapsBooks1ToBookTicker) {
  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(
          &buffer, "BTCUSDT",
          aquila::bitget::evaluation::BookTickerPayloadOptions{
              .block_length = 59,
              .ts = 1'700'000'000'000'001,
              .bid1_price = 6'569'038,
              .bid1_size = 15'000,
              .ask1_price = 6'569'042,
              .ask1_size = 20'000,
              .price_exponent = -2,
              .size_exponent = -4,
              .seq = 42,
              .sts = 1'700'000'000'001'001,
              .category = 1,
          });

  const aquila::bitget::SbeDispatchResult dispatch =
      aquila::bitget::DispatchSbeMessage(payload);
  ASSERT_EQ(dispatch.status, aquila::bitget::SbeDispatchStatus::kReady);

  aquila::BookTicker ticker{};
  aquila::bitget::DecodeBookTickerWithHeader(
      payload, dispatch.header, 1'700'000'000'002'003'000, 7, ticker);

  EXPECT_EQ(ticker.id, 42);
  EXPECT_EQ(ticker.symbol_id, 7);
  EXPECT_EQ(ticker.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(ticker.exchange_ns, 1'700'000'000'001'001'000);
  EXPECT_EQ(ticker.event_ns, 1'700'000'000'000'001'000);
  EXPECT_EQ(ticker.local_ns, 1'700'000'000'002'003'000);
  EXPECT_DOUBLE_EQ(ticker.bid_price, 65'690.38);
  EXPECT_DOUBLE_EQ(ticker.bid_volume, 1.5);
  EXPECT_DOUBLE_EQ(ticker.ask_price, 65'690.42);
  EXPECT_DOUBLE_EQ(ticker.ask_volume, 2.0);
}
```

Also add tests for:

```cpp
TEST(BitgetSbeBookTickerDecoderTest, AcceptsLiveObservedBlockLength64);
TEST(BitgetSbeBookTickerDecoderTest, FallsBackExchangeTimeToEventTimeWithoutSts);
TEST(BitgetSbeBookTickerDecoderTest, ExtractsBookTickerSymbol);
TEST(BitgetSbeBookTickerDecoderTest, RejectsShortPayloadForTestHelper);
```

- [ ] **Step 3: Run tests to verify they fail before implementation**

Run:

```bash
cmake --build build/debug --target bitget_sbe_book_ticker_decoder_test bitget_sbe_message_dispatcher_test -j8
```

Expected: fails because `exchange/bitget/sbe/*` and test targets do not exist.

- [ ] **Step 4: Implement minimal SBE header and dispatcher**

Implement header shape:

```cpp
namespace aquila::bitget {

inline constexpr std::size_t kSbeMessageHeaderBytes = 8;

struct SbeMessageHeader {
  std::uint16_t block_length{0};
  std::uint16_t template_id{0};
  std::uint16_t schema_id{0};
  std::uint16_t version{0};
};

bool ParseSbeMessageHeader(std::string_view payload,
                           SbeMessageHeader* out) noexcept;

}  // namespace aquila::bitget
```

Implement dispatcher constants:

```cpp
inline constexpr std::uint16_t kBitgetSbeSchemaId = 1;
inline constexpr std::uint16_t kBitgetSbeSchemaVersion = 2;
inline constexpr std::uint16_t kBitgetSbeBookTickerTemplateId = 1002;
```

Statuses mirror Gate naming: `kReady`, `kNeedMore`, `kUnsupportedSchema`, `kUnsupportedSchemaVersion`, `kUnsupportedTemplate`。

- [ ] **Step 5: Implement BookTicker decoder**

Decode little-endian fields in this order after the 8-byte SBE header:

```text
uint64 ts
int64  bid1Price
int64  bid1Size
int64  ask1Price
int64  ask1Size
int8   priceExponent
int8   sizeExponent
uint64 seq
optional uint64 sts
optional uint8  category
varString8 symbol
```

Implement:

```cpp
inline void DecodeBookTickerWithHeader(std::string_view payload,
                                       const SbeMessageHeader& header,
                                       std::int64_t local_ns,
                                       std::int32_t symbol_id,
                                       BookTicker& out) noexcept;

inline std::string_view ExtractTrustedBookTickerSymbol(
    std::string_view payload, const SbeMessageHeader& header) noexcept;
```

Use a fixed power-of-ten table matching Gate's `DecimalExponentScale` contract.

- [ ] **Step 6: Run decoder tests**

Run:

```bash
cmake --build build/debug --target bitget_sbe_book_ticker_decoder_test bitget_sbe_message_dispatcher_test -j8
ctest --test-dir build/debug -R '^bitget_sbe_(book_ticker_decoder|message_dispatcher)_test$' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 7: Commit decoder slice**

Run:

```bash
git add exchange/bitget evaluation/exchange/bitget test/exchange/bitget test/exchange/CMakeLists.txt
git commit -m "feat: add Bitget book ticker SBE decoder"
```

---

### Task 2: Bitget Market Data Client and Subscription State

**Files:**
- Create: `exchange/bitget/market_data/subscription.h`
- Create: `exchange/bitget/market_data/text_envelope_parser.h`
- Create: `exchange/bitget/market_data/subscription_controller.h`
- Create: `exchange/bitget/market_data/client.h`
- Create: `test/exchange/bitget/market_data/CMakeLists.txt`
- Create: `test/exchange/bitget/market_data/futures_market_data_client_test.cpp`
- Modify: `exchange/bitget/CMakeLists.txt`

- [ ] **Step 1: Write failing subscription and client tests**

Test subscribe JSON:

```cpp
TEST(BitgetFuturesMarketDataClientTest, BuildsBooks1SubscribeRequest) {
  const std::array<std::string_view, 2> symbols{"BTCUSDT", "ETHUSDT"};

  const std::string request = aquila::bitget::BuildBooks1SubscribeRequest(
      "usdt-futures", symbols);

  EXPECT_EQ(
      request,
      R"({"op":"subscribe","args":[{"instType":"usdt-futures","topic":"books1","symbol":"BTCUSDT"},{"instType":"usdt-futures","topic":"books1","symbol":"ETHUSDT"}]})");
}
```

Test binary BBO emission:

```cpp
TEST(BitgetFuturesMarketDataClientTest, EmitsBookTickerFromBinaryBooks1Payload) {
  const std::array<aquila::bitget::SymbolBinding, 1> symbols{
      aquila::bitget::SymbolBinding{.exchange_symbol = "BTCUSDT",
                                    .symbol_id = 11}};
  RecordingConsumer consumer;
  aquila::bitget::FuturesMarketDataClient client(symbols, consumer);

  std::array<char, 128> buffer{};
  const std::string_view payload =
      aquila::bitget::evaluation::BuildBookTickerPayload(&buffer, "BTCUSDT");

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  ASSERT_EQ(consumer.book_ticker_calls, 1);
  EXPECT_EQ(consumer.last.exchange, aquila::Exchange::kBitget);
  EXPECT_EQ(consumer.last.symbol_id, 11);
  EXPECT_EQ(consumer.last.id, 42);
  EXPECT_EQ(consumer.last.local_ns, 999'000);
}
```

Also cover:

```cpp
TEST(BitgetFuturesMarketDataClientTest, EmplacesBookTickerWhenConsumerSupportsSlotWriter);
TEST(BitgetFuturesMarketDataClientTest, IgnoresUnknownTemplate);
TEST(BitgetFuturesMarketDataClientTest, AcceptsSubscribeAckText);
TEST(BitgetFuturesMarketDataClientTest, RecordsUnsupportedTemplateDiagnostic);
```

- [ ] **Step 2: Run client tests to verify failure**

Run:

```bash
cmake --build build/debug --target bitget_futures_market_data_client_test -j8
```

Expected: fails because Bitget market data client files do not exist.

- [ ] **Step 3: Implement subscription helpers**

Implement:

```cpp
std::string BuildBooks1SubscribeRequest(
    std::string_view inst_type, std::span<const std::string_view> symbols);

std::string BuildBooks1UnsubscribeRequest(
    std::string_view inst_type, std::span<const std::string_view> symbols);
```

Use `fmt::memory_buffer` and no `std::cout` / `printf` paths.

- [ ] **Step 4: Implement text envelope parser**

Parser output:

```cpp
enum class TextEvent : std::uint8_t {
  kSubscribe,
  kUnsubscribe,
  kUnknown,
};

struct TextEnvelope {
  TextEvent event{TextEvent::kUnknown};
  bool has_error{false};
  bool result_success{false};
};
```

Accept success when `event` or `op` is subscribe/unsubscribe and either `code` is absent or `code == "0"`。Mark `has_error=true` for nonzero `code` or explicit `error` object.

- [ ] **Step 5: Implement client**

Mirror Gate `FuturesMarketDataClient` with Bitget names:

```cpp
struct SymbolBinding {
  std::string_view exchange_symbol{};
  std::int32_t symbol_id{-1};
};

template <typename DataSink,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions>
class FuturesMarketDataClient;
```

Client behavior:

- Ignore non-binary payloads in `OnMessage()` and let session handle text control.
- For binary payload: dispatch SBE, extract symbol, lookup `symbol_id`, decode into `BookTicker`, publish to sink.
- Prefer `EmplaceBookTickerWith()` when the sink supports it.
- Fill `DataSessionBookTickerTiming` with `exchange=Exchange::kBitget` and `book_ticker_event_ns=ticker.event_ns` when diagnostics are enabled.

- [ ] **Step 6: Run client tests**

Run:

```bash
cmake --build build/debug --target bitget_futures_market_data_client_test -j8
ctest --test-dir build/debug -R '^bitget_futures_market_data_client_test$' --output-on-failure
```

Expected: pass.

- [ ] **Step 7: Commit client slice**

Run:

```bash
git add exchange/bitget/market_data test/exchange/bitget/market_data exchange/bitget/CMakeLists.txt
git commit -m "feat: add Bitget market data client"
```

---

### Task 3: Bitget Data Session and Config

**Files:**
- Create: `exchange/bitget/market_data/data_session.h`
- Create: `exchange/bitget/market_data/data_session_config.h`
- Create: `exchange/bitget/market_data/data_session_config.cpp`
- Create: `test/exchange/bitget/market_data/data_session_test.cpp`
- Create: `test/exchange/bitget/market_data/data_session_config_test.cpp`
- Modify: `exchange/bitget/CMakeLists.txt`

- [ ] **Step 1: Write failing config tests**

Use this TOML fixture:

```toml
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[data_session]
name = "bitget_data_session"
inst_type = "usdt-futures"
subscribe_symbols = ["BTCUSDT", "ETHUSDT"]
feeds = ["book_ticker"]

[data_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"
target = "/v3/ws/public/sbe"
enable_tls = true

[data_shm_sink]
enabled = true
shm_name = "aquila_bitget_market_data"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = false
```

Assertions:

```cpp
EXPECT_EQ(config.name, "bitget_data_session");
EXPECT_EQ(config.inst_type, "usdt-futures");
EXPECT_TRUE(config.feeds.book_ticker);
EXPECT_FALSE(config.feeds.trade);
EXPECT_EQ(config.connection.host, "vip-ws-uta.bitget.com");
EXPECT_EQ(config.connection.target, "/v3/ws/public/sbe");
EXPECT_TRUE(config.connection.enable_tls);
EXPECT_EQ(config.data_shm.shm_name, "aquila_bitget_market_data");
EXPECT_EQ(config.data_shm.book_ticker_channel_name, "book_ticker_channel");
```

Also cover duplicate feeds, unknown feed, missing symbols, and invalid SHM channel names matching Gate failure style.

- [ ] **Step 2: Write failing data session tests**

Cover:

```cpp
TEST(BitgetDataSessionTest, SendsBooks1SubscribeWhenConnectionBecomesActive);
TEST(BitgetDataSessionTest, HandlesSubscribeAckForBooks1);
TEST(BitgetDataSessionTest, CapturesLocalNsBeforeDecode);
TEST(BitgetDataSessionTest, PublishesBookTickerToSink);
```

Use fake WebSocket policy as Gate tests do, not a real network connection.

- [ ] **Step 3: Run tests to verify failure**

Run:

```bash
cmake --build build/debug --target bitget_data_session_config_test bitget_data_session_test -j8
```

Expected: fails because data session files do not exist.

- [ ] **Step 4: Implement config parser**

Config structs:

```cpp
struct DataSessionFeeds {
  bool book_ticker{true};
  bool trade{false};
};

struct DataSessionConfig {
  std::string name;
  std::string inst_type{"usdt-futures"};
  websocket::ConnectionConfig connection;
  std::vector<std::string> exchange_symbols;
  std::vector<std::int32_t> symbol_ids;
  DataSessionFeeds feeds;
  ::aquila::market_data::DataShmConfig data_shm;
  ::aquila::market_data::BookTickerShmConfig book_ticker_shm;
  ::aquila::market_data::DataSessionDiagnosticsConfig diagnostics;
};
```

Only accept `book_ticker` feed in this task. Reject `trade` explicitly with an error mentioning Bitget trade feed is unsupported by this adapter.

- [ ] **Step 5: Implement data session**

Mirror Gate session structure:

- `DefaultTlsWebSocketPolicy` uses `websocket::TlsSocket` and `ClockSource::kRealtime`。
- `Handle()` records `local_ns = websocket::NowNs(kClockSource)` for binary frames before calling the client.
- `SendSubscribe()` sends `BuildBooks1SubscribeRequest(inst_type, symbols)`。
- `RequestUnsubscribe()` sends `BuildBooks1UnsubscribeRequest(inst_type, symbols)`。
- Text control frames are parsed by Bitget text parser and update the subscription controller.

- [ ] **Step 6: Run data session tests**

Run:

```bash
cmake --build build/debug --target bitget_data_session_config_test bitget_data_session_test -j8
ctest --test-dir build/debug -R '^bitget_(data_session_config|data_session)_test$' --output-on-failure
```

Expected: pass.

- [ ] **Step 7: Commit data session slice**

Run:

```bash
git add exchange/bitget/market_data test/exchange/bitget/market_data
git commit -m "feat: add Bitget market data session"
```

---

### Task 4: Bitget Data Session CLI and Config

**Files:**
- Create: `tools/bitget/bitget_data_session.cpp`
- Create: `config/data_sessions/bitget_data_session.toml`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: Write dry-run behavior through existing CLI pattern**

The CLI must support:

```bash
./build/debug/tools/bitget_data_session --config config/data_sessions/bitget_data_session.toml
./build/debug/tools/bitget_data_session --config config/data_sessions/bitget_data_session.toml --connect
```

Without `--connect`, it parses config, initializes logging, prints session details, and exits 0.

- [ ] **Step 2: Implement `bitget_data_session.cpp`**

Mirror `tools/gate/data_session.cpp` with Bitget namespaces:

```cpp
namespace aq_bitget = aquila::bitget;
namespace aq_md = aquila::market_data;
namespace ws = aquila::websocket;
```

Counting sink logs every 1000 BBOs:

```cpp
NOVA_INFO(
    "book_ticker count={} id={} symbol_id={} exchange={} exchange_ns={} "
    "event_ns={} local_ns={} bid_price={:.12g} bid_volume={:.12g} "
    "ask_price={:.12g} ask_volume={:.12g}",
    book_tickers, book_ticker.id, book_ticker.symbol_id,
    magic_enum::enum_name(book_ticker.exchange), book_ticker.exchange_ns,
    book_ticker.event_ns, book_ticker.local_ns, book_ticker.bid_price,
    book_ticker.bid_volume, book_ticker.ask_price, book_ticker.ask_volume);
```

If `data_shm.enabled`, construct `aquila::market_data::DataShmPublisher` from `config.data_shm`.

- [ ] **Step 3: Add checked-in default config**

Use `config/data_sessions/bitget_data_session.toml`:

```toml
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[log]
log_level = "info"
file_sink_name = "/home/liuxiang/log/bitget_data_session.log"
console_sink_name = "bitget_data_session_console"
backend_thread_name = "bitget_data_session_log"
backend_cpu_affinity = 5
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[data_session]
name = "bitget_data_session"
inst_type = "usdt-futures"
subscribe_symbols = ["BTCUSDT", "ETHUSDT", "SOLUSDT"]
feeds = ["book_ticker"]

[data_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"
target = "/v3/ws/public/sbe"
enable_tls = true

[data_session.websocket.execution_policy]
bind_cpu_id = 2

[data_shm_sink]
enabled = true
shm_name = "aquila_bitget_market_data"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = false
```

- [ ] **Step 4: Verify dry-run**

Run:

```bash
cmake --build build/debug --target bitget_data_session -j8
./build/debug/tools/bitget_data_session --config config/data_sessions/bitget_data_session.toml
```

Expected: exit 0 and logs include `name=bitget_data_session`, `host=vip-ws-uta.bitget.com`, and each configured symbol.

- [ ] **Step 5: Commit CLI slice**

Run:

```bash
git add tools/bitget/bitget_data_session.cpp tools/CMakeLists.txt config/data_sessions/bitget_data_session.toml
git commit -m "feat: add Bitget data session CLI"
```

---

### Task 5: Bitget Data Fusion Launcher and Config Parser

**Files:**
- Create: `tools/bitget/bitget_data_fusion.cpp`
- Create: `tools/bitget/bitget_data_fusion_config.h`
- Create: `tools/bitget/bitget_data_fusion_config.cpp`
- Create: `test/tools/bitget/CMakeLists.txt`
- Create: `test/tools/bitget/bitget_data_fusion_config_test.cpp`
- Create: `config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml`
- Create: `config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml`
- Modify: `tools/CMakeLists.txt`
- Modify: `test/tools/CMakeLists.txt`

- [ ] **Step 1: Write failing fusion config tests**

Test TOML:

```toml
[launch]
name = "bitget_data_fusion_book_ticker_4sources"
feeds = ["book_ticker"]

[launch.fusion_configs]
book_ticker = "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/bitget_data_session.toml"
data_session_name = "bitget_source_0"
data_shm_name = "aquila_bitget_book_ticker_src_0"
book_ticker_channel_name = "book_ticker_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
```

Assertions mirror Gate:

```cpp
EXPECT_EQ(result.value.name, "bitget_data_fusion_book_ticker_4sources");
EXPECT_EQ(result.value.feeds[0], support::DataFusionFeed::kBookTicker);
EXPECT_EQ(result.value.book_ticker_fusion_config,
          "config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml");
EXPECT_EQ(result.value.sources[0].data_shm_name,
          "aquila_bitget_book_ticker_src_0");
```

Also test duplicate feed, missing `launch.fusion_configs.book_ticker`, duplicate source id, duplicate SHM name, and same book/trade channel rejection where the shared parser applies.

- [ ] **Step 2: Implement fusion config parser**

Types mirror Gate with Bitget names:

```cpp
struct BitgetDataFusionSourceConfig {
  std::int32_t source_id{-1};
  std::filesystem::path data_session_config;
  std::string data_session_name;
  std::string data_shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  std::string trade_channel_name{"trade_channel"};
  bool remove_existing_source_shm{true};
  std::int32_t bind_cpu_id{-1};
};

struct BitgetDataFusionConfig {
  std::string name;
  std::filesystem::path book_ticker_fusion_config;
  std::filesystem::path trade_fusion_config;
  std::vector<aquila::tools::market_data::DataFusionFeed> feeds;
  std::int32_t backend_cpu_affinity{-1};
  std::vector<BitgetDataFusionSourceConfig> sources;
};
```

Use `tools/market_data/data_fusion_launch_config_parser.h`.

- [ ] **Step 3: Implement `bitget_data_fusion.cpp`**

Mirror `tools/gate/gate_data_fusion.cpp`, replacing Gate types with Bitget types. Use `BookTickerFusionThread` only; do not enable trade feeds for Bitget in this task.

Important runtime behavior:

- For each source, load one Bitget data session config.
- Apply source overrides via `ApplyFusionSourceOverrides()`.
- Start one `DataShmPublisher` + Bitget `DataSession` per source.
- Start one `BookTickerFusionThread` using `bitget_book_ticker_fusion_4sources.toml`.
- Dry-run mode logs source/fusion alignment and exits 0 without connecting.

- [ ] **Step 4: Add Bitget fusion configs**

`config/market_data_fusion/bitget_book_ticker_fusion_4sources.toml`:

```toml
[fusion]
name = "bitget_book_ticker_fusion_4sources"
max_events_per_source = 1
bind_cpu_id = 16
max_symbol_id = 4096

[fusion.output]
shm_name = "aquila_bitget_book_ticker_fusion"
channel_name = "book_ticker_channel"
remove_existing = true
metadata_bin = "/home/liuxiang/tmp/bitget_book_ticker_fusion_4sources/fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "bitget_source_0"
shm_name = "aquila_bitget_book_ticker_src_0"
channel_name = "book_ticker_channel"
```

Add source entries 1, 2, and 3 with matching names and SHM names.

`config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml` should point each source to `config/data_sessions/bitget_data_session.toml`, override `data_session_name`, source SHM name, channel name, `remove_existing_source_shm = true`, and CPU binding.

- [ ] **Step 5: Run fusion config and dry-run tests**

Run:

```bash
cmake --build build/debug --target bitget_data_fusion bitget_data_fusion_config_test -j8
ctest --test-dir build/debug -R '^bitget_data_fusion_config_test$' --output-on-failure
./build/debug/tools/bitget_data_fusion --config config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml
```

Expected: tests pass; dry-run exits 0 and logs four Bitget sources plus one canonical BookTicker output.

- [ ] **Step 6: Commit fusion slice**

Run:

```bash
git add tools/bitget test/tools/bitget tools/CMakeLists.txt test/tools/CMakeLists.txt config/market_data_fusion/bitget_*_fusion_4sources.toml
git commit -m "feat: add Bitget book ticker fusion launch"
```

---

### Task 6: DataReader and Recorder Config Verification

**Files:**
- Create: `config/data_readers/bitget_book_ticker_fusion_canonical_recorder.toml`
- Create: `config/data_readers/bitget_book_ticker_fusion_4sources_source0_recorder.toml`
- Create: `config/data_readers/bitget_book_ticker_fusion_4sources_source1_recorder.toml`
- Create: `config/data_readers/bitget_book_ticker_fusion_4sources_source2_recorder.toml`
- Create: `config/data_readers/bitget_book_ticker_fusion_4sources_source3_recorder.toml`
- Modify only if required by tests: `test/tools/market_data/data_reader_recorder_test.cpp`

- [ ] **Step 1: Add recorder configs**

Canonical recorder source:

```toml
[data_reader]
name = "bitget_book_ticker_fusion_canonical_recorder"
mode = "realtime"

[[data_reader.sources]]
name = "bitget_book_ticker_fusion_canonical"
type = "shm"
feed = "book_ticker"
shm_name = "aquila_bitget_book_ticker_fusion"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "drain"
```

Each source recorder uses:

```toml
shm_name = "aquila_bitget_book_ticker_src_0"
channel_name = "book_ticker_channel"
feed = "book_ticker"
```

Output paths should live under `/home/liuxiang/tmp/bitget_book_ticker_fusion_4sources/...` to follow project temp-output rules.

- [ ] **Step 2: Verify parser accepts configs**

Run a no-network parse path using existing recorder binary:

```bash
cmake --build build/debug --target data_reader_recorder data_reader_probe -j8
./build/debug/tools/data_reader_recorder --config config/data_readers/bitget_book_ticker_fusion_canonical_recorder.toml --help >/dev/null
```

If the recorder does not provide a pure config dry-run mode, add a focused unit test that parses the new config path without attaching SHM.

- [ ] **Step 3: Verify runtime attach with synthetic SHM**

After Task 5, use `bitget_data_session --connect` or a test fixture publisher to create Bitget SHM. Then run:

```bash
./build/debug/tools/data_reader_probe --config <bitget_realtime_reader_config> --max-events 3
```

Expected: `BookTicker` records print with `exchange=kBitget` and nonzero `event_ns` / `local_ns`.

- [ ] **Step 4: Verify recorder typed binary output**

Run:

```bash
timeout 10s ./build/debug/tools/data_reader_recorder --config config/data_readers/bitget_book_ticker_fusion_canonical_recorder.toml
```

Expected:

- Recorder exits cleanly on timeout/SIGTERM.
- Log prints `book_ticker_abi_size=72`.
- Output typed binary header has `feed=book_ticker` and `record_size=72`.

- [ ] **Step 5: Commit DataReader/recorder config slice**

Run:

```bash
git add config/data_readers/bitget_book_ticker_fusion_*.toml
git commit -m "config: add Bitget book ticker recorder configs"
```

---

### Task 7: Documentation

**Files:**
- Create: `docs/agent-handoff-bitget-market-data.md`
- Modify: `docs/data_session_config.md`
- Modify: `docs/data_session_shm_communication_design.md`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/project_onboarding_guide.md`

- [ ] **Step 1: Write Bitget handoff doc**

Include:

- Endpoint and subscription payload.
- `books1` field mapping table.
- Decoder entry points.
- Data session CLI and config path.
- Fusion CLI and config path.
- DataReader/recorder config paths.
- Verification commands.

- [ ] **Step 2: Update shared docs**

Document these facts:

- Bitget `BookTicker.exchange_ns = books1.sts * 1000`。
- Bitget `BookTicker.event_ns = books1.ts * 1000`。
- Bitget `BookTicker.id = books1.seq`。
- If a historical/probe-only frame lacks `sts`, Bitget decoder writes `exchange_ns = event_ns`。
- Bitget data session publishes the same 72-byte `aquila::BookTicker` ABI as Gate/Binance.

- [ ] **Step 3: Run doc checks**

Run:

```bash
git diff --check
```

Expected: no whitespace errors.

- [ ] **Step 4: Commit docs**

Run:

```bash
git add docs/agent-handoff-bitget-market-data.md docs/data_session_config.md docs/data_session_shm_communication_design.md docs/diagnostic_fields.md docs/project_onboarding_guide.md
git commit -m "docs: document Bitget market data integration"
```

---

### Task 8: Full Build, Test, and Live Smoke

**Files:**
- No planned source edits. Fix only real failures discovered by verification.

- [ ] **Step 1: Run focused build**

Run:

```bash
cmake --build build/debug --target \
  bitget_sbe_book_ticker_decoder_test \
  bitget_sbe_message_dispatcher_test \
  bitget_futures_market_data_client_test \
  bitget_data_session_config_test \
  bitget_data_session_test \
  bitget_data_fusion_config_test \
  bitget_data_session \
  bitget_data_fusion \
  -j8
```

Expected: all targets build.

- [ ] **Step 2: Run focused tests**

Run:

```bash
ctest --test-dir build/debug -R '^bitget_' --output-on-failure
```

Expected: all Bitget tests pass.

- [ ] **Step 3: Run full debug build**

Run:

```bash
cmake --build build/debug -j8
```

Expected: build succeeds.

- [ ] **Step 4: Run full CTest**

Run:

```bash
ctest --test-dir build/debug --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 5: Run optional live Bitget BBO smoke**

Only run when network and credentials-free public endpoint access are available:

```bash
timeout 20s ./build/debug/tools/bitget_data_session \
  --config config/data_sessions/bitget_data_session.toml \
  --connect
```

Expected: process connects, subscribes to `books1`, logs `book_ticker` count, and publishes SHM. If the endpoint rejects or times out, record the exact response and do not mark live smoke as passed.

- [ ] **Step 6: Run optional multi-source fusion smoke**

Only run after single-source smoke succeeds:

```bash
timeout 30s ./build/debug/tools/bitget_data_fusion \
  --config config/market_data_fusion/bitget_data_fusion_book_ticker_4sources.toml \
  --connect
```

Expected: source sessions publish Bitget BBO and canonical fusion SHM receives records. If network blocks multi-connection, record the exact failure and keep unit/dry-run verification as the committed evidence.

- [ ] **Step 7: Final whitespace and status check**

Run:

```bash
git diff --check
git status --short --branch
```

Expected: `git diff --check` is clean; status shows only intended committed branch ahead count or clean worktree after final commit.

## Self-Review

- Spec coverage: The plan covers Bitget SBE decode, market data client/session, single-source CLI, multi-source fusion launcher, DataReader/recorder configs, tests, docs, and verification.
- Placeholder scan: The plan contains no unresolved marker text, no unspecified implementation buckets, and no "same as previous task" shortcuts.
- Type consistency: All timestamp names match current `aquila::BookTicker`: `exchange_ns`, `event_ns`, `local_ns`; trade fields are out of scope.
