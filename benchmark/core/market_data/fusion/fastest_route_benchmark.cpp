#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "core/common/types.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/fusion/book_ticker.h"
#include "core/market_data/fusion/trade.h"

namespace {

namespace md = aquila::market_data;

constexpr int kMultiSourceCount = 4;
constexpr int kBatchSize = 8;
constexpr int kMaxSymbolId = 1024;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

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

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
[[gnu::noinline]] void PublishBookTickerToShm(
    md::DataShmPublisher* publisher,
    const aquila::BookTicker& book_ticker) noexcept {
  publisher->OnBookTicker(book_ticker);
}

[[gnu::noinline]] void PublishTradeToShm(md::DataShmPublisher* publisher,
                                         const aquila::Trade& trade) noexcept {
  publisher->OnTrade(trade);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

struct NoopBookTickerMetadataPolicy {
  static constexpr bool kEnabled = false;

  explicit NoopBookTickerMetadataPolicy(
      const md::BookTickerFusionConfig& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept {
    return true;
  }
};

struct NoopTradeMetadataPolicy {
  static constexpr bool kEnabled = false;

  explicit NoopTradeMetadataPolicy(
      const md::TradeFusionConfig& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept {
    return true;
  }
};

struct BookTickerBenchmarkFeed {
  using Config = md::BookTickerFusionConfig;
  using OutputConfig = md::BookTickerFusionOutputConfig;
  using Record = aquila::BookTicker;
  using ShmConfig = md::BookTickerShmConfig;
  using SourceConfig = md::BookTickerFusionSourceConfig;

  template <typename MetadataPolicy>
  using Runner = md::BasicBookTickerFusionRunner<MetadataPolicy>;

  [[nodiscard]] static ShmConfig MakeCreateConfig(std::string_view suffix) {
    return MakeBookTickerCreateConfig(suffix);
  }

  [[nodiscard]] static Record MakeRecord(std::int64_t id) noexcept {
    return MakeBookTicker(id);
  }

  static void Publish(md::DataShmPublisher* publisher,
                      const Record& record) noexcept {
    PublishBookTickerToShm(publisher, record);
  }
};

struct TradeBenchmarkFeed {
  using Config = md::TradeFusionConfig;
  using OutputConfig = md::TradeFusionOutputConfig;
  using Record = aquila::Trade;
  using ShmConfig = md::TradeShmConfig;
  using SourceConfig = md::TradeFusionSourceConfig;

  template <typename MetadataPolicy>
  using Runner = md::BasicTradeFusionRunner<MetadataPolicy>;

  [[nodiscard]] static ShmConfig MakeCreateConfig(std::string_view suffix) {
    return MakeTradeCreateConfig(suffix);
  }

  [[nodiscard]] static Record MakeRecord(std::int64_t id) noexcept {
    return MakeTrade(id);
  }

  static void Publish(md::DataShmPublisher* publisher,
                      const Record& record) noexcept {
    PublishTradeToShm(publisher, record);
  }
};

template <typename Decision>
std::int64_t DecisionRecordId(const Decision& decision) noexcept {
  if constexpr (requires { decision.record_id; }) {
    return decision.record_id;
  } else if constexpr (requires { decision.book_ticker_id; }) {
    return decision.book_ticker_id;
  } else {
    return decision.trade_id;
  }
}

void BM_BookTickerFusionCoreOnRecord(benchmark::State& state) {
  md::BookTickerFusionCore fusion(/*max_symbol_id=*/1024);
  std::int64_t id = 1;
  for (auto _ : state) {
    const aquila::BookTicker ticker = MakeBookTicker(id++);
    const auto decision =
        fusion.OnBookTicker(/*source_id=*/0, ticker, ticker.local_ns + 100);
    bool publish = decision.publish;
    std::int64_t record_id = DecisionRecordId(decision);
    benchmark::DoNotOptimize(publish);
    benchmark::DoNotOptimize(record_id);
  }
}

void BM_TradeFusionCoreOnRecord(benchmark::State& state) {
  md::TradeFusionCore fusion(/*max_symbol_id=*/1024);
  std::int64_t id = 1;
  for (auto _ : state) {
    const aquila::Trade trade = MakeTrade(id++);
    const auto decision =
        fusion.OnTrade(/*source_id=*/0, trade, trade.local_ns + 100);
    bool publish = decision.publish;
    std::int64_t record_id = DecisionRecordId(decision);
    benchmark::DoNotOptimize(publish);
    benchmark::DoNotOptimize(record_id);
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
          .output =
              md::BookTickerFusionOutputConfig{
                  .shm_name = output.shm_name,
                  .channel_name = output.channel_name,
                  .remove_existing = true,
                  .metadata_bin = {},
              },
          .sources =
              {
                  md::BookTickerFusionSourceConfig{
                      .source_id = 0,
                      .name = "source",
                      .shm_name = source.shm_name,
                      .channel_name = source.channel_name,
                  },
              },
      });
  std::int64_t id = 1;
  for (auto _ : state) {
    PublishBookTickerToShm(&source_publisher, MakeBookTicker(id++));
    const auto stats = runner.PollOnce();
    std::uint64_t read_count = stats.read_count;
    std::uint64_t published_count = stats.published_count;
    benchmark::DoNotOptimize(read_count);
    benchmark::DoNotOptimize(published_count);
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
          .output =
              md::TradeFusionOutputConfig{
                  .shm_name = output.shm_name,
                  .channel_name = output.channel_name,
                  .remove_existing = true,
                  .metadata_bin = {},
              },
          .sources =
              {
                  md::TradeFusionSourceConfig{
                      .source_id = 0,
                      .name = "source",
                      .shm_name = source.shm_name,
                      .channel_name = source.channel_name,
                  },
              },
      });
  std::int64_t id = 1;
  for (auto _ : state) {
    PublishTradeToShm(&source_publisher, MakeTrade(id++));
    const auto stats = runner.PollOnce();
    std::uint64_t read_count = stats.read_count;
    std::uint64_t published_count = stats.published_count;
    benchmark::DoNotOptimize(read_count);
    benchmark::DoNotOptimize(published_count);
  }
}

template <typename Feed, typename MetadataPolicy>
void RunMultiSourceBatchBenchmark(benchmark::State& state,
                                  std::string_view shm_prefix,
                                  std::filesystem::path metadata_bin) {
  std::vector<typename Feed::ShmConfig> sources;
  sources.reserve(kMultiSourceCount);
  std::vector<std::unique_ptr<ShmCleanup>> cleanups;
  cleanups.reserve(kMultiSourceCount + 1);
  std::vector<std::unique_ptr<md::DataShmPublisher>> source_publishers;
  source_publishers.reserve(kMultiSourceCount);

  typename Feed::Config config{
      .name = std::string(shm_prefix),
      .max_events_per_source = kBatchSize,
      .bind_cpu_id = -1,
      .max_symbol_id = kMaxSymbolId,
      .output =
          typename Feed::OutputConfig{
              .metadata_bin = std::move(metadata_bin),
          },
  };

  for (int source_index = 0; source_index < kMultiSourceCount; ++source_index) {
    sources.push_back(Feed::MakeCreateConfig(
        fmt::format("{}_src{}", shm_prefix, source_index)));
    const typename Feed::ShmConfig& source = sources.back();
    cleanups.push_back(std::make_unique<ShmCleanup>(source.shm_name));
    source_publishers.push_back(std::make_unique<md::DataShmPublisher>(source));
    config.sources.push_back(typename Feed::SourceConfig{
        .source_id = source_index,
        .name = fmt::format("source{}", source_index),
        .shm_name = source.shm_name,
        .channel_name = source.channel_name,
    });
  }

  const typename Feed::ShmConfig output =
      Feed::MakeCreateConfig(fmt::format("{}_out", shm_prefix));
  cleanups.push_back(std::make_unique<ShmCleanup>(output.shm_name));
  config.output.shm_name = output.shm_name;
  config.output.channel_name = output.channel_name;
  config.output.remove_existing = true;

  typename Feed::template Runner<MetadataPolicy> runner(config);
  std::int64_t id = 1;
  for (auto _ : state) {
    for (std::unique_ptr<md::DataShmPublisher>& source_publisher :
         source_publishers) {
      for (int batch_index = 0; batch_index < kBatchSize; ++batch_index) {
        Feed::Publish(source_publisher.get(), Feed::MakeRecord(id++));
      }
    }
    const auto stats = runner.PollOnce();
    std::uint64_t read_count = stats.read_count;
    std::uint64_t published_count = stats.published_count;
    std::uint64_t metadata_write_errors = stats.metadata_write_errors;
    benchmark::DoNotOptimize(read_count);
    benchmark::DoNotOptimize(published_count);
    benchmark::DoNotOptimize(metadata_write_errors);
  }
  state.SetItemsProcessed(state.iterations() * kMultiSourceCount * kBatchSize);
}

void BM_BookTickerFusionRunnerPollOnce4SourcesBatch8NoopMetadata(
    benchmark::State& state) {
  RunMultiSourceBatchBenchmark<BookTickerBenchmarkFeed,
                               NoopBookTickerMetadataPolicy>(
      state, "bt_4src_batch8_noop", {});
}

void BM_TradeFusionRunnerPollOnce4SourcesBatch8NoopMetadata(
    benchmark::State& state) {
  RunMultiSourceBatchBenchmark<TradeBenchmarkFeed, NoopTradeMetadataPolicy>(
      state, "tr_4src_batch8_noop", {});
}

void BM_BookTickerFusionRunnerPollOnce4SourcesBatch8FileMetadata(
    benchmark::State& state) {
  RunMultiSourceBatchBenchmark<BookTickerBenchmarkFeed,
                               md::FileBookTickerFusionMetadataPolicy>(
      state, "bt_4src_batch8_file", "/dev/null");
}

void BM_TradeFusionRunnerPollOnce4SourcesBatch8FileMetadata(
    benchmark::State& state) {
  RunMultiSourceBatchBenchmark<TradeBenchmarkFeed,
                               md::FileTradeFusionMetadataPolicy>(
      state, "tr_4src_batch8_file", "/dev/null");
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
BENCHMARK(BM_BookTickerFusionRunnerPollOnce4SourcesBatch8NoopMetadata)
    ->Name("fusion/book_ticker_runner_poll_once_4sources_batch8_noop_metadata")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TradeFusionRunnerPollOnce4SourcesBatch8NoopMetadata)
    ->Name("fusion/trade_runner_poll_once_4sources_batch8_noop_metadata")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_BookTickerFusionRunnerPollOnce4SourcesBatch8FileMetadata)
    ->Name("fusion/book_ticker_runner_poll_once_4sources_batch8_file_metadata")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_TradeFusionRunnerPollOnce4SourcesBatch8FileMetadata)
    ->Name("fusion/trade_runner_poll_once_4sources_batch8_file_metadata")
    ->Unit(benchmark::kNanosecond);

}  // namespace
