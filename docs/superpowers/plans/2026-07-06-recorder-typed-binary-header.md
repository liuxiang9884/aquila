# Recorder Typed Binary Header Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate market data recorder outputs, historical replay inputs, manifest tooling, and Python numpy scripts from raw `BookTicker` / `Trade` record streams to typed-header binary format v1.

**Architecture:** Add one shared C++ binary-format header under `core/market_data` so recorder tools and `HistoricalDataReader` validate the same ABI. Add one shared Python helper under `scripts/market_data` so numpy scripts read, memmap, chunk, and write the same typed format. Manifest remains a segment index and gains typed-header metadata for preflight, while each binary file header remains the source of truth.

**Tech Stack:** C++20, CMake, GoogleTest, fmtlib, toml++, Python 3, numpy, unittest, optional `zstd` CLI for existing compressed preflight tests.

---

## Inputs And Locked Rules

- Design source: `docs/superpowers/specs/2026-07-06-recorder-typed-binary-header-design.md`.
- Header ABI is exactly 16 bytes, little-endian, magic bytes `41 51 4d 44` (`AQMD`), `version = 1`, `header_size = 16`, `flags = 0`.
- `feed_type = 1` means `book_ticker`; `feed_type = 2` means `trade`.
- `record_size` must equal the current ABI size for the feed.
- 0-byte files are invalid. Header-only files are valid and mean 0 records.
- No legacy raw fallback, raw auto-detection, raw converter, or `--legacy-raw` switch.
- `binary_file` TOML sources must explicitly set `feed`; SHM source default remains `book_ticker`.
- Hot path `HistoricalDataReader::Poll()` / `Drain()` must not parse headers.

## File Map

- Create `core/market_data/market_data_binary_format.h`: C++ typed binary header ABI, feed/type mapping, writer, mapped-memory parser, validation, record-count helper.
- Modify `tools/market_data/data_reader_recorder.h`: write typed headers in single-file and rotation modes, validate append targets, write manifest metadata.
- Modify `core/market_data/historical_data_reader.h`: mmap typed files, validate header against TOML feed, dispatch only payload records.
- Modify `core/config/data_reader_config.cpp`: require explicit `feed` for `binary_file`; preserve SHM default.
- Modify `scripts/market_data/typed_binary.py`: Python header parser/writer, `book_ticker_dtype()`, `trade_dtype()`, `load_records()`, `memmap_records()`, chunk iterators, typed write helpers.
- Modify `scripts/market_data/analyze_book_ticker_latency.py`: import dtype and typed loader from helper.
- Modify `scripts/market_data/analyze_book_ticker_fusion_latency.py`: keep fusion metadata raw; load BookTicker typed files through helper.
- Modify `scripts/market_data/compare_fusion_tardis_book_ticker.py`: read fusion BookTicker inputs through typed memmap/chunks.
- Modify `scripts/market_data/split_book_ticker_by_symbol.py`: read typed inputs and write typed per-symbol outputs.
- Modify `scripts/hdf_book_ticker_to_binary.py`: write typed BookTicker binary output.
- Modify `scripts/lead_lag/generate_preflight_config_params.py`: parse `.bin` and `.bin.zst` as whole typed binary files.
- Modify `scripts/market_data/manifest_to_data_reader_config.py`: validate manifest typed-header metadata before rendering TOML.
- Modify tests listed below in each task.
- Modify docs: `docs/data_reader_config.md`, `docs/diagnostic_fields.md`, `docs/lead_lag_live_replay_testing.md`, `docs/lead_lag_cancelled_order_fillability_analysis.md`, `docs/project_onboarding_guide.md`.

## Task 1: Add Shared C++ Typed Binary Format ABI

**Files:**
- Create: `core/market_data/market_data_binary_format.h`
- Modify: `test/core/market_data/historical_data_reader_test.cpp`

- [ ] **Step 1: Write the failing ABI tests**

Append these tests near the helper section in `test/core/market_data/historical_data_reader_test.cpp`, after `MakeTrade()` and before the current raw writer helpers:

```cpp
TEST(MarketDataBinaryFormatTest, BuildsBookTickerHeader) {
  const md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kBookTicker);

  EXPECT_EQ(header.magic, md::kMarketDataBinaryMagic);
  EXPECT_EQ(header.version, md::kMarketDataBinaryVersion);
  EXPECT_EQ(header.header_size, sizeof(md::MarketDataBinaryHeader));
  EXPECT_EQ(header.feed_type, md::kMarketDataBinaryBookTickerFeedType);
  EXPECT_EQ(header.record_size, sizeof(aquila::BookTicker));
  EXPECT_EQ(header.flags, 0U);
}

TEST(MarketDataBinaryFormatTest, BuildsTradeHeader) {
  const md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kTrade);

  EXPECT_EQ(header.magic, md::kMarketDataBinaryMagic);
  EXPECT_EQ(header.version, md::kMarketDataBinaryVersion);
  EXPECT_EQ(header.header_size, sizeof(md::MarketDataBinaryHeader));
  EXPECT_EQ(header.feed_type, md::kMarketDataBinaryTradeFeedType);
  EXPECT_EQ(header.record_size, sizeof(aquila::Trade));
  EXPECT_EQ(header.flags, 0U);
}
```

Add the include:

```cpp
#include "core/market_data/market_data_binary_format.h"
```

- [ ] **Step 2: Run the failing test target**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_market_data_historical_data_reader_test
```

Expected: compile fails because `core/market_data/market_data_binary_format.h` and `md::MarketDataBinaryHeader` do not exist.

- [ ] **Step 3: Add the shared header**

Create `core/market_data/market_data_binary_format.h` with this implementation:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_MARKET_DATA_BINARY_FORMAT_H_
#define AQUILA_CORE_MARKET_DATA_MARKET_DATA_BINARY_FORMAT_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

static_assert(std::endian::native == std::endian::little,
              "market data typed binary format currently requires little-endian hosts");

struct MarketDataBinaryHeader {
  std::uint32_t magic;
  std::uint16_t version;
  std::uint16_t header_size;
  std::uint16_t feed_type;
  std::uint16_t record_size;
  std::uint32_t flags;
};

static_assert(sizeof(MarketDataBinaryHeader) == 16);
static_assert(std::is_trivially_copyable_v<MarketDataBinaryHeader>);
static_assert(std::is_standard_layout_v<MarketDataBinaryHeader>);

inline constexpr std::uint32_t kMarketDataBinaryMagic = 0x444d5141U;
inline constexpr std::uint16_t kMarketDataBinaryVersion = 1;
inline constexpr std::uint16_t kMarketDataBinaryHeaderSize =
    sizeof(MarketDataBinaryHeader);
inline constexpr std::uint16_t kMarketDataBinaryBookTickerFeedType = 1;
inline constexpr std::uint16_t kMarketDataBinaryTradeFeedType = 2;
inline constexpr std::uint32_t kMarketDataBinaryFlags = 0;
inline constexpr std::string_view kMarketDataBinaryFormatName{
    "aquila.market_data.binary"};

[[nodiscard]] constexpr std::uint16_t MarketDataBinaryFeedTypeForFeed(
    config::DataReaderFeed feed) noexcept {
  switch (feed) {
    case config::DataReaderFeed::kBookTicker:
      return kMarketDataBinaryBookTickerFeedType;
    case config::DataReaderFeed::kTrade:
      return kMarketDataBinaryTradeFeedType;
  }
  return 0;
}

[[nodiscard]] constexpr std::size_t MarketDataBinaryRecordSizeForFeed(
    config::DataReaderFeed feed) noexcept {
  switch (feed) {
    case config::DataReaderFeed::kBookTicker:
      return sizeof(BookTicker);
    case config::DataReaderFeed::kTrade:
      return sizeof(Trade);
  }
  return 0;
}

[[nodiscard]] inline std::string_view MarketDataBinaryFeedName(
    config::DataReaderFeed feed) noexcept {
  switch (feed) {
    case config::DataReaderFeed::kBookTicker:
      return "book_ticker";
    case config::DataReaderFeed::kTrade:
      return "trade";
  }
  return "unknown";
}

[[nodiscard]] inline std::string_view MarketDataBinaryFeedTypeName(
    std::uint16_t feed_type) noexcept {
  switch (feed_type) {
    case kMarketDataBinaryBookTickerFeedType:
      return "book_ticker";
    case kMarketDataBinaryTradeFeedType:
      return "trade";
  }
  return "unknown";
}

[[nodiscard]] constexpr MarketDataBinaryHeader MakeMarketDataBinaryHeader(
    config::DataReaderFeed feed) noexcept {
  return MarketDataBinaryHeader{
      .magic = kMarketDataBinaryMagic,
      .version = kMarketDataBinaryVersion,
      .header_size = kMarketDataBinaryHeaderSize,
      .feed_type = MarketDataBinaryFeedTypeForFeed(feed),
      .record_size =
          static_cast<std::uint16_t>(MarketDataBinaryRecordSizeForFeed(feed)),
      .flags = kMarketDataBinaryFlags,
  };
}

[[nodiscard]] inline bool WriteMarketDataBinaryHeader(
    std::ostream& output, config::DataReaderFeed feed) {
  const MarketDataBinaryHeader header = MakeMarketDataBinaryHeader(feed);
  output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  return output.good();
}

[[nodiscard]] inline MarketDataBinaryHeader ReadMarketDataBinaryHeader(
    std::istream& input, const std::filesystem::path& file) {
  MarketDataBinaryHeader header{};
  input.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (input.gcount() != static_cast<std::streamsize>(sizeof(header))) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' is missing market data header", file.string()));
  }
  return header;
}

[[nodiscard]] inline MarketDataBinaryHeader ReadMarketDataBinaryHeaderFromData(
    const char* data, std::size_t size, const std::filesystem::path& file) {
  if (size < sizeof(MarketDataBinaryHeader)) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' is missing market data header", file.string()));
  }
  MarketDataBinaryHeader header{};
  std::memcpy(&header, data, sizeof(header));
  return header;
}

inline void ValidateMarketDataBinaryHeader(
    const MarketDataBinaryHeader& header, config::DataReaderFeed expected_feed,
    const std::filesystem::path& file) {
  if (header.magic != kMarketDataBinaryMagic) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has invalid market data magic 0x{:08x}",
        file.string(), header.magic));
  }
  if (header.version != kMarketDataBinaryVersion) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has unsupported market data version {}",
        file.string(), header.version));
  }
  if (header.header_size != kMarketDataBinaryHeaderSize) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has unsupported market data header size {}",
        file.string(), header.header_size));
  }
  if (header.flags != kMarketDataBinaryFlags) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' has unsupported market data flags 0x{:08x}",
        file.string(), header.flags));
  }

  const std::uint16_t expected_feed_type =
      MarketDataBinaryFeedTypeForFeed(expected_feed);
  if (header.feed_type != expected_feed_type) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' feed mismatch: header={}, expected={}",
        file.string(), MarketDataBinaryFeedTypeName(header.feed_type),
        MarketDataBinaryFeedName(expected_feed)));
  }

  const std::size_t expected_record_size =
      MarketDataBinaryRecordSizeForFeed(expected_feed);
  if (header.record_size != expected_record_size) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' {} record size mismatch: header={}, expected={}",
        file.string(), MarketDataBinaryFeedName(expected_feed),
        header.record_size, expected_record_size));
  }
}

[[nodiscard]] inline std::uint64_t CheckedMarketDataBinaryRecordCount(
    const std::filesystem::path& file, std::uintmax_t file_size,
    const MarketDataBinaryHeader& header,
    config::DataReaderFeed expected_feed) {
  ValidateMarketDataBinaryHeader(header, expected_feed, file);
  if (file_size < header.header_size) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' size {} is smaller than market data header {}",
        file.string(), file_size, header.header_size));
  }
  const std::uintmax_t payload_bytes = file_size - header.header_size;
  if (header.record_size == 0 || payload_bytes % header.record_size != 0) {
    throw std::runtime_error(fmt::format(
        "binary data file '{}' payload size {} is not a multiple of {} record "
        "size {}",
        file.string(), payload_bytes, MarketDataBinaryFeedName(expected_feed),
        header.record_size));
  }
  const std::uintmax_t record_count = payload_bytes / header.record_size;
  if (record_count > static_cast<std::uintmax_t>(
                         std::numeric_limits<std::uint64_t>::max())) {
    throw std::runtime_error(
        fmt::format("binary data file '{}' is too large", file.string()));
  }
  return static_cast<std::uint64_t>(record_count);
}

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_MARKET_DATA_BINARY_FORMAT_H_
```

