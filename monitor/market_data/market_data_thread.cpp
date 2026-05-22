#include "monitor/market_data/market_data_thread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <span>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "core/market_data/realtime_data_reader.h"

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

}  // namespace

struct MarketDataThread::State {
  using RealtimeReader = market_data::RealtimeDataReader<
      market_data::RealtimeDataReaderDiagnostics>;

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
        std::this_thread::yield();
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
  config::DataReaderConfigResult config_result =
      config::LoadDataReaderConfigFile(state_->config_path);
  if (!config_result.ok) {
    state_->last_error = config_result.error;
    return false;
  }

  state_->drain_budget = config_result.value.max_events_per_drain;
  std::vector<MarketDataKey> keys = BuildMarketDataKeys(config_result.value);

  try {
    auto store = std::make_unique<MarketDataStore>(
        std::span<const MarketDataKey>{keys.data(), keys.size()});
    auto reader =
        std::make_unique<State::RealtimeReader>(std::move(config_result.value));

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

}  // namespace aquila::monitor
