#pragma once

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <sched.h>
#include <string>
#include <string_view>
#include <vector>

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

inline void PrintReport(std::string_view name, std::vector<std::uint64_t> samples_ns,
                        bool tls_enabled, std::string_view endpoint,
                        std::string_view detail_name,
                        std::uint64_t detail_value) {
  const std::string affinity = FormatAffinity();
  if (samples_ns.empty()) {
    std::printf(
        "name=%.*s samples=0 affinity=%s scheduling=%s tls=%s endpoint=%.*s\n",
        static_cast<int>(name.size()), name.data(), affinity.c_str(),
        FormatSchedulingPolicy(), tls_enabled ? "enabled" : "disabled",
        static_cast<int>(endpoint.size()), endpoint.data());
    return;
  }

  const auto minmax = std::minmax_element(samples_ns.begin(), samples_ns.end());
  const std::uint64_t min_ns = *minmax.first;
  const std::uint64_t max_ns = *minmax.second;
  std::vector<std::uint64_t> quantile_samples = samples_ns;
  const std::uint64_t p50_ns = SelectQuantile(quantile_samples, 0.50);
  const std::uint64_t p99_ns = SelectQuantile(quantile_samples, 0.99);
  const std::uint64_t p999_ns = SelectQuantile(quantile_samples, 0.999);

  std::printf(
      "name=%.*s samples=%zu min_ns=%" PRIu64 " p50_ns=%" PRIu64
      " p99_ns=%" PRIu64 " p999_ns=%" PRIu64 " max_ns=%" PRIu64
      " affinity=%s scheduling=%s tls=%s endpoint=%.*s %.*s=%" PRIu64 "\n",
      static_cast<int>(name.size()), name.data(), samples_ns.size(), min_ns,
      p50_ns, p99_ns, p999_ns, max_ns, affinity.c_str(),
      FormatSchedulingPolicy(), tls_enabled ? "enabled" : "disabled",
      static_cast<int>(endpoint.size()), endpoint.data(),
      static_cast<int>(detail_name.size()), detail_name.data(), detail_value);
}

}  // namespace aquila::websocket::benchmarking
