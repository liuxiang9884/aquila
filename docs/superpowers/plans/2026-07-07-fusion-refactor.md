# Market Data Fusion Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deduplicate BookTicker / Trade fastest-route fusion under `core/market_data`, unify fusion metadata ABI v2, and verify the hot path has no reproducible performance regression.

**Architecture:** Keep public BookTicker / Trade entry points, TOML schema, CLI names, and runner loop semantics. Move duplicate config, decision, metadata, and runner trait code into shallow shared headers driven by feed traits. Add a dedicated fusion benchmark before production refactoring so the current implementation can be measured as the baseline.

**Tech Stack:** C++20, CMake, GTest, Google Benchmark, Python unittest / numpy, existing nova SHM, `DataShmPublisher`, `BookTickerShmReader`, `TradeShmReader`.

---

## File Map

- Create `benchmark/core/market_data/fastest_route_fusion_benchmark.cpp`: benchmark BookTicker / Trade core decision path and runner publish path before and after refactor.
- Modify `benchmark/core/market_data/CMakeLists.txt`: add `fastest_route_fusion_benchmark`.
- Create `core/market_data/fusion_config.h`: common source/output/config structs.
- Create `core/market_data/fusion_metadata.h`: unified ABI v2 `FusionMetadataRecord` and writer alias.
- Create `core/market_data/fusion_metadata_policy.h`: generic file/noop metadata policy.
- Modify `core/market_data/fastest_route_fusion.h`: keep generic core and expose `FastestRouteFusionDecision` as the shared decision ABI.
- Modify `core/market_data/fastest_route_fusion_runner.h`: add shared runner traits helper only; do not change `BasicFastestRouteFusionRunner::PollOnce()`.
- Modify `core/market_data/book_ticker_fusion_config.h` and `core/market_data/trade_fusion_config.h`: preserve public type names as aliases to common config types.
- Modify `core/market_data/book_ticker_fusion.h` and `core/market_data/trade_fusion.h`: replace feed-specific decision structs with shared decision alias and keep thin `OnBookTicker()` / `OnTrade()` facades.
- Modify `core/market_data/book_ticker_fusion_metadata.h` and `core/market_data/trade_fusion_metadata.h`: re-export unified metadata types.
- Modify `core/market_data/book_ticker_fusion_metadata_policy.h` and `core/market_data/trade_fusion_metadata_policy.h`: keep public policy names as aliases to generic policies.
- Modify `core/market_data/book_ticker_fusion_runner.h` and `core/market_data/trade_fusion_runner.h`: shrink to feed traits plus aliases.
- Modify `core/market_data/CMakeLists.txt`: list new headers.
- Modify C++ fusion tests under `test/core/market_data`: update decision/metadata field names and add shared-layer coverage.
- Modify `test/core/market_data/CMakeLists.txt`: add shared metadata policy test if it is a new test file.
- Modify `scripts/market_data/analyze_book_ticker_fusion_latency.py`: update metadata dtype and field access to `record_id` / `event_ns`.
- Modify `scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py`: assert 48-byte v2 dtype and generic field names.
- Modify `docs/diagnostic_fields.md`, `docs/gate_fastest_route_fusion_design.md`, and `docs/trade_fastest_route_fusion_design.md`: document metadata ABI v2.

## Task 1: Add Fusion Benchmark and Record Baseline

**Files:**
- Create: `benchmark/core/market_data/fastest_route_fusion_benchmark.cpp`
- Modify: `benchmark/core/market_data/CMakeLists.txt`

- [ ] **Step 1: Add benchmark target**

Append this target to `benchmark/core/market_data/CMakeLists.txt`:

```cmake
add_executable(fastest_route_fusion_benchmark
    fastest_route_fusion_benchmark.cpp
)

target_link_libraries(fastest_route_fusion_benchmark
    PRIVATE
        aquila_core
        benchmark::benchmark_main
        fmt::fmt-header-only
        nova
)
```

- [ ] **Step 2: Create the benchmark**

Create `benchmark/core/market_data/fastest_route_fusion_benchmark.cpp` with:

