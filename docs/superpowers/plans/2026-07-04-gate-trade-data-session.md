# Gate Trade Data Session Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add Gate SBE public trade market data to the existing data session path, publishing normalized `Trade` records into a second SHM channel beside `BookTicker`.

**Architecture:** Keep one Gate data session process and one WebSocket connection. Decode SBE `bbo` and `publicTrade` into two fixed-size structs, then publish them into two independent `SPBroadcastQueue` channels in the same SHM object. Preserve existing `BookTicker` reader and fusion behavior; strategy runtime does not consume `Trade` in this plan.

**Tech Stack:** C++20, CMake, Gate SBE generated headers, `sbepp`, `toml++`, Nova `SPBroadcastQueue` / SHM allocator, GTest.

---

## File Map

- Modify `core/market_data/types.h`: add `Trade`.
- Modify `test/core/market_data/types_test.cpp`: ABI/layout test for `Trade`.
- Modify `core/market_data/data_shm_config.h`: add `TradeShmConfig` and combined `DataShmConfig`.
- Modify `core/market_data/data_shm.h`: add trade SHM channel, typed helpers, `TradeShmReader`, `OnTrade()`, `EmplaceTradeWith()`.
- Modify `test/core/market_data/data_shm_test.cpp`: cover book/trade independent channels and trade reader behavior.
- Create `exchange/gate/sbe/trade_decoder.h`: decode Gate SBE `publicTrade`.
- Create `evaluation/exchange/gate/sbe/trade_payload_builder.h`: test payload builder.
- Create `test/exchange/gate/sbe/trade_decoder_test.cpp`: decoder tests.
- Modify `test/exchange/gate/sbe/CMakeLists.txt`: add `gate_sbe_trade_decoder_test`.
- Modify `exchange/gate/market_data/subscription.h`: generic book/trade subscription builders while preserving existing book ticker wrappers.
- Modify `exchange/gate/market_data/text_envelope_parser.h`: parse channel as typed feed instead of only `channel_is_book_ticker`.
- Modify `exchange/gate/market_data/subscription_controller.h`: aggregate multi-feed subscription state.
- Modify `exchange/gate/market_data/client.h`: route `kPublicTrade` to trade decoder.
- Modify `exchange/gate/market_data/data_session.h`: store enabled feeds, send per-feed subscribe/unsubscribe, expose per-feed last request for tests.
- Modify `exchange/gate/market_data/data_session_config.h/.cpp`: parse `feeds`, old `feed` alias, trade channel config.
- Modify `tools/gate/data_session.cpp`: instantiate combined publisher and count/log trades.
- Modify Gate market-data tests and benchmarks that instantiate Gate client/session sinks: add `OnTrade()` methods.
- Modify `config/data_sessions/gate_data_session.toml`: move to `feeds = ["book_ticker"]` and add `trade_channel_name`; keep other Gate configs valid via parser alias or migrate them in the same commit.
- Modify docs: `docs/data_session_config.md`, `docs/data_session_shm_communication_design.md`, and `docs/project_onboarding_guide.md`.

---

## Task 1: Add `Trade` ABI

**Files:**
- Modify: `core/market_data/types.h`
- Modify: `test/core/market_data/types_test.cpp`

- [ ] **Step 1: Add failing ABI test**

Append this test to `test/core/market_data/types_test.cpp`:

```cpp
TEST(CoreMarketDataTypesTest, TradeCarriesGatePublicTradeEvent) {
  static_assert(std::is_standard_layout_v<aquila::Trade>);
  static_assert(std::is_trivially_copyable_v<aquila::Trade>);
  static_assert(alignof(aquila::Trade) == alignof(double));
  static_assert(sizeof(aquila::Trade) == aquila::kCacheLineBytes);
  static_assert(offsetof(aquila::Trade, id) == 0);
  static_assert(offsetof(aquila::Trade, symbol_id) == 8);
  static_assert(offsetof(aquila::Trade, exchange) == 12);
  static_assert(offsetof(aquila::Trade, side) == 13);
  static_assert(offsetof(aquila::Trade, reserved) == 14);
  static_assert(offsetof(aquila::Trade, exchange_ns) == 16);
  static_assert(offsetof(aquila::Trade, trade_ns) == 24);
  static_assert(offsetof(aquila::Trade, local_ns) == 32);
  static_assert(offsetof(aquila::Trade, price) == 40);
  static_assert(offsetof(aquila::Trade, volume) == 48);
  static_assert(offsetof(aquila::Trade, batch_index) == 56);
  static_assert(offsetof(aquila::Trade, batch_count) == 60);

  const aquila::Trade trade{
      .id = 123456789,
      .symbol_id = 42,
      .exchange = aquila::Exchange::kGate,
      .side = aquila::OrderSide::kBuy,
      .reserved = 0,
      .exchange_ns = 1'770'000'000'001'000'000,
      .trade_ns = 1'770'000'000'000'990'000,
      .local_ns = 1'770'000'000'001'200'000,
      .price = 65'012.5,
      .volume = 17.5,
      .batch_index = 1,
      .batch_count = 3,
  };

  EXPECT_EQ(trade.id, 123456789);
  EXPECT_EQ(trade.symbol_id, 42);
  EXPECT_EQ(trade.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(trade.side, aquila::OrderSide::kBuy);
  EXPECT_EQ(trade.reserved, 0);
  EXPECT_EQ(trade.exchange_ns, 1'770'000'000'001'000'000);
  EXPECT_EQ(trade.trade_ns, 1'770'000'000'000'990'000);
  EXPECT_EQ(trade.local_ns, 1'770'000'000'001'200'000);
  EXPECT_DOUBLE_EQ(trade.price, 65'012.5);
  EXPECT_DOUBLE_EQ(trade.volume, 17.5);
  EXPECT_EQ(trade.batch_index, 1U);
  EXPECT_EQ(trade.batch_count, 3U);
}
```

