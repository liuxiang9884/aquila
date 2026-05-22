#include "monitor/market_data/market_data_thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "core/market_data/data_shm.h"

namespace aquila::monitor {
namespace {

[[nodiscard]] bool ContainsExchange(const std::vector<Exchange>& exchanges,
                                    Exchange exchange) noexcept {
  return std::find(exchanges.begin(), exchanges.end(), exchange) !=
         exchanges.end();
}

[[nodiscard]] bool ContainsKey(const std::vector<MarketDataKey>& keys,
                               Exchange exchange,
                               std::int32_t symbol_id) noexcept {
  for (const MarketDataKey& key : keys) {
    if (key.exchange == exchange && key.symbol_id == symbol_id) {
      return true;
    }
  }
  return false;
}

struct MonitorRealtimeReaderSourceStats {
  std::uint64_t overruns{0};
};

struct MonitorRealtimeReaderStats {
  std::vector<MonitorRealtimeReaderSourceStats> sources;
};

class MonitorRealtimeReaderDiagnostics {
 public:
  explicit MonitorRealtimeReaderDiagnostics(std::size_t source_count)
      : stats_{.sources =
                   std::vector<MonitorRealtimeReaderSourceStats>(source_count)} {}

  void RecordOverrun(std::size_t source_index,
                     std::uint64_t overrun_delta) noexcept {
    stats_.sources[source_index].overruns += overrun_delta;
  }

  [[nodiscard]] const MonitorRealtimeReaderStats& stats() const noexcept {
    return stats_;
  }

 private:
  MonitorRealtimeReaderStats stats_;
};

class MonitorRealtimeReader {
 public:
  MonitorRealtimeReader(
      config::DataReaderConfig* config,
      std::vector<MarketDataUnavailableSource>* unavailable_sources)
      : diagnostics_(0) {
    std::vector<config::DataReaderSourceConfig> active_sources;
    active_sources.reserve(config->sources.size());
    sources_.reserve(config->sources.size());

    for (const config::DataReaderSourceConfig& source_config : config->sources) {
      ValidateSource(source_config);
      market_data::BookTickerShmConfig shm_config{
          .enabled = true,
          .shm_name = source_config.shm_name,
          .channel_name = source_config.channel_name,
          .create = false,
          .remove_existing = false,
      };

      try {
        auto source =
            std::make_unique<Source>(source_config.read_mode, shm_config);
        if (source_config.start_position ==
            config::DataReaderStartPosition::kLatest) {
          source->reader.SeekLatest();
        } else {
          source->reader.SeekEarliestVisible();
        }
        sources_.push_back(std::move(source));
        active_sources.push_back(source_config);
      } catch (const std::exception& exc) {
        unavailable_sources->push_back(MarketDataUnavailableSource{
            .exchange = source_config.exchange,
            .name = source_config.name,
            .reason = exc.what(),
            .required = source_config.required,
        });
        if (source_config.required) {
          throw std::runtime_error(fmt::format(
              "required market data source '{}' unavailable: {}",
              source_config.name, exc.what()));
        }
      }
    }

    if (sources_.empty()) {
      throw std::runtime_error("no market data SHM sources available");
    }

    config->sources = std::move(active_sources);
    diagnostics_ = MonitorRealtimeReaderDiagnostics(sources_.size());
    if (sources_.size() > 1) {
      scan_sources_.reserve(sources_.size() * 2);
      scan_source_indices_.reserve(sources_.size() * 2);
      for (std::size_t repeat = 0; repeat < 2; ++repeat) {
        for (std::size_t i = 0; i < sources_.size(); ++i) {
          scan_sources_.push_back(sources_[i].get());
          scan_source_indices_.push_back(i);
        }
      }
    }
  }

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept {
    const std::size_t source_count = sources_.size();
    if (source_count == 0) {
      return 0;
    }
    if (source_count == 1) {
      return PollSource(0, *sources_[0], handler);
    }

    const std::size_t scan_end = next_source_index_ + source_count;
    for (std::size_t scan_position = next_source_index_;
         scan_position < scan_end; ++scan_position) {
      Source& source = *scan_sources_[scan_position];
      const std::size_t source_index = scan_source_indices_[scan_position];
      const std::uint64_t handled = PollSource(source_index, source, handler);
      if (handled != 0) {
        next_source_index_ = scan_source_indices_[scan_position + 1];
        return handled;
      }
    }
    return 0;
  }

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    std::uint64_t count = 0;
    while (count < max_events) {
      const std::uint64_t handled = Poll(handler);
      if (handled == 0) {
        break;
      }
      count += handled;
    }
    return count;
  }

  [[nodiscard]] const MonitorRealtimeReaderDiagnostics& diagnostics()
      const noexcept {
    return diagnostics_;
  }