- [ ] **Step 4: Build and run the target**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_market_data_historical_data_reader_test
ctest --test-dir build/debug -R core_market_data_historical_data_reader_test --output-on-failure
```

Expected: build succeeds and the new `MarketDataBinaryFormatTest` tests pass. Existing historical reader tests may still fail after later tasks rewrite fixtures; if they fail now because raw fixtures are still accepted, continue to Task 4 before treating that as a blocker.

- [ ] **Step 5: Commit Task 1**

```bash
git add core/market_data/market_data_binary_format.h test/core/market_data/historical_data_reader_test.cpp
git commit -m "feat: add market data typed binary format"
```

## Task 2: Write Typed Header In Single-File Recorder Mode

**Files:**
- Modify: `tools/market_data/data_reader_recorder.h`
- Modify: `test/tools/market_data/data_reader_recorder_test.cpp`

- [ ] **Step 1: Update recorder test helpers to read typed files**

In `test/tools/market_data/data_reader_recorder_test.cpp`, add:

```cpp
#include "core/market_data/market_data_binary_format.h"
```

Replace `ReadBookTickers()` and `ReadTrades()` with typed-header versions:

```cpp
template <typename RecordT>
std::vector<RecordT> ReadTypedRecords(const std::filesystem::path& output_path,
                                      cfg::DataReaderFeed feed) {
  const std::uintmax_t size = std::filesystem::file_size(output_path);
  std::ifstream input(output_path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  const md::MarketDataBinaryHeader header =
      md::ReadMarketDataBinaryHeader(input, output_path);
  EXPECT_NO_THROW(md::ValidateMarketDataBinaryHeader(header, feed, output_path));
  EXPECT_GE(size, static_cast<std::uintmax_t>(header.header_size));
  EXPECT_EQ((size - header.header_size) % sizeof(RecordT), 0U);
  std::vector<RecordT> records((size - header.header_size) / sizeof(RecordT));
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(records.size() * sizeof(RecordT)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

std::vector<BookTicker> ReadBookTickers(
    const std::filesystem::path& output_path) {
  return ReadTypedRecords<BookTicker>(output_path,
                                      cfg::DataReaderFeed::kBookTicker);
}

std::vector<Trade> ReadTrades(const std::filesystem::path& output_path) {
  return ReadTypedRecords<Trade>(output_path, cfg::DataReaderFeed::kTrade);
}
```

- [ ] **Step 2: Add failing tests for single-file header and append validation**

Append these tests near existing single-file recorder tests:

```cpp
TEST(DataReaderRecorderTest, BookTickerBinaryRecorderWritesTypedHeader) {
  const std::filesystem::path root =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("aquila_recorder_header_test_{}", ::getpid());
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const std::filesystem::path output = root / "book_ticker.bin";

  BookTickerBinaryRecorder recorder(output, RecorderWriteMode::kTruncate);
  const BookTicker expected =
      MakeTicker(1, Exchange::kGate, 1'000'000, 1'000'100);
  recorder.OnBookTicker(expected);
  ASSERT_TRUE(recorder.Flush());

  EXPECT_EQ(std::filesystem::file_size(output),
            sizeof(md::MarketDataBinaryHeader) + sizeof(BookTicker));
  const std::vector<BookTicker> records = ReadBookTickers(output);
  ASSERT_EQ(records.size(), 1U);
  ExpectBookTickerEq(records[0], expected);
  std::filesystem::remove_all(root);
}

TEST(DataReaderRecorderTest, TradeBinaryRecorderWritesTypedHeader) {
  const std::filesystem::path root =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("aquila_recorder_trade_header_test_{}", ::getpid());
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const std::filesystem::path output = root / "trade.bin";

  TradeBinaryRecorder recorder(output, RecorderWriteMode::kTruncate);
  const Trade expected =
      MakeTrade(2, Exchange::kBinance, 2'000'000, 2'000'050, 2'000'100);
  recorder.OnTrade(expected);
  ASSERT_TRUE(recorder.Flush());

  EXPECT_EQ(std::filesystem::file_size(output),
            sizeof(md::MarketDataBinaryHeader) + sizeof(Trade));
  const std::vector<Trade> records = ReadTrades(output);
  ASSERT_EQ(records.size(), 1U);
  ExpectTradeEq(records[0], expected);
  std::filesystem::remove_all(root);
}

TEST(DataReaderRecorderTest, AppendModeKeepsSingleTypedHeader) {
  const std::filesystem::path root =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("aquila_recorder_append_header_test_{}", ::getpid());
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const std::filesystem::path output = root / "book_ticker.bin";

  {
    BookTickerBinaryRecorder recorder(output, RecorderWriteMode::kTruncate);
    recorder.OnBookTicker(MakeTicker(1, Exchange::kGate, 10, 20));
    ASSERT_TRUE(recorder.Flush());
  }
  {
    BookTickerBinaryRecorder recorder(output, RecorderWriteMode::kAppend);
    recorder.OnBookTicker(MakeTicker(2, Exchange::kGate, 30, 40));
    ASSERT_TRUE(recorder.Flush());
  }

  EXPECT_EQ(std::filesystem::file_size(output),
            sizeof(md::MarketDataBinaryHeader) + 2 * sizeof(BookTicker));
  const std::vector<BookTicker> records = ReadBookTickers(output);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].id, 1);
  EXPECT_EQ(records[1].id, 2);
  std::filesystem::remove_all(root);
}

TEST(DataReaderRecorderTest, AppendModeRejectsRawBookTickerFile) {
  const std::filesystem::path root =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("aquila_recorder_append_raw_test_{}", ::getpid());
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const std::filesystem::path output = root / "book_ticker.bin";
  const BookTicker raw = MakeTicker(1, Exchange::kGate, 10, 20);
  {
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out.write(reinterpret_cast<const char*>(&raw), sizeof(raw));
    ASSERT_TRUE(out.good());
  }

  EXPECT_THROW((BookTickerBinaryRecorder{output, RecorderWriteMode::kAppend}),
               std::runtime_error);
  std::filesystem::remove_all(root);
}

TEST(DataReaderRecorderTest, AppendModeRejectsWrongFeedHeader) {
  const std::filesystem::path root =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("aquila_recorder_append_wrong_feed_test_{}", ::getpid());
  std::filesystem::remove_all(root);
  ASSERT_TRUE(std::filesystem::create_directories(root));
  const std::filesystem::path output = root / "book_ticker.bin";
  {
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    ASSERT_TRUE(md::WriteMarketDataBinaryHeader(out, cfg::DataReaderFeed::kTrade));
  }

  EXPECT_THROW((BookTickerBinaryRecorder{output, RecorderWriteMode::kAppend}),
               std::runtime_error);
  std::filesystem::remove_all(root);
}
```

- [ ] **Step 3: Run the failing recorder test target**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_reader_recorder_test
ctest --test-dir build/debug -R data_reader_recorder_test --output-on-failure
```

Expected: tests fail because `TypedBinaryRecorder` still writes raw payload records and append mode does not validate a header.

- [ ] **Step 4: Add feed metadata to recorder traits**

In `tools/market_data/data_reader_recorder.h`, add the shared include:

```cpp
#include "core/market_data/market_data_binary_format.h"
```

Update the traits:

```cpp
struct BookTickerRecorderTraits {
  static constexpr std::string_view kFeedName{"book_ticker"};
  static constexpr config::DataReaderFeed kFeed{
      config::DataReaderFeed::kBookTicker};

  [[nodiscard]] static Exchange ExchangeOf(const BookTicker& record) noexcept {
    return record.exchange;
  }

  [[nodiscard]] static std::int64_t ExchangeNsOf(
      const BookTicker& record) noexcept {
    return record.exchange_ns;
  }

  [[nodiscard]] static std::int64_t LocalNsOf(
      const BookTicker& record) noexcept {
    return record.local_ns;
  }
};

struct TradeRecorderTraits {
  static constexpr std::string_view kFeedName{"trade"};
  static constexpr config::DataReaderFeed kFeed{config::DataReaderFeed::kTrade};

  [[nodiscard]] static Exchange ExchangeOf(const Trade& record) noexcept {
    return record.exchange;
  }

  [[nodiscard]] static std::int64_t ExchangeNsOf(const Trade& record) noexcept {
    return record.exchange_ns;
  }

  [[nodiscard]] static std::int64_t LocalNsOf(const Trade& record) noexcept {
    return record.local_ns;
  }
};
```

- [ ] **Step 5: Implement single-file header write and append validation**

Replace the open logic in `TypedBinaryRecorder` with this structure:

```cpp
  TypedBinaryRecorder(std::filesystem::path output_path,
                      RecorderWriteMode write_mode)
      : output_path_(std::move(output_path)) {
    PreflightSingleOutputPath(output_path_, Traits::kFeedName);
    OpenOutput(write_mode);
  }
```

Add these private helpers above the data members:

```cpp
  void OpenOutput(RecorderWriteMode write_mode) {
    switch (write_mode) {
      case RecorderWriteMode::kTruncate:
        output_.open(output_path_, std::ios::binary | std::ios::trunc);
        if (!output_.is_open()) {
          throw std::runtime_error(fmt::format("failed to open output file '{}'",
                                               output_path_.string()));
        }
        if (!market_data::WriteMarketDataBinaryHeader(output_, Traits::kFeed)) {
          throw std::runtime_error(fmt::format(
              "failed to write market data header to '{}'",
              output_path_.string()));
        }
        return;

      case RecorderWriteMode::kAppend:
        PrepareAppendTarget();
        output_.open(output_path_, std::ios::binary | std::ios::app);
        if (!output_.is_open()) {
          throw std::runtime_error(fmt::format("failed to open output file '{}'",
                                               output_path_.string()));
        }
        return;
    }
  }

  void PrepareAppendTarget() {
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(output_path_, exists_error);
    if (exists_error) {
      throw std::runtime_error(fmt::format(
          "failed to inspect output file '{}': {}", output_path_.string(),
          exists_error.message()));
    }
    if (!exists) {
      std::ofstream create(output_path_, std::ios::binary | std::ios::trunc);
      if (!create.is_open() ||
          !market_data::WriteMarketDataBinaryHeader(create, Traits::kFeed)) {
        throw std::runtime_error(fmt::format(
            "failed to create typed market data file '{}'",
            output_path_.string()));
      }
      return;
    }

    std::error_code size_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(output_path_, size_error);
    if (size_error) {
      throw std::runtime_error(fmt::format(
          "failed to inspect output file '{}': {}", output_path_.string(),
          size_error.message()));
    }
    if (file_size == 0) {
      std::ofstream create(output_path_, std::ios::binary | std::ios::trunc);
      if (!create.is_open() ||
          !market_data::WriteMarketDataBinaryHeader(create, Traits::kFeed)) {
        throw std::runtime_error(fmt::format(
            "failed to initialize typed market data file '{}'",
            output_path_.string()));
      }
      return;
    }

    std::ifstream input(output_path_, std::ios::binary);
    if (!input.is_open()) {
      throw std::runtime_error(fmt::format("failed to open output file '{}'",
                                           output_path_.string()));
    }
    const market_data::MarketDataBinaryHeader header =
        market_data::ReadMarketDataBinaryHeader(input, output_path_);
    (void)market_data::CheckedMarketDataBinaryRecordCount(
        output_path_, file_size, header, Traits::kFeed);
  }
```

Keep `OnRecord()`, `Flush()`, and `stats()` unchanged except that they now write payload after the header.

- [ ] **Step 6: Run recorder tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_reader_recorder_test
ctest --test-dir build/debug -R data_reader_recorder_test --output-on-failure
```

Expected: single-file tests pass. Rotation tests may still fail until Task 3 updates rotation file-size expectations and manifest fields.

- [ ] **Step 7: Commit Task 2**

```bash
git add tools/market_data/data_reader_recorder.h test/tools/market_data/data_reader_recorder_test.cpp
git commit -m "feat: write typed headers in recorder files"
```

## Task 3: Write Typed Header And Metadata In Rotation Recorder Mode

**Files:**
- Modify: `tools/market_data/data_reader_recorder.h`
- Modify: `test/tools/market_data/data_reader_recorder_test.cpp`

- [ ] **Step 1: Add failing rotation manifest assertions**

Find the rotation tests in `test/tools/market_data/data_reader_recorder_test.cpp`. For each closed BookTicker and Trade segment, assert total size includes the header:

```cpp
EXPECT_EQ(std::filesystem::file_size(segment_path),
          sizeof(md::MarketDataBinaryHeader) + expected_records * sizeof(BookTicker));
```

For the manifest JSONL assertion, parse the first manifest line with toml++ or string matching as the existing test does. Add explicit checks:

```cpp
EXPECT_NE(line.find("\"format\":\"aquila.market_data.binary\""),
          std::string::npos);
EXPECT_NE(line.find("\"version\":1"), std::string::npos);
EXPECT_NE(line.find("\"feed\":\"book_ticker\""), std::string::npos);
EXPECT_NE(line.find("\"header_bytes\":16"), std::string::npos);
EXPECT_NE(line.find(fmt::format("\"record_size\":{}", sizeof(BookTicker))),
          std::string::npos);
EXPECT_NE(line.find(fmt::format("\"bytes\":{}",
                                sizeof(md::MarketDataBinaryHeader) +
                                    expected_records * sizeof(BookTicker))),
          std::string::npos);
```

Add the same checks for the trade manifest, replacing feed and `sizeof(Trade)`.

- [ ] **Step 2: Add a zero-record rotation assertion**

In the existing zero-record rotation test, assert that `.tmp` is removed and manifest remains empty:

```cpp
EXPECT_TRUE(FilesWithExtension(output_dir, ".bin").empty());
EXPECT_TRUE(FilesWithExtension(output_dir, ".tmp").empty());
EXPECT_TRUE(ReadLines(manifest_path).empty());
```

- [ ] **Step 3: Run the failing rotation tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_reader_recorder_test
ctest --test-dir build/debug -R data_reader_recorder_test --output-on-failure
```

Expected: rotation file-size and manifest metadata assertions fail.

- [ ] **Step 4: Write header when opening each rotation segment**

In `RotatingTypedBinaryRecorder<RecordT, Traits>::OpenSegment()`, after `output_.open(current_tmp_path_, std::ios::binary | std::ios::trunc);` succeeds, write the header:

```cpp
    if (!market_data::WriteMarketDataBinaryHeader(output_, Traits::kFeed)) {
      write_error_ = true;
      return false;
    }
```

- [ ] **Step 5: Validate rotation payload size and append metadata**

In `FinalizeCurrentSegment()`, keep the existing 0-record deletion behavior. Replace the size check:

```cpp
    const std::uintmax_t expected_size =
        sizeof(market_data::MarketDataBinaryHeader) +
        current_stats_.total_records * sizeof(RecordT);
    if (size_error || file_size != expected_size) {
      write_error_ = true;
      return false;
    }
```

Replace `AppendManifestLine()` formatting with:

```cpp
    manifest << fmt::format(
        "{{\"sequence\":{},\"file\":\"{}\",\"records\":{},"
        "\"bytes\":{},\"format\":\"{}\",\"version\":{},\"feed\":\"{}\","
        "\"header_bytes\":{},\"record_size\":{},"
        "\"first_exchange_ns\":{},\"last_exchange_ns\":{},"
        "\"first_local_ns\":{},\"last_local_ns\":{},"
        "\"closed_reason\":\"{}\"}}\n",
        current_sequence_, JsonEscape(current_final_path_.string()),
        current_stats_.total_records, file_size,
        market_data::kMarketDataBinaryFormatName,
        market_data::kMarketDataBinaryVersion, Traits::kFeedName,
        sizeof(market_data::MarketDataBinaryHeader), sizeof(RecordT),
        current_stats_.first_exchange_ns.value_or(0),
        current_stats_.last_exchange_ns.value_or(0),
        current_stats_.first_local_ns.value_or(0),
        current_stats_.last_local_ns.value_or(0), JsonEscape(reason));
```

- [ ] **Step 6: Run recorder tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_reader_recorder_test
ctest --test-dir build/debug -R data_reader_recorder_test --output-on-failure
```

Expected: all `data_reader_recorder_test` tests pass.

- [ ] **Step 7: Commit Task 3**

```bash
git add tools/market_data/data_reader_recorder.h test/tools/market_data/data_reader_recorder_test.cpp
git commit -m "feat: add typed rotation segment metadata"
```

## Task 4: Require Typed Headers In HistoricalDataReader And Binary TOML

**Files:**
- Modify: `core/market_data/historical_data_reader.h`
- Modify: `core/config/data_reader_config.cpp`
- Modify: `test/core/market_data/historical_data_reader_test.cpp`
- Modify: `test/config/data_reader_config_test.cpp`

- [ ] **Step 1: Rewrite historical reader test fixtures to write typed files**

In `test/core/market_data/historical_data_reader_test.cpp`, replace `WriteBookTickerFile()` and `WriteTradeFile()`:

```cpp
template <typename RecordT>
void WriteTypedFile(const std::filesystem::path& path,
                    cfg::DataReaderFeed feed,
                    const std::vector<RecordT>& records) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  ASSERT_TRUE(md::WriteMarketDataBinaryHeader(output, feed)) << path;
  if (!records.empty()) {
    output.write(reinterpret_cast<const char*>(records.data()),
                 static_cast<std::streamsize>(records.size() *
                                              sizeof(RecordT)));
  }
  ASSERT_TRUE(output.good()) << path;
}

