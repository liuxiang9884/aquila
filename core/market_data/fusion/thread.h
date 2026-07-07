#ifndef AQUILA_CORE_MARKET_DATA_FUSION_THREAD_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_THREAD_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "core/market_data/fusion/book_ticker.h"
#include "core/market_data/fusion/trade.h"
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

  void Stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
  }

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

using BookTickerFusionThreadStats = FastestRouteFusionThreadStats;
using BookTickerFusionThread =
    BasicFastestRouteFusionThread<BookTickerFusionRunner,
                                  BookTickerFusionConfig>;

using TradeFusionThreadStats = FastestRouteFusionThreadStats;
using TradeFusionThread =
    BasicFastestRouteFusionThread<TradeFusionRunner, TradeFusionConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_THREAD_H_