- [ ] **Step 2: Run the focused test and verify it fails to compile**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_market_data_types_test -j8
```

Expected: compile failure because `aquila::Trade` is not defined.

- [ ] **Step 3: Add `Trade` to `core/market_data/types.h`**

Insert below `BookTicker`:

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

- [ ] **Step 4: Run the focused test and verify it passes**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_market_data_types_test -j8
./build/debug/test/core/market_data/core_market_data_types_test
```

Expected: all `CoreMarketDataTypesTest` tests pass.

- [ ] **Step 5: Commit**

Run:

```bash
git add core/market_data/types.h test/core/market_data/types_test.cpp
git commit -m "feat: add normalized Trade market data type"
```

---

## Task 2: Add trade SHM channel and reader

**Files:**
- Modify: `core/market_data/data_shm_config.h`
- Modify: `core/market_data/data_shm.h`
- Modify: `test/core/market_data/data_shm_test.cpp`

- [ ] **Step 1: Add failing SHM tests**

Add helpers to `test/core/market_data/data_shm_test.cpp`:

```cpp
md::TradeShmConfig MakeTradeCreateConfig(std::string_view suffix) {
  return md::TradeShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .channel_name = "trade_channel",
      .create = true,
      .remove_existing = true,
  };
}

md::TradeShmConfig MakeTradeAttachConfig(
    const md::TradeShmConfig& create_config) {
  md::TradeShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

aquila::Trade MakeTrade(std::int64_t id) {
  return aquila::Trade{
      .id = id,
      .symbol_id = 42,
      .exchange = aquila::Exchange::kGate,
      .side = id % 2 == 0 ? aquila::OrderSide::kBuy
                          : aquila::OrderSide::kSell,
      .reserved = 0,
      .exchange_ns = 1'770'000'000'000'000'000 + id,
      .trade_ns = 1'770'000'000'000'010'000 + id,
      .local_ns = 1'770'000'000'000'100'000 + id,
      .price = 65'000.0 + static_cast<double>(id),
      .volume = 10.0 + static_cast<double>(id),
      .batch_index = static_cast<std::uint32_t>(id % 3),
      .batch_count = 3,
  };
}

void ExpectTradeEq(const aquila::Trade& actual,
                   const aquila::Trade& expected) {
  EXPECT_EQ(actual.id, expected.id);
  EXPECT_EQ(actual.symbol_id, expected.symbol_id);
  EXPECT_EQ(actual.exchange, expected.exchange);
  EXPECT_EQ(actual.side, expected.side);
  EXPECT_EQ(actual.reserved, expected.reserved);
  EXPECT_EQ(actual.exchange_ns, expected.exchange_ns);
  EXPECT_EQ(actual.trade_ns, expected.trade_ns);
  EXPECT_EQ(actual.local_ns, expected.local_ns);
  EXPECT_DOUBLE_EQ(actual.price, expected.price);
  EXPECT_DOUBLE_EQ(actual.volume, expected.volume);
  EXPECT_EQ(actual.batch_index, expected.batch_index);
  EXPECT_EQ(actual.batch_count, expected.batch_count);
}
```

Add these tests:

```cpp
TEST(DataShmTest, PublisherWritesAndReaderReadsTrade) {
  const md::TradeShmConfig config = MakeTradeCreateConfig("trade_publish_read");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::TradeShmReader reader(MakeTradeAttachConfig(config));
  reader.SeekLatest();

  const aquila::Trade expected = MakeTrade(7);
  publisher.OnTrade(expected);

  aquila::Trade actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  ExpectTradeEq(actual, expected);
  EXPECT_FALSE(reader.TryReadOne(&actual));
  EXPECT_EQ(publisher.published_trades(), 1U);
}

TEST(DataShmTest, PublisherEmplaceWithWritesAndReaderReadsTrade) {
  const md::TradeShmConfig config = MakeTradeCreateConfig("trade_emplace_read");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::TradeShmReader reader(MakeTradeAttachConfig(config));
  reader.SeekLatest();

  const aquila::Trade expected = MakeTrade(8);
  publisher.EmplaceTradeWith(
      [&expected](aquila::Trade& out) noexcept { out = expected; });

  aquila::Trade actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  ExpectTradeEq(actual, expected);
  EXPECT_FALSE(reader.TryReadOne(&actual));
  EXPECT_EQ(publisher.published_trades(), 1U);
}

TEST(DataShmTest, TradeOverrunDoesNotMoveBookTickerReader) {
  md::DataShmConfig config{
      .enabled = true,
      .shm_name = UniqueShmName("dual_channel"),
      .book_ticker_channel_name = "book_ticker_channel",
      .trade_channel_name = "trade_channel",
      .create = true,
      .remove_existing = true,
  };
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader book_reader(config.BookTickerConfigForAttach());
  md::TradeShmReader trade_reader(config.TradeConfigForAttach());
  book_reader.SeekLatest();
  trade_reader.SeekLatest();

  publisher.OnBookTicker(MakeBookTicker(1));
  for (std::uint64_t id = 0; id < md::kTradeShmCapacity + 2; ++id) {
    publisher.OnTrade(MakeTrade(static_cast<std::int64_t>(id)));
  }

  aquila::BookTicker book{};
  ASSERT_TRUE(book_reader.TryReadOne(&book));
  EXPECT_EQ(book.id, 1);
  EXPECT_EQ(book_reader.overrun_count(), 0U);

  aquila::Trade trade{};
  ASSERT_TRUE(trade_reader.TryReadOne(&trade));
  EXPECT_EQ(trade.id, 2);
  EXPECT_EQ(trade_reader.overrun_count(), 1U);
}
```

