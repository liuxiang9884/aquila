#ifndef AQUILA_CORE_MARKET_DATA_DATA_SESSION_DIAGNOSTICS_H_
#define AQUILA_CORE_MARKET_DATA_DATA_SESSION_DIAGNOSTICS_H_

#include <cstdint>

#include "core/common/data_session_diagnostic_level.h"
#include "core/common/types.h"
#include "core/websocket/message_view.h"
#include "core/websocket/socket_timestamping.h"

#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 1
#include "nova/utils/log.h"
#endif

namespace aquila::market_data {

inline constexpr std::int64_t kDefaultDataSessionLatencyOutlierThresholdNs =
    5'000'000;
inline constexpr std::uint32_t kDefaultDataSessionLatencyOutlierMaxLogsPerSec =
    1000;

struct DataSessionLatencyOutlierConfig {
  bool enabled{false};
  std::int32_t source_id{0};
  std::int64_t threshold_ns{kDefaultDataSessionLatencyOutlierThresholdNs};
  std::uint32_t max_logs_per_second{
      kDefaultDataSessionLatencyOutlierMaxLogsPerSec};
};

struct DataSessionDiagnosticsConfig {
  DataSessionLatencyOutlierConfig latency_outlier{};
  websocket::SocketTimestampingConfig socket_timestamping{};
};

struct DataSessionMessageTiming {
  bool available{false};
  bool transport_tls{false};
  bool kernel_rx_available{false};
  std::int64_t kernel_rx_ns{0};
  std::int64_t drive_read_enter_ns{0};
  std::int64_t read_return_ns{0};
  std::int64_t handler_entry_ns{0};
  std::int32_t read_bytes{0};
};

struct DataSessionBookTickerTiming {
  Exchange exchange{Exchange::kBinance};
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t book_ticker_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t book_ticker_event_ns{0};
  std::int64_t book_ticker_local_ns{0};
  std::int64_t parse_done_ns{0};
  std::int64_t shm_publish_done_ns{0};
  DataSessionMessageTiming message{};
};

struct DataSessionLatencyBreakdown {
  std::int64_t network_or_exchange_ns{-1};
  std::int64_t kernel_queue_ns{-1};
  std::int64_t read_syscall_or_tls_ns{-1};
  std::int64_t ws_dispatch_ns{-1};
  std::int64_t parse_ns{-1};
  std::int64_t shm_publish_ns{-1};
  std::int64_t user_after_read_ns{-1};
};

[[nodiscard]] inline std::int64_t DataSessionDiffOrMinusOne(
    std::int64_t end_ns, std::int64_t begin_ns) noexcept {
  return end_ns > 0 && begin_ns > 0 && end_ns >= begin_ns ? end_ns - begin_ns
                                                          : -1;
}

[[nodiscard]] inline DataSessionLatencyBreakdown
ComputeDataSessionLatencyBreakdown(
    const DataSessionBookTickerTiming& timing) noexcept {
  return DataSessionLatencyBreakdown{
      .network_or_exchange_ns =
          timing.message.kernel_rx_available
              ? DataSessionDiffOrMinusOne(timing.message.kernel_rx_ns,
                                          timing.exchange_ns)
              : -1,
      .kernel_queue_ns =
          timing.message.kernel_rx_available
              ? DataSessionDiffOrMinusOne(timing.message.drive_read_enter_ns,
                                          timing.message.kernel_rx_ns)
              : -1,
      .read_syscall_or_tls_ns = DataSessionDiffOrMinusOne(
          timing.message.read_return_ns, timing.message.drive_read_enter_ns),
      .ws_dispatch_ns = DataSessionDiffOrMinusOne(
          timing.message.handler_entry_ns, timing.message.read_return_ns),
      .parse_ns = DataSessionDiffOrMinusOne(timing.parse_done_ns,
                                            timing.message.handler_entry_ns),
      .shm_publish_ns = DataSessionDiffOrMinusOne(timing.shm_publish_done_ns,
                                                  timing.parse_done_ns),
      .user_after_read_ns = DataSessionDiffOrMinusOne(
          timing.shm_publish_done_ns, timing.message.read_return_ns),
  };
}

[[nodiscard]] inline bool IsDataSessionLatencyOutlier(
    const DataSessionLatencyOutlierConfig& config,
    const DataSessionBookTickerTiming& timing) noexcept {
  if constexpr (!core::kDataSessionDiagnosticCorrelationEnabled) {
    (void)config;
    (void)timing;
    return false;
  } else {
    return config.enabled && config.threshold_ns >= 0 &&
           timing.book_ticker_local_ns > 0 && timing.exchange_ns > 0 &&
           timing.book_ticker_local_ns - timing.exchange_ns >
               config.threshold_ns;
  }
}

[[nodiscard]] inline const char* DataSessionExchangeName(
    Exchange exchange) noexcept {
  switch (exchange) {
    case Exchange::kBinance:
      return "kBinance";
    case Exchange::kOkx:
      return "kOkx";
    case Exchange::kGate:
      return "kGate";
    case Exchange::kBybit:
      return "kBybit";
    case Exchange::kBitget:
      return "kBitget";
    case Exchange::kCoinbase:
      return "kCoinbase";
  }
  return "unknown";
}

#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 2
[[nodiscard]] inline DataSessionMessageTiming MakeDataSessionMessageTiming(
    const websocket::MessageView& view, bool transport_tls) noexcept {
  DataSessionMessageTiming timing{
      .available = true,
      .transport_tls = transport_tls,
      .drive_read_enter_ns = view.drive_read_enter_ns,
      .read_return_ns = view.read_return_ns,
      .handler_entry_ns = view.handler_entry_ns,
      .read_bytes = view.read_bytes,
  };
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 4
  timing.kernel_rx_available = view.kernel_rx_available;
  timing.kernel_rx_ns = view.kernel_rx_ns;
#endif
  return timing;
}
#endif

class DataSessionLatencyOutlierLogger {
 public:
  explicit DataSessionLatencyOutlierLogger(
      DataSessionLatencyOutlierConfig config = {}) noexcept
      : config_(config) {}