```cpp
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "core/market_data/book_ticker_fusion_runner.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/trade_fusion_runner.h"

namespace {

namespace md = aquila::market_data;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}
  ~ShmCleanup() { ::shm_unlink(shm_name.c_str()); }
  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_fastest_route_fusion_bench_{}_{}", ::getpid(),
                     suffix);
}

aquila::BookTicker MakeBookTicker(std::int64_t id) noexcept {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = static_cast<std::int32_t>(id & 1023),
      .exchange = aquila::Exchange::kGate,
      .exchange_ns = 1'780'000'000'000'000'000 + id,
      .local_ns = 1'780'000'000'000'100'000 + id,
      .bid_price = 100.0 + static_cast<double>(id),
      .bid_volume = 1.0,
      .ask_price = 101.0 + static_cast<double>(id),
      .ask_volume = 2.0,
  };
}

aquila::Trade MakeTrade(std::int64_t id) noexcept {
  return aquila::Trade{
      .id = id,
      .symbol_id = static_cast<std::int32_t>(id & 1023),
      .exchange = aquila::Exchange::kGate,
      .side = aquila::OrderSide::kBuy,
      .reserved = 0,
      .exchange_ns = 1'780'000'000'000'000'000 + id,
      .trade_ns = 1'780'000'000'000'010'000 + id,
      .local_ns = 1'780'000'000'000'100'000 + id,
      .price = 100.0 + static_cast<double>(id),
      .volume = 1.0,
      .batch_index = 0,
      .batch_count = 1,
  };
}

md::BookTickerShmConfig MakeBookTickerCreateConfig(std::string_view suffix) {
  return md::BookTickerShmConfig{.enabled = true,
                                 .shm_name = UniqueShmName(suffix),
                                 .channel_name = "book_ticker_channel",
                                 .create = true,
                                 .remove_existing = true};
}

md::TradeShmConfig MakeTradeCreateConfig(std::string_view suffix) {
  return md::TradeShmConfig{.enabled = true,
                            .shm_name = UniqueShmName(suffix),
                            .channel_name = "trade_channel",
                            .create = true,
                            .remove_existing = true};
}

struct NoopBookTickerMetadataPolicy {
  static constexpr bool kEnabled = false;
  explicit NoopBookTickerMetadataPolicy(
      const md::BookTickerFusionConfig& /*config*/) noexcept {}
  [[nodiscard]] bool Flush() noexcept { return true; }
};

struct NoopTradeMetadataPolicy {
  static constexpr bool kEnabled = false;
  explicit NoopTradeMetadataPolicy(
      const md::TradeFusionConfig& /*config*/) noexcept {}
  [[nodiscard]] bool Flush() noexcept { return true; }
};

void BM_BookTickerFusionCoreOnRecord(benchmark::State& state) {
  md::BookTickerFusionCore fusion(/*max_symbol_id=*/1024);
  std::int64_t id = 1;
  for (auto _ : state) {
    const aquila::BookTicker ticker = MakeBookTicker(id++);
    const auto decision =
        fusion.OnBookTicker(/*source_id=*/0, ticker, ticker.local_ns + 100);
    benchmark::DoNotOptimize(decision);
  }
}

void BM_TradeFusionCoreOnRecord(benchmark::State& state) {
  md::TradeFusionCore fusion(/*max_symbol_id=*/1024);
  std::int64_t id = 1;
  for (auto _ : state) {
    const aquila::Trade trade = MakeTrade(id++);
    const auto decision =
        fusion.OnTrade(/*source_id=*/0, trade, trade.local_ns + 100);
    benchmark::DoNotOptimize(decision);
  }
}

void BM_BookTickerFusionRunnerPollOnceNoopMetadata(benchmark::State& state) {
  const md::BookTickerShmConfig source = MakeBookTickerCreateConfig("bt_src");
  const md::BookTickerShmConfig output = MakeBookTickerCreateConfig("bt_out");
  ShmCleanup source_cleanup(source.shm_name);
  ShmCleanup output_cleanup(output.shm_name);
  md::DataShmPublisher source_publisher(source);
  md::BasicBookTickerFusionRunner<NoopBookTickerMetadataPolicy> runner(
      md::BookTickerFusionConfig{
          .name = "book_ticker_runner_benchmark",
          .max_events_per_source = 1,
          .bind_cpu_id = -1,
          .max_symbol_id = 1024,
          .output = md::BookTickerFusionOutputConfig{
              .shm_name = output.shm_name,
              .channel_name = output.channel_name,
              .remove_existing = true,
              .metadata_bin = {},
          },
          .sources = {md::BookTickerFusionSourceConfig{
              .source_id = 0,
              .name = "source",
              .shm_name = source.shm_name,
              .channel_name = source.channel_name,
          }},
      });
  std::int64_t id = 1;
  for (auto _ : state) {
    source_publisher.OnBookTicker(MakeBookTicker(id++));
    const auto stats = runner.PollOnce();
    benchmark::DoNotOptimize(stats);
  }
}

void BM_TradeFusionRunnerPollOnceNoopMetadata(benchmark::State& state) {
  const md::TradeShmConfig source = MakeTradeCreateConfig("tr_src");
  const md::TradeShmConfig output = MakeTradeCreateConfig("tr_out");
  ShmCleanup source_cleanup(source.shm_name);
  ShmCleanup output_cleanup(output.shm_name);
  md::DataShmPublisher source_publisher(source);
  md::BasicTradeFusionRunner<NoopTradeMetadataPolicy> runner(
      md::TradeFusionConfig{
          .name = "trade_runner_benchmark",
          .max_events_per_source = 1,
          .bind_cpu_id = -1,
          .max_symbol_id = 1024,
          .output = md::TradeFusionOutputConfig{
              .shm_name = output.shm_name,
              .channel_name = output.channel_name,
              .remove_existing = true,
              .metadata_bin = {},
          },
          .sources = {md::TradeFusionSourceConfig{
              .source_id = 0,
              .name = "source",
              .shm_name = source.shm_name,
              .channel_name = source.channel_name,
          }},
      });
  std::int64_t id = 1;
  for (auto _ : state) {
    source_publisher.OnTrade(MakeTrade(id++));
    const auto stats = runner.PollOnce();
    benchmark::DoNotOptimize(stats);
  }
}

BENCHMARK(BM_BookTickerFusionCoreOnRecord)
    ->Name("fusion/book_ticker_core_on_record")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TradeFusionCoreOnRecord)
    ->Name("fusion/trade_core_on_record")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_BookTickerFusionRunnerPollOnceNoopMetadata)
    ->Name("fusion/book_ticker_runner_poll_once_noop_metadata")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TradeFusionRunnerPollOnceNoopMetadata)
    ->Name("fusion/trade_runner_poll_once_noop_metadata")
    ->Unit(benchmark::kNanosecond);

}  // namespace
```

