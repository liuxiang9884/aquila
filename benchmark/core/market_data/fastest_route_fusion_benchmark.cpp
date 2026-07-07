#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "core/common/types.h"
#include "core/market_data/book_ticker_fusion_runner.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/trade_fusion_runner.h"

namespace {

namespace md = aquila::market_data;

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

[[gnu::noinline]] void PublishTradeToShm(
    md::DataShmPublisher* publisher, const aquila::Trade& trade) noexcept {
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