- [ ] **Step 2: Run the focused test and verify it fails**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_market_data_shm_test -j8
```

Expected: compile failure for missing `TradeShmConfig`, `TradeShmReader`, `OnTrade`, and combined `DataShmConfig`.

- [ ] **Step 3: Implement config types**

In `core/market_data/data_shm_config.h`, keep `BookTickerShmConfig` and add:

```cpp
inline constexpr std::uint64_t kTradeShmCapacity = 65536;

struct TradeShmConfig {
  bool enabled{false};
  std::string shm_name;
  std::string channel_name;
  bool create{true};
  bool remove_existing{false};
};

struct DataShmConfig {
  bool enabled{false};
  std::string shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  std::string trade_channel_name{"trade_channel"};
  bool create{true};
  bool remove_existing{false};

  [[nodiscard]] BookTickerShmConfig BookTickerConfig() const {
    return BookTickerShmConfig{.enabled = enabled,
                               .shm_name = shm_name,
                               .channel_name = book_ticker_channel_name,
                               .create = create,
                               .remove_existing = remove_existing};
  }

  [[nodiscard]] TradeShmConfig TradeConfig() const {
    return TradeShmConfig{.enabled = enabled,
                          .shm_name = shm_name,
                          .channel_name = trade_channel_name,
                          .create = create,
                          .remove_existing = remove_existing};
  }

  [[nodiscard]] BookTickerShmConfig BookTickerConfigForAttach() const {
    BookTickerShmConfig config = BookTickerConfig();
    config.create = false;
    config.remove_existing = false;
    return config;
  }

  [[nodiscard]] TradeShmConfig TradeConfigForAttach() const {
    TradeShmConfig config = TradeConfig();
    config.create = false;
    config.remove_existing = false;
    return config;
  }
};
```

- [ ] **Step 4: Implement trade channel types**

In `core/market_data/data_shm.h`, add a trade magic/version pair and queue:

```cpp
inline constexpr std::uint32_t kTradeShmMagic = 0x41514C54U;
inline constexpr std::uint32_t kTradeShmVersion = 1;

using TradeQueue =
    nova::static_impl::SPBroadcastQueue<aquila::Trade, kTradeShmCapacity>;

struct TradeShmHeader {
  std::uint32_t magic{kTradeShmMagic};
  std::uint32_t version{kTradeShmVersion};
  std::uint32_t abi_size{sizeof(aquila::Trade)};
  std::uint32_t capacity{kTradeShmCapacity};
  std::uint64_t producer_pid{0};
  std::uint64_t created_ns{0};
  std::atomic<std::uint64_t> published_count{0};
  std::atomic<std::uint64_t> heartbeat_ns{0};
};

struct TradeShmChannel {
  TradeShmHeader header{};
  TradeQueue queue{};
};
```

Then add `TradeShmManager` and `TradeShmReader` with the same public methods and overrun semantics as `BookTickerShmManager` / `BookTickerShmReader`, replacing `BookTicker` with `Trade`, `BookTickerQueue` with `TradeQueue`, and `kBookTickerShmCapacity` with `kTradeShmCapacity`.

- [ ] **Step 5: Expand `DataShmPublisher`**

Implement three constructors:

```cpp
explicit DataShmPublisher(const BookTickerShmConfig& config);
explicit DataShmPublisher(const TradeShmConfig& config);
explicit DataShmPublisher(const DataShmConfig& config);
```

Keep the existing book-ticker hot path API and add:

```cpp
void OnTrade(const aquila::Trade& trade) noexcept;

template <typename Writer>
void EmplaceTradeWith(Writer&& writer) noexcept;

[[nodiscard]] std::uint64_t published_book_tickers() const noexcept;
[[nodiscard]] std::uint64_t published_trades() const noexcept;
```

Preserve `published_count()` as an alias for `published_book_tickers()` so existing call sites compile.

- [ ] **Step 6: Run the focused test and verify it passes**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_market_data_shm_test -j8
./build/debug/test/core/market_data/core_market_data_shm_test
```

Expected: all `DataShmTest` tests pass.

- [ ] **Step 7: Commit**

Run:

```bash
git add core/market_data/data_shm_config.h core/market_data/data_shm.h test/core/market_data/data_shm_test.cpp
git commit -m "feat: add trade market data SHM channel"
```

---

## Task 3: Add Gate SBE trade decoder

**Files:**
- Create: `exchange/gate/sbe/trade_decoder.h`
- Create: `evaluation/exchange/gate/sbe/trade_payload_builder.h`
- Create: `test/exchange/gate/sbe/trade_decoder_test.cpp`
- Modify: `test/exchange/gate/sbe/CMakeLists.txt`

- [ ] **Step 1: Add failing decoder test target**

Append to `test/exchange/gate/sbe/CMakeLists.txt`:

```cmake
add_executable(gate_sbe_trade_decoder_test
    trade_decoder_test.cpp
)

target_link_libraries(gate_sbe_trade_decoder_test
    PRIVATE
        aquila_evaluation
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_sbe_trade_decoder_test
         COMMAND gate_sbe_trade_decoder_test)
```

- [ ] **Step 2: Add trade payload builder**

Create `evaluation/exchange/gate/sbe/trade_payload_builder.h` with a builder that writes:

```text
SBE header:
  blockLength = publicTrade block length
  templateId = kGateSbePublicTradeTemplateId
  schemaId = kGateSbeSchemaId
  version = kGateSbeSchemaVersion

message fixed fields:
  time = 1'770'000'000'001'000
  e = Event::Update
  pxExponent = -4
  szExponent = -3

trades group header:
  blockLength = 32
  numInGroup = trade_count

trade entries:
  t, id, size, price

dynamic fields:
  channel = "futures.trades"
  contract = symbol
```

Use the existing helpers from `book_ticker_payload_builder.h` by including it:

```cpp
#include "evaluation/exchange/gate/sbe/book_ticker_payload_builder.h"
#include "exchange/gate/sbe/generated/gate/messages/publicTrade.hpp"
```

Expose:

```cpp
struct PublicTradePayloadEntry {
  std::int64_t t;
  std::uint64_t id;
  std::int64_t size;
  std::int64_t price;
};

template <size_t N>
std::string_view BuildPublicTradePayload(
    std::array<char, N>* buffer, std::string_view symbol,
    std::span<const PublicTradePayloadEntry> entries,
    std::uint16_t template_id = kGateSbePublicTradeTemplateId,
    std::uint16_t block_length =
        ::sbepp::message_traits<
            ::gate::schema::messages::publicTrade>::block_length()) noexcept;
```

- [ ] **Step 3: Add decoder tests**

Create `test/exchange/gate/sbe/trade_decoder_test.cpp` with these tests:

```cpp
TEST(GateSbeTradeDecoderTest, DecodesSingleBuyTrade) {
  std::array<char, 256> buffer{};
  const std::array<aquila::gate::evaluation::PublicTradePayloadEntry, 1>
      entries{{
          {.t = 1'770'000'000'000'990,
           .id = 123456789,
           .size = 17'500,
           .price = 650'125'000},
      }};
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(
          &buffer, "BTC_USDT", entries);

  const aquila::gate::SbeDispatchResult dispatch =
      aquila::gate::DispatchSbeMessage(payload);
  ASSERT_EQ(dispatch.status, aquila::gate::SbeDispatchStatus::kReady);
  ASSERT_EQ(dispatch.message_type,
            aquila::gate::GateSbeMessageType::kPublicTrade);
  EXPECT_EQ(aquila::gate::ExtractTrustedTradeSymbol(payload, dispatch.header),
            "BTC_USDT");

  std::vector<aquila::Trade> trades;
  aquila::gate::DecodeTradesWithHeader(
      payload, dispatch.header, 1'770'000'000'001'200'000, 42,
      [&](const aquila::Trade& trade) noexcept { trades.push_back(trade); });

  ASSERT_EQ(trades.size(), 1U);
  EXPECT_EQ(trades[0].id, 123456789);
  EXPECT_EQ(trades[0].symbol_id, 42);
  EXPECT_EQ(trades[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(trades[0].side, aquila::OrderSide::kBuy);
  EXPECT_EQ(trades[0].reserved, 0);
  EXPECT_EQ(trades[0].exchange_ns, 1'770'000'000'001'000'000);
  EXPECT_EQ(trades[0].trade_ns, 1'770'000'000'000'990'000);
  EXPECT_EQ(trades[0].local_ns, 1'770'000'000'001'200'000);
  EXPECT_DOUBLE_EQ(trades[0].price, 65'012.5);
  EXPECT_DOUBLE_EQ(trades[0].volume, 17.5);
  EXPECT_EQ(trades[0].batch_index, 0U);
  EXPECT_EQ(trades[0].batch_count, 1U);
}

TEST(GateSbeTradeDecoderTest, DecodesSellSideAndBatchFields) {
  std::array<char, 512> buffer{};
  const std::array<aquila::gate::evaluation::PublicTradePayloadEntry, 2>
      entries{{
          {.t = 1'770'000'000'000'990,
           .id = 123456789,
           .size = 17'500,
           .price = 650'125'000},
          {.t = 1'770'000'000'000'991,
           .id = 123456790,
           .size = -21'000,
           .price = 650'120'000},
      }};
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(&buffer, "BTC_USDT",
                                                        entries);

  const aquila::gate::SbeDispatchResult dispatch =
      aquila::gate::DispatchSbeMessage(payload);
  std::vector<aquila::Trade> trades;
  aquila::gate::DecodeTradesWithHeader(
      payload, dispatch.header, 1'770'000'000'001'200'000, 42,
      [&](const aquila::Trade& trade) noexcept { trades.push_back(trade); });

  ASSERT_EQ(trades.size(), 2U);
  EXPECT_EQ(trades[0].side, aquila::OrderSide::kBuy);
  EXPECT_DOUBLE_EQ(trades[0].volume, 17.5);
  EXPECT_EQ(trades[0].batch_index, 0U);
  EXPECT_EQ(trades[0].batch_count, 2U);
  EXPECT_EQ(trades[1].side, aquila::OrderSide::kSell);
  EXPECT_DOUBLE_EQ(trades[1].volume, 21.0);
  EXPECT_EQ(trades[1].batch_index, 1U);
  EXPECT_EQ(trades[1].batch_count, 2U);
}
```

- [ ] **Step 4: Run the test target and verify it fails**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target gate_sbe_trade_decoder_test -j8
```

Expected: compile failure because `trade_decoder.h` does not exist.

- [ ] **Step 5: Implement `exchange/gate/sbe/trade_decoder.h`**

Expose these APIs:

```cpp
inline std::string_view ExtractTrustedTradeSymbol(
    std::string_view payload, const SbeMessageHeader& header) noexcept;

template <typename Handler>
inline void DecodeTradesWithHeader(std::string_view payload,
                                   const SbeMessageHeader& header,
                                   std::int64_t local_ns,
                                   std::int32_t symbol_id,
                                   Handler&& handler) noexcept;