- [ ] **Step 3: Build and run baseline**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target fastest_route_fusion_benchmark -j8
mkdir -p /home/liuxiang/tmp/fusion_refactor_perf
build/release/benchmark/core/market_data/fastest_route_fusion_benchmark \
  --benchmark_min_time=0.2s \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=false \
  --benchmark_out=/home/liuxiang/tmp/fusion_refactor_perf/baseline.json \
  --benchmark_out_format=json
```

Expected: benchmark exits `0` and writes `/home/liuxiang/tmp/fusion_refactor_perf/baseline.json`.

- [ ] **Step 4: Commit benchmark baseline support**

Run:

```bash
git add benchmark/core/market_data/CMakeLists.txt benchmark/core/market_data/fastest_route_fusion_benchmark.cpp
git commit -m "benchmark: add fastest route fusion benchmark"
```

## Task 2: RED Tests for Metadata ABI v2

**Files:**
- Modify: `test/core/market_data/book_ticker_fusion_test.cpp`
- Modify: `test/core/market_data/trade_fusion_test.cpp`
- Modify: `test/core/market_data/book_ticker_fusion_metadata_test.cpp`
- Modify: `test/core/market_data/trade_fusion_metadata_test.cpp`
- Modify: `test/core/market_data/book_ticker_fusion_runner_test.cpp`
- Modify: `test/core/market_data/trade_fusion_runner_test.cpp`
- Modify: `scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py`

- [ ] **Step 1: Update C++ expectations to shared decision fields**

Replace `decision.book_ticker_id` and `decision.trade_id` expectations with:

```cpp
EXPECT_EQ(decision.record_id, source.id);
```

Replace drop-decision checks with:

```cpp
EXPECT_EQ(decision.record_id, 0);
```

- [ ] **Step 2: Update metadata writer tests to ABI v2**

Use `record_id` and `event_ns` in both metadata tests. Add:

```cpp
static_assert(sizeof(aquila::market_data::FusionMetadataRecord) == 48);
static_assert(sizeof(aquila::market_data::TradeFusionMetadataRecord) == 48);
```

BookTicker expected records set `.event_ns = exchange_ns`; Trade expected records set `.event_ns = trade_ns`.

- [ ] **Step 3: Update runner metadata assertions**

BookTicker runner metadata assertions should read:

```cpp
EXPECT_EQ(metadata[0].record_id, source0_first.id);
EXPECT_EQ(metadata[0].event_ns, source0_first.exchange_ns);
```

Trade runner metadata assertions should read:

```cpp
EXPECT_EQ(metadata[0].record_id, source0_first.id);
EXPECT_EQ(metadata[0].event_ns, source0_first.trade_ns);
```

- [ ] **Step 4: Update Python dtype test to ABI v2**

In `scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py`, expect:

```python
self.assertEqual(dtype.itemsize, 48)
self.assertEqual(
    dtype.names,
    (
        "source_id",
        "symbol_id",
        "record_id",
        "exchange_ns",
        "event_ns",
        "source_local_ns",
        "fusion_publish_ns",
    ),
)
self.assertEqual(dtype.fields["record_id"][1], 8)
self.assertEqual(dtype.fields["event_ns"][1], 24)
self.assertEqual(dtype.fields["fusion_publish_ns"][1], 40)
```

Replace test fixture writes from `book_ticker_id` to `record_id`, and set `event_ns = exchange_ns`.

- [ ] **Step 5: Verify RED**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_book_ticker_fusion_test \
  core_market_data_trade_fusion_test \
  core_market_data_book_ticker_fusion_metadata_test \
  core_market_data_trade_fusion_metadata_test \
  core_market_data_book_ticker_fusion_runner_test \
  core_market_data_trade_fusion_runner_test -j8
ctest --test-dir build/debug -R '(book_ticker_fusion|trade_fusion)' --output-on-failure
python3 scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
```

