#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SEQUENTIAL_COORDINATOR_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SEQUENTIAL_COORDINATOR_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace aquila::tools::bitget_order_session_rtt_probe {

class SequentialCoordinator {
 public:
  SequentialCoordinator(std::size_t session_count,
                        std::uint32_t samples_per_session,
                        std::uint64_t order_session_interval_us,
                        std::uint64_t cycle_cooldown_us) noexcept
      : session_count_(session_count),
        samples_per_session_(samples_per_session),
        order_session_interval_ns_(
            MicrosecondsToNanoseconds(order_session_interval_us)),
        cycle_cooldown_ns_(MicrosecondsToNanoseconds(cycle_cooldown_us)),
        complete_(session_count == 0 || samples_per_session == 0) {}

  [[nodiscard]] std::optional<std::size_t> NextGrant(
      std::int64_t now_ns) noexcept {
    if (complete_ || failed_ || active_session_index_.has_value() ||
        now_ns < next_grant_ns_) {
      return std::nullopt;
    }
    active_session_index_ = next_session_index_;
    return active_session_index_;
  }

  [[nodiscard]] bool MarkSampleFinished(std::size_t session_index, bool success,
                                        std::int64_t now_ns) noexcept {
    if (!active_session_index_.has_value() ||
        *active_session_index_ != session_index || complete_ || failed_) {
      failed_ = true;
      active_session_index_.reset();
      return false;
    }
    active_session_index_.reset();
    if (!success) {
      failed_ = true;
      return true;
    }

    ++completed_samples_;
    if (next_session_index_ + 1 < session_count_) {
      ++next_session_index_;
      next_grant_ns_ = AddDelay(now_ns, order_session_interval_ns_);
      return true;
    }

    next_session_index_ = 0;
    ++completed_cycles_;
    if (completed_cycles_ == samples_per_session_) {
      complete_ = true;
      return true;
    }
    next_grant_ns_ = AddDelay(now_ns, cycle_cooldown_ns_);
    return true;
  }

  [[nodiscard]] bool complete() const noexcept {
    return complete_;
  }
  [[nodiscard]] bool failed() const noexcept {
    return failed_;
  }
  [[nodiscard]] bool has_active_sample() const noexcept {
    return active_session_index_.has_value();
  }
  [[nodiscard]] std::uint64_t completed_samples() const noexcept {
    return completed_samples_;
  }

 private:
  [[nodiscard]] static constexpr std::int64_t MicrosecondsToNanoseconds(
      std::uint64_t value) noexcept {
    constexpr auto kMax =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return value > kMax / 1000 ? std::numeric_limits<std::int64_t>::max()
                               : static_cast<std::int64_t>(value * 1000);
  }

  [[nodiscard]] static constexpr std::int64_t AddDelay(
      std::int64_t now_ns, std::int64_t delay_ns) noexcept {
    if (delay_ns > 0 &&
        now_ns > std::numeric_limits<std::int64_t>::max() - delay_ns) {
      return std::numeric_limits<std::int64_t>::max();
    }
    return now_ns + delay_ns;
  }

  std::size_t session_count_{0};
  std::uint32_t samples_per_session_{0};
  std::int64_t order_session_interval_ns_{0};
  std::int64_t cycle_cooldown_ns_{0};
  std::size_t next_session_index_{0};
  std::uint32_t completed_cycles_{0};
  std::uint64_t completed_samples_{0};
  std::int64_t next_grant_ns_{0};
  std::optional<std::size_t> active_session_index_;
  bool complete_{false};
  bool failed_{false};
};

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SEQUENTIAL_COORDINATOR_H_
