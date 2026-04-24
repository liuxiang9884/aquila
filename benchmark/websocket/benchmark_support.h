#pragma once

#include <algorithm>
#include <cstdint>
#include <sched.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

namespace aquila::websocket::benchmarking {

inline std::string FormatAffinity() {
  cpu_set_t affinity_mask;
  CPU_ZERO(&affinity_mask);
  if (sched_getaffinity(0, sizeof(affinity_mask), &affinity_mask) != 0) {
    return "unknown";
  }

  std::string description;
  bool first = true;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &affinity_mask)) {
      continue;
    }
    if (!first) {
      description += ",";
    }
    description += std::to_string(cpu);
    first = false;
  }
  return description.empty() ? "none" : description;
}

inline const char* FormatSchedulingPolicy() {
  switch (sched_getscheduler(0)) {
    case SCHED_FIFO:
      return "fifo";
    case SCHED_RR:
      return "rr";
#ifdef SCHED_BATCH
    case SCHED_BATCH:
      return "batch";
#endif
#ifdef SCHED_IDLE
    case SCHED_IDLE:
      return "idle";
#endif
#ifdef SCHED_DEADLINE
    case SCHED_DEADLINE:
      return "deadline";
#endif
    case SCHED_OTHER:
      return "other";
    default:
      return "unknown";
  }
}

inline std::uint64_t SelectQuantile(std::vector<std::uint64_t>& samples,
                                    double quantile) {
  if (samples.empty()) {
    return 0;
  }
  const double scaled_index =
      quantile * static_cast<double>(samples.size() - 1);
  const auto index = static_cast<size_t>(scaled_index + 0.5);
  std::nth_element(samples.begin(), samples.begin() + index, samples.end());
  return samples[index];
}

inline void SetLatencyCounters(benchmark::State& state,
                               std::vector<std::uint64_t> samples_ns,
                               std::string_view detail_name,
                               std::uint64_t detail_value) {
  state.counters["samples"] = static_cast<double>(samples_ns.size());
  if (!detail_name.empty()) {
    state.counters[std::string(detail_name)] = static_cast<double>(detail_value);
  }
  if (samples_ns.empty()) {
    return;
  }

  const auto minmax = std::minmax_element(samples_ns.begin(), samples_ns.end());
  std::vector<std::uint64_t> quantile_samples = samples_ns;
  state.counters["min_ns"] = static_cast<double>(*minmax.first);
  state.counters["p50_ns"] =
      static_cast<double>(SelectQuantile(quantile_samples, 0.50));
  state.counters["p99_ns"] =
      static_cast<double>(SelectQuantile(quantile_samples, 0.99));
  state.counters["p999_ns"] =
      static_cast<double>(SelectQuantile(quantile_samples, 0.999));
  state.counters["max_ns"] = static_cast<double>(*minmax.second);
}

inline std::string BuildBenchmarkLabel(bool tls_enabled,
                                       std::string_view endpoint,
                                       std::string_view affinity,
                                       std::string_view scheduling_policy) {
  std::string label = "tls=";
  label += tls_enabled ? "enabled" : "disabled";
  label += " endpoint=";
  label += endpoint;
  label += " affinity=";
  label += affinity;
  label += " scheduling=";
  label += scheduling_policy;
  return label;
}

}  // namespace aquila::websocket::benchmarking
