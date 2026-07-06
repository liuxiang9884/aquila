# Trade Fastest-Route Fusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build Trade fastest-route fusion for Gate / Binance while preserving existing BookTicker fusion behavior and external compatibility.

**Architecture:** Add a shared traits-based fastest-route fusion core / runner / thread layer, then keep feed-specific facades for `BookTickerFusion*` and add parallel `TradeFusion*` facades. Trade fusion uses `(symbol_id, Trade.id)` first arrival, writes canonical `Trade` SHM with `local_ns = fusion_publish_ns`, and writes a Trade-specific raw sidecar metadata record.

**Tech Stack:** C++20, CMake, GTest, toml++, CLI11, nova SHM / logging, existing `DataShmPublisher`, `BookTickerShmReader`, and `TradeShmReader`.

---

## File Structure

Create:

- `core/market_data/fastest_route_fusion.h`: generic per-symbol first-arrival core.
- `core/market_data/fusion_metadata_writer.h`: typed binary sidecar writer template.
- `core/market_data/fastest_route_fusion_runner.h`: generic SHM polling runner.
- `core/market_data/fastest_route_fusion_thread.h`: generic runner thread wrapper.
- `core/market_data/trade_fusion.h`: Trade-specific decision and core facade.
- `core/market_data/trade_fusion_config.h`: Trade fusion config structs.
- `core/market_data/trade_fusion_metadata.h`: Trade sidecar metadata ABI.
- `core/market_data/trade_fusion_metadata_policy.h`: Trade metadata policy using the existing fusion metadata compile mode.
- `core/market_data/trade_fusion_runner.h`: Trade runner traits and alias.
- `core/market_data/trade_fusion_thread.h`: Trade thread alias.
- `core/config/trade_fusion_config.h`: Trade TOML parser API.
- `core/config/trade_fusion_config.cpp`: Trade TOML parser implementation.
- `tools/market_data/trade_fusion_cli.h`: shared Trade fusion CLI API.
- `tools/market_data/trade_fusion_cli.cpp`: Trade fusion CLI implementation.
- `tools/market_data/trade_fusion.cpp`: Gate default Trade fusion executable entry.
- `tools/market_data/binance_trade_fusion.cpp`: Binance default Trade fusion executable entry.
- `test/core/market_data/fastest_route_fusion_test.cpp`: generic core tests.
- `test/core/market_data/trade_fusion_test.cpp`: Trade core facade tests.
- `test/core/market_data/trade_fusion_metadata_test.cpp`: Trade metadata writer tests.
- `test/core/market_data/trade_fusion_runner_test.cpp`: Trade runner SHM tests.
- `test/core/market_data/trade_fusion_thread_test.cpp`: Trade thread tests.
- `test/config/trade_fusion_config_test.cpp`: Trade TOML parser tests.
- `test/tools/market_data/trade_fusion_cli_test.cpp`: Trade fusion CLI startup tests.
- `config/market_data_fusion/gate_trade_fusion_4sources.toml`: Gate Trade fusion example.
- `config/market_data_fusion/binance_trade_fusion_4sources.toml`: Binance Trade fusion example.
- `config/market_data_fusion/gate_data_fusion_trade_4sources.toml`: Gate bundled Trade data fusion example.
- `config/market_data_fusion/binance_data_fusion_trade_4sources.toml`: Binance bundled Trade data fusion example.

Modify:

- `core/market_data/CMakeLists.txt`: add new header files to `aquila_core`.
- `core/market_data/book_ticker_fusion.h`: keep public API, delegate to the generic core.
- `core/market_data/book_ticker_fusion_metadata.h`: keep existing `FusionMetadataRecord` ABI, use generic writer.
- `core/market_data/book_ticker_fusion_runner.h`: keep public API, delegate to generic runner.
- `core/market_data/book_ticker_fusion_thread.h`: keep public API, delegate to generic thread.
- `core/config/CMakeLists.txt`: add Trade config parser files.
- `test/core/market_data/CMakeLists.txt`: add generic and Trade fusion tests.
- `test/config/CMakeLists.txt`: add Trade config parser test.
- `tools/CMakeLists.txt`: add `gate_trade_fusion` and `binance_trade_fusion`; existing data fusion targets keep linking `aquila_config`, `aquila_core`, and exchange libraries.
- `tools/market_data/data_fusion_tool_support.h`: add Trade alignment, source override, dry-run, and summary helpers.
- `tools/gate/gate_data_fusion_config.h`: add launch `feed` and Trade SHM source fields while defaulting existing configs to BookTicker.
- `tools/gate/gate_data_fusion_config.cpp`: parse `feed = "book_ticker" | "trade"` and feed-specific SHM fields.
- `tools/gate/gate_data_fusion.cpp`: branch between BookTicker and Trade bundle execution.
- `tools/binance/binance_data_fusion_config.h`: add launch `feed` and Trade SHM source fields while defaulting existing configs to BookTicker.
- `tools/binance/binance_data_fusion_config.cpp`: parse `feed = "book_ticker" | "trade"` and feed-specific SHM fields.
- `tools/binance/binance_data_fusion.cpp`: branch between BookTicker and Trade bundle execution.
- `test/tools/gate/gate_data_fusion_config_test.cpp`: add Trade launch parser tests.
- `test/tools/binance/binance_data_fusion_config_test.cpp`: add Trade launch parser tests.
- `test/tools/market_data/CMakeLists.txt`: add Trade CLI test.
- `test/tools/market_data/data_fusion_tool_support_test.cpp`: add Trade helper tests.
- `docs/diagnostic_fields.md`: document Trade fusion diagnostics and metadata fields.
- `docs/project_onboarding_guide.md`: add Trade fusion entry points after implementation is verified.
- `docs/trade_fastest_route_fusion_design.md`: update only if implementation changes a documented boundary.

---

### Task 1: Generic Fastest-Route Core

**Files:**
- Create: `core/market_data/fastest_route_fusion.h`
- Create: `test/core/market_data/fastest_route_fusion_test.cpp`
- Modify: `core/market_data/book_ticker_fusion.h`
- Modify: `test/core/market_data/CMakeLists.txt`
- Test: `test/core/market_data/fastest_route_fusion_test.cpp`
- Test: `test/core/market_data/book_ticker_fusion_test.cpp`

- [ ] **Step 1: Write the failing generic core test**

Add this test target to `test/core/market_data/CMakeLists.txt`:

```cmake
add_executable(core_market_data_fastest_route_fusion_test
    fastest_route_fusion_test.cpp
)

target_link_libraries(core_market_data_fastest_route_fusion_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_market_data_fastest_route_fusion_test
         COMMAND core_market_data_fastest_route_fusion_test)
```

Create `test/core/market_data/fastest_route_fusion_test.cpp`:

```cpp
#include "core/market_data/fastest_route_fusion.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

struct TestRecord {
  std::int32_t symbol_id{0};
  std::int64_t id{0};
  std::int64_t local_ns{0};
};

struct TestTraits {
  using Record = TestRecord;

  [[nodiscard]] static std::int32_t SymbolId(
      const TestRecord& record) noexcept {
    return record.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(
      const TestRecord& record) noexcept {
    return record.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(
      const TestRecord& record) noexcept {
    return record.local_ns;
  }
};

using TestFusion = aquila::market_data::BasicFastestRouteFusionCore<TestTraits>;

TEST(FastestRouteFusionCoreTest, PublishesOnlyIncreasingIdsPerSymbol) {
  TestFusion fusion(/*max_symbol_id=*/16);

  const TestRecord first{.symbol_id = 3, .id = 100, .local_ns = 1'000};
  const auto first_decision =
      fusion.OnRecord(/*source_id=*/0, first, /*fusion_publish_ns=*/2'000);
  ASSERT_TRUE(first_decision.publish);
  EXPECT_EQ(first_decision.source_id, 0);
  EXPECT_EQ(first_decision.symbol_id, 3);
  EXPECT_EQ(first_decision.record_id, 100);
  EXPECT_EQ(first_decision.source_local_ns, 1'000);
  EXPECT_EQ(first_decision.fusion_publish_ns, 2'000);

  const TestRecord duplicate{.symbol_id = 3, .id = 100, .local_ns = 1'100};
  EXPECT_FALSE(
      fusion.OnRecord(/*source_id=*/1, duplicate, /*fusion_publish_ns=*/2'100)
          .publish);

  const TestRecord older{.symbol_id = 3, .id = 99, .local_ns = 1'200};
  EXPECT_FALSE(
      fusion.OnRecord(/*source_id=*/2, older, /*fusion_publish_ns=*/2'200)
          .publish);

  const TestRecord next{.symbol_id = 3, .id = 101, .local_ns = 1'300};
  const auto next_decision =
      fusion.OnRecord(/*source_id=*/3, next, /*fusion_publish_ns=*/2'300);
  ASSERT_TRUE(next_decision.publish);
  EXPECT_EQ(next_decision.source_id, 3);
  EXPECT_EQ(next_decision.record_id, 101);
}

TEST(FastestRouteFusionCoreTest, MaintainsIndependentSymbolState) {
  TestFusion fusion(/*max_symbol_id=*/16);

  EXPECT_TRUE(fusion
                  .OnRecord(/*source_id=*/0,
                            TestRecord{.symbol_id = 1,
                                       .id = 10,
                                       .local_ns = 1'000},
                            /*fusion_publish_ns=*/2'000)
                  .publish);
  EXPECT_TRUE(fusion
                  .OnRecord(/*source_id=*/1,
                            TestRecord{.symbol_id = 2,
                                       .id = 5,
                                       .local_ns = 1'100},
                            /*fusion_publish_ns=*/2'100)
                  .publish);
  EXPECT_FALSE(fusion
                   .OnRecord(/*source_id=*/2,
                             TestRecord{.symbol_id = 1,
                                        .id = 9,
                                        .local_ns = 1'200},
                             /*fusion_publish_ns=*/2'200)
                   .publish);
}

TEST(FastestRouteFusionCoreTest, DropsOutOfRangeSymbolsWithUnsetMetadata) {
  TestFusion fusion(/*max_symbol_id=*/4);

  const auto negative = fusion.OnRecord(
      /*source_id=*/0, TestRecord{.symbol_id = -1, .id = 1, .local_ns = 1'000},
      /*fusion_publish_ns=*/2'000);
  EXPECT_FALSE(negative.publish);
  EXPECT_EQ(negative.source_id, -1);
  EXPECT_EQ(negative.symbol_id, -1);
  EXPECT_EQ(negative.record_id, 0);
  EXPECT_EQ(negative.source_local_ns, 0);
  EXPECT_EQ(negative.fusion_publish_ns, 0);

  const auto too_large = fusion.OnRecord(
      /*source_id=*/1, TestRecord{.symbol_id = 4, .id = 1, .local_ns = 1'100},
      /*fusion_publish_ns=*/2'100);
  EXPECT_FALSE(too_large.publish);
  EXPECT_EQ(too_large.source_id, -1);
  EXPECT_EQ(too_large.symbol_id, -1);
  EXPECT_EQ(too_large.record_id, 0);
}

}  // namespace
```