```

Implementation rules:

- Assert header matches `publicTrade` block length, template id, schema id, and version.
- Assert `view.e() == ::gate::types::Event::Update`.
- Use `view.time().value() * 1000` for `exchange_ns`.
- Iterate `view.trades()`; `view.trades().numInGroup().value()` is `batch_count`.
- For each entry:
  - `id = static_cast<std::int64_t>(entry.id().value())`
  - `side = entry.size().value() >= 0 ? OrderSide::kBuy : OrderSide::kSell`
  - `volume = abs(size) * DecimalExponentScale(view.szExponent().value())`
  - `price = entry.price().value() * DecimalExponentScale(view.pxExponent().value())`
  - `trade_ns = entry.t().value() * 1000`
  - `batch_index` increments from 0
  - `batch_count` is the group count

Reuse `detail::ReadVarString8()` and `detail::DecimalExponentScale()` from `book_ticker_decoder.h`.

- [ ] **Step 6: Run the focused tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target gate_sbe_trade_decoder_test gate_sbe_message_dispatcher_test -j8
./build/debug/test/exchange/gate/sbe/gate_sbe_trade_decoder_test
./build/debug/test/exchange/gate/sbe/gate_sbe_message_dispatcher_test
```

Expected: both tests pass.

- [ ] **Step 7: Commit**

Run:

```bash
git add exchange/gate/sbe/trade_decoder.h \
        evaluation/exchange/gate/sbe/trade_payload_builder.h \
        test/exchange/gate/sbe/trade_decoder_test.cpp \
        test/exchange/gate/sbe/CMakeLists.txt
git commit -m "feat: decode Gate SBE public trades"
```

---

## Task 4: Route Gate market-data client trades to sinks

**Files:**
- Modify: `exchange/gate/market_data/client.h`
- Modify: `test/exchange/gate/market_data/futures_market_data_client_test.cpp`
- Modify: `test/exchange/gate/market_data/data_session_test.cpp`
- Modify: `benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp`
- Modify any other Gate data-session/client sink found by `rg 'OnBookTicker' exchange/gate tools/gate test/exchange/gate benchmark/exchange/gate`.

- [ ] **Step 1: Add failing client tests**

In `test/exchange/gate/market_data/futures_market_data_client_test.cpp`, include the new payload builder and extend sinks:

```cpp
#include "evaluation/exchange/gate/sbe/trade_payload_builder.h"

struct RecordingConsumer {
  int book_ticker_calls{0};
  int trade_calls{0};
  aquila::BookTicker last{};
  aquila::Trade last_trade{};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++book_ticker_calls;
    last = book_ticker;
  }

  void OnTrade(const aquila::Trade& trade) noexcept {
    ++trade_calls;
    last_trade = trade;
  }
};
```

Update existing assertions from `consumer.calls` to `consumer.book_ticker_calls`.

Add:

```cpp
TEST(GateFuturesMarketDataClientTest, EmitsTradesFromBinaryPublicTradePayload) {
  const std::array<aquila::gate::SymbolBinding, 1> symbols{
      aquila::gate::SymbolBinding{.exchange_symbol = "BTC_USDT",
                                  .symbol_id = 11}};
  RecordingConsumer consumer;
  DefaultClient client(symbols, consumer);

  std::array<char, 256> buffer{};
  const aquila::gate::evaluation::PublicTradePayloadEntry entry{
      .t = 1'770'000'000'000'990,
      .id = 123456789,
      .size = -21'000,
      .price = 650'120'000,
  };
  const std::string_view payload =
      aquila::gate::evaluation::BuildPublicTradePayload(
          &buffer, "BTC_USDT", std::span<const decltype(entry)>(&entry, 1));

  const auto result = client.OnMessage(BinaryView(payload), 999'000);

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_EQ(consumer.book_ticker_calls, 0);
  ASSERT_EQ(consumer.trade_calls, 1);
  EXPECT_EQ(consumer.last_trade.symbol_id, 11);
  EXPECT_EQ(consumer.last_trade.exchange, aquila::Exchange::kGate);
  EXPECT_EQ(consumer.last_trade.id, 123456789);
  EXPECT_EQ(consumer.last_trade.side, aquila::OrderSide::kSell);
  EXPECT_EQ(consumer.last_trade.local_ns, 999'000);
  EXPECT_DOUBLE_EQ(consumer.last_trade.price, 65'012.0);
  EXPECT_DOUBLE_EQ(consumer.last_trade.volume, 21.0);
}
```

- [ ] **Step 2: Run client test and verify it fails**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target gate_futures_market_data_client_test -j8
```

Expected: compile failure until client includes `trade_decoder.h` and handles `kPublicTrade`.

- [ ] **Step 3: Implement client trade branch**

In `exchange/gate/market_data/client.h`:

- Include `exchange/gate/sbe/trade_decoder.h`.
- Add trade counters to `FuturesMarketDataClientStats`:

```cpp
std::uint64_t public_trades{0};
```

- Add an `OnPublicTradePayload()` private method that:
  - extracts contract via `ExtractTrustedTradeSymbol()`
  - resolves `symbol_id`
  - calls `DecodeTradesWithHeader()`
  - publishes each decoded trade through `PublishDecodedTrade()`

Use this publishing helper:

```cpp
void PublishDecodedTrade(const Trade& trade) noexcept {
  if constexpr (requires(DataSink& data_sink, Trade value) {
                  data_sink.OnTrade(value);
                }) {
    data_sink_.OnTrade(trade);
  }
}
```

If the sink supports `EmplaceTradeWith`, prefer that path in the same style as `BookTicker`:

```cpp
if constexpr (requires(DataSink& data_sink) {
                data_sink.EmplaceTradeWith(
                    [](Trade&) noexcept {});
              }) {
  data_sink_.EmplaceTradeWith([&](Trade& out) noexcept { out = trade; });
} else {
  data_sink_.OnTrade(trade);
}
```

- [ ] **Step 4: Add `OnTrade` to all existing Gate client/session sinks**

Run:

```bash
rg -n 'struct .*DataSink|struct .*Consumer|OnBookTicker' \
  tools/gate test/exchange/gate benchmark/exchange/gate exchange/gate \
  -g '*.cpp' -g '*.h'
