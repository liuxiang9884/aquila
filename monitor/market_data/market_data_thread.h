#ifndef AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_THREAD_H_
#define AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_THREAD_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/config/data_reader_config.h"
#include "core/market_data/types.h"
#include "monitor/market_data/market_data_store.h"
#include "monitor/market_data/market_data_update.h"
#include "monitor/model/monitor_spsc_queue.h"

namespace aquila::monitor {

inline constexpr std::int64_t kMarketDataPublishIntervalNs = 100'000'000;
inline constexpr std::size_t kMarketDataThreadQueueCapacity = 64;

using MarketDataThreadQueue =
    MonitorSpscQueue<MarketDataBatch, kMarketDataThreadQueueCapacity>;

struct MarketDataPumpOptions {
  std::uint64_t drain_budget{64};
  std::int64_t publish_interval_ns{kMarketDataPublishIntervalNs};
};

struct MarketDataPumpResult {
  std::uint64_t drained_count{0};
  std::uint64_t overrun_delta{0};
  bool publish_due{false};
  bool batch_pushed{false};
  bool batch_dropped{false};
  std::uint16_t row_count{0};
};

struct MarketDataUnavailableSource {
  Exchange exchange{Exchange::kGate};
  std::string name;
  std::string reason;
  bool required{false};
};

class SteadyMarketDataClock {
 public:
  [[nodiscard]] std::int64_t NowNs() const noexcept;
};

template <typename Reader, typename Queue, typename Clock>
class MarketDataPump {
 public:
  MarketDataPump(Reader& reader, MarketDataStore& store, Queue& queue,
                 Clock& clock, MarketDataPumpOptions options = {})
      : reader_(reader),
        store_(store),
        queue_(queue),
        clock_(clock),
        options_(NormalizeOptions(options)),
        last_publish_ns_(clock_.NowNs()) {}

  [[nodiscard]] MarketDataPumpResult StepOnce() noexcept {
    StoreBookTickerHandler handler{.store = &store_};
    MarketDataPumpResult result{
        .drained_count = reader_.Drain(handler, options_.drain_budget)};
    const std::uint64_t overrun_count = ReaderOverrunCount(reader_);
    if (overrun_count > last_reader_overrun_count_) {
      result.overrun_delta = overrun_count - last_reader_overrun_count_;
      store_.RecordOverrun(result.overrun_delta);
      last_reader_overrun_count_ = overrun_count;
    }

    const std::int64_t now_ns = clock_.NowNs();
    if (now_ns - last_publish_ns_ < options_.publish_interval_ns) {
      return result;
    }

    result.publish_due = true;
    last_publish_ns_ = now_ns;

    MarketDataBatch batch = store_.BuildChangedBatch(now_ns);
    result.row_count = batch.row_count;
    if (batch.row_count == 0) {
      return result;
    }

    if (queue_.TryPush(batch)) {
      store_.ClearChangedRows(std::span<const MarketDataRowUpdate>{
          batch.rows.data(), batch.row_count});
      result.batch_pushed = true;
      return result;
    }

    store_.RecordDroppedBatch();
    result.batch_dropped = true;
    return result;
  }

 private:
  struct StoreBookTickerHandler {
    MarketDataStore* store{nullptr};

    void OnBookTicker(const BookTicker& ticker) noexcept {
      store->OnBookTicker(ticker);
    }
  };

  [[nodiscard]] static MarketDataPumpOptions NormalizeOptions(
      MarketDataPumpOptions options) noexcept {
    if (options.drain_budget == 0) {
      options.drain_budget = 1;
    }
    if (options.publish_interval_ns <= 0) {
      options.publish_interval_ns = kMarketDataPublishIntervalNs;
    }
    return options;
  }

  [[nodiscard]] static std::uint64_t ReaderOverrunCount(
      const Reader& reader) noexcept {
    if constexpr (requires { reader.diagnostics().stats().sources; }) {
      std::uint64_t count = 0;
      for (const auto& source : reader.diagnostics().stats().sources) {
        count += source.overruns;
      }
      return count;
    } else {
      return 0;
    }
  }

  Reader& reader_;
  MarketDataStore& store_;
  Queue& queue_;
  Clock& clock_;
  MarketDataPumpOptions options_;
  std::int64_t last_publish_ns_{0};
  std::uint64_t last_reader_overrun_count_{0};
};

[[nodiscard]] std::vector<MarketDataKey> BuildMarketDataKeys(
    const config::DataReaderConfig& config);

class MarketDataThread {
 public:
  MarketDataThread(std::filesystem::path config_path,
                   MarketDataThreadQueue& queue);
  MarketDataThread(const MarketDataThread&) = delete;
  MarketDataThread& operator=(const MarketDataThread&) = delete;
  MarketDataThread(MarketDataThread&&) = delete;
  MarketDataThread& operator=(MarketDataThread&&) = delete;
  ~MarketDataThread();

  [[nodiscard]] bool Start();
  void Stop() noexcept;
  void Join();

  [[nodiscard]] const std::string& last_error() const noexcept;
  [[nodiscard]] std::span<const MarketDataUnavailableSource>
  unavailable_sources() const noexcept;

 private:
  struct State;

  std::unique_ptr<State> state_;
};

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_THREAD_H_
