#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/historical_data_reader.h"
#include "core/market_data/realtime_data_reader.h"

namespace {

namespace cfg = aquila::config;
namespace md = aquila::market_data;

constexpr std::uint64_t kHistoricalRecords = 1'048'576;

struct NoopHandler {
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    ++count;
    last_id = book_ticker.id;
  }

  std::uint64_t count{0};
  std::int64_t last_id{0};
};

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

class TempDir {
 public:
  TempDir()
      : path_(std::filesystem::temp_directory_path() /
              fmt::format("aquila_data_reader_benchmark_{}_{}", ::getpid(),
                          next_id_++)) {
    std::filesystem::create_directories(path_);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path File(std::string_view name) const {
    return path_ / std::string{name};
  }

 private:
  std::filesystem::path path_;
  inline static std::uint32_t next_id_{0};
};

std::string UniqueShmName(std::string_view suffix, std::size_t index) {
  return fmt::format("/aquila_data_reader_bench_{}_{}_{}", ::getpid(), suffix,
                     index);
}

md::BookTickerShmConfig MakeCreateConfig(std::string_view suffix,
                                         std::size_t index) {
  return md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix, index),
      .channel_name = "book_ticker_channel",
      .create = true,
      .remove_existing = true,
  };
}

cfg::DataReaderSourceConfig MakeRealtimeSourceConfig(
    std::string name, aquila::Exchange exchange, std::string shm_name,
    cfg::DataReaderReadMode read_mode) {
  return cfg::DataReaderSourceConfig{
      .name = std::move(name),
      .type = cfg::DataReaderSourceType::kShm,
      .exchange = exchange,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = std::move(shm_name),
      .channel_name = "book_ticker_channel",
      .start_position = cfg::DataReaderStartPosition::kLatest,
      .read_mode = read_mode,
      .required = true,
  };
}

aquila::BookTicker MakeBookTicker(std::int64_t id) noexcept {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = static_cast<std::int32_t>(id & 0x7),
      .exchange = aquila::Exchange::kGate,
      .exchange_ns = 1'770'000'000'000'000'000 + id,
      .local_ns = 1'770'000'000'000'100'000 + id,
      .bid_price = 65'000.0 + static_cast<double>(id),
      .bid_volume = 10.0 + static_cast<double>(id),
      .ask_price = 65'001.0 + static_cast<double>(id),
      .ask_volume = 11.0 + static_cast<double>(id),
  };
}

void WriteBookTickerFile(const std::filesystem::path& path,
                         std::uint64_t record_count) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error(fmt::format("failed to open {}", path.string()));
  }
  for (std::uint64_t i = 0; i < record_count; ++i) {
    const aquila::BookTicker book_ticker =
        MakeBookTicker(static_cast<std::int64_t>(i));
    output.write(reinterpret_cast<const char*>(&book_ticker),
                 sizeof(book_ticker));
  }
  if (!output.good()) {
    throw std::runtime_error(fmt::format("failed to write {}", path.string()));
  }
}

cfg::DataReaderConfig MakeHistoricalConfig(const std::filesystem::path& file) {
  cfg::DataReaderConfig config;
  config.name = "historical_reader_benchmark";
  config.max_events_per_source = 4096;
  config.sources.push_back(cfg::DataReaderSourceConfig{
      .name = "historical_book_ticker",
      .type = cfg::DataReaderSourceType::kBinaryFile,
      .exchange = aquila::Exchange::kGate,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = {},
      .channel_name = {},
      .files = {file},
      .start_position = cfg::DataReaderStartPosition::kEarliestVisible,
      .read_mode = cfg::DataReaderReadMode::kDrain,
      .required = true,
  });
  return config;
}

void BM_RealtimeDataReaderEmptyPoll(benchmark::State& state) {
  const std::size_t source_count = static_cast<std::size_t>(state.range(0));
  std::vector<md::BookTickerShmConfig> shm_configs;
  std::vector<std::unique_ptr<ShmCleanup>> cleanups;
  std::vector<std::unique_ptr<md::DataShmPublisher>> publishers;
  shm_configs.reserve(source_count);
  cleanups.reserve(source_count);
  publishers.reserve(source_count);

  cfg::DataReaderConfig config;
  config.name = "realtime_reader_benchmark";
  config.max_events_per_source = 64;
  for (std::size_t i = 0; i < source_count; ++i) {
    shm_configs.push_back(MakeCreateConfig("empty_poll", i));
    cleanups.push_back(
        std::make_unique<ShmCleanup>(shm_configs.back().shm_name));
    publishers.push_back(
        std::make_unique<md::DataShmPublisher>(shm_configs.back()));
    config.sources.push_back(MakeRealtimeSourceConfig(
        fmt::format("source_{}", i), aquila::Exchange::kGate,
        shm_configs.back().shm_name, cfg::DataReaderReadMode::kLatest));
  }

  md::RealtimeDataReader reader(std::move(config));
  NoopHandler handler;

  for (auto _ : state) {
    benchmark::DoNotOptimize(reader.Poll(handler));
  }

  benchmark::DoNotOptimize(handler.count);
  state.SetItemsProcessed(state.iterations());
}

void BM_HistoricalDataReaderDrainSingleFile(benchmark::State& state) {
  const std::uint64_t drain_budget = static_cast<std::uint64_t>(state.range(0));
  TempDir temp_dir;
  const std::filesystem::path file = temp_dir.File("book_ticker.bin");
  WriteBookTickerFile(file, kHistoricalRecords);

  cfg::DataReaderConfig config = MakeHistoricalConfig(file);
  md::HistoricalDataReader reader(config);
  NoopHandler handler;
  std::uint64_t handled_total = 0;

  for (auto _ : state) {
    if (reader.finished()) {
      state.PauseTiming();
      reader = md::HistoricalDataReader(config);
      state.ResumeTiming();
    }
    const std::uint64_t handled = reader.Drain(handler, drain_budget);
    handled_total += handled;
  }

  benchmark::DoNotOptimize(handler.count);
  benchmark::DoNotOptimize(handler.last_id);
  benchmark::DoNotOptimize(handled_total);
  state.SetItemsProcessed(handled_total);
}

BENCHMARK(BM_RealtimeDataReaderEmptyPoll)
    ->Arg(1)
    ->Arg(2)
    ->Arg(4)
    ->Unit(benchmark::kNanosecond);

BENCHMARK(BM_HistoricalDataReaderDrainSingleFile)
    ->Arg(1)
    ->Arg(64)
    ->Arg(4096)
    ->Unit(benchmark::kNanosecond);

}  // namespace