- [ ] **Step 2: Run the generic core test and verify it fails**

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'core_market_data_fastest_route_fusion_test|core_market_data_book_ticker_fusion_test' --output-on-failure
```

Expected: build fails because `core/market_data/fastest_route_fusion.h` does not exist.

- [ ] **Step 3: Implement the generic core**

Create `core/market_data/fastest_route_fusion.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_H_
#define AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_H_

#include <cstdint>
#include <vector>

namespace aquila::market_data {

struct FastestRouteFusionDecision {
  bool publish{false};
  std::int32_t source_id{-1};
  std::int32_t symbol_id{-1};
  std::int64_t record_id{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

template <typename Traits>
class BasicFastestRouteFusionCore {
 public:
  using Record = typename Traits::Record;

  explicit BasicFastestRouteFusionCore(std::uint32_t max_symbol_id)
      : states_(max_symbol_id) {}

  [[nodiscard]] FastestRouteFusionDecision OnRecord(
      std::int32_t source_id, const Record& record,
      std::int64_t fusion_publish_ns) noexcept {
    const std::int32_t symbol_id = Traits::SymbolId(record);
    if (symbol_id < 0 ||
        static_cast<std::uint32_t>(symbol_id) >= states_.size()) {
      return {};
    }

    SymbolState& state = states_[static_cast<std::uint32_t>(symbol_id)];
    const std::int64_t record_id = Traits::RecordId(record);
    if (record_id <= state.last_published_id) {
      return {};
    }

    state.last_published_id = record_id;
    state.last_published_source = source_id;
    return FastestRouteFusionDecision{
        .publish = true,
        .source_id = source_id,
        .symbol_id = symbol_id,
        .record_id = record_id,
        .source_local_ns = Traits::LocalNs(record),
        .fusion_publish_ns = fusion_publish_ns,
    };
  }

 private:
  struct SymbolState {
    std::int64_t last_published_id{0};
    std::int32_t last_published_source{-1};
  };

  std::vector<SymbolState> states_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_H_
```

- [ ] **Step 4: Refactor BookTicker core facade without changing public fields**

Modify `core/market_data/book_ticker_fusion.h` so `BookTickerFusionCore` delegates to the generic core and `BookTickerFusionDecision` still exposes `book_ticker_id`:

```cpp
#include "core/market_data/fastest_route_fusion.h"
```

Use these traits and wrapper methods:

```cpp
struct BookTickerFusionTraits {
  using Record = BookTicker;

  [[nodiscard]] static std::int32_t SymbolId(const BookTicker& ticker) noexcept {
    return ticker.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(const BookTicker& ticker) noexcept {
    return ticker.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(const BookTicker& ticker) noexcept {
    return ticker.local_ns;
  }
};

class BookTickerFusionCore {
 public:
  explicit BookTickerFusionCore(std::uint32_t max_symbol_id)
      : core_(max_symbol_id) {}

  [[nodiscard]] BookTickerFusionDecision OnBookTicker(
      std::int32_t source_id, const BookTicker& ticker,
      std::int64_t fusion_publish_ns) noexcept {
    const FastestRouteFusionDecision decision =
        core_.OnRecord(source_id, ticker, fusion_publish_ns);
    if (!decision.publish) {
      return {};
    }
    return BookTickerFusionDecision{
        .publish = true,
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .book_ticker_id = decision.record_id,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
  }

 private:
  BasicFastestRouteFusionCore<BookTickerFusionTraits> core_;
};
```

- [ ] **Step 5: Run core tests**

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'core_market_data_fastest_route_fusion_test|core_market_data_book_ticker_fusion_test' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 6: Commit**

```bash
git add core/market_data/fastest_route_fusion.h \
  core/market_data/book_ticker_fusion.h \
  test/core/market_data/fastest_route_fusion_test.cpp \
  test/core/market_data/CMakeLists.txt
git commit -m "refactor: add generic fastest route fusion core"
```

---

### Task 2: Generic Metadata Writer And Trade Metadata ABI

**Files:**
- Create: `core/market_data/fusion_metadata_writer.h`
- Create: `core/market_data/trade_fusion_metadata.h`
- Create: `test/core/market_data/trade_fusion_metadata_test.cpp`
- Modify: `core/market_data/book_ticker_fusion_metadata.h`
- Modify: `core/market_data/CMakeLists.txt`
- Modify: `test/core/market_data/CMakeLists.txt`
- Test: `test/core/market_data/book_ticker_fusion_metadata_test.cpp`
- Test: `test/core/market_data/trade_fusion_metadata_test.cpp`

- [ ] **Step 1: Write the failing Trade metadata test**

Add to `test/core/market_data/CMakeLists.txt`:

```cmake
add_executable(core_market_data_trade_fusion_metadata_test
    trade_fusion_metadata_test.cpp
)

target_link_libraries(core_market_data_trade_fusion_metadata_test
    PRIVATE
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
)

add_test(NAME core_market_data_trade_fusion_metadata_test
         COMMAND core_market_data_trade_fusion_metadata_test)
```

Create `test/core/market_data/trade_fusion_metadata_test.cpp`:

```cpp
#include "core/market_data/trade_fusion_metadata.h"

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace {

std::filesystem::path UniquePath() {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("aquila_trade_fusion_metadata_test_{}.bin", ::getpid());
}

std::vector<aquila::market_data::TradeFusionMetadataRecord> ReadRecords(
    const std::filesystem::path& path) {
  const std::uintmax_t size = std::filesystem::file_size(path);
  EXPECT_EQ(size % sizeof(aquila::market_data::TradeFusionMetadataRecord), 0U);
  std::vector<aquila::market_data::TradeFusionMetadataRecord> records(
      size / sizeof(aquila::market_data::TradeFusionMetadataRecord));
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(
                   records.size() *
                   sizeof(aquila::market_data::TradeFusionMetadataRecord)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

TEST(TradeFusionMetadataTest, WritesRawRecords) {
  const std::filesystem::path path = UniquePath();
  std::filesystem::remove(path);

  aquila::market_data::TradeFusionMetadataWriter writer(path);
  const aquila::market_data::TradeFusionMetadataRecord first{
      .source_id = 0,
      .symbol_id = 42,
      .trade_id = 100,
      .exchange_ns = 1'000,
      .trade_ns = 1'010,
      .source_local_ns = 2'000,
      .fusion_publish_ns = 3'000,
  };
  const aquila::market_data::TradeFusionMetadataRecord second{
      .source_id = 1,
      .symbol_id = 42,
      .trade_id = 101,
      .exchange_ns = 1'100,
      .trade_ns = 1'110,
      .source_local_ns = 2'100,
      .fusion_publish_ns = 3'100,
  };

  ASSERT_TRUE(writer.Write(first));
  ASSERT_TRUE(writer.Write(second));
  ASSERT_TRUE(writer.Flush());

  const std::vector<aquila::market_data::TradeFusionMetadataRecord> records =
      ReadRecords(path);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].source_id, first.source_id);
  EXPECT_EQ(records[0].trade_id, first.trade_id);
  EXPECT_EQ(records[0].trade_ns, first.trade_ns);
  EXPECT_EQ(records[1].source_id, second.source_id);
  EXPECT_EQ(records[1].fusion_publish_ns, second.fusion_publish_ns);

  std::filesystem::remove(path);
}

}  // namespace
```

- [ ] **Step 2: Run metadata tests and verify failure**

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'core_market_data_(book_ticker|trade)_fusion_metadata_test' --output-on-failure
```

Expected: build fails because `trade_fusion_metadata.h` does not exist.

- [ ] **Step 3: Add the generic metadata writer**

Create `core/market_data/fusion_metadata_writer.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_WRITER_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_WRITER_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <utility>

namespace aquila::market_data {

template <typename Record>
class BasicFusionMetadataWriter {
 public:
  explicit BasicFusionMetadataWriter(std::filesystem::path output_path)
      : output_path_(std::move(output_path)) {
    if (output_path_.empty()) {
      throw std::invalid_argument("metadata output path must not be empty");
    }
    if (output_path_.has_parent_path()) {
      std::filesystem::create_directories(output_path_.parent_path());
    }
    output_.open(output_path_, std::ios::binary | std::ios::trunc);
    if (!output_.is_open()) {
      throw std::runtime_error("failed to open metadata output");
    }
  }

  BasicFusionMetadataWriter(const BasicFusionMetadataWriter&) = delete;
  BasicFusionMetadataWriter& operator=(const BasicFusionMetadataWriter&) =
      delete;

  [[nodiscard]] bool Write(const Record& record) noexcept {
    if (write_error_) {
      return false;
    }
    output_.write(reinterpret_cast<const char*>(&record), sizeof(record));
    if (!output_.good()) {
      write_error_ = true;
      return false;
    }
    ++records_written_;
    return true;
  }

  [[nodiscard]] bool Flush() noexcept {
    if (write_error_) {
      return false;
    }
    output_.flush();
    if (!output_.good()) {
      write_error_ = true;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool write_error() const noexcept { return write_error_; }

  [[nodiscard]] std::uint64_t records_written() const noexcept {
    return records_written_;
  }

  [[nodiscard]] const std::filesystem::path& output_path() const noexcept {
    return output_path_;
  }

 private:
  std::filesystem::path output_path_;
  std::ofstream output_;
  std::uint64_t records_written_{0};
  bool write_error_{false};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_WRITER_H_
```

- [ ] **Step 4: Keep BookTicker metadata ABI and alias its writer**

Modify `core/market_data/book_ticker_fusion_metadata.h`:

```cpp
#include "core/market_data/fusion_metadata_writer.h"
```

Remove the existing `FusionMetadataWriter` class body and replace it with:

```cpp
using FusionMetadataWriter = BasicFusionMetadataWriter<FusionMetadataRecord>;
```

Keep `FusionMetadataRecord` field order unchanged:

```cpp
struct FusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t book_ticker_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};
```

- [ ] **Step 5: Add Trade metadata ABI**

Create `core/market_data/trade_fusion_metadata.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_H_

#include <cstdint>
#include <type_traits>

#include "core/market_data/fusion_metadata_writer.h"

namespace aquila::market_data {

struct TradeFusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t trade_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t trade_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

static_assert(std::is_standard_layout_v<TradeFusionMetadataRecord>);
static_assert(std::is_trivially_copyable_v<TradeFusionMetadataRecord>);

using TradeFusionMetadataWriter =
    BasicFusionMetadataWriter<TradeFusionMetadataRecord>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_H_
```

Add `fusion_metadata_writer.h` and `trade_fusion_metadata.h` to `core/market_data/CMakeLists.txt`.

- [ ] **Step 6: Run metadata tests**

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'core_market_data_(book_ticker|trade)_fusion_metadata_test' --output-on-failure
```

Expected: both tests pass, and existing BookTicker metadata test still reads the same raw ABI.

- [ ] **Step 7: Commit**

```bash
git add core/market_data/fusion_metadata_writer.h \
  core/market_data/book_ticker_fusion_metadata.h \
  core/market_data/trade_fusion_metadata.h \
  core/market_data/CMakeLists.txt \
  test/core/market_data/trade_fusion_metadata_test.cpp \
  test/core/market_data/CMakeLists.txt
git commit -m "feat: add trade fusion metadata records"
```

---

### Task 3: Generic Runner And Thread For Existing BookTicker Fusion

**Files:**
- Create: `core/market_data/fastest_route_fusion_runner.h`
- Create: `core/market_data/fastest_route_fusion_thread.h`
- Modify: `core/market_data/book_ticker_fusion_runner.h`
- Modify: `core/market_data/book_ticker_fusion_thread.h`
- Modify: `core/market_data/CMakeLists.txt`
- Test: `test/core/market_data/book_ticker_fusion_runner_test.cpp`
- Test: `test/core/market_data/book_ticker_fusion_thread_test.cpp`

- [ ] **Step 1: Run the existing runner and thread tests before refactor**

Run:

```bash
ctest --test-dir build/debug -R 'core_market_data_book_ticker_fusion_(runner|thread)_test|core_market_data_book_ticker_fusion_static_review' --output-on-failure
```

Expected: existing tests pass before refactor.

- [ ] **Step 2: Add the generic runner**

Create `core/market_data/fastest_route_fusion_runner.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_RUNNER_H_
#define AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_RUNNER_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/market_data/data_shm.h"
#include "core/websocket/runtime_clock.h"

namespace aquila::market_data {

struct FastestRouteFusionPollStats {
  std::uint64_t read_count{0};
  std::uint64_t published_count{0};
  std::uint64_t metadata_write_errors{0};
};

template <typename Traits, typename MetadataPolicy>
class BasicFastestRouteFusionRunner {
 public:
  using Config = typename Traits::Config;
  using Record = typename Traits::Record;
  using SourceConfig = typename Traits::SourceConfig;

  explicit BasicFastestRouteFusionRunner(const Config& config)
      : max_events_per_source_(config.max_events_per_source),
        fusion_(config.max_symbol_id),
        publisher_(Traits::MakeOutputShmConfig(config.output)),
        metadata_policy_(config) {
    sources_.reserve(config.sources.size());
    for (const SourceConfig& source_config : config.sources) {
      sources_.push_back(std::make_unique<Source>(
          source_config.source_id, Traits::MakeSourceShmConfig(source_config)));
    }
  }

  [[nodiscard]] FastestRouteFusionPollStats PollOnce() noexcept {
    FastestRouteFusionPollStats stats;
    for (std::unique_ptr<Source>& source : sources_) {
      for (std::uint32_t i = 0; i < max_events_per_source_; ++i) {
        Record record{};
        if (!source->reader.TryReadOne(&record)) {
          break;
        }
        ++stats.read_count;

        const std::int64_t fusion_publish_ns =
            static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
        const auto decision =
            Traits::OnRecord(fusion_, source->source_id, record,
                             fusion_publish_ns);
        if (!decision.publish) {
          continue;
        }

        Traits::SetLocalNs(&record, fusion_publish_ns);
        Traits::Publish(publisher_, record);
        ++stats.published_count;

        if constexpr (MetadataPolicy::kEnabled) {
          if (!metadata_policy_.Write(decision, record)) {
            ++stats.metadata_write_errors;
          }
        }
      }
    }
    total_read_count_ += stats.read_count;
    total_published_count_ += stats.published_count;
    total_metadata_write_errors_ += stats.metadata_write_errors;
    return stats;
  }

  [[nodiscard]] bool Flush() noexcept {
    publisher_.FlushPublishedCount();
    return metadata_policy_.Flush();
  }

  [[nodiscard]] std::uint64_t total_read_count() const noexcept {
    return total_read_count_;
  }

  [[nodiscard]] std::uint64_t total_published_count() const noexcept {
    return total_published_count_;
  }

  [[nodiscard]] std::uint64_t total_metadata_write_errors() const noexcept {
    return total_metadata_write_errors_;
  }

 private:
  struct Source {
    Source(std::int32_t source_id_in, const typename Traits::ShmConfig& config)
        : source_id(source_id_in), reader(config) {}

    std::int32_t source_id{-1};
    typename Traits::Reader reader;
  };

  std::uint32_t max_events_per_source_{0};
  typename Traits::Core fusion_;
  DataShmPublisher publisher_;
  MetadataPolicy metadata_policy_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::uint64_t total_read_count_{0};
  std::uint64_t total_published_count_{0};
  std::uint64_t total_metadata_write_errors_{0};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_RUNNER_H_
```

- [ ] **Step 3: Add the generic thread**

Create `core/market_data/fastest_route_fusion_thread.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_THREAD_H_
#define AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_THREAD_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "core/websocket/runtime_policy.h"

namespace aquila::market_data {

struct FastestRouteFusionThreadStats {
  bool ok{false};
  bool flush_ok{false};
  std::uint64_t total_read_count{0};
  std::uint64_t total_published_count{0};
  std::uint64_t total_metadata_write_errors{0};
  std::string error;
};

template <typename Runner, typename Config>
class BasicFastestRouteFusionThread {
 public:
  explicit BasicFastestRouteFusionThread(Config config)
      : config_(std::move(config)) {}

  BasicFastestRouteFusionThread(const BasicFastestRouteFusionThread&) = delete;
  BasicFastestRouteFusionThread& operator=(
      const BasicFastestRouteFusionThread&) = delete;

  ~BasicFastestRouteFusionThread() {
    Stop();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  void Start() {
    if (thread_.joinable()) {
      throw std::logic_error("fusion thread already started");
    }
    stop_requested_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      init_complete_ = false;
      init_error_.clear();
      stats_ = FastestRouteFusionThreadStats{};
    }

    thread_ = std::thread([this] { Run(); });

    std::unique_lock<std::mutex> lock(state_mutex_);
    state_cv_.wait(lock, [this] { return init_complete_; });
    if (!init_error_.empty()) {
      lock.unlock();
      if (thread_.joinable()) {
        thread_.join();
      }
      throw std::runtime_error(init_error_);
    }
  }

  void Stop() noexcept { stop_requested_.store(true, std::memory_order_release); }

  [[nodiscard]] FastestRouteFusionThreadStats Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
    std::lock_guard<std::mutex> lock(state_mutex_);
    return stats_;
  }

 private:
  void MarkInitComplete(std::string error) {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      init_error_ = std::move(error);
      init_complete_ = true;
    }
    state_cv_.notify_all();
  }

  void StoreStats(FastestRouteFusionThreadStats stats) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    stats_ = std::move(stats);
  }

  void Run() noexcept {
    FastestRouteFusionThreadStats stats;
    try {
      ApplyRuntimePolicy();
      Runner runner(config_);
      MarkInitComplete({});
      while (!stop_requested_.load(std::memory_order_acquire)) {
        const auto poll_stats = runner.PollOnce();
        if (poll_stats.read_count == 0) {
          std::this_thread::yield();
        }
      }

      stats.flush_ok = runner.Flush();
      stats.total_read_count = runner.total_read_count();
      stats.total_published_count = runner.total_published_count();
      stats.total_metadata_write_errors = runner.total_metadata_write_errors();
      stats.ok = stats.flush_ok && stats.total_metadata_write_errors == 0;
    } catch (const std::exception& exc) {
      stats.ok = false;
      stats.error = exc.what();
      MarkInitComplete(stats.error);
    } catch (...) {
      stats.ok = false;
      stats.error = "unknown fusion thread error";
      MarkInitComplete(stats.error);
    }
    StoreStats(std::move(stats));
  }

  void ApplyRuntimePolicy() noexcept {
    if (config_.bind_cpu_id < 0) {
      return;
    }
    aquila::websocket::RuntimePolicy policy;
    policy.affinity_mode = aquila::websocket::AffinityMode::kBestEffort;
    policy.io_cpu_id = config_.bind_cpu_id;
    policy.lock_memory = false;
    policy.prefault_stack = true;
    policy.active_spin = true;
    (void)aquila::websocket::ApplyRuntimePolicy(policy);
  }

  Config config_;
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
  std::mutex state_mutex_;
  std::condition_variable state_cv_;
  bool init_complete_{false};
  std::string init_error_;
  FastestRouteFusionThreadStats stats_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_THREAD_H_
```

- [ ] **Step 4: Refactor BookTicker runner to traits**

Modify `core/market_data/book_ticker_fusion_runner.h`:

```cpp
#include "core/market_data/fastest_route_fusion_runner.h"
```

Define `BookTickerFusionRunnerTraits`:

```cpp
struct BookTickerFusionRunnerTraits {
  using Config = BookTickerFusionConfig;
  using Core = BookTickerFusionCore;
  using Decision = BookTickerFusionDecision;
  using Record = BookTicker;
  using Reader = BookTickerShmReader;
  using ShmConfig = BookTickerShmConfig;
  using SourceConfig = BookTickerFusionSourceConfig;
  using OutputConfig = BookTickerFusionOutputConfig;

  [[nodiscard]] static BookTickerShmConfig MakeSourceShmConfig(
      const BookTickerFusionSourceConfig& source) {
    return BookTickerShmConfig{.enabled = true,
                               .shm_name = source.shm_name,
                               .channel_name = source.channel_name,
                               .create = false,
                               .remove_existing = false};
  }

  [[nodiscard]] static BookTickerShmConfig MakeOutputShmConfig(
      const BookTickerFusionOutputConfig& output) {
    return BookTickerShmConfig{.enabled = true,
                               .shm_name = output.shm_name,
                               .channel_name = output.channel_name,
                               .create = true,
                               .remove_existing = output.remove_existing};
  }

  [[nodiscard]] static BookTickerFusionDecision OnRecord(
      BookTickerFusionCore& core, std::int32_t source_id,
      const BookTicker& ticker, std::int64_t fusion_publish_ns) noexcept {
    return core.OnBookTicker(source_id, ticker, fusion_publish_ns);
  }

  static void SetLocalNs(BookTicker* ticker, std::int64_t local_ns) noexcept {
    ticker->local_ns = local_ns;
  }

  static void Publish(DataShmPublisher& publisher,
                      const BookTicker& ticker) noexcept {
    publisher.OnBookTicker(ticker);
  }
};
```

Replace the old `BasicBookTickerFusionRunner` class body with:

```cpp
template <typename MetadataPolicy>
using BasicBookTickerFusionRunner =
    BasicFastestRouteFusionRunner<BookTickerFusionRunnerTraits, MetadataPolicy>;

using BookTickerFusionPollStats = FastestRouteFusionPollStats;
using BookTickerFusionRunner =
    BasicBookTickerFusionRunner<DefaultBookTickerFusionMetadataPolicy>;
```

- [ ] **Step 5: Refactor BookTicker thread to generic thread**

Modify `core/market_data/book_ticker_fusion_thread.h` to use:

```cpp
#include "core/market_data/fastest_route_fusion_thread.h"
```

Keep public names:

```cpp
using BookTickerFusionThreadStats = FastestRouteFusionThreadStats;
using BookTickerFusionThread =
    BasicFastestRouteFusionThread<BookTickerFusionRunner,
                                  BookTickerFusionConfig>;
```

Remove the old concrete `BookTickerFusionThread` class after verifying no custom behavior remains.

- [ ] **Step 6: Register new headers**

Add `fastest_route_fusion_runner.h` and `fastest_route_fusion_thread.h` to `core/market_data/CMakeLists.txt`.

- [ ] **Step 7: Run BookTicker fusion regression tests**

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'core_market_data_book_ticker_fusion|book_ticker_fusion_config_test|book_ticker_fusion_cli_test|data_fusion_tool_support_test' --output-on-failure
```

Expected: existing BookTicker fusion tests pass.

- [ ] **Step 8: Commit**

```bash
git add core/market_data/fastest_route_fusion_runner.h \
  core/market_data/fastest_route_fusion_thread.h \
  core/market_data/book_ticker_fusion_runner.h \
  core/market_data/book_ticker_fusion_thread.h \
  core/market_data/CMakeLists.txt
git commit -m "refactor: share fastest route fusion runner"
```

---

### Task 4: Trade Core, Runner, Metadata Policy, And Thread

**Files:**
- Create: `core/market_data/trade_fusion.h`
- Create: `core/market_data/trade_fusion_metadata_policy.h`
- Create: `core/market_data/trade_fusion_runner.h`
- Create: `core/market_data/trade_fusion_thread.h`
- Create: `test/core/market_data/trade_fusion_test.cpp`
- Create: `test/core/market_data/trade_fusion_runner_test.cpp`
- Create: `test/core/market_data/trade_fusion_thread_test.cpp`
- Modify: `core/market_data/CMakeLists.txt`
- Modify: `test/core/market_data/CMakeLists.txt`
- Test: Trade fusion core / runner / thread tests.

- [ ] **Step 1: Write the failing Trade core test**

Add `core_market_data_trade_fusion_test` to `test/core/market_data/CMakeLists.txt`.

Create `test/core/market_data/trade_fusion_test.cpp` with these assertions:

```cpp
#include "core/market_data/trade_fusion.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace {

aquila::Trade MakeTrade(std::int32_t symbol_id, std::int64_t id,
                        std::int64_t source_local_ns) {
  return aquila::Trade{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = aquila::Exchange::kGate,
      .side = aquila::OrderSide::kBuy,
      .reserved = 0,
      .exchange_ns = 1'780'000'000'000'000'000 + id,
      .trade_ns = 1'780'000'000'000'100'000 + id,
      .local_ns = source_local_ns,
      .price = 100.0 + static_cast<double>(id),
      .volume = 0.01 * static_cast<double>(symbol_id + 1),
      .batch_index = 1,
      .batch_count = 3,
  };
}

void ExpectPublishedDecision(const aquila::market_data::TradeFusionDecision& d,
                             const aquila::Trade& source,
                             std::int64_t fusion_publish_ns,
                             std::int32_t source_id) {
  ASSERT_TRUE(d.publish);
  EXPECT_EQ(d.source_id, source_id);
  EXPECT_EQ(d.symbol_id, source.symbol_id);
  EXPECT_EQ(d.trade_id, source.id);
  EXPECT_EQ(d.source_local_ns, source.local_ns);
  EXPECT_EQ(d.fusion_publish_ns, fusion_publish_ns);
}

TEST(TradeFusionCoreTest, PublishesOnlyIncreasingTradeIdsPerSymbol) {
  aquila::market_data::TradeFusionCore fusion(/*max_symbol_id=*/128);

  const aquila::Trade first = MakeTrade(42, 100, 1'000);
  ExpectPublishedDecision(
      fusion.OnTrade(/*source_id=*/0, first, /*fusion_publish_ns=*/2'000),
      first, 2'000, 0);

  EXPECT_FALSE(fusion
                   .OnTrade(/*source_id=*/1, MakeTrade(42, 100, 1'100),
                            /*fusion_publish_ns=*/2'100)
                   .publish);
  EXPECT_FALSE(fusion
                   .OnTrade(/*source_id=*/2, MakeTrade(42, 99, 1'200),
                            /*fusion_publish_ns=*/2'200)
                   .publish);

  const aquila::Trade next = MakeTrade(42, 101, 1'300);
  ExpectPublishedDecision(
      fusion.OnTrade(/*source_id=*/3, next, /*fusion_publish_ns=*/2'300),
      next, 2'300, 3);
}

TEST(TradeFusionCoreTest, MaintainsIndependentStatePerSymbol) {
  aquila::market_data::TradeFusionCore fusion(/*max_symbol_id=*/128);

  ExpectPublishedDecision(
      fusion.OnTrade(/*source_id=*/0, MakeTrade(1, 10, 3'000),
                     /*fusion_publish_ns=*/4'000),
      MakeTrade(1, 10, 3'000), 4'000, 0);
  ExpectPublishedDecision(
      fusion.OnTrade(/*source_id=*/1, MakeTrade(2, 5, 3'100),
                     /*fusion_publish_ns=*/4'100),
      MakeTrade(2, 5, 3'100), 4'100, 1);

  EXPECT_FALSE(fusion
                   .OnTrade(/*source_id=*/2, MakeTrade(1, 9, 3'200),
                            /*fusion_publish_ns=*/4'200)
                   .publish);
}

TEST(TradeFusionCoreTest, DropsOutOfRangeSymbolsWithUnsetMetadata) {
  aquila::market_data::TradeFusionCore fusion(/*max_symbol_id=*/8);

  const aquila::market_data::TradeFusionDecision invalid =
      fusion.OnTrade(/*source_id=*/0, MakeTrade(8, 1, 5'000),
                     /*fusion_publish_ns=*/6'000);
  EXPECT_FALSE(invalid.publish);
  EXPECT_EQ(invalid.source_id, -1);
  EXPECT_EQ(invalid.symbol_id, -1);
  EXPECT_EQ(invalid.trade_id, 0);
  EXPECT_EQ(invalid.source_local_ns, 0);
  EXPECT_EQ(invalid.fusion_publish_ns, 0);
}

}  // namespace
```

- [ ] **Step 2: Implement `TradeFusionCore`**

Create `core/market_data/trade_fusion.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_H_

#include <cstdint>

#include "core/market_data/fastest_route_fusion.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

struct TradeFusionDecision {
  bool publish{false};
  std::int32_t source_id{-1};
  std::int32_t symbol_id{-1};
  std::int64_t trade_id{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

struct TradeFusionTraits {
  using Record = Trade;

  [[nodiscard]] static std::int32_t SymbolId(const Trade& trade) noexcept {
    return trade.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(const Trade& trade) noexcept {
    return trade.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(const Trade& trade) noexcept {
    return trade.local_ns;
  }
};

class TradeFusionCore {
 public:
  explicit TradeFusionCore(std::uint32_t max_symbol_id)
      : core_(max_symbol_id) {}

  [[nodiscard]] TradeFusionDecision OnTrade(
      std::int32_t source_id, const Trade& trade,
      std::int64_t fusion_publish_ns) noexcept {
    const FastestRouteFusionDecision decision =
        core_.OnRecord(source_id, trade, fusion_publish_ns);
    if (!decision.publish) {
      return {};
    }
    return TradeFusionDecision{
        .publish = true,
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .trade_id = decision.record_id,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
  }

 private:
  BasicFastestRouteFusionCore<TradeFusionTraits> core_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_H_
```

- [ ] **Step 3: Implement Trade metadata policy**

Create `core/market_data/trade_fusion_metadata_policy.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_

#include <type_traits>

#include "core/common/book_ticker_fusion_metadata_mode.h"
#include "core/market_data/trade_fusion.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_metadata.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

class FileTradeFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = true;

  explicit FileTradeFusionMetadataPolicy(const TradeFusionConfig& config)
      : writer_(config.output.metadata_bin) {}

  [[nodiscard]] bool Write(const TradeFusionDecision& decision,
                           const Trade& trade) noexcept {
    const TradeFusionMetadataRecord record{
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .trade_id = decision.trade_id,
        .exchange_ns = trade.exchange_ns,
        .trade_ns = trade.trade_ns,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
    return writer_.Write(record);
  }

  [[nodiscard]] bool Flush() noexcept { return writer_.Flush(); }

 private:
  TradeFusionMetadataWriter writer_;
};

class NoopTradeFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopTradeFusionMetadataPolicy(
      const TradeFusionConfig& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept { return true; }
};

using DefaultTradeFusionMetadataPolicy = std::conditional_t<
    aquila::kBookTickerFusionMetadataEnabled, FileTradeFusionMetadataPolicy,
    NoopTradeFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_
```

- [ ] **Step 4: Add Trade config structs needed by runner**

Create `core/market_data/trade_fusion_config.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aquila::market_data {

struct TradeFusionSourceConfig {
  std::int32_t source_id{-1};
  std::string name;
  std::string shm_name;
  std::string channel_name;
};

struct TradeFusionOutputConfig {
  std::string shm_name;
  std::string channel_name;
  bool remove_existing{false};
  std::filesystem::path metadata_bin;
};

struct TradeFusionConfig {
  std::string name;
  std::uint32_t max_events_per_source{1};
  std::int32_t bind_cpu_id{-1};
  std::uint32_t max_symbol_id{4096};
  TradeFusionOutputConfig output;
  std::vector<TradeFusionSourceConfig> sources;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_CONFIG_H_
```

- [ ] **Step 5: Write the failing Trade runner test**

Add `core_market_data_trade_fusion_runner_test` to `test/core/market_data/CMakeLists.txt`.

Create `test/core/market_data/trade_fusion_runner_test.cpp` using the same SHM shape as `book_ticker_fusion_runner_test.cpp`; the key assertions are:

```cpp
EXPECT_EQ(stats.read_count, 3U);
EXPECT_EQ(stats.published_count, 2U);
EXPECT_EQ(first.id, source0_first.id);
EXPECT_EQ(first.symbol_id, source0_first.symbol_id);
EXPECT_EQ(first.trade_ns, source0_first.trade_ns);
EXPECT_GE(first.local_ns, source0_first.local_ns);
EXPECT_EQ(second.id, source1_next.id);
EXPECT_EQ(second.trade_ns, source1_next.trade_ns);
```

When metadata is enabled, assert:

```cpp
ASSERT_EQ(metadata.size(), 2U);
EXPECT_EQ(metadata[0].source_id, 0);
EXPECT_EQ(metadata[0].symbol_id, source0_first.symbol_id);
EXPECT_EQ(metadata[0].trade_id, source0_first.id);
EXPECT_EQ(metadata[0].exchange_ns, source0_first.exchange_ns);
EXPECT_EQ(metadata[0].trade_ns, source0_first.trade_ns);
EXPECT_EQ(metadata[0].source_local_ns, source0_first.local_ns);
EXPECT_EQ(metadata[0].fusion_publish_ns, first.local_ns);
EXPECT_EQ(metadata[1].source_id, 1);
EXPECT_EQ(metadata[1].trade_id, source1_next.id);
EXPECT_EQ(metadata[1].source_local_ns, source1_next.local_ns);
EXPECT_EQ(metadata[1].fusion_publish_ns, second.local_ns);
```

Use `TradeShmConfig`, `TradeShmReader`, and `DataShmPublisher::OnTrade`.

- [ ] **Step 6: Implement Trade runner**

Create `core/market_data/trade_fusion_runner.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_RUNNER_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_RUNNER_H_

#include <cstdint>

#include "core/market_data/data_shm.h"
#include "core/market_data/fastest_route_fusion_runner.h"
#include "core/market_data/trade_fusion.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_metadata_policy.h"

namespace aquila::market_data {

struct TradeFusionRunnerTraits {
  using Config = TradeFusionConfig;
  using Core = TradeFusionCore;
  using Decision = TradeFusionDecision;
  using Record = Trade;
  using Reader = TradeShmReader;
  using ShmConfig = TradeShmConfig;
  using SourceConfig = TradeFusionSourceConfig;
  using OutputConfig = TradeFusionOutputConfig;

  [[nodiscard]] static TradeShmConfig MakeSourceShmConfig(
      const TradeFusionSourceConfig& source) {
    return TradeShmConfig{.enabled = true,
                          .shm_name = source.shm_name,
                          .channel_name = source.channel_name,
                          .create = false,
                          .remove_existing = false};
  }

  [[nodiscard]] static TradeShmConfig MakeOutputShmConfig(
      const TradeFusionOutputConfig& output) {
    return TradeShmConfig{.enabled = true,
                          .shm_name = output.shm_name,
                          .channel_name = output.channel_name,
                          .create = true,
                          .remove_existing = output.remove_existing};
  }

  [[nodiscard]] static TradeFusionDecision OnRecord(
      TradeFusionCore& core, std::int32_t source_id, const Trade& trade,
      std::int64_t fusion_publish_ns) noexcept {
    return core.OnTrade(source_id, trade, fusion_publish_ns);
  }

  static void SetLocalNs(Trade* trade, std::int64_t local_ns) noexcept {
    trade->local_ns = local_ns;
  }

  static void Publish(DataShmPublisher& publisher,
                      const Trade& trade) noexcept {
    publisher.OnTrade(trade);
  }
};

template <typename MetadataPolicy>
using BasicTradeFusionRunner =
    BasicFastestRouteFusionRunner<TradeFusionRunnerTraits, MetadataPolicy>;

using TradeFusionPollStats = FastestRouteFusionPollStats;
using TradeFusionRunner =
    BasicTradeFusionRunner<DefaultTradeFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_RUNNER_H_
```

- [ ] **Step 7: Write the failing Trade thread test**

Create `test/core/market_data/trade_fusion_thread_test.cpp` with the same structure as `book_ticker_fusion_thread_test.cpp`, but publish a `Trade` and read from `TradeShmReader`. Assert:

```cpp
ASSERT_TRUE(stats.ok) << stats.error;
EXPECT_TRUE(stats.flush_ok);
EXPECT_GE(stats.total_read_count, 1U);
EXPECT_GE(stats.total_published_count, 1U);
EXPECT_EQ(stats.total_metadata_write_errors, 0U);
EXPECT_EQ(canonical.id, 100);
EXPECT_EQ(canonical.symbol_id, 42);
EXPECT_EQ(canonical.trade_ns, 1'780'000'000'000'100'100);
```

When metadata is enabled, assert file size is `sizeof(md::TradeFusionMetadataRecord)`.

- [ ] **Step 8: Implement Trade thread alias**

Create `core/market_data/trade_fusion_thread.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_THREAD_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_THREAD_H_

#include "core/market_data/fastest_route_fusion_thread.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_runner.h"

namespace aquila::market_data {

using TradeFusionThreadStats = FastestRouteFusionThreadStats;
using TradeFusionThread =
    BasicFastestRouteFusionThread<TradeFusionRunner, TradeFusionConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_THREAD_H_
```

- [ ] **Step 9: Register files and run tests**

Add all new Trade fusion headers to `core/market_data/CMakeLists.txt`.

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'core_market_data_(fastest_route|book_ticker_fusion|trade_fusion)' --output-on-failure
```

Expected: generic, BookTicker, and Trade core / metadata / runner / thread tests pass.

- [ ] **Step 10: Commit**

```bash
git add core/market_data/trade_fusion.h \
  core/market_data/trade_fusion_config.h \
  core/market_data/trade_fusion_metadata_policy.h \
  core/market_data/trade_fusion_runner.h \
  core/market_data/trade_fusion_thread.h \
  core/market_data/CMakeLists.txt \
  test/core/market_data/trade_fusion_test.cpp \
  test/core/market_data/trade_fusion_runner_test.cpp \
  test/core/market_data/trade_fusion_thread_test.cpp \
  test/core/market_data/CMakeLists.txt
git commit -m "feat: add trade fastest route fusion"
```

---

### Task 5: Trade Fusion TOML Parser And Example Configs

**Files:**
- Create: `core/config/trade_fusion_config.h`
- Create: `core/config/trade_fusion_config.cpp`
- Create: `test/config/trade_fusion_config_test.cpp`
- Create: `config/market_data_fusion/gate_trade_fusion_4sources.toml`
- Create: `config/market_data_fusion/binance_trade_fusion_4sources.toml`
- Modify: `core/config/CMakeLists.txt`
- Modify: `test/config/CMakeLists.txt`
- Test: `test/config/trade_fusion_config_test.cpp`

- [ ] **Step 1: Write the failing config test**

Add `trade_fusion_config_test` to `test/config/CMakeLists.txt` with `aquila_config`, `GTest::gtest_main`, and `PkgConfig::tomlplusplus`, plus `AQUILA_PROJECT_SOURCE_DIR`.

Create `test/config/trade_fusion_config_test.cpp`:

```cpp
#include "core/config/trade_fusion_config.h"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "core/common/book_ticker_fusion_metadata_mode.h"

namespace {

toml::parse_result ParseToml(const std::string& text) {
  return toml::parse(text);
}

TEST(TradeFusionConfigTest, ParsesFourSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_trade_fusion_4src"
max_events_per_source = 1
bind_cpu_id = 16
max_symbol_id = 4096

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"
remove_existing = true
metadata_bin = "/home/liuxiang/tmp/gate_trade_fusion/fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "gate_trade_src_0"
shm_name = "aquila_gate_trade_src_0"
channel_name = "trade_channel"

[[fusion.sources]]
source_id = 1
name = "gate_trade_src_1"
shm_name = "aquila_gate_trade_src_1"
channel_name = "trade_channel"
)toml");

  const auto result = aquila::config::ParseTradeFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "gate_trade_fusion_4src");
  EXPECT_EQ(result.value.max_events_per_source, 1U);
  EXPECT_EQ(result.value.bind_cpu_id, 16);
  EXPECT_EQ(result.value.max_symbol_id, 4096U);
  EXPECT_EQ(result.value.output.shm_name, "aquila_gate_trade_fusion");
  EXPECT_EQ(result.value.output.channel_name, "trade_channel");
  EXPECT_TRUE(result.value.output.remove_existing);
  EXPECT_EQ(result.value.output.metadata_bin,
            "/home/liuxiang/tmp/gate_trade_fusion/fusion_metadata.bin");
  ASSERT_EQ(result.value.sources.size(), 2U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].name, "gate_trade_src_0");
  EXPECT_EQ(result.value.sources[0].shm_name, "aquila_gate_trade_src_0");
}

TEST(TradeFusionConfigTest, LoadsCommittedBinanceFourSourceConfig) {
  const std::filesystem::path config_path =
      std::filesystem::path{AQUILA_PROJECT_SOURCE_DIR} /
      "config/market_data_fusion/binance_trade_fusion_4sources.toml";

  const auto result = aquila::config::LoadTradeFusionConfigFile(config_path);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "binance_trade_fusion_4sources");
  EXPECT_EQ(result.value.output.shm_name, "aquila_binance_trade_fusion");
  EXPECT_EQ(result.value.output.channel_name, "trade_channel");
  ASSERT_EQ(result.value.sources.size(), 4U);
  EXPECT_EQ(result.value.sources[0].source_id, 0);
  EXPECT_EQ(result.value.sources[0].name, "binance_trade_source_0");
  EXPECT_EQ(result.value.sources[0].shm_name, "aquila_binance_trade_src_0");
}

TEST(TradeFusionConfigTest, RejectsDuplicateSourceId) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_trade_fusion"

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"
metadata_bin = "/home/liuxiang/tmp/gate_trade_fusion/fusion_metadata.bin"

[[fusion.sources]]
source_id = 1
name = "gate_trade_src_1a"
shm_name = "aquila_gate_trade_src_1a"

[[fusion.sources]]
source_id = 1
name = "gate_trade_src_1b"
shm_name = "aquila_gate_trade_src_1b"
)toml");

  const auto result = aquila::config::ParseTradeFusionConfig(parsed);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("source_id"), std::string::npos);
}

TEST(TradeFusionConfigTest, HandlesMissingMetadataBinForBuildMode) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_trade_fusion"

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"

[[fusion.sources]]
source_id = 0
name = "gate_trade_src_0"
shm_name = "aquila_gate_trade_src_0"
)toml");