Expected: compile or test failure referencing missing `record_id` / `event_ns` fields and old 40-byte Python dtype.

## Task 3: Implement Shared Config, Decision, Metadata, and Policies

**Files:**
- Create: `core/market_data/fusion_config.h`
- Create: `core/market_data/fusion_metadata.h`
- Create: `core/market_data/fusion_metadata_policy.h`
- Modify: `core/market_data/book_ticker_fusion_config.h`
- Modify: `core/market_data/trade_fusion_config.h`
- Modify: `core/market_data/book_ticker_fusion.h`
- Modify: `core/market_data/trade_fusion.h`
- Modify: `core/market_data/book_ticker_fusion_metadata.h`
- Modify: `core/market_data/trade_fusion_metadata.h`
- Modify: `core/market_data/book_ticker_fusion_metadata_policy.h`
- Modify: `core/market_data/trade_fusion_metadata_policy.h`
- Modify: `core/market_data/CMakeLists.txt`

- [ ] **Step 1: Add common config header**

Create `core/market_data/fusion_config.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aquila::market_data {

struct FusionSourceConfig {
  std::int32_t source_id{-1};
  std::string name;
  std::string shm_name;
  std::string channel_name;
};

struct FusionOutputConfig {
  std::string shm_name;
  std::string channel_name;
  bool remove_existing{false};
  std::filesystem::path metadata_bin;
};

template <typename SourceConfig, typename OutputConfig>
struct BasicFusionConfig {
  std::string name;
  std::uint32_t max_events_per_source{1};
  std::int32_t bind_cpu_id{-1};
  std::uint32_t max_symbol_id{4096};
  OutputConfig output;
  std::vector<SourceConfig> sources;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_
```