```

For every sink that instantiates `aquila::gate::FuturesMarketDataClient` or `aquila::gate::DataSession`, add one of:

```cpp
void OnTrade(const aquila::Trade&) noexcept {}
```

or, for counting sinks:

```cpp
void OnTrade(const aquila::Trade& trade) noexcept {
  ++trades;
  last_trade = trade;
}
```

- [ ] **Step 5: Run focused market-data tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target gate_futures_market_data_client_test gate_data_session_test -j8
./build/debug/test/exchange/gate/market_data/gate_futures_market_data_client_test
./build/debug/test/exchange/gate/market_data/gate_data_session_test
```

Expected: both tests pass.

- [ ] **Step 6: Commit**

Run:

```bash
git add exchange/gate/market_data/client.h \
        test/exchange/gate/market_data/futures_market_data_client_test.cpp \
        test/exchange/gate/market_data/data_session_test.cpp \
        benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp
git commit -m "feat: route Gate public trades through market data client"
```

---

## Task 5: Add multi-feed config and subscriptions

**Files:**
- Modify: `exchange/gate/market_data/data_session_config.h`
- Modify: `exchange/gate/market_data/data_session_config.cpp`
- Modify: `exchange/gate/market_data/subscription.h`
- Modify: `exchange/gate/market_data/text_envelope_parser.h`
- Modify: `exchange/gate/market_data/subscription_controller.h`
- Modify: `exchange/gate/market_data/data_session.h`
- Modify: `test/config/data_session_config_test.cpp`
- Modify: `test/exchange/gate/market_data/text_envelope_parser_test.cpp`
- Modify: `test/exchange/gate/market_data/data_session_test.cpp`
- Modify: `test/exchange/gate/market_data/futures_market_data_client_test.cpp`

- [ ] **Step 1: Add failing config parser tests**

In `test/config/data_session_config_test.cpp`, add:

```cpp
TEST(DataSessionConfigTest, ParsesGateFeedsAndTradeShmConfig) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feeds = ["book_ticker", "trade"]

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = false

[data_session.websocket.execution_policy]
bind_cpu_id = 2

[data_shm_sink]
enabled = true
shm_name = "aquila_gate_market_data"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = false
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::gate::ParseDataSessionConfig(parsed);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.feeds.book_ticker);
  EXPECT_TRUE(result.value.feeds.trade);
  EXPECT_EQ(result.value.book_ticker_shm.channel_name,
            "book_ticker_channel");
  EXPECT_EQ(result.value.trade_shm.channel_name, "trade_channel");
}

TEST(DataSessionConfigTest, RejectsGateFeedAndFeedsTogether) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feed = "book_ticker"
feeds = ["book_ticker"]

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = false

[data_session.websocket.execution_policy]
bind_cpu_id = 2
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::gate::ParseDataSessionConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_session.feed and data_session.feeds"),
            std::string::npos);
}

TEST(DataSessionConfigTest, RejectsUnknownGateFeed) {
  const std::string toml_text = std::string{R"toml(
[instrument_catalog]
file = ")toml"} + SourcePath("config/instruments/usdt_futures.csv").string() +
                                R"toml("
schema = "aquila.instrument.v1"

[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT"]
settle = "usdt"
wire_format = "sbe"
sbe_schema_id = 1
feeds = ["book_ticker", "depth"]

[data_session.websocket.endpoint]
host = "fx-ws.gateio.ws"
enable_tls = false

[data_session.websocket.execution_policy]
bind_cpu_id = 2
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::gate::ParseDataSessionConfig(parsed);
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("unknown Gate data_session feed"),
            std::string::npos);
}
```

- [ ] **Step 2: Add failing subscription and text envelope tests**

Add to `test/exchange/gate/market_data/futures_market_data_client_test.cpp`:

```cpp
TEST(GateFuturesMarketDataClientTest, BuildsTradeSubscribeRequest) {
  const std::array<std::string_view, 2> symbols{"BTC_USDT", "ETH_USDT"};

  const std::string request =
      aquila::gate::BuildFuturesTradeSubscribeRequest(symbols, 123);

  EXPECT_EQ(
      request,
      R"({"time":123,"channel":"futures.trades","event":"subscribe","payload":["BTC_USDT","ETH_USDT"]})");
}
```

Add to `test/exchange/gate/market_data/text_envelope_parser_test.cpp`:

```cpp
TEST(GateTextEnvelopeParserTest, ParsesTradeSubscribeChannel) {
  simdjson::ondemand::parser parser;
  aquila::gate::detail::TextEnvelope envelope{};
  ASSERT_TRUE(aquila::gate::detail::ParseTextEnvelope(
      R"({"time":1,"channel":"futures.trades","event":"subscribe","result":{"status":"success"}})",
      0, parser, envelope));
  EXPECT_EQ(envelope.channel, aquila::gate::detail::TextChannel::kTrade);
  EXPECT_EQ(envelope.event, aquila::gate::detail::TextEvent::kSubscribe);
  EXPECT_TRUE(envelope.result_success);
}
```