void WriteBookTickerFile(const std::filesystem::path& path,
                         const std::vector<aquila::BookTicker>& records) {
  WriteTypedFile(path, cfg::DataReaderFeed::kBookTicker, records);
}

void WriteTradeFile(const std::filesystem::path& path,
                    const std::vector<aquila::Trade>& records) {
  WriteTypedFile(path, cfg::DataReaderFeed::kTrade, records);
}
```

Add raw fixture writers for negative tests:

```cpp
void WriteRawBookTickerFile(const std::filesystem::path& path,
                            const std::vector<aquila::BookTicker>& records) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  if (!records.empty()) {
    output.write(reinterpret_cast<const char*>(records.data()),
                 static_cast<std::streamsize>(records.size() *
                                              sizeof(aquila::BookTicker)));
  }
  ASSERT_TRUE(output.good()) << path;
}

void WriteCorruptHeaderFile(const std::filesystem::path& path,
                            md::MarketDataBinaryHeader header) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  output.write(reinterpret_cast<const char*>(&header), sizeof(header));
  ASSERT_TRUE(output.good()) << path;
}
```

Update trailing-byte helpers so they write a valid typed header before the payload and trailing byte.

- [ ] **Step 2: Add failing HistoricalDataReader header validation tests**

Add tests near the existing malformed-file tests:

```cpp
TEST(HistoricalDataReaderTest, HeaderOnlyBookTickerFileIsEmptyCompletedFile) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("empty_typed.bin");
  WriteBookTickerFile(file, {});

  md::HistoricalDataReader<md::HistoricalDataReaderDiagnostics> reader(
      MakeBinaryReaderConfig({file}));
  RecordingHandler handler;

  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_TRUE(reader.finished());
  EXPECT_EQ(reader.diagnostics().stats().files_completed, 1U);
  EXPECT_EQ(handler.book_tickers.size(), 0U);
}