 private:
  struct Source {
    Source(config::DataReaderReadMode read_mode_in,
           const market_data::BookTickerShmConfig& shm_config)
        : read_mode(read_mode_in), reader(shm_config) {}

    config::DataReaderReadMode read_mode{config::DataReaderReadMode::kLatest};
    market_data::BookTickerShmReader reader;
    std::uint64_t last_overrun_count{0};
  };

  static void ValidateSource(const config::DataReaderSourceConfig& source) {
    if (source.type != config::DataReaderSourceType::kShm) {
      throw std::invalid_argument(fmt::format(
          "monitor market data source '{}' must have type shm", source.name));
    }
    if (source.feed != config::DataReaderFeed::kBookTicker) {
      throw std::invalid_argument(fmt::format(
          "monitor market data source '{}' must use book_ticker feed",
          source.name));
    }
  }

  template <typename Handler>
  std::uint64_t PollSource(std::size_t source_index, Source& source,
                           Handler& handler) noexcept {
    switch (source.read_mode) {
      case config::DataReaderReadMode::kLatest:
        return PollLatestSource(source_index, source, handler);
      case config::DataReaderReadMode::kDrain:
        return PollDrainSource(source_index, source, handler);
    }
    return 0;
  }

  template <typename Handler>
  std::uint64_t PollLatestSource(std::size_t source_index, Source& source,
                                 Handler& handler) noexcept {
    BookTicker book_ticker;
    if (!source.reader.TryReadLatest(&book_ticker, nullptr)) {
      return 0;
    }
    RecordOverrun(source_index, source);
    handler.OnBookTicker(book_ticker);
    return 1;
  }

  template <typename Handler>
  std::uint64_t PollDrainSource(std::size_t source_index, Source& source,
                                Handler& handler) noexcept {
    BookTicker book_ticker;
    if (!source.reader.TryReadOne(&book_ticker)) {
      return 0;
    }
    RecordOverrun(source_index, source);
    handler.OnBookTicker(book_ticker);
    return 1;
  }

  void RecordOverrun(std::size_t source_index, Source& source) noexcept {
    const std::uint64_t overrun = source.reader.overrun_count();
    if (overrun > source.last_overrun_count) {
      diagnostics_.RecordOverrun(source_index,
                                 overrun - source.last_overrun_count);
      source.last_overrun_count = overrun;
    }
  }

  std::size_t next_source_index_{0};
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<Source*> scan_sources_;
  std::vector<std::size_t> scan_source_indices_;
  MonitorRealtimeReaderDiagnostics diagnostics_;
};

struct StoreBookTickerHandler {
  MarketDataStore* store{nullptr};

  void OnBookTicker(const BookTicker& ticker) noexcept {
    store->OnBookTicker(ticker);
  }
};

[[nodiscard]] std::uint64_t TotalOverrunCount(
    const MonitorRealtimeReader& reader) noexcept {
  std::uint64_t count = 0;
  for (const MonitorRealtimeReaderSourceStats& source :
       reader.diagnostics().stats().sources) {
    count += source.overruns;
  }
  return count;
}

void ForceSnapshotReadMode(config::DataReaderConfig* config) noexcept {
  for (config::DataReaderSourceConfig& source : config->sources) {
    source.start_position = config::DataReaderStartPosition::kEarliestVisible;
    source.read_mode = config::DataReaderReadMode::kDrain;
  }
}

[[nodiscard]] std::uint64_t SnapshotDrainBudget(
    const config::DataReaderConfig& config) noexcept {
  const std::uint64_t visible_window_budget =
      static_cast<std::uint64_t>(market_data::kBookTickerShmCapacity) *
      static_cast<std::uint64_t>(config.sources.size());
  return std::max<std::uint64_t>(config.max_events_per_drain,
                                 visible_window_budget);
}

}  // namespace

struct MarketDataThread::State {
  using RealtimeReader = MonitorRealtimeReader;

  State(std::filesystem::path config_path_in, MarketDataThreadQueue& queue_in)
      : config_path(std::move(config_path_in)), queue(&queue_in) {}