  const auto result = aquila::config::ParseTradeFusionConfig(parsed);

#if AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED
  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("metadata_bin"), std::string::npos);
#else
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.output.metadata_bin.empty());
#endif
}

}  // namespace
```

- [ ] **Step 2: Add Trade example configs**

Create `config/market_data_fusion/gate_trade_fusion_4sources.toml`:

```toml
[fusion]
name = "gate_trade_fusion_4sources"
max_events_per_source = 1
bind_cpu_id = 16
max_symbol_id = 4096

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"
remove_existing = true
metadata_bin = "/home/liuxiang/tmp/gate_trade_fusion_4sources/fusion_metadata.bin"

[[fusion.sources]]
source_id = 0
name = "gate_trade_source_0"
shm_name = "aquila_gate_trade_src_0"
channel_name = "trade_channel"

[[fusion.sources]]
source_id = 1
name = "gate_trade_source_1"
shm_name = "aquila_gate_trade_src_1"
channel_name = "trade_channel"

[[fusion.sources]]
source_id = 2
name = "gate_trade_source_2"
shm_name = "aquila_gate_trade_src_2"
channel_name = "trade_channel"

[[fusion.sources]]
source_id = 3
name = "gate_trade_source_3"
shm_name = "aquila_gate_trade_src_3"
channel_name = "trade_channel"
```

Create `config/market_data_fusion/binance_trade_fusion_4sources.toml` with the same structure and these names:

```toml
[fusion]
name = "binance_trade_fusion_4sources"
max_events_per_source = 1
bind_cpu_id = 16
max_symbol_id = 4096