TEST(HistoricalDataReaderTest, RejectsRawBookTickerFileWithoutHeader) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("raw.bin");
  WriteRawBookTickerFile(file, {MakeBookTicker(1, aquila::Exchange::kGate)});

  EXPECT_THROW((md::HistoricalDataReader<>{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}

TEST(HistoricalDataReaderTest, RejectsWrongMagic) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("wrong_magic.bin");
  md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kBookTicker);
  header.magic = 0;
  WriteCorruptHeaderFile(file, header);

  EXPECT_THROW((md::HistoricalDataReader<>{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}

TEST(HistoricalDataReaderTest, RejectsWrongVersion) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("wrong_version.bin");
  md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kBookTicker);
  header.version = 2;
  WriteCorruptHeaderFile(file, header);

  EXPECT_THROW((md::HistoricalDataReader<>{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}

TEST(HistoricalDataReaderTest, RejectsWrongFeedHeader) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("trade.bin");
  WriteTradeFile(file, {MakeTrade(1, aquila::Exchange::kGate)});

  EXPECT_THROW((md::HistoricalDataReader<>{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}

TEST(HistoricalDataReaderTest, RejectsWrongRecordSize) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("wrong_size.bin");
  md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kBookTicker);
  header.record_size = 63;
  WriteCorruptHeaderFile(file, header);

  EXPECT_THROW((md::HistoricalDataReader<>{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}

TEST(HistoricalDataReaderTest, RejectsUnknownFlags) {
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("wrong_flags.bin");
  md::MarketDataBinaryHeader header =
      md::MakeMarketDataBinaryHeader(cfg::DataReaderFeed::kBookTicker);
  header.flags = 1;
  WriteCorruptHeaderFile(file, header);

  EXPECT_THROW((md::HistoricalDataReader<>{MakeBinaryReaderConfig({file})}),
               std::runtime_error);
}
```

- [ ] **Step 3: Add failing config parser tests**

In `test/config/data_reader_config_test.cpp`, add:

```cpp
TEST(DataReaderConfigTest, RejectsBinaryFileWithoutExplicitFeed) {
  const auto parsed = toml::parse(R"toml(
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[data_reader]
name = "binary_reader"

[[data_reader.sources]]
name = "recorded"
type = "binary_file"
files = ["recorded.bin"]
)toml");

  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("data_reader.sources.feed"), std::string::npos);
}

TEST(DataReaderConfigTest, ShmSourceWithoutFeedDefaultsToBookTicker) {
  const auto parsed = toml::parse(R"toml(
[instrument_catalog]
file = "config/instruments/usdt_futures.csv"
schema = "aquila.instrument.v1"

[data_reader]
name = "live_reader"

[[data_reader.sources]]
name = "gate"
type = "shm"
exchange = "gate"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
)toml");

  const auto result = aquila::config::ParseDataReaderConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].feed,
            aquila::config::DataReaderFeed::kBookTicker);
}
```

- [ ] **Step 4: Run failing targets**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_historical_data_reader_test \
  data_reader_config_test
ctest --test-dir build/debug -R '(core_market_data_historical_data_reader|data_reader_config)' --output-on-failure
```

Expected: historical tests fail because the reader still treats the header as payload; config test fails because `binary_file` still defaults feed to `book_ticker`.

- [ ] **Step 5: Implement explicit binary feed validation**

In `core/config/data_reader_config.cpp`, update `ParseFeedField()`:

```cpp
  void ParseFeedField(const toml::table& source_table,
                      DataReaderSourceConfig* source) {
    if (source->type == DataReaderSourceType::kBinaryFile &&
        !source_table["feed"]) {
      Fail("data_reader.sources.feed", " is required for binary_file sources");
      return;
    }
    const std::string feed_text =
        StringOr(source_table["feed"], std::string{"book_ticker"});
    if (!ParseFeed(feed_text, &source->feed)) {
      Fail("data_reader.sources.feed", " must be book_ticker or trade");
    }
  }
```

- [ ] **Step 6: Implement typed-header historical open**

In `core/market_data/historical_data_reader.h`, add:

```cpp
#include "core/market_data/market_data_binary_format.h"
```

Change `FileState` so it stores payload offset:

```cpp
  struct FileState {
    std::filesystem::path path;
    std::uint64_t record_count{0};
    std::size_t payload_offset{0};
    MappedFile mapping;
  };
```

Replace the constructor file loop with:

```cpp
      for (const std::filesystem::path& file : source.files) {
        files_.push_back(OpenTypedFile(file, feed_));
      }
```

Add this helper:

```cpp
  [[nodiscard]] static FileState OpenTypedFile(
      const std::filesystem::path& file, config::DataReaderFeed feed) {
    if (file.empty()) {
      throw std::invalid_argument("binary data reader file path is empty");
    }

    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(file, error);
    if (error) {
      throw std::runtime_error(
          fmt::format("failed to inspect binary data file '{}': {}",
                      file.string(), error.message()));
    }
    if (file_size < sizeof(MarketDataBinaryHeader)) {
      throw std::runtime_error(fmt::format(
          "binary data file '{}' is missing market data header", file.string()));
    }

    MappedFile mapping(file, MappedFileAccessPattern::kSequential);
    if (mapping.size() != file_size) {
      throw std::runtime_error(fmt::format(
          "binary data file '{}' size changed during open", file.string()));
    }

    const MarketDataBinaryHeader header = ReadMarketDataBinaryHeaderFromData(
        mapping.data(), mapping.size(), file);
    const std::uint64_t record_count =
        CheckedMarketDataBinaryRecordCount(file, file_size, header, feed);
    return FileState{
        .path = file,
        .record_count = record_count,
        .payload_offset = header.header_size,
        .mapping = std::move(mapping),
    };
  }
```

Remove the old `CheckedRecordCount()` and `OpenMappedFile()` helpers once no code calls them.

Update `PrepareCurrentFile()`:

```cpp
    current_records_remaining_ = files_[current_file_index_].record_count;
    current_cursor_ = files_[current_file_index_].mapping.data() +
                      files_[current_file_index_].payload_offset;
```

Keep `DispatchBookTicker()` and `DispatchTrade()` unchanged so the hot path only copies payload records.

- [ ] **Step 7: Run C++ reader and config tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_historical_data_reader_test \
  data_reader_config_test
ctest --test-dir build/debug -R '(core_market_data_historical_data_reader|data_reader_config)' --output-on-failure
```

Expected: both test targets pass.

- [ ] **Step 8: Update data_reader_probe CLI test fixture**

In `test/tools/market_data/data_reader_probe_cli_test.cpp`, include the C++ format header:

```cpp
#include "core/market_data/market_data_binary_format.h"
```

Update `WriteTradeFile()`:

```cpp
void WriteTradeFile(const std::filesystem::path& path,
                    const std::vector<aquila::Trade>& trades) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  ASSERT_TRUE(aquila::market_data::WriteMarketDataBinaryHeader(
      output, aquila::config::DataReaderFeed::kTrade));
  for (const aquila::Trade& trade : trades) {
    output.write(reinterpret_cast<const char*>(&trade), sizeof(trade));
  }
  ASSERT_TRUE(output.good()) << path;
}
```

- [ ] **Step 9: Run probe CLI test**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_reader_probe_cli_test
ctest --test-dir build/debug -R data_reader_probe_cli_test --output-on-failure
```

Expected: `data_reader_probe_cli_test` passes and reports `handler_trades` for typed Trade input.

- [ ] **Step 10: Commit Task 4**

```bash
git add core/market_data/historical_data_reader.h \
  core/config/data_reader_config.cpp \
  test/core/market_data/historical_data_reader_test.cpp \
  test/config/data_reader_config_test.cpp \
  test/tools/market_data/data_reader_probe_cli_test.cpp
git commit -m "feat: require typed headers for historical data"
```

## Task 5: Add Python Typed Binary Helper And Migrate Numpy Readers

**Files:**
- Create: `scripts/market_data/typed_binary.py`
- Modify: `scripts/market_data/analyze_book_ticker_latency.py`
- Modify: `scripts/market_data/analyze_book_ticker_fusion_latency.py`
- Modify: `scripts/market_data/compare_fusion_tardis_book_ticker.py`
- Modify: `scripts/market_data/split_book_ticker_by_symbol.py`
- Modify: `scripts/lead_lag/generate_preflight_config_params.py`
- Modify tests under `scripts/test/market_data` and `scripts/test/lead_lag`.

- [ ] **Step 1: Add Python helper tests**

Create `scripts/test/market_data/typed_binary_test.py`:

```python
#!/usr/bin/env python3

import io
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

MODULE_DIR = Path(__file__).resolve().parents[2] / "market_data"
if str(MODULE_DIR) not in sys.path:
    sys.path.insert(0, str(MODULE_DIR))

import typed_binary  # noqa: E402


class TypedBinaryTest(unittest.TestCase):
    def test_book_ticker_dtype_matches_cpp_abi(self):
        dtype = typed_binary.book_ticker_dtype()
        self.assertEqual(dtype.itemsize, 64)
        self.assertEqual(dtype.fields["exchange_ns"][1], 16)
        self.assertEqual(dtype.fields["local_ns"][1], 24)
        self.assertEqual(dtype.fields["ask_volume"][1], 56)

    def test_trade_dtype_matches_cpp_abi(self):
        dtype = typed_binary.trade_dtype()
        self.assertEqual(dtype.itemsize, 64)
        self.assertEqual(dtype.fields["side"][1], 13)
        self.assertEqual(dtype.fields["exchange_ns"][1], 16)
        self.assertEqual(dtype.fields["trade_ns"][1], 24)
        self.assertEqual(dtype.fields["batch_count"][1], 60)

    def test_write_and_load_book_ticker_records(self):
        dtype = typed_binary.book_ticker_dtype()
        records = np.zeros(2, dtype=dtype)
        records["id"] = [10, 11]
        records["local_ns"] = [100, 200]
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(path, "book_ticker", records)
            header = typed_binary.read_header(path)
            loaded = typed_binary.load_records(path, "book_ticker")
        self.assertEqual(header.feed, "book_ticker")
        self.assertEqual(header.header_size, 16)
        self.assertEqual(header.record_size, 64)
        np.testing.assert_array_equal(loaded["id"], records["id"])
        np.testing.assert_array_equal(loaded["local_ns"], records["local_ns"])

    def test_memmap_records_skips_header(self):
        dtype = typed_binary.book_ticker_dtype()
        records = np.zeros(3, dtype=dtype)
        records["id"] = [1, 2, 3]
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(path, "book_ticker", records)
            mapped = typed_binary.memmap_records(path, "book_ticker")
            ids = np.asarray(mapped["id"]).copy()
        np.testing.assert_array_equal(ids, records["id"])

    def test_iter_record_chunks_rejects_trailing_bytes(self):
        dtype = typed_binary.book_ticker_dtype()
        records = np.zeros(1, dtype=dtype)
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "bad.bin"
            typed_binary.write_records(path, "book_ticker", records)
            with path.open("ab") as handle:
                handle.write(b"x")
            with self.assertRaisesRegex(ValueError, "payload size"):
                list(typed_binary.iter_record_chunks(path, "book_ticker", chunk_records=4))

    def test_stream_chunks_parse_header_first(self):
        dtype = typed_binary.book_ticker_dtype()
        records = np.zeros(2, dtype=dtype)
        records["id"] = [7, 8]
        stream = io.BytesIO()
        typed_binary.write_header(stream, "book_ticker")
        stream.write(records.tobytes())
        stream.seek(0)
        chunks = list(
            typed_binary.iter_record_chunks_from_stream(
                stream, "book_ticker", chunk_records=1, source_name="memory"
            )
        )
        self.assertEqual([int(chunk["id"][0]) for chunk in chunks], [7, 8])

    def test_rejects_wrong_feed(self):
        dtype = typed_binary.book_ticker_dtype()
        records = np.zeros(1, dtype=dtype)
        with tempfile.TemporaryDirectory() as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(path, "book_ticker", records)
            with self.assertRaisesRegex(ValueError, "feed mismatch"):
                typed_binary.load_records(path, "trade")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run the failing helper test**

```bash
python3 scripts/test/market_data/typed_binary_test.py
```

Expected: import fails because `typed_binary.py` does not exist.

- [ ] **Step 3: Implement `scripts/market_data/typed_binary.py`**

Create `scripts/market_data/typed_binary.py`:

```python
#!/usr/bin/env python3

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterator

import numpy as np


FORMAT_NAME = "aquila.market_data.binary"
MAGIC_BYTES = b"AQMD"
VERSION = 1
HEADER_SIZE = 16
FLAGS = 0
BOOK_TICKER_FEED_TYPE = 1
TRADE_FEED_TYPE = 2
_HEADER_STRUCT = struct.Struct("<IHHHHI")
_MAGIC_INT = int.from_bytes(MAGIC_BYTES, "little")


@dataclass(frozen=True)
class MarketDataBinaryHeader:
    magic: int
    version: int
    header_size: int
    feed_type: int
    record_size: int
    flags: int

    @property
    def feed(self) -> str:
        return feed_name_from_type(self.feed_type)


def book_ticker_dtype() -> np.dtype:
    return np.dtype(
        {
            "names": [
                "id",
                "symbol_id",
                "exchange",
                "exchange_ns",
                "local_ns",
                "bid_price",
                "bid_volume",
                "ask_price",
                "ask_volume",
            ],
            "formats": ["<i8", "<i4", "u1", "<i8", "<i8", "<f8", "<f8", "<f8", "<f8"],
            "offsets": [0, 8, 12, 16, 24, 32, 40, 48, 56],
            "itemsize": 64,
        }
    )


def trade_dtype() -> np.dtype:
    return np.dtype(
        {
            "names": [
                "id",
                "symbol_id",
                "exchange",
                "side",
                "reserved",
                "exchange_ns",
                "trade_ns",
                "local_ns",
                "price",
                "volume",
                "batch_index",
                "batch_count",
            ],
            "formats": [
                "<i8",
                "<i4",
                "u1",
                "u1",
                "<u2",
                "<i8",
                "<i8",
                "<i8",
                "<f8",
                "<f8",
                "<u4",
                "<u4",
            ],
            "offsets": [0, 8, 12, 13, 14, 16, 24, 32, 40, 48, 56, 60],
            "itemsize": 64,
        }
    )


def dtype_for_feed(feed: str) -> np.dtype:
    if feed == "book_ticker":
        return book_ticker_dtype()
    if feed == "trade":
        return trade_dtype()
    raise ValueError("feed must be book_ticker or trade")


def feed_type_for_name(feed: str) -> int:
    if feed == "book_ticker":
        return BOOK_TICKER_FEED_TYPE
    if feed == "trade":
        return TRADE_FEED_TYPE
    raise ValueError("feed must be book_ticker or trade")


def feed_name_from_type(feed_type: int) -> str:
    if feed_type == BOOK_TICKER_FEED_TYPE:
        return "book_ticker"
    if feed_type == TRADE_FEED_TYPE:
        return "trade"
    return "unknown"


def make_header(feed: str) -> MarketDataBinaryHeader:
    dtype = dtype_for_feed(feed)
    return MarketDataBinaryHeader(
        magic=_MAGIC_INT,
        version=VERSION,
        header_size=HEADER_SIZE,
        feed_type=feed_type_for_name(feed),
        record_size=dtype.itemsize,
        flags=FLAGS,
    )


def _decode_header(data: bytes, source_name: str) -> MarketDataBinaryHeader:
    if len(data) != HEADER_SIZE:
        raise ValueError(f"{source_name} is missing market data header")
    values = _HEADER_STRUCT.unpack(data)
    return MarketDataBinaryHeader(*values)


def validate_header(header: MarketDataBinaryHeader, feed: str, source_name: str) -> None:
    expected_dtype = dtype_for_feed(feed)
    expected_feed_type = feed_type_for_name(feed)
    if header.magic != _MAGIC_INT:
        raise ValueError(f"{source_name} has invalid market data magic 0x{header.magic:08x}")
    if header.version != VERSION:
        raise ValueError(f"{source_name} has unsupported market data version {header.version}")
    if header.header_size != HEADER_SIZE:
        raise ValueError(f"{source_name} has unsupported header size {header.header_size}")
    if header.flags != FLAGS:
        raise ValueError(f"{source_name} has unsupported flags 0x{header.flags:08x}")
    if header.feed_type != expected_feed_type:
        raise ValueError(
            f"{source_name} feed mismatch: header={feed_name_from_type(header.feed_type)}, expected={feed}"
        )
    if header.record_size != expected_dtype.itemsize:
        raise ValueError(
            f"{source_name} {feed} record size mismatch: header={header.record_size}, expected={expected_dtype.itemsize}"
        )


def read_header(path: Path) -> MarketDataBinaryHeader:
    with path.open("rb") as handle:
        return _decode_header(handle.read(HEADER_SIZE), str(path))


def write_header(handle: BinaryIO, feed: str) -> None:
    header = make_header(feed)
    handle.write(
        _HEADER_STRUCT.pack(
            header.magic,
            header.version,
            header.header_size,
            header.feed_type,
            header.record_size,
            header.flags,
        )
    )


def checked_record_count(path: Path, feed: str) -> tuple[MarketDataBinaryHeader, int]:
    file_size = path.stat().st_size
    if file_size < HEADER_SIZE:
        raise ValueError(f"{path} is missing market data header")
    header = read_header(path)
    validate_header(header, feed, str(path))
    payload_size = file_size - header.header_size
    if payload_size % header.record_size != 0:
        raise ValueError(
            f"{path} payload size {payload_size} is not a multiple of {feed} record size {header.record_size}"
        )
    return header, payload_size // header.record_size


def load_records(path: Path, feed: str) -> np.ndarray:
    header, record_count = checked_record_count(path, feed)
    dtype = dtype_for_feed(feed)
    if record_count == 0:
        return np.zeros(0, dtype=dtype)
    return np.fromfile(path, dtype=dtype, offset=header.header_size, count=record_count)


def memmap_records(path: Path, feed: str) -> np.memmap:
    header, record_count = checked_record_count(path, feed)
    dtype = dtype_for_feed(feed)
    return np.memmap(path, dtype=dtype, mode="r", offset=header.header_size, shape=(record_count,))


def iter_record_chunks(path: Path, feed: str, chunk_records: int) -> Iterator[np.ndarray]:
    if chunk_records <= 0:
        raise ValueError("chunk_records must be positive")
    header, record_count = checked_record_count(path, feed)
    dtype = dtype_for_feed(feed)
    if record_count == 0:
        return
    mapped = np.memmap(path, dtype=dtype, mode="r", offset=header.header_size, shape=(record_count,))
    for start in range(0, record_count, chunk_records):
        end = min(start + chunk_records, record_count)
        yield np.asarray(mapped[start:end])


def iter_record_chunks_from_stream(
    stream: BinaryIO, feed: str, chunk_records: int, source_name: str
) -> Iterator[np.ndarray]:
    if chunk_records <= 0:
        raise ValueError("chunk_records must be positive")
    dtype = dtype_for_feed(feed)
    header = _decode_header(stream.read(HEADER_SIZE), source_name)
    validate_header(header, feed, source_name)
    chunk_bytes = chunk_records * dtype.itemsize
    pending = b""
    while True:
        data = stream.read(chunk_bytes)
        if not data:
            break
        data = pending + data
        complete_size = (len(data) // dtype.itemsize) * dtype.itemsize
        if complete_size:
            yield np.frombuffer(data[:complete_size], dtype=dtype).copy()
        pending = data[complete_size:]
    if pending:
        raise ValueError(
            f"{source_name} payload size has {len(pending)} trailing bytes after {feed} records"
        )


def write_records(path: Path, feed: str, records: np.ndarray) -> None:
    dtype = dtype_for_feed(feed)
    if records.dtype != dtype:
        records = records.astype(dtype, copy=False)
    with path.open("wb") as handle:
        write_header(handle, feed)
        if len(records) > 0:
            handle.write(records.tobytes())
```

- [ ] **Step 4: Migrate `analyze_book_ticker_latency.py`**

Replace the local dtype body with a module import and a wrapper so existing callers can keep importing `book_ticker_dtype` from this script:

```python
import typed_binary  # noqa: E402
```

Use these functions:

```python
def book_ticker_dtype() -> np.dtype:
    return typed_binary.book_ticker_dtype()


def load_book_tickers(path: Path) -> np.ndarray:
    return typed_binary.load_records(path, "book_ticker")
```

Update `scripts/test/market_data/analyze_book_ticker_latency_test.py` so `test_load_book_tickers_reads_numpy_binary` writes typed data:

```python
            self.module.typed_binary.write_records(path, "book_ticker", records)
```

- [ ] **Step 5: Migrate fusion latency and compare scripts**

In `scripts/market_data/analyze_book_ticker_fusion_latency.py`, keep `fusion_metadata_dtype()` and `load_fusion_metadata()` unchanged. Ensure BookTicker inputs use `load_book_tickers()` from `analyze_book_ticker_latency.py`.

In `scripts/market_data/compare_fusion_tardis_book_ticker.py`, replace `_read_fusion_chunks()` implementation:

```python
def _read_fusion_chunks(
    path: Path,
    *,
    dtype: np.dtype,
    chunk_records: int,
) -> Iterable[np.ndarray]:
    del dtype
    yield from typed_binary.iter_record_chunks(
        path, "book_ticker", chunk_records=chunk_records
    )
```

Add:

```python
import typed_binary  # noqa: E402
```

Keep `book_ticker_dtype()` imports for callers that assert dtype layout.

Update these tests to write typed BookTicker fixtures with `typed_binary.write_records()`:

```text
scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py
```

- [ ] **Step 6: Migrate split-by-symbol input and output**

In `scripts/market_data/split_book_ticker_by_symbol.py`, replace `_read_chunks()` with:

```python
def _read_chunks(
    path: Path,
    *,
    dtype: np.dtype,
    chunk_records: int,
    allow_trailing_bytes: bool,
):
    del dtype
    if allow_trailing_bytes:
        raise ValueError("allow_trailing_bytes is not supported for typed BookTicker input")
    for records in typed_binary.iter_record_chunks(
        path, "book_ticker", chunk_records=chunk_records
    ):
        yield records, 0
```

When opening per-symbol outputs, write headers once:

```python
with output_path.open("wb") as handle:
    typed_binary.write_header(handle, "book_ticker")
    handles[symbol] = handle
```

Retain `selected.tofile(handles[symbol])` after the header has been written.

Update `scripts/test/market_data/split_book_ticker_by_symbol_test.py`:

- Use `typed_binary.write_records(path, "book_ticker", records)` for input fixtures.
- Load outputs with `typed_binary.load_records(output_path, "book_ticker")`.

- [ ] **Step 7: Migrate `.bin` and `.bin.zst` preflight chunks**

In `scripts/lead_lag/generate_preflight_config_params.py`, import helper:

```python
import typed_binary  # noqa: E402
```

Replace `_iter_raw_chunks()`:

```python
def _iter_raw_chunks(path: Path, *, dtype: np.dtype, chunk_records: int):
    del dtype
    yield from typed_binary.iter_record_chunks(
        path, "book_ticker", chunk_records=chunk_records
    )
```

Replace `_iter_zstd_chunks()` with a whole-typed-file stream parser:

```python
def _iter_zstd_chunks(path: Path, *, dtype: np.dtype, chunk_records: int):
    del dtype
    process = subprocess.Popen(
        ["zstd", "-dc", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None
    try:
        yield from typed_binary.iter_record_chunks_from_stream(
            process.stdout,
            "book_ticker",
            chunk_records=chunk_records,
            source_name=str(path),
        )
    finally:
        process.stdout.close()
    stderr = process.stderr.read().decode("utf-8", errors="replace")
    return_code = process.wait()
    if return_code != 0:
        raise ValueError(f"zstd failed for {path}: {stderr.strip()}")
```

Update `scripts/test/lead_lag/generate_preflight_config_params_test.py` fixtures:

- Use `typed_binary.write_records(binary_path, "book_ticker", records)` for `.bin`.
- For `.bin.zst`, write a typed `.bin` first and compress the entire file with `zstd -f`.

- [ ] **Step 8: Run migrated Python tests**

```bash
python3 scripts/test/market_data/typed_binary_test.py
python3 scripts/test/market_data/analyze_book_ticker_latency_test.py
python3 scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
python3 scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py
python3 scripts/test/market_data/split_book_ticker_by_symbol_test.py
python3 scripts/test/lead_lag/generate_preflight_config_params_test.py
```

Expected: all listed Python tests pass. If `zstd` is not installed, record the missing binary and run the non-zstd test subset before continuing.

- [ ] **Step 9: Commit Task 5**

```bash
git add scripts/market_data/typed_binary.py \
  scripts/market_data/analyze_book_ticker_latency.py \
  scripts/market_data/analyze_book_ticker_fusion_latency.py \
  scripts/market_data/compare_fusion_tardis_book_ticker.py \
  scripts/market_data/split_book_ticker_by_symbol.py \
  scripts/lead_lag/generate_preflight_config_params.py \
  scripts/test/market_data/typed_binary_test.py \
  scripts/test/market_data/analyze_book_ticker_latency_test.py \
  scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py \
  scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py \
  scripts/test/market_data/split_book_ticker_by_symbol_test.py \
  scripts/test/lead_lag/generate_preflight_config_params_test.py
git commit -m "feat: add typed binary python helpers"
```

## Task 6: Migrate Python Writers And Manifest Preflight

**Files:**
- Modify: `scripts/hdf_book_ticker_to_binary.py`
- Modify: `scripts/test/hdf_book_ticker_to_binary_test.py`
- Modify: `scripts/market_data/manifest_to_data_reader_config.py`
- Modify: `scripts/test/market_data/manifest_to_data_reader_config_test.py`

- [ ] **Step 1: Update HDF writer tests to expect typed output**

In `scripts/test/hdf_book_ticker_to_binary_test.py`, import `typed_binary` and load output with:

```python
loaded = typed_binary.load_records(output_path, "book_ticker")
```

Add an assertion that the file starts with magic:

```python
self.assertEqual(output_path.read_bytes()[:4], b"AQMD")
```

- [ ] **Step 2: Update HDF writer implementation**

In `scripts/hdf_book_ticker_to_binary.py`, use the same `sys.path` pattern as the market data scripts:

```python
import sys

SCRIPT_DIR = Path(__file__).resolve().parent
MARKET_DATA_SCRIPT_DIR = SCRIPT_DIR / "market_data"
if str(MARKET_DATA_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(MARKET_DATA_SCRIPT_DIR))
import typed_binary  # noqa: E402
```

Replace:

```python
            output_records.tofile(output)
```

with:

```python
            typed_binary.write_header(output, "book_ticker")
            if len(output_records) > 0:
                output.write(output_records.astype(typed_binary.book_ticker_dtype(), copy=False).tobytes())
```

- [ ] **Step 3: Add manifest metadata tests**

In `scripts/test/market_data/manifest_to_data_reader_config_test.py`, add a helper:

```python
def manifest_entry(path: Path, *, feed: str = "book_ticker", records: int = 2) -> dict:
    return {
        "sequence": 1,
        "file": str(path),
        "records": records,
        "bytes": 16 + records * 64,
        "format": "aquila.market_data.binary",
        "version": 1,
        "feed": feed,
        "header_bytes": 16,
        "record_size": 64,
    }
```

Update existing manifest fixtures to include metadata by replacing `json.dumps({"sequence": 1, "file": str(first)})` with `json.dumps(manifest_entry(first, feed="book_ticker", records=2))` and replacing trade fixtures with `json.dumps(manifest_entry(trade_file, feed="trade", records=2))`.

Add rejection tests:

```python
def test_rejects_manifest_feed_mismatch(self):
    with tempfile.TemporaryDirectory() as temp_dir:
        root = Path(temp_dir)
        data_file = root / "segments" / "trade_000001.bin"
        manifest = root / "manifest.jsonl"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        manifest.write_text(
            json.dumps(manifest_entry(data_file, feed="trade")) + "\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(ValueError, "feed"):
            manifest_config.render_data_reader_config(
                manifest_path=manifest,
                name="book_replay",
                catalog_path=Path("config/instruments/usdt_futures.csv"),
                feed="book_ticker",
            )

def test_rejects_manifest_bytes_mismatch(self):
    with tempfile.TemporaryDirectory() as temp_dir:
        root = Path(temp_dir)
        data_file = root / "segments" / "book_ticker_000001.bin"
        manifest = root / "manifest.jsonl"
        manifest.parent.mkdir(parents=True, exist_ok=True)
        entry = manifest_entry(data_file)
        entry["bytes"] = 65
        manifest.write_text(json.dumps(entry) + "\n", encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "bytes"):
            manifest_config.render_data_reader_config(
                manifest_path=manifest,
                name="bad_replay",
                catalog_path=Path("config/instruments/usdt_futures.csv"),
            )
```

- [ ] **Step 4: Implement manifest metadata validation**

In `scripts/market_data/manifest_to_data_reader_config.py`, import the helper:

```python
import typed_binary
```

Change `load_replayable_files()` signature:

```python
def load_replayable_files(manifest_path: Path, feed: str) -> list[Path]:
```

Add validation before appending `file_path`:

```python
            expected_record_size = typed_binary.dtype_for_feed(feed).itemsize
            if entry.get("format") != typed_binary.FORMAT_NAME:
                raise ValueError(f"{manifest_path}:{line_number}: format must be {typed_binary.FORMAT_NAME}")
            if entry.get("version") != typed_binary.VERSION:
                raise ValueError(f"{manifest_path}:{line_number}: version must be {typed_binary.VERSION}")
            if entry.get("feed") != feed:
                raise ValueError(f"{manifest_path}:{line_number}: feed must be {feed}")
            if entry.get("header_bytes") != typed_binary.HEADER_SIZE:
                raise ValueError(f"{manifest_path}:{line_number}: header_bytes must be {typed_binary.HEADER_SIZE}")
            if entry.get("record_size") != expected_record_size:
                raise ValueError(f"{manifest_path}:{line_number}: record_size must be {expected_record_size}")
            records = entry.get("records")
            bytes_value = entry.get("bytes")
            if not isinstance(records, int) or records < 0:
                raise ValueError(f"{manifest_path}:{line_number}: records must be a non-negative integer")
            expected_bytes = typed_binary.HEADER_SIZE + records * expected_record_size
            if bytes_value != expected_bytes:
                raise ValueError(f"{manifest_path}:{line_number}: bytes must be {expected_bytes}")
```

Update `render_data_reader_config()`:

```python
    files = load_replayable_files(manifest_path, feed)
```

- [ ] **Step 5: Run writer and manifest tests**

```bash
python3 scripts/test/hdf_book_ticker_to_binary_test.py
python3 scripts/test/market_data/manifest_to_data_reader_config_test.py
```

Expected: both tests pass and manifest fixtures without typed metadata no longer pass.

- [ ] **Step 6: Commit Task 6**

```bash
git add scripts/hdf_book_ticker_to_binary.py \
  scripts/test/hdf_book_ticker_to_binary_test.py \
  scripts/market_data/manifest_to_data_reader_config.py \
  scripts/test/market_data/manifest_to_data_reader_config_test.py
git commit -m "feat: validate typed binary python outputs"
```

## Task 7: Documentation And Onboarding Updates

**Files:**
- Modify: `docs/data_reader_config.md`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/lead_lag_live_replay_testing.md`
- Modify: `docs/lead_lag_cancelled_order_fillability_analysis.md`
- Modify: `docs/project_onboarding_guide.md`

- [ ] **Step 1: Replace raw binary language in `docs/data_reader_config.md`**

Update the recorder and historical reader sections so they state:

```md
`data_reader_recorder` 输出 typed binary format v1。每个 `.bin` 文件以 16-byte
`MarketDataBinaryHeader` 开头，随后是连续 payload records。`bytes` / 文件大小包含 header；
record 数量为 `(file_size - 16) / record_size`。0-byte 文件无效；header-only 文件表示 0 records。
```

Update the TOML field table so `data_reader.sources.feed` says:

```md
SHM source 默认 `book_ticker`；`binary_file` source 必须显式写 `feed`，并且文件 header 的 feed/type
必须与 TOML 一致。
```

Update recorder notes so they no longer say “裸 binary” or “不写 header”.

- [ ] **Step 2: Update diagnostic fields**

In `docs/diagnostic_fields.md`, update the recorder rows so they reference typed binary:

```md
| `book_ticker_output` / `trade_output` | `data_reader_recorder` result summary Nova log | stable | path | 本次 recorder 实际写出的 BookTicker / Trade typed binary 单文件路径；rotation 模式下作为启动参数和默认派生路径记录，真实 segment 见 manifest。 | recorder output schema 替换后同步迁移。 |
```

Add a stable manifest row near recorder fields:

```md
| `format` / `version` / `feed` / `header_bytes` / `record_size` | recorder rotation manifest JSONL | stable | typed binary metadata | 每个已关闭 segment 的 typed-header metadata；`bytes` 包含 16-byte header，`records` 是 payload record 数。脚本可用这些字段做 manifest preflight，但二进制文件 header 仍是事实源。 | typed binary format v1 被替换后同步迁移。 |
```

- [ ] **Step 3: Update replay and fillability docs**

In `docs/lead_lag_live_replay_testing.md`, replace size checks:

```md
- `recorded_book_ticker.bin` / `recorded_trade.bin` 必须是 typed binary v1：文件大小至少 16 bytes，
  header feed/type 与 replay TOML `feed` 一致，且 `(file_size - 16) % record_size == 0`。
```

In `docs/lead_lag_cancelled_order_fillability_analysis.md`, update split instructions:

```md
拆分脚本输入和输出都使用 typed BookTicker binary v1；拆出的每个 `symbol.bin` 也包含 16-byte header。
```

- [ ] **Step 4: Update onboarding**

In `docs/project_onboarding_guide.md`, replace the recorder status bullet so it says:

```md
`data_reader_recorder` 已将 BookTicker / Trade 单文件和 rotation segment 切到 typed binary format v1；
`HistoricalDataReader` / `data_reader_probe` / Python numpy 脚本不再接受旧裸结构体流。`binary_file`
source 必须显式写 `feed`，且 header feed/type 必须与 TOML 一致。旧 raw artifact 需要重录。
```

- [ ] **Step 5: Check docs diff**

```bash
git diff -- docs/data_reader_config.md docs/diagnostic_fields.md docs/lead_lag_live_replay_testing.md docs/lead_lag_cancelled_order_fillability_analysis.md docs/project_onboarding_guide.md
git diff --check
```

Expected: docs describe typed-header v1 consistently and `git diff --check` reports no whitespace errors.

- [ ] **Step 6: Commit Task 7**

```bash
git add docs/data_reader_config.md docs/diagnostic_fields.md \
  docs/lead_lag_live_replay_testing.md \
  docs/lead_lag_cancelled_order_fillability_analysis.md \
  docs/project_onboarding_guide.md
git commit -m "docs: document market data typed binary format"
```

## Task 8: Final Verification And Regression Sweep

**Files:**
- Read-only verification plus any minimal fixes discovered by the commands below.

- [ ] **Step 1: Search for raw binary readers and writers**

Run:

```bash
rg -n 'np\\.fromfile|np\\.frombuffer|np\\.memmap|\\.tofile\\(|file_size % dtype\\.itemsize|size .*multiple of BookTicker|裸 binary|不写 header|raw BookTicker|raw replay binary' scripts docs test core tools
```

Expected: remaining matches are one of:

- fusion metadata raw sidecar in `analyze_book_ticker_fusion_latency.py`;
- `typed_binary.py` internals;
- tests that intentionally create raw corrupt fixtures;
- docs explaining old raw artifacts are invalid.

- [ ] **Step 2: Run focused C++ build and tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  data_reader_recorder_test \
  core_market_data_historical_data_reader_test \
  data_reader_config_test \
  data_reader_probe_cli_test

ctest --test-dir build/debug -R \
  '(data_reader_recorder|core_market_data_historical_data_reader|data_reader_config|data_reader_probe_cli)' \
  --output-on-failure
```

Expected: all four C++ test targets pass.

- [ ] **Step 3: Run focused Python tests**

```bash
python3 scripts/test/market_data/typed_binary_test.py
python3 scripts/test/market_data/analyze_book_ticker_latency_test.py
python3 scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
python3 scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py
python3 scripts/test/market_data/split_book_ticker_by_symbol_test.py
python3 scripts/test/market_data/manifest_to_data_reader_config_test.py
python3 scripts/test/lead_lag/generate_preflight_config_params_test.py
python3 scripts/test/hdf_book_ticker_to_binary_test.py
```

Expected: all listed Python tests pass. If `zstd` is absent, report the skipped compressed-file coverage explicitly and keep `.bin` coverage passing.

- [ ] **Step 4: Run boundary checks**

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
git diff --check
git status --short --branch
```

Expected: evaluation boundary checks produce no matches, `git diff --check` produces no output, and status shows only intentional changes if a fix commit remains.

- [ ] **Step 5: Commit verification fixes**

If Step 1-4 required fixes, commit only files touched by those fixes. Use the exact relevant subset from this command; do not add unrelated files:

```bash
git add core/market_data/market_data_binary_format.h \
  core/market_data/historical_data_reader.h \
  core/config/data_reader_config.cpp \
  tools/market_data/data_reader_recorder.h \
  scripts/market_data/typed_binary.py \
  scripts/market_data/analyze_book_ticker_latency.py \
  scripts/market_data/analyze_book_ticker_fusion_latency.py \
  scripts/market_data/compare_fusion_tardis_book_ticker.py \
  scripts/market_data/split_book_ticker_by_symbol.py \
  scripts/market_data/manifest_to_data_reader_config.py \
  scripts/hdf_book_ticker_to_binary.py \
  scripts/lead_lag/generate_preflight_config_params.py \
  test/core/market_data/historical_data_reader_test.cpp \
  test/config/data_reader_config_test.cpp \
  test/tools/market_data/data_reader_recorder_test.cpp \
  test/tools/market_data/data_reader_probe_cli_test.cpp \
  scripts/test/market_data/typed_binary_test.py \
  scripts/test/market_data/analyze_book_ticker_latency_test.py \
  scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py \
  scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py \
  scripts/test/market_data/split_book_ticker_by_symbol_test.py \
  scripts/test/market_data/manifest_to_data_reader_config_test.py \
  scripts/test/lead_lag/generate_preflight_config_params_test.py \
  scripts/test/hdf_book_ticker_to_binary_test.py \
  docs/data_reader_config.md \
  docs/diagnostic_fields.md \
  docs/lead_lag_live_replay_testing.md \
  docs/lead_lag_cancelled_order_fillability_analysis.md \
  docs/project_onboarding_guide.md
git commit -m "fix: complete typed binary migration checks"
```

If no fixes were needed, do not create an empty commit.

## Completion Criteria

- Recorder single-file truncate writes exactly one typed header before payload.
- Recorder single-file append initializes missing/empty files and rejects raw, wrong feed, wrong version, wrong flags, wrong record-size, and trailing-byte targets.
- Recorder rotation segments write header to `.tmp`, delete 0-record segments, finalize with total bytes including header, and append typed metadata to manifest.
- `HistoricalDataReader` accepts typed BookTicker and Trade files, accepts header-only files as empty completed files, rejects 0-byte/raw/corrupt/mismatched files at construction, and keeps header parsing out of `Poll()` / `Drain()`.
- `binary_file` TOML sources require explicit `feed`; SHM source default remains `book_ticker`.
- Python scripts and tests read/write typed BookTicker files through `typed_binary.py`; `.bin.zst` is treated as a compressed whole typed binary file.
- Manifest script refuses JSONL entries missing or contradicting typed-header metadata.
- Docs no longer describe recorder outputs as raw structure streams.
