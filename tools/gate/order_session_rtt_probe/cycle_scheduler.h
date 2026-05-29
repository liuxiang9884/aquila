#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CYCLE_SCHEDULER_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CYCLE_SCHEDULER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aquila::tools::gate_order_session_rtt_probe {

struct CycleSchedulerOptions {
  std::vector<std::string> candidate_ips;
  std::size_t active_session_count{1};
  std::uint32_t samples_per_ip{1};
  std::uint32_t cycles_per_connection_generation{1};
};

struct ProbeCycle {
  std::uint64_t cycle_index{0};
  std::size_t group_index{0};
  std::vector<std::string> connect_ips;
};

class CycleScheduler {
 public:
  explicit CycleScheduler(CycleSchedulerOptions options)
      : options_(std::move(options)),
        sample_counts_(options_.candidate_ips.size(), 0) {
    if (options_.active_session_count == 0) {
      options_.active_session_count = 1;
    }
    if (options_.samples_per_ip == 0) {
      options_.samples_per_ip = 1;
    }
    (void)options_.cycles_per_connection_generation;
  }

  [[nodiscard]] bool HasNextCycle() const noexcept {
    if (options_.candidate_ips.empty()) {
      return false;
    }
    for (const std::uint32_t count : sample_counts_) {
      if (count < options_.samples_per_ip) {
        return true;
      }
    }
    return false;
  }

  [[nodiscard]] ProbeCycle NextCycle() {
    ProbeCycle cycle{
        .cycle_index = next_cycle_index_++,
        .group_index = next_index_ / options_.active_session_count};
    if (options_.candidate_ips.empty()) {
      return cycle;
    }

    const std::size_t begin = next_index_;
    const std::size_t end = std::min(begin + options_.active_session_count,
                                     options_.candidate_ips.size());
    cycle.connect_ips.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      cycle.connect_ips.push_back(options_.candidate_ips[i]);
      ++sample_counts_[i];
    }

    next_index_ = end >= options_.candidate_ips.size() ? 0 : end;
    return cycle;
  }

 private:
  CycleSchedulerOptions options_;
  std::vector<std::uint32_t> sample_counts_;
  std::uint64_t next_cycle_index_{0};
  std::size_t next_index_{0};
};

class OrderSessionDispatchPacer {
 public:
  explicit OrderSessionDispatchPacer(std::uint32_t order_session_interval_ms)
      : interval_ns_(static_cast<std::int64_t>(order_session_interval_ms) *
                     1'000'000) {}

  [[nodiscard]] bool CanDispatch(std::int64_t now_ns,
                                 bool has_new_market_event) const noexcept {
    if (!waiting_) {
      return true;
    }
    if (interval_ns_ == 0) {
      return has_new_market_event;
    }
    return now_ns >= next_dispatch_ns_;
  }

  void MarkDispatched(std::int64_t now_ns) noexcept {
    waiting_ = true;
    next_dispatch_ns_ = now_ns + interval_ns_;
  }

  void MarkDispatchConsumed() noexcept {
    waiting_ = false;
    next_dispatch_ns_ = 0;
  }

 private:
  std::int64_t interval_ns_{0};
  std::int64_t next_dispatch_ns_{0};
  bool waiting_{false};
};

struct MultiSessionDispatchSchedulerOptions {
  std::size_t session_count{1};
  std::uint64_t sample_count_per_session{1};
  std::uint32_t order_session_interval_ms{0};
  std::uint32_t cycle_cooldown_ms{0};
};

class MultiSessionDispatchScheduler {
 public:
  explicit MultiSessionDispatchScheduler(
      MultiSessionDispatchSchedulerOptions options) noexcept
      : options_(options),
        order_session_interval_ns_(
            static_cast<std::int64_t>(options.order_session_interval_ms) *
            1'000'000),
        cycle_cooldown_ns_(
            static_cast<std::int64_t>(options.cycle_cooldown_ms) * 1'000'000) {}

  [[nodiscard]] bool NextGrant(
      std::uint64_t total_market_events,
      const std::vector<std::uint64_t>& samples_started_by_session,
      std::int64_t now_ns, std::size_t* session_index) noexcept {
    if (session_index == nullptr || options_.session_count == 0 ||
        options_.sample_count_per_session == 0 ||
        current_cycle_ >= options_.sample_count_per_session) {
      return false;
    }
    if (samples_started_by_session.size() < options_.session_count) {
      return false;
    }

    ObserveOutstandingGrant(samples_started_by_session, now_ns,
                            total_market_events);
    if (grant_outstanding_) {
      return false;
    }
    if (waiting_for_cycle_cooldown_) {
      if (now_ns < cycle_deadline_ns_) {
        return false;
      }
      waiting_for_cycle_cooldown_ = false;
    }
    if (waiting_for_market_event_) {
      if (total_market_events <= market_event_baseline_) {
        return false;
      }
      waiting_for_market_event_ = false;
    }
    if (waiting_for_interval_) {
      if (now_ns < interval_deadline_ns_) {
        return false;
      }
      waiting_for_interval_ = false;
    }

    *session_index = next_session_index_;
    grant_outstanding_ = true;
    outstanding_session_index_ = next_session_index_;
    outstanding_target_started_ = current_cycle_ + 1;
    market_event_baseline_ = total_market_events;
    return true;
  }

 private:
  void ObserveOutstandingGrant(
      const std::vector<std::uint64_t>& samples_started_by_session,
      std::int64_t now_ns, std::uint64_t total_market_events) noexcept {
    if (!grant_outstanding_) {
      return;
    }
    if (samples_started_by_session[outstanding_session_index_] <
        outstanding_target_started_) {
      return;
    }

    grant_outstanding_ = false;
    ++next_session_index_;
    if (next_session_index_ < options_.session_count) {
      if (order_session_interval_ns_ == 0) {
        waiting_for_market_event_ = true;
        market_event_baseline_ = total_market_events;
      } else {
        waiting_for_interval_ = true;
        interval_deadline_ns_ = now_ns + order_session_interval_ns_;
      }
      return;
    }

    next_session_index_ = 0;
    ++current_cycle_;
    if (current_cycle_ < options_.sample_count_per_session) {
      waiting_for_cycle_cooldown_ = true;
      cycle_deadline_ns_ = now_ns + cycle_cooldown_ns_;
    }
  }

  MultiSessionDispatchSchedulerOptions options_;
  std::int64_t order_session_interval_ns_{0};
  std::int64_t cycle_cooldown_ns_{0};
  std::uint64_t current_cycle_{0};
  std::size_t next_session_index_{0};
  bool grant_outstanding_{false};
  std::size_t outstanding_session_index_{0};
  std::uint64_t outstanding_target_started_{0};
  bool waiting_for_market_event_{false};
  bool waiting_for_interval_{false};
  bool waiting_for_cycle_cooldown_{false};
  std::uint64_t market_event_baseline_{0};
  std::int64_t interval_deadline_ns_{0};
  std::int64_t cycle_deadline_ns_{0};
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CYCLE_SCHEDULER_H_