[fusion.output]
shm_name = "aquila_binance_trade_fusion"
channel_name = "trade_channel"
remove_existing = true
metadata_bin = "/home/liuxiang/tmp/binance_trade_fusion_4sources/fusion_metadata.bin"
```

Use source names `binance_trade_source_0..3` and SHM names `aquila_binance_trade_src_0..3`.

- [ ] **Step 3: Implement Trade config parser API**

Create `core/config/trade_fusion_config.h`:

```cpp
#ifndef AQUILA_CORE_CONFIG_TRADE_FUSION_CONFIG_H_
#define AQUILA_CORE_CONFIG_TRADE_FUSION_CONFIG_H_

#include <filesystem>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/market_data/trade_fusion_config.h"

namespace aquila::config {

using TradeFusionConfigResult =
    Result<aquila::market_data::TradeFusionConfig>;

[[nodiscard]] TradeFusionConfigResult ParseTradeFusionConfig(
    const toml::table& node);

[[nodiscard]] TradeFusionConfigResult LoadTradeFusionConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_TRADE_FUSION_CONFIG_H_
```

Implement `core/config/trade_fusion_config.cpp` by copying the structure of `book_ticker_fusion_config.cpp` and replacing:

```cpp
BookTickerFusionConfigResult -> TradeFusionConfigResult
BookTickerFusionConfig -> TradeFusionConfig
BookTickerFusionSourceConfig -> TradeFusionSourceConfig
ParseBookTickerFusionConfig -> ParseTradeFusionConfig
LoadBookTickerFusionConfigFile -> LoadTradeFusionConfigFile
"book_ticker_channel" default -> "trade_channel"
"failed to load fusion config: " -> "failed to load trade fusion config: "
```

The metadata build-mode behavior stays tied to `aquila::kBookTickerFusionMetadataEnabled` for compatibility with the existing CMake option.

- [ ] **Step 4: Register parser and run tests**

Add `trade_fusion_config.cpp` and `trade_fusion_config.h` to `core/config/CMakeLists.txt`.

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'book_ticker_fusion_config_test|trade_fusion_config_test' --output-on-failure
```

Expected: both config parser tests pass.

- [ ] **Step 5: Commit**

```bash
git add core/config/trade_fusion_config.h \
  core/config/trade_fusion_config.cpp \
  core/config/CMakeLists.txt \
  test/config/trade_fusion_config_test.cpp \
  test/config/CMakeLists.txt \
  config/market_data_fusion/gate_trade_fusion_4sources.toml \
  config/market_data_fusion/binance_trade_fusion_4sources.toml
git commit -m "feat: add trade fusion config parser"
```

---

### Task 6: Standalone Trade Fusion CLI

**Files:**
- Create: `tools/market_data/trade_fusion_cli.h`
- Create: `tools/market_data/trade_fusion_cli.cpp`
- Create: `tools/market_data/trade_fusion.cpp`
- Create: `tools/market_data/binance_trade_fusion.cpp`
- Create: `test/tools/market_data/trade_fusion_cli_test.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `test/tools/market_data/CMakeLists.txt`
- Test: `test/tools/market_data/trade_fusion_cli_test.cpp`

- [ ] **Step 1: Write the failing CLI test**

Add a `trade_fusion_cli_test` target to `test/tools/market_data/CMakeLists.txt`, following `book_ticker_fusion_cli_test`, with:

```cmake
target_compile_definitions(trade_fusion_cli_test
    PRIVATE
        AQUILA_GATE_TRADE_FUSION_BINARY="$<TARGET_FILE:gate_trade_fusion>"
        AQUILA_MISSING_TRADE_FUSION_CONFIG="${PROJECT_BINARY_DIR}/missing_trade_fusion_config.toml"
)
add_dependencies(trade_fusion_cli_test gate_trade_fusion)
```

Create `test/tools/market_data/trade_fusion_cli_test.cpp` by copying `book_ticker_fusion_cli_test.cpp` and replacing the binary/config macros. Keep the assertion:

```cpp
EXPECT_NE(result.exit_code, 0) << result.output;
EXPECT_NE(result.output.find("config_error="), std::string::npos)
    << result.output;
```

- [ ] **Step 2: Add Trade CLI API**

Create `tools/market_data/trade_fusion_cli.h`:

```cpp
#ifndef AQUILA_TOOLS_MARKET_DATA_TRADE_FUSION_CLI_H_
#define AQUILA_TOOLS_MARKET_DATA_TRADE_FUSION_CLI_H_

#include <filesystem>
#include <string>

namespace aquila::tools::market_data {

int RunTradeFusionCli(int argc, char** argv,
                      std::filesystem::path default_config_path,
                      std::string app_description, std::string error_key);

}  // namespace aquila::tools::market_data

#endif  // AQUILA_TOOLS_MARKET_DATA_TRADE_FUSION_CLI_H_
```

- [ ] **Step 3: Implement Trade CLI**

Create `tools/market_data/trade_fusion_cli.cpp` by following `book_ticker_fusion_cli.cpp` and replacing:

```cpp
#include "core/config/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_runner.h"
ParseBookTickerFusionConfig -> ParseTradeFusionConfig
BookTickerFusionConfig -> TradeFusionConfig
BookTickerFusionRunner -> TradeFusionRunner
BookTickerFusionPollStats -> TradeFusionPollStats
RunBookTickerFusionCli -> RunTradeFusionCli
```

Use the same `FusionMetadataEnabledText()` and `metadata_output` logging semantics for now, because the compile-time metadata switch is shared.

- [ ] **Step 4: Add executable entries**

Create `tools/market_data/trade_fusion.cpp`:

```cpp
#include "tools/market_data/trade_fusion_cli.h"

int main(int argc, char** argv) {
  return aquila::tools::market_data::RunTradeFusionCli(
      argc, argv, "config/market_data_fusion/gate_trade_fusion_4sources.toml",
      "Gate Trade fastest-route fusion", "gate_trade_fusion");
}
```

Create `tools/market_data/binance_trade_fusion.cpp`:

```cpp
#include "tools/market_data/trade_fusion_cli.h"

int main(int argc, char** argv) {
  return aquila::tools::market_data::RunTradeFusionCli(
      argc, argv,
      "config/market_data_fusion/binance_trade_fusion_4sources.toml",
      "Binance Trade fastest-route fusion", "binance_trade_fusion");
}
```

- [ ] **Step 5: Register executables and run tests**

Add to `tools/CMakeLists.txt` near the BookTicker fusion targets:

```cmake
add_executable(gate_trade_fusion
    market_data/trade_fusion.cpp
    market_data/trade_fusion_cli.cpp
)

target_link_libraries(gate_trade_fusion
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
)

add_executable(binance_trade_fusion
    market_data/binance_trade_fusion.cpp
    market_data/trade_fusion_cli.cpp
)

target_link_libraries(binance_trade_fusion
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
)
```

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'book_ticker_fusion_cli_test|trade_fusion_cli_test|trade_fusion_config_test' --output-on-failure
```

Expected: BookTicker CLI and Trade CLI tests pass.

- [ ] **Step 6: Commit**

```bash
git add tools/market_data/trade_fusion_cli.h \
  tools/market_data/trade_fusion_cli.cpp \
  tools/market_data/trade_fusion.cpp \
  tools/market_data/binance_trade_fusion.cpp \
  tools/CMakeLists.txt \
  test/tools/market_data/trade_fusion_cli_test.cpp \
  test/tools/market_data/CMakeLists.txt
git commit -m "feat: add trade fusion cli"
```

---

### Task 7: Gate / Binance Data Fusion Bundle Support For Trade

**Files:**
- Create: `config/market_data_fusion/gate_data_fusion_trade_4sources.toml`
- Create: `config/market_data_fusion/binance_data_fusion_trade_4sources.toml`
- Modify: `tools/market_data/data_fusion_tool_support.h`
- Modify: `tools/gate/gate_data_fusion_config.h`
- Modify: `tools/gate/gate_data_fusion_config.cpp`
- Modify: `tools/gate/gate_data_fusion.cpp`
- Modify: `tools/binance/binance_data_fusion_config.h`
- Modify: `tools/binance/binance_data_fusion_config.cpp`
- Modify: `tools/binance/binance_data_fusion.cpp`
- Modify: `test/tools/gate/gate_data_fusion_config_test.cpp`
- Modify: `test/tools/binance/binance_data_fusion_config_test.cpp`
- Modify: `test/tools/market_data/data_fusion_tool_support_test.cpp`
- Test: Gate / Binance data fusion config tests and dry-run commands.

- [ ] **Step 1: Add Trade launch config parser tests**

In `test/tools/gate/gate_data_fusion_config_test.cpp`, add:

```cpp
TEST(GateDataFusionConfigTest, ParsesTradeSources) {
  const toml::parse_result parsed = ParseToml(R"toml(
[launch]
name = "gate_data_fusion_trade_4sources"
feed = "trade"
fusion_config = "config/market_data_fusion/gate_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_trade_source_0"
trade_shm_name = "aquila_gate_trade_src_0"
trade_channel_name = "trade_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
)toml");

  const auto result = aquila::tools::gate::ParseGateDataFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "gate_data_fusion_trade_4sources");
  EXPECT_EQ(result.value.feed, aquila::tools::market_data::DataFusionFeed::kTrade);
  ASSERT_EQ(result.value.sources.size(), 1U);
  EXPECT_EQ(result.value.sources[0].trade_shm_name, "aquila_gate_trade_src_0");
  EXPECT_EQ(result.value.sources[0].trade_channel_name, "trade_channel");
}
```

Add the equivalent Binance test in `test/tools/binance/binance_data_fusion_config_test.cpp` with `aquila::tools::binance::ParseBinanceDataFusionConfig`.

- [ ] **Step 2: Add shared feed enum and Trade helper functions**

Modify `tools/market_data/data_fusion_tool_support.h`:

```cpp
enum class DataFusionFeed : std::uint8_t {
  kBookTicker,
  kTrade,
};

[[nodiscard]] inline const char* DataFusionFeedName(
    DataFusionFeed feed) noexcept {
  switch (feed) {
    case DataFusionFeed::kBookTicker:
      return "book_ticker";
    case DataFusionFeed::kTrade:
      return "trade";
  }
  return "unknown";
}
```

Add Trade analogues of existing BookTicker helpers:

```cpp
[[nodiscard]] inline std::string FormatTradeFusionMetadataOutput(
    const aquila::market_data::TradeFusionConfig& fusion_config);

template <typename LaunchConfig>
[[nodiscard]] bool ValidateTradeFusionAlignment(
    const LaunchConfig& launch_config,
    const aquila::market_data::TradeFusionConfig& fusion_config,
    std::string* error);

template <typename SourceConfig, typename DataSessionConfig>
void ApplyTradeSourceOverride(const SourceConfig& source,
                              DataSessionConfig* data_session_config);

template <typename LaunchConfig, typename PreparedSources>
void LogTradeDataFusionDryRun(
    const LaunchConfig& launch_config,
    const aquila::market_data::TradeFusionConfig& fusion_config,
    const PreparedSources& sources);

inline void LogTradeDataFusionRunSummary(
    std::string_view launch_name, std::size_t source_count,
    std::uint64_t source_published_count,
    const aquila::market_data::TradeFusionThreadStats& fusion_stats);
```

`ApplyTradeSourceOverride` must set:

```cpp
data_session_config->trade_shm.enabled = true;
data_session_config->trade_shm.shm_name = source.trade_shm_name;
data_session_config->trade_shm.channel_name = source.trade_channel_name;
data_session_config->trade_shm.create = true;
data_session_config->trade_shm.remove_existing = source.remove_existing_source_shm;
data_session_config->feeds.book_ticker = false;
data_session_config->feeds.trade = true;
```

- [ ] **Step 3: Extend Gate / Binance launch config structs**

In both `tools/gate/gate_data_fusion_config.h` and `tools/binance/binance_data_fusion_config.h`:

```cpp
#include "tools/market_data/data_fusion_tool_support.h"
```

Add to source config:

```cpp
std::string trade_shm_name;
std::string trade_channel_name{"trade_channel"};
```

Add to launch config:

```cpp
aquila::tools::market_data::DataFusionFeed feed{
    aquila::tools::market_data::DataFusionFeed::kBookTicker};
```

- [ ] **Step 4: Extend launch config parsers**

In both parser `.cpp` files:

```cpp
[[nodiscard]] aq_tool_md::DataFusionFeed ParseFeed(
    toml::node_view<const toml::node> value_node) {
  const std::optional<std::string> value = value_node.value<std::string>();
  if (!value || value->empty() || *value == "book_ticker") {
    return aq_tool_md::DataFusionFeed::kBookTicker;
  }
  if (*value == "trade") {
    return aq_tool_md::DataFusionFeed::kTrade;
  }
  Fail("launch.feed", " must be book_ticker or trade");
  return aq_tool_md::DataFusionFeed::kBookTicker;
}
```

Parse `launch.feed` before sources. For each source:

```cpp
if (config_.feed == aq_tool_md::DataFusionFeed::kBookTicker) {
  source.book_ticker_shm_name =
      RequiredString((*source_table)["book_ticker_shm_name"],
                     "launch.sources.book_ticker_shm_name");
  source.book_ticker_channel_name = OptionalString(
      (*source_table)["book_ticker_channel_name"], "book_ticker_channel");
  if (source.book_ticker_channel_name.empty()) {
    Fail("launch.sources.book_ticker_channel_name", " must not be empty");
    return;
  }
} else {
  source.trade_shm_name = RequiredString((*source_table)["trade_shm_name"],
                                         "launch.sources.trade_shm_name");
  source.trade_channel_name = OptionalString(
      (*source_table)["trade_channel_name"], "trade_channel");
  if (source.trade_channel_name.empty()) {
    Fail("launch.sources.trade_channel_name", " must not be empty");
    return;
  }
}
```

Existing BookTicker configs continue to parse because missing `launch.feed` defaults to `book_ticker`.

- [ ] **Step 5: Add Trade bundle configs**

Create `config/market_data_fusion/gate_data_fusion_trade_4sources.toml` with:

```toml
[log]
log_level = "info"
file_sink_name = "/home/liuxiang/log/gate_data_fusion_trade_4sources.log"
console_sink_name = "gate_data_fusion_trade_4sources_console"
backend_thread_name = "gate_data_fusion_trade_log"
backend_cpu_affinity = 31
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[launch]
name = "gate_data_fusion_trade_4sources"
feed = "trade"
fusion_config = "config/market_data_fusion/gate_trade_fusion_4sources.toml"

[[launch.sources]]
source_id = 0
data_session_config = "config/data_sessions/gate_data_session_30symbols_private_plain_20260604.toml"
data_session_name = "gate_trade_source_0"
trade_shm_name = "aquila_gate_trade_src_0"
trade_channel_name = "trade_channel"
remove_existing_source_shm = true
bind_cpu_id = 17
```

Add source IDs `1..3` with names and SHM suffixes `_1.._3` and CPU IDs `18..20`.

Create `config/market_data_fusion/binance_data_fusion_trade_4sources.toml` with Binance names and `config/data_sessions/binance_data_session_30symbols_20260604.toml`.

- [ ] **Step 6: Branch Gate data fusion runtime**

Modify `tools/gate/gate_data_fusion.cpp`:

- Include Trade config and thread:

```cpp
#include "core/config/trade_fusion_config.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_thread.h"
```

- Add `ApplyTradeSourceOverride` in `LoadPreparedSources` when `launch_config.feed == kTrade`.
- In `GateSourceWorker`, create separate BookTicker and Trade worker templates or a traits-based worker. Trade worker must construct `DataShmPublisher(config.trade_shm)` and report `publisher_.published_trades()`.
- Keep the existing BookTicker path unchanged for `feed == kBookTicker`.
- For `feed == kTrade`, load `TradeFusionConfig`, validate with `ValidateTradeFusionAlignment`, run `TradeFusionThread`, and log with `LogTradeDataFusionRunSummary`.

The dry-run branch must call `LogTradeDataFusionDryRun` and return `0` without connecting.

- [ ] **Step 7: Branch Binance data fusion runtime**

Modify `tools/binance/binance_data_fusion.cpp` with the same structure as Gate:

- Use `aquila::config::LoadTradeFusionConfigFile`.
- Use `aq_binance::DataSession<aq_md::DataShmPublisher, WebSocketPolicy, aq_binance::DataSessionDiagnosticsPolicy>`.
- Trade worker reports `publisher_.published_trades()`.
- Dry-run must not connect.

- [ ] **Step 8: Add helper tests**

In `test/tools/market_data/data_fusion_tool_support_test.cpp`, add a Trade alignment test that builds a launch config with `trade_shm_name = "source_trade"` and a `TradeFusionConfig` source with matching `shm_name = "source_trade"`. Assert `ValidateTradeFusionAlignment(...)` returns true, then change the channel name and assert false with `"channel mismatch"` in the error.

- [ ] **Step 9: Run parser and dry-run tests**

Run:

```bash
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'gate_data_fusion_config_test|binance_data_fusion_config_test|data_fusion_tool_support_test' --output-on-failure
build/debug/tools/gate_data_fusion --config config/market_data_fusion/gate_data_fusion_trade_4sources.toml --max-runtime-ms 1
build/debug/tools/binance_data_fusion --config config/market_data_fusion/binance_data_fusion_trade_4sources.toml --max-runtime-ms 1
```

Expected:

- CTest parser/helper tests pass.
- Both dry-run commands exit `0`.
- Dry-run logs include `feed=trade` or equivalent Trade launch name, `metadata_enabled`, and Trade output SHM.

- [ ] **Step 10: Commit**

```bash
git add tools/market_data/data_fusion_tool_support.h \
  tools/gate/gate_data_fusion_config.h \
  tools/gate/gate_data_fusion_config.cpp \
  tools/gate/gate_data_fusion.cpp \
  tools/binance/binance_data_fusion_config.h \
  tools/binance/binance_data_fusion_config.cpp \
  tools/binance/binance_data_fusion.cpp \
  test/tools/gate/gate_data_fusion_config_test.cpp \
  test/tools/binance/binance_data_fusion_config_test.cpp \
  test/tools/market_data/data_fusion_tool_support_test.cpp \
  config/market_data_fusion/gate_data_fusion_trade_4sources.toml \
  config/market_data_fusion/binance_data_fusion_trade_4sources.toml
git commit -m "feat: support trade data fusion bundles"
```

---

### Task 8: Documentation And Final Verification

**Files:**
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/project_onboarding_guide.md`
- Modify: `docs/trade_fastest_route_fusion_design.md`
- Test: final build and focused CTest suite.

- [ ] **Step 1: Update diagnostic fields**

In `docs/diagnostic_fields.md`, update the Market Data Fusion section:

```markdown
BookTicker / Trade fusion 当前通过 `AQUILA_BOOK_TICKER_FUSION_METADATA_MODE=file|off`
在编译期决定是否启用 sidecar metadata。`file` 模式分别写 feed-specific raw metadata；
`off` 模式不打开 metadata 文件，不构造 metadata record，但仍保留基础 read / publish 运行统计。
```

Add field rows:

```markdown
| `trade_id` / `trade_ns` | `TradeFusionMetadataRecord` | stable | exchange trade id / ns | 关联 fused Trade winner 与 source Trade 输入，用于离线 attribution。 | Trade metadata ABI 替换后同步迁移。 |
| `source_local_ns` / `fusion_publish_ns` | `FusionMetadataRecord` / `TradeFusionMetadataRecord` | stable | ns | 拆分 source ingress latency 与 fusion hop latency；fused SHM 中 `local_ns` 等于 `fusion_publish_ns`。 | fusion metadata 被其他稳定 build metadata 取代后重审。 |
```

- [ ] **Step 2: Update onboarding entry points**

In `docs/project_onboarding_guide.md`, extend the Gate / Binance fastest-route fusion code-entry row to include:

```text
core/market_data/fastest_route_fusion.h
core/market_data/fastest_route_fusion_runner.h
core/market_data/fastest_route_fusion_thread.h
core/market_data/trade_fusion.h
core/market_data/trade_fusion_config.h
core/market_data/trade_fusion_metadata.h
core/market_data/trade_fusion_runner.h
core/market_data/trade_fusion_thread.h
core/config/trade_fusion_config.*
tools/market_data/trade_fusion_cli.*
tools/market_data/trade_fusion.cpp
tools/market_data/binance_trade_fusion.cpp
config/market_data_fusion/*trade_fusion*4sources.toml
```

Add a short “recently completed” bullet only after code verification succeeds:

```markdown
- 2026-07-06 Trade fastest-route fusion V1 已落地：内部 fusion core / runner / thread 泛化，外部保留 BookTicker / Trade feed-specific facade。Trade canonical SHM 按 `(symbol_id, Trade.id)` first-arrival 推进，输出 `Trade.local_ns = fusion_publish_ns`，并写 Trade-specific sidecar metadata。Gate / Binance standalone Trade fusion 和 data fusion dry-run 配置已加入。
```

- [ ] **Step 3: Re-run design doc consistency check**

Read `docs/trade_fastest_route_fusion_design.md` and update only if implementation changed one of these confirmed boundaries:

- `(symbol_id, Trade.id)` identity
- `batch_index` / `batch_count` not participating in V1 deduplication
- `local_ns = fusion_publish_ns` on canonical output
- feed-specific metadata ABI
- source data session only enabling `feed.trade` for Trade bundle

- [ ] **Step 4: Run final focused verification**

Run:

```bash
git diff --check
./build.sh -n 8 debug
ctest --test-dir build/debug -R 'fastest_route|book_ticker_fusion|trade_fusion|data_fusion_config|data_fusion_tool_support' --output-on-failure
build/debug/tools/gate_data_fusion --config config/market_data_fusion/gate_data_fusion_trade_4sources.toml --max-runtime-ms 1
build/debug/tools/binance_data_fusion --config config/market_data_fusion/binance_data_fusion_trade_4sources.toml --max-runtime-ms 1
```

Expected:

- `git diff --check` has no output.
- Focused CTest suite passes.
- Data fusion dry-run commands exit `0` because `--connect` is not set.

- [ ] **Step 5: Commit docs**

```bash
git add docs/diagnostic_fields.md \
  docs/project_onboarding_guide.md \
  docs/trade_fastest_route_fusion_design.md
git commit -m "docs: document trade fusion implementation"
```

---

## Self-Review

**Spec coverage:** The plan covers internal generic core / runner / thread, external BookTicker compatibility, new Trade facade, Trade sidecar metadata, Trade config files, standalone Trade CLI, Gate / Binance data fusion bundle support, diagnostics, onboarding, and focused verification.

**Placeholder scan:** The plan avoids open placeholders and names concrete files, structs, tests, commands, and expected outcomes.

**Type consistency:** Public BookTicker names remain `BookTickerFusion*`; public Trade names are `TradeFusion*`. Generic names use `FastestRouteFusion*`. Trade metadata uses `trade_id`; BookTicker metadata keeps `book_ticker_id`.
