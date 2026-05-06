#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "core/market_data/data_shm.h"

namespace {

namespace md = aquila::market_data;

constexpr std::uint64_t kReaderPrefillMessages = 4096;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_data_shm_bench_{}_{}", ::getpid(), suffix);
}

md::BookTickerShmConfig MakeCreateConfig(std::string_view suffix) {
  return md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .channel_name = "book_ticker_channel",
      .create = true,
      .remove_existing = true,
  };
}

md::BookTickerShmConfig MakeAttachConfig(
    const md::BookTickerShmConfig& create_config) {
  md::BookTickerShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

void AssignBookTicker(std::int64_t id, aquila::BookTicker& out) noexcept {
  out.id = id;
  out.symbol_id = 42;
  out.exchange = aquila::Exchange::kGate;
  out.exchange_ns = 1'770'000'000'000'000'000 + id;
  out.local_ns = 1'770'000'000'000'100'000 + id;
  out.bid_price = 65'000.0 + static_cast<double>(id);
  out.bid_volume = 10.0 + static_cast<double>(id);
  out.ask_price = 65'001.0 + static_cast<double>(id);
  out.ask_volume = 11.0 + static_cast<double>(id);
}

aquila::BookTicker MakeBookTicker(std::int64_t id) {
  aquila::BookTicker book_ticker{};
  AssignBookTicker(id, book_ticker);
  return book_ticker;
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
[[gnu::noinline]] void PrefillVisibleWindow(md::DataShmPublisher* publisher) {
  for (std::uint64_t id = 0; id < kReaderPrefillMessages; ++id) {
    publisher->OnBookTicker(MakeBookTicker(static_cast<std::int64_t>(id)));
  }
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void BM_DataShmPublisherOnBookTicker(benchmark::State& state) {
  const md::BookTickerShmConfig config = MakeCreateConfig("publisher");
  ShmCleanup cleanup(config.shm_name);
  md::DataShmPublisher publisher(config);
  std::int64_t id = 1;

  for (auto _ : state) {
    aquila::BookTicker book_ticker{};
    AssignBookTicker(id, book_ticker);
    publisher.OnBookTicker(book_ticker);
    ++id;
  }
  state.counters["published"] =
      static_cast<double>(publisher.published_count());
}

void BM_DataShmPublisherEmplaceBookTickerWith(benchmark::State& state) {
  const md::BookTickerShmConfig config = MakeCreateConfig("publisher_emplace");
  ShmCleanup cleanup(config.shm_name);
  md::DataShmPublisher publisher(config);
  std::int64_t id = 1;

  for (auto _ : state) {
    benchmark::DoNotOptimize(id);
    publisher.EmplaceBookTickerWith(
        [&id](aquila::BookTicker& out) noexcept { AssignBookTicker(id, out); });
    ++id;
  }
  state.counters["published"] =
      static_cast<double>(publisher.published_count());
}

void BM_BookTickerShmReaderTryReadOne(benchmark::State& state) {
  const md::BookTickerShmConfig config = MakeCreateConfig("reader");
  ShmCleanup cleanup(config.shm_name);
  md::DataShmPublisher publisher(config);
  PrefillVisibleWindow(&publisher);

  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekEarliestVisible();
  aquila::BookTicker book_ticker{};

  for (auto _ : state) {
    if (!reader.TryReadOne(&book_ticker)) {
      state.PauseTiming();
      reader.SeekEarliestVisible();
      state.ResumeTiming();
      (void)reader.TryReadOne(&book_ticker);
    }
    benchmark::DoNotOptimize(book_ticker);
  }
}

BENCHMARK(BM_DataShmPublisherOnBookTicker)
    ->Name("data_shm/publisher_temp_book_ticker_push")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_DataShmPublisherEmplaceBookTickerWith)
    ->Name("data_shm/publisher_emplace_book_ticker_with")
    ->Unit(benchmark::kNanosecond);
BENCHMARK(BM_BookTickerShmReaderTryReadOne);

}  // namespace