- [ ] **Step 3: Run focused tests and verify they fail**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_session_config_test gate_text_envelope_parser_test gate_futures_market_data_client_test -j8
```

Expected: compile failures for missing feed/config/channel APIs.

- [ ] **Step 4: Implement feed config structs**

In `exchange/gate/market_data/data_session_config.h`, add:

```cpp
struct DataSessionFeeds {
  bool book_ticker{true};
  bool trade{false};
};
```

Extend `DataSessionConfig`:

```cpp
DataSessionFeeds feeds;
::aquila::market_data::DataShmConfig data_shm;
::aquila::market_data::TradeShmConfig trade_shm;
```

Keep `book_ticker_shm` to avoid breaking Gate/Binance fusion code that only needs the book ticker channel.

- [ ] **Step 5: Implement parser rules**

In `exchange/gate/market_data/data_session_config.cpp`:

- Replace raw single `feed` with `DataSessionFeeds feeds`.
- Parse `feeds` array if present.
- Parse old `feed` only when `feeds` is absent.
- Reject duplicate feed names by checking if a bool is already true before setting it.
- Reject unknown feed names with error text containing `unknown Gate data_session feed`.
- Build config must accept `book_ticker`, `trade`, or both; it must still reject non-SBE `wire_format`.
- Parse `[data_shm_sink]` fields:
  - Old `channel_name` maps to `book_ticker_channel_name`.
  - New `book_ticker_channel_name` overrides default.
  - New `trade_channel_name` defaults to `"trade_channel"`.
  - If `feeds.trade && enabled && trade_channel_name.empty()`, fail.

- [ ] **Step 6: Implement subscription builders**

In `exchange/gate/market_data/subscription.h`, add:

```cpp
inline std::string BuildFuturesTradeSubscribeRequest(
    std::span<const std::string_view> symbols,
    std::int64_t epoch_seconds) {
  return detail::BuildFuturesSubscriptionRequest(
      "futures.trades", symbols, epoch_seconds, "subscribe");
}

inline std::string BuildFuturesTradeUnsubscribeRequest(
    std::span<const std::string_view> symbols,
    std::int64_t epoch_seconds) {
  return detail::BuildFuturesSubscriptionRequest(
      "futures.trades", symbols, epoch_seconds, "unsubscribe");
}
```

Refactor the existing book ticker wrappers to call the same generic builder.

- [ ] **Step 7: Implement typed text envelope**

In `text_envelope_parser.h`, replace `bool channel_is_book_ticker` with:

```cpp
enum class TextChannel : std::uint8_t {
  kUnknown = 0,
  kBookTicker,
  kTrade,
};

struct TextEnvelope {
  TextEvent event{TextEvent::kUnknown};
  TextChannel channel{TextChannel::kUnknown};
  bool result_success{false};
  bool has_error{false};
};
```

Map `futures.book_ticker` to `kBookTicker` and `futures.trades` to `kTrade`.

- [ ] **Step 8: Implement multi-feed session subscription**

In `data_session.h`:

- Store `DataSessionFeeds feeds_`.
- On active, send one subscribe request for each enabled feed.
- Keep `subscription_state()` aggregate:
  - `kSubscribeSent` after all enabled subscribe sends return `kOk`.
  - `kSubscribed` after all enabled subscribe acks are success.
  - `kRejected` after any enabled feed returns error.
- Add:

```cpp
[[nodiscard]] std::string_view last_book_ticker_subscribe_request() const noexcept;
[[nodiscard]] std::string_view last_trade_subscribe_request() const noexcept;
```

Preserve `last_subscribe_request()` as the last successfully sent request for existing tests.

- [ ] **Step 9: Run focused tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_session_config_test gate_text_envelope_parser_test gate_data_session_test gate_futures_market_data_client_test -j8
./build/debug/test/config/data_session_config_test
./build/debug/test/exchange/gate/market_data/gate_text_envelope_parser_test
./build/debug/test/exchange/gate/market_data/gate_data_session_test
./build/debug/test/exchange/gate/market_data/gate_futures_market_data_client_test
```

Expected: all four tests pass.

- [ ] **Step 10: Commit**

Run:

```bash
git add exchange/gate/market_data/data_session_config.h \
        exchange/gate/market_data/data_session_config.cpp \
        exchange/gate/market_data/subscription.h \
        exchange/gate/market_data/text_envelope_parser.h \
        exchange/gate/market_data/subscription_controller.h \
        exchange/gate/market_data/data_session.h \
        test/config/data_session_config_test.cpp \
        test/exchange/gate/market_data/text_envelope_parser_test.cpp \
        test/exchange/gate/market_data/data_session_test.cpp \
        test/exchange/gate/market_data/futures_market_data_client_test.cpp
git commit -m "feat: support Gate multi-feed data session config"
```

---

## Task 6: Wire the Gate data session tool and sample config

**Files:**
- Modify: `tools/gate/data_session.cpp`
- Modify: `config/data_sessions/gate_data_session.toml`
- Review/modify: other `config/data_sessions/gate_data_session_*.toml` files if parser alias is not enough for checked-in validation tests.

- [ ] **Step 1: Update tool sink**

In `tools/gate/data_session.cpp`, update `CountingDataSink`:

```cpp
struct CountingDataSink {
  std::uint64_t book_tickers{0};
  std::uint64_t trades{0};

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
  void OnTrade(const aquila::Trade& trade) noexcept;
};
```

Log every 1000 trades with:

```cpp
NOVA_INFO(
    "trade count={} id={} symbol_id={} exchange={} side={} exchange_ns={} "
    "trade_ns={} local_ns={} price={:.12g} volume={:.12g} "
    "batch_index={} batch_count={}",
    trades, trade.id, trade.symbol_id,
    magic_enum::enum_name(trade.exchange),
    magic_enum::enum_name(trade.side), trade.exchange_ns, trade.trade_ns,
    trade.local_ns, trade.price, trade.volume, trade.batch_index,
    trade.batch_count);
```

