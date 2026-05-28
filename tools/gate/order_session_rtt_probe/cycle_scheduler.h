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

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CYCLE_SCHEDULER_H_