  [[nodiscard]] const DataSessionLatencyOutlierConfig& config() const noexcept {
    return config_;
  }

  void MaybeLog(const DataSessionBookTickerTiming& timing) noexcept {
    (void)timing;
#if AQUILA_DATA_SESSION_DIAG_LEVEL >= 1
    if (!IsDataSessionLatencyOutlier(config_, timing) ||
        !AllowLog(LogClockNs(timing))) {
      return;
    }
    const DataSessionLatencyBreakdown breakdown =
        ComputeDataSessionLatencyBreakdown(timing);
    NOVA_WARNING(
        "data_session_book_ticker_latency_outlier exchange={} source_id={} "
        "symbol_id={} book_ticker_id={} latency_ns={} threshold_ns={} "
        "exchange_ns={} book_ticker_event_ns={} book_ticker_local_ns={} "
        "kernel_rx_available={} kernel_rx_ns={} drive_read_enter_ns={} "
        "read_return_ns={} handler_entry_ns={} parse_done_ns={} "
        "shm_publish_done_ns={} "
        "network_or_exchange_ns={} kernel_queue_ns={} "
        "read_syscall_or_tls_ns={} ws_dispatch_ns={} parse_ns={} "
        "shm_publish_ns={} user_after_read_ns={} read_bytes={} "
        "transport={}",
        DataSessionExchangeName(timing.exchange), timing.source_id,
        timing.symbol_id, timing.book_ticker_id,
        timing.book_ticker_local_ns - timing.exchange_ns, config_.threshold_ns,
        timing.exchange_ns, timing.book_ticker_event_ns,
        timing.book_ticker_local_ns,
        timing.message.kernel_rx_available ? "true" : "false",
        timing.message.kernel_rx_ns, timing.message.drive_read_enter_ns,
        timing.message.read_return_ns, timing.message.handler_entry_ns,
        timing.parse_done_ns, timing.shm_publish_done_ns,
        breakdown.network_or_exchange_ns, breakdown.kernel_queue_ns,
        breakdown.read_syscall_or_tls_ns, breakdown.ws_dispatch_ns,
        breakdown.parse_ns, breakdown.shm_publish_ns,
        breakdown.user_after_read_ns, timing.message.read_bytes,
        timing.message.transport_tls ? "tls" : "plain");
#endif
  }

 private:
  [[nodiscard]] static std::int64_t LogClockNs(
      const DataSessionBookTickerTiming& timing) noexcept {
    if (timing.shm_publish_done_ns > 0) {
      return timing.shm_publish_done_ns;
    }
    return timing.book_ticker_local_ns;
  }

  [[nodiscard]] bool AllowLog(std::int64_t now_ns) noexcept {
    if (config_.max_logs_per_second == 0 || now_ns <= 0) {
      return false;
    }
    constexpr std::int64_t kOneSecondNs = 1'000'000'000;
    if (rate_limit_window_start_ns_ == 0 ||
        now_ns - rate_limit_window_start_ns_ >= kOneSecondNs) {
      rate_limit_window_start_ns_ = now_ns;
      logs_in_rate_limit_window_ = 0;
    }
    if (logs_in_rate_limit_window_ >= config_.max_logs_per_second) {
      return false;
    }
    ++logs_in_rate_limit_window_;
    return true;
  }

  DataSessionLatencyOutlierConfig config_{};
  std::int64_t rate_limit_window_start_ns_{0};
  std::uint32_t logs_in_rate_limit_window_{0};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_SESSION_DIAGNOSTICS_H_