- [ ] **Step 2: Use combined SHM config for Gate**

In `RunDataSession()` use:

```cpp
if (data_session_config.data_shm.enabled) {
  aq_md::DataShmPublisher data_sink{data_session_config.data_shm};
  return RunDataSessionWithSink<WebSocketPolicy>(
      std::move(data_session_config), data_sink, connect);
}
```

Keep the counting sink branch for `data_shm.enabled == false`.

- [ ] **Step 3: Update the default Gate data session config**

In `config/data_sessions/gate_data_session.toml`:

```toml
[data_session]
feeds = ["book_ticker"]

[data_shm_sink]
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
```

Remove the old `feed = "book_ticker"` and old `channel_name` from this file only. If other checked-in Gate configs still use `feed` and `channel_name`, they remain valid through the alias parser.

- [ ] **Step 4: Dry-run the Gate data session config**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target gate_data_session data_session_config_test -j8
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml
./build/debug/test/config/data_session_config_test
```

Expected: dry-run exits 0 and prints the configured symbols; config tests pass.

- [ ] **Step 5: Commit**

Run:

```bash
git add tools/gate/data_session.cpp config/data_sessions/gate_data_session.toml test/config/data_session_config_test.cpp
git commit -m "feat: wire Gate data session trade SHM config"
```

---

## Task 7: Documentation sync

**Files:**
- Modify: `docs/data_session_config.md`
- Modify: `docs/data_session_shm_communication_design.md`
- Modify: `docs/project_onboarding_guide.md`
- Modify if new diagnostic fields are added: `docs/diagnostic_fields.md`

- [ ] **Step 1: Update data session config docs**

In `docs/data_session_config.md`, update the Gate example to:

```toml
[data_session]
name = "gate_data_session"
subscribe_symbols = ["BTC_USDT", "ETH_USDT", "SOL_USDT"]
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

Add field descriptions for:

- `data_session.feeds`
- `data_shm_sink.book_ticker_channel_name`
- `data_shm_sink.trade_channel_name`

State that `feed = "book_ticker"` remains a legacy single-feed alias for old Gate configs, and `feed` plus `feeds` together is rejected.

- [ ] **Step 2: Update SHM communication design**

In `docs/data_session_shm_communication_design.md`, replace the first-version scope statement with:

```text
第一版生产 reader 仍主要消费固定大小 BookTicker；Gate data session 现在也可以把固定大小 Trade 发布到同一 SHM object 内的独立 trade channel。两个 channel 都是 single-producer broadcast queue，reader cursor 仍在 reader 本地。
```

Add `Trade` capacity and channel description:

```text
BookTicker channel: SPBroadcastQueue<BookTicker, 65536>
Trade channel:      SPBroadcastQueue<Trade, 65536>
```

- [ ] **Step 3: Update onboarding**

In `docs/project_onboarding_guide.md`, update the `DataReader / Data Session` current facts with:

```text
Gate data session supports SBE book_ticker and public trade feed selection via feeds = ["book_ticker", "trade"]. It publishes BookTicker and Trade into separate typed channels in the same SHM object. Strategy DataReader still consumes BookTicker only unless a later task adds Trade reader support.
```

- [ ] **Step 4: Run documentation checks**

Run:

```bash
git diff --check
rg -n 'feed = "book_ticker"' docs/data_session_config.md docs/data_session_shm_communication_design.md docs/project_onboarding_guide.md
```

Expected: `git diff --check` exits 0. `rg` may show legacy alias discussion only; no Gate example should still use the old single `feed` field.

- [ ] **Step 5: Commit**

Run:

```bash
git add docs/data_session_config.md docs/data_session_shm_communication_design.md docs/project_onboarding_guide.md docs/diagnostic_fields.md
git commit -m "docs: document Gate trade data session feed"
```

If `docs/diagnostic_fields.md` is unchanged, omit it from `git add`.

---

## Task 8: Full focused verification

**Files:**
- No source edits unless verification exposes a defect.

- [ ] **Step 1: Build focused targets**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_types_test \
  core_market_data_shm_test \
  gate_sbe_trade_decoder_test \
  gate_sbe_book_ticker_decoder_test \
  gate_sbe_message_dispatcher_test \
  gate_futures_market_data_client_test \
  gate_data_session_test \
  gate_text_envelope_parser_test \
  data_session_config_test \
  gate_data_session \
  -j8
```

Expected: build exits 0.

- [ ] **Step 2: Run focused ctest**

Run:

```bash
ctest --test-dir build/debug -R '(core_market_data_types|core_market_data_shm|gate_sbe_|gate_.*market_data|data_session_config)' --output-on-failure
```

Expected: all matching tests pass.

- [ ] **Step 3: Run Gate data session dry-run**

Run:

```bash
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml
```

Expected: exits 0 without network connection and prints `book_ticker_channel` plus `trade_channel` config in logs or config-derived diagnostics.

- [ ] **Step 4: Run evaluation boundary checks**

Run:

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: both commands have no matches.

- [ ] **Step 5: Run final diff check**

Run:

```bash
git diff --check
git status --short --branch
```

Expected: no whitespace errors. Branch should show only intended committed work or be clean after the final verification commit.

- [ ] **Step 6: Commit verification-only fixes if needed**

If a verification command exposes a defect, fix only that defect, rerun the failed command plus `git diff --check`, then commit:

```bash
git add <fixed-files>
git commit -m "fix: complete Gate trade data session verification"
```

If no defect appears and all task commits are already present, do not create an empty verification commit.