Then make BookTicker / Trade config headers expose aliases:

```cpp
using BookTickerFusionSourceConfig = FusionSourceConfig;
using BookTickerFusionOutputConfig = FusionOutputConfig;
using BookTickerFusionConfig =
    BasicFusionConfig<BookTickerFusionSourceConfig,
                      BookTickerFusionOutputConfig>;
```

and:

```cpp
using TradeFusionSourceConfig = FusionSourceConfig;
using TradeFusionOutputConfig = FusionOutputConfig;
using TradeFusionConfig =
    BasicFusionConfig<TradeFusionSourceConfig, TradeFusionOutputConfig>;
```

- [ ] **Step 2: Add unified metadata header**

Create `core/market_data/fusion_metadata.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_

#include <cstdint>
#include <type_traits>

#include "core/market_data/fusion_metadata_writer.h"

namespace aquila::market_data {

struct FusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t record_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t event_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

static_assert(sizeof(FusionMetadataRecord) == 48);
static_assert(std::is_standard_layout_v<FusionMetadataRecord>);
static_assert(std::is_trivially_copyable_v<FusionMetadataRecord>);

using FusionMetadataWriter = BasicFusionMetadataWriter<FusionMetadataRecord>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
```

Make `book_ticker_fusion_metadata.h` include this header. Make `trade_fusion_metadata.h` include it and add:

```cpp
using TradeFusionMetadataRecord = FusionMetadataRecord;
using TradeFusionMetadataWriter = FusionMetadataWriter;
```

- [ ] **Step 3: Replace feed-specific decisions with shared decision**

In `book_ticker_fusion.h` add:

```cpp
using BookTickerFusionDecision = FastestRouteFusionDecision;
```

Return `core_.OnRecord(...)` directly from `OnBookTicker()`. Do the same in `trade_fusion.h`:

```cpp
using TradeFusionDecision = FastestRouteFusionDecision;
```

Return `core_.OnRecord(...)` directly from `OnTrade()`.

- [ ] **Step 4: Add event timestamp traits**

Add to `BookTickerFusionTraits`:

```cpp
[[nodiscard]] static std::int64_t EventNs(
    const BookTicker& ticker) noexcept {
  return ticker.exchange_ns;
}
```

Add to `TradeFusionTraits`:

```cpp
[[nodiscard]] static std::int64_t EventNs(const Trade& trade) noexcept {
  return trade.trade_ns;
}
```

- [ ] **Step 5: Add generic metadata policy**

Create `core/market_data/fusion_metadata_policy.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_POLICY_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_POLICY_H_

#include "core/market_data/fastest_route_fusion.h"
#include "core/market_data/fusion_metadata.h"

namespace aquila::market_data {

template <typename Traits>
class FileFusionMetadataPolicy {
 public:
  using Config = typename Traits::Config;
  using Record = typename Traits::Record;
  static constexpr bool kEnabled = true;

  explicit FileFusionMetadataPolicy(const Config& config)
      : writer_(config.output.metadata_bin) {}

  [[nodiscard]] bool Write(const FastestRouteFusionDecision& decision,
                           const Record& record) noexcept {
    const FusionMetadataRecord metadata{
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .record_id = decision.record_id,
        .exchange_ns = record.exchange_ns,
        .event_ns = Traits::EventNs(record),
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
    return writer_.Write(metadata);
  }

  [[nodiscard]] bool Flush() noexcept { return writer_.Flush(); }

 private:
  FusionMetadataWriter writer_;
};

template <typename Config>
class NoopFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopFusionMetadataPolicy(const Config& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept { return true; }
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_POLICY_H_
```