  void Run() noexcept {
    SteadyMarketDataClock clock;
    MarketDataPump<RealtimeReader, MarketDataThreadQueue, SteadyMarketDataClock>
        pump(*reader, *store, *queue, clock,
             MarketDataPumpOptions{.drain_budget = drain_budget,
                                   .publish_interval_ns = publish_interval_ns});

    while (!stop_requested.load(std::memory_order_relaxed)) {
      const MarketDataPumpResult result = pump.StepOnce();
      if (result.drained_count == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
  }

  std::filesystem::path config_path;
  MarketDataThreadQueue* queue{nullptr};
  std::unique_ptr<RealtimeReader> reader;
  std::unique_ptr<MarketDataStore> store;
  std::thread worker;
  std::atomic<bool> stop_requested{false};
  std::uint64_t drain_budget{64};
  std::int64_t publish_interval_ns{kMarketDataPublishIntervalNs};
  std::string last_error;
  std::vector<MarketDataUnavailableSource> unavailable_sources;
};

std::int64_t SteadyMarketDataClock::NowNs() const noexcept {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::vector<MarketDataKey> BuildMarketDataKeys(
    const config::DataReaderConfig& config) {
  std::vector<Exchange> source_exchanges;
  source_exchanges.reserve(config.sources.size());
  for (const config::DataReaderSourceConfig& source : config.sources) {
    if (!ContainsExchange(source_exchanges, source.exchange)) {
      source_exchanges.push_back(source.exchange);
    }
  }

  std::vector<MarketDataKey> keys;
  keys.reserve(config.instrument_catalog.instruments().size());
  for (const config::InstrumentInfo& instrument :
       config.instrument_catalog.instruments()) {
    if (!ContainsExchange(source_exchanges, instrument.exchange)) {
      continue;
    }
    if (ContainsKey(keys, instrument.exchange, instrument.symbol_id)) {
      continue;
    }
    keys.push_back(MarketDataKey{.exchange = instrument.exchange,
                                 .symbol_id = instrument.symbol_id});
  }
  return keys;
}

MarketDataSnapshotResult ReadMarketDataSnapshot(
    const std::filesystem::path& config_path, std::int64_t published_ns) {
  MarketDataSnapshotResult result;
  config::DataReaderConfigResult config_result =
      config::LoadDataReaderConfigFile(config_path);
  if (!config_result.ok) {
    result.error = std::move(config_result.error);
    return result;
  }

  ForceSnapshotReadMode(&config_result.value);
  try {
    MonitorRealtimeReader reader(&config_result.value,
                                 &result.unavailable_sources);
    std::vector<MarketDataKey> keys = BuildMarketDataKeys(config_result.value);
    MarketDataStore store(std::span<const MarketDataKey>{keys.data(),
                                                         keys.size()});
    StoreBookTickerHandler handler{.store = &store};
    reader.Drain(handler, SnapshotDrainBudget(config_result.value));
    const std::uint64_t overrun_count = TotalOverrunCount(reader);
    if (overrun_count != 0) {
      store.RecordOverrun(overrun_count);
    }
    result.batch = store.BuildChangedBatch(published_ns);
    result.ok = true;
  } catch (const std::exception& exc) {
    result.error = exc.what();
  } catch (...) {
    result.error = "unknown market data snapshot error";
  }
  return result;
}

MarketDataThread::MarketDataThread(std::filesystem::path config_path,
                                   MarketDataThreadQueue& queue)
    : state_(std::make_unique<State>(std::move(config_path), queue)) {}

MarketDataThread::~MarketDataThread() {
  Stop();
  Join();
}

bool MarketDataThread::Start() {
  if (state_->queue == nullptr) {
    state_->last_error = "market data queue is not set";
    return false;
  }
  if (state_->worker.joinable()) {
    state_->last_error = "market data thread is already running";
    return false;
  }

  state_->last_error.clear();
  state_->unavailable_sources.clear();
  config::DataReaderConfigResult config_result =
      config::LoadDataReaderConfigFile(state_->config_path);
  if (!config_result.ok) {
    state_->last_error = config_result.error;
    return false;
  }

  try {
    state_->drain_budget = config_result.value.max_events_per_drain;
    auto reader = std::make_unique<State::RealtimeReader>(
        &config_result.value, &state_->unavailable_sources);
    std::vector<MarketDataKey> keys = BuildMarketDataKeys(config_result.value);
    auto store = std::make_unique<MarketDataStore>(
        std::span<const MarketDataKey>{keys.data(), keys.size()});

    state_->store = std::move(store);
    state_->reader = std::move(reader);
    state_->stop_requested.store(false, std::memory_order_relaxed);
    state_->worker = std::thread([this] { state_->Run(); });
    return true;
  } catch (const std::exception& exc) {
    state_->last_error =
        fmt::format("failed to start market data thread: {}", exc.what());
  } catch (...) {
    state_->last_error = "failed to start market data thread: unknown error";
  }

  state_->reader.reset();
  state_->store.reset();
  return false;
}

void MarketDataThread::Stop() noexcept {
  state_->stop_requested.store(true, std::memory_order_relaxed);
}

void MarketDataThread::Join() {
  if (state_->worker.joinable()) {
    state_->worker.join();
  }
}

const std::string& MarketDataThread::last_error() const noexcept {
  return state_->last_error;
}

std::span<const MarketDataUnavailableSource>
MarketDataThread::unavailable_sources() const noexcept {
  return state_->unavailable_sources;
}

}  // namespace aquila::monitor