Use aliases in feed-specific metadata policy headers:

```cpp
using FileBookTickerFusionMetadataPolicy =
    FileFusionMetadataPolicy<BookTickerFusionTraits>;
using NoopBookTickerFusionMetadataPolicy =
    NoopFusionMetadataPolicy<BookTickerFusionConfig>;
```

and:

```cpp
using FileTradeFusionMetadataPolicy =
    FileFusionMetadataPolicy<TradeFusionTraits>;
using NoopTradeFusionMetadataPolicy =
    NoopFusionMetadataPolicy<TradeFusionConfig>;
```

- [ ] **Step 6: Verify green for core and metadata tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_book_ticker_fusion_test \
  core_market_data_trade_fusion_test \
  core_market_data_book_ticker_fusion_metadata_test \
  core_market_data_trade_fusion_metadata_test -j8
ctest --test-dir build/debug -R '(core_market_data_book_ticker_fusion_test|core_market_data_trade_fusion_test|core_market_data_book_ticker_fusion_metadata_test|core_market_data_trade_fusion_metadata_test)' --output-on-failure
```

Expected: all listed tests pass.

## Task 4: Deduplicate Runner Traits Without Touching Poll Loop

**Files:**
- Modify: `core/market_data/fastest_route_fusion_runner.h`
- Modify: `core/market_data/book_ticker_fusion_runner.h`
- Modify: `core/market_data/trade_fusion_runner.h`
- Modify: `test/core/market_data/book_ticker_fusion_runner_test.cpp`
- Modify: `test/core/market_data/trade_fusion_runner_test.cpp`
- Modify: `test/core/market_data/book_ticker_fusion_thread_test.cpp`
- Modify: `test/core/market_data/trade_fusion_thread_test.cpp`

- [ ] **Step 1: Add shared runner traits helper**

Add this helper above `BasicFastestRouteFusionRunner` in `fastest_route_fusion_runner.h`:

```cpp
template <typename FeedTraits>
struct BasicFusionRunnerTraits {
  using Config = typename FeedTraits::Config;
  using Core = typename FeedTraits::Core;
  using Decision = typename FeedTraits::Decision;
  using Record = typename FeedTraits::Record;
  using Reader = typename FeedTraits::Reader;
  using ShmConfig = typename FeedTraits::ShmConfig;
  using SourceConfig = typename FeedTraits::SourceConfig;
  using OutputConfig = typename FeedTraits::OutputConfig;

  [[nodiscard]] static ShmConfig MakeSourceShmConfig(
      const SourceConfig& source) {
    return ShmConfig{.enabled = true,
                     .shm_name = source.shm_name,
                     .channel_name = source.channel_name,
                     .create = false,
                     .remove_existing = false};
  }

  [[nodiscard]] static ShmConfig MakeOutputShmConfig(
      const OutputConfig& output) {
    return ShmConfig{.enabled = true,
                     .shm_name = output.shm_name,
                     .channel_name = output.channel_name,
                     .create = true,
                     .remove_existing = output.remove_existing};
  }

  [[nodiscard]] static Decision OnRecord(
      Core& core, std::int32_t source_id, const Record& record,
      std::int64_t fusion_publish_ns) noexcept {
    return FeedTraits::OnRecord(core, source_id, record, fusion_publish_ns);
  }

  static void SetLocalNs(Record* record, std::int64_t local_ns) noexcept {
    record->local_ns = local_ns;
  }

  static void Publish(DataShmPublisher& publisher,
                      const Record& record) noexcept {
    FeedTraits::Publish(publisher, record);
  }
};
```

- [ ] **Step 2: Shrink BookTicker runner header**

Keep `BookTickerFusionRunnerTraits` as:

```cpp
struct BookTickerFusionRunnerFeedTraits {
  using Config = BookTickerFusionConfig;
  using Core = BookTickerFusionCore;
  using Decision = BookTickerFusionDecision;
  using Record = BookTicker;
  using Reader = BookTickerShmReader;
  using ShmConfig = BookTickerShmConfig;
  using SourceConfig = BookTickerFusionSourceConfig;
  using OutputConfig = BookTickerFusionOutputConfig;

  [[nodiscard]] static Decision OnRecord(
      Core& core, std::int32_t source_id, const BookTicker& ticker,
      std::int64_t fusion_publish_ns) noexcept {
    return core.OnBookTicker(source_id, ticker, fusion_publish_ns);
  }

  static void Publish(DataShmPublisher& publisher,
                      const BookTicker& ticker) noexcept {
    publisher.OnBookTicker(ticker);
  }
};

using BookTickerFusionRunnerTraits =
    BasicFusionRunnerTraits<BookTickerFusionRunnerFeedTraits>;
```

- [ ] **Step 3: Shrink Trade runner header**

Keep `TradeFusionRunnerTraits` as:

```cpp
struct TradeFusionRunnerFeedTraits {
  using Config = TradeFusionConfig;
  using Core = TradeFusionCore;
  using Decision = TradeFusionDecision;
  using Record = Trade;
  using Reader = TradeShmReader;
  using ShmConfig = TradeShmConfig;
  using SourceConfig = TradeFusionSourceConfig;
  using OutputConfig = TradeFusionOutputConfig;

  [[nodiscard]] static Decision OnRecord(
      Core& core, std::int32_t source_id, const Trade& trade,
      std::int64_t fusion_publish_ns) noexcept {
    return core.OnTrade(source_id, trade, fusion_publish_ns);
  }

  static void Publish(DataShmPublisher& publisher,
                      const Trade& trade) noexcept {
    publisher.OnTrade(trade);
  }
};

using TradeFusionRunnerTraits =
    BasicFusionRunnerTraits<TradeFusionRunnerFeedTraits>;
```

- [ ] **Step 4: Verify runner and thread behavior**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_book_ticker_fusion_runner_test \
  core_market_data_trade_fusion_runner_test \
  core_market_data_book_ticker_fusion_thread_test \
  core_market_data_trade_fusion_thread_test -j8
ctest --test-dir build/debug -R '(fusion_runner|fusion_thread)' --output-on-failure
```

Expected: runner and thread tests pass; metadata file size assertions use `sizeof(FusionMetadataRecord)` / `sizeof(TradeFusionMetadataRecord)` and both are 48.

## Task 5: Update Python Analyzer and Docs

**Files:**
- Modify: `scripts/market_data/analyze_book_ticker_fusion_latency.py`
- Modify: `scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/gate_fastest_route_fusion_design.md`
- Modify: `docs/trade_fastest_route_fusion_design.md`

- [ ] **Step 1: Update Python dtype and field access**

Change `fusion_metadata_dtype()` to:

```python
def fusion_metadata_dtype() -> np.dtype:
    return np.dtype(
        {
            "names": [
                "source_id",
                "symbol_id",
                "record_id",
                "exchange_ns",
                "event_ns",
                "source_local_ns",
                "fusion_publish_ns",
            ],
            "formats": ["<i4", "<i4", "<i8", "<i8", "<i8", "<i8", "<i8"],
            "offsets": [0, 4, 8, 16, 24, 32, 40],
            "itemsize": 48,
        }
    )
```

Replace every metadata access of `book_ticker_id` with `record_id`. Do not use `event_ns` for BookTicker latency math; continue using `exchange_ns` so published-fusion summaries keep the same meaning.

- [ ] **Step 2: Update docs**

In `docs/diagnostic_fields.md`, replace the two feed-specific sidecar rows with one row:

```markdown
| `source_id` / `symbol_id` / `record_id` / `exchange_ns` / `event_ns` / `source_local_ns` / `fusion_publish_ns` | BookTicker / Trade `FusionMetadataRecord` sidecar binary v2 | stable | id / ns | 记录 canonical record 由哪个 source 首先发布；BookTicker `record_id=BookTicker.id` 且 `event_ns=exchange_ns`，Trade `record_id=Trade.id` 且 `event_ns=trade_ns`。 | metadata binary schema 被替换后同步更新。 |
```

Update the struct snippets in `docs/gate_fastest_route_fusion_design.md` and `docs/trade_fastest_route_fusion_design.md` to the ABI v2 struct.

- [ ] **Step 3: Verify script test**

Run:

```bash
python3 scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
```

Expected: Python unittest passes.

## Task 6: Full Regression and Performance Comparison

**Files:**
- No new files required unless a failure reveals a missing scoped fix.

- [ ] **Step 1: Debug regression tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  core_market_data_book_ticker_fusion_test \
  core_market_data_trade_fusion_test \
  core_market_data_book_ticker_fusion_metadata_test \
  core_market_data_trade_fusion_metadata_test \
  core_market_data_book_ticker_fusion_runner_test \
  core_market_data_trade_fusion_runner_test \
  core_market_data_book_ticker_fusion_thread_test \
  core_market_data_trade_fusion_thread_test \
  book_ticker_fusion_config_test \
  trade_fusion_config_test \
  book_ticker_fusion_cli_test \
  trade_fusion_cli_test -j8
ctest --test-dir build/debug -R '(book_ticker_fusion|trade_fusion|fusion_config)' --output-on-failure
python3 scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
```

Expected: all commands exit `0`.

- [ ] **Step 2: Static and diff checks**

Run:

```bash
git diff --check
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: `git diff --check` exits `0`; evaluation boundary checks have no matches.

- [ ] **Step 3: Release benchmark after refactor**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target fastest_route_fusion_benchmark -j8
build/release/benchmark/core/market_data/fastest_route_fusion_benchmark \
  --benchmark_min_time=0.2s \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=false \
  --benchmark_out=/home/liuxiang/tmp/fusion_refactor_perf/refactor.json \
  --benchmark_out_format=json
```

Expected: benchmark exits `0` and writes `/home/liuxiang/tmp/fusion_refactor_perf/refactor.json`.

- [ ] **Step 4: Compare benchmark JSON**

Run a short Python comparison:

```bash
python3 - <<'PY'
import json
from pathlib import Path

base = json.loads(Path("/home/liuxiang/tmp/fusion_refactor_perf/baseline.json").read_text())
new = json.loads(Path("/home/liuxiang/tmp/fusion_refactor_perf/refactor.json").read_text())

def means(doc):
    result = {}
    for item in doc["benchmarks"]:
        name = item["name"]
        if name.endswith("_mean"):
            result[name[:-5]] = float(item["real_time"])
    return result

base_means = means(base)
new_means = means(new)
for name in sorted(base_means):
    before = base_means[name]
    after = new_means[name]
    delta_pct = (after - before) / before * 100.0 if before else 0.0
    print(f"{name}: before={before:.3f}ns after={after:.3f}ns delta={delta_pct:.2f}%")
PY
```

Expected: no reproducible hot-path regression. If any benchmark regresses materially, rerun once; if the regression repeats, stop and optimize before committing completion.

- [ ] **Step 5: Commit implementation**

Run:

```bash
git status --short --branch
git add core/market_data test/core/market_data scripts/market_data scripts/test/market_data docs/diagnostic_fields.md docs/gate_fastest_route_fusion_design.md docs/trade_fastest_route_fusion_design.md benchmark/core/market_data
git commit -m "refactor: deduplicate market data fusion"
```

Expected: commit contains only scoped fusion refactor, tests, analyzer, docs, and benchmark files.

## Self-Review Checklist

- Spec coverage: plan covers `core/market_data` dedup, metadata ABI v2, Python analyzer, docs, and performance evidence.
- Scope guard: plan does not restructure `core/config/*_fusion_config.cpp` or `tools/market_data/data_fusion_tool_support.h`.
- Hot path guard: plan does not change `BasicFastestRouteFusionRunner::PollOnce()` control flow.
- TDD guard: production code starts only after tests are changed and expected to fail.
- Performance guard: benchmark is added and run before refactor, then rerun after refactor with JSON artifacts in `/home/liuxiang/tmp`.
