#ifndef AQUILA_BENCHMARK_STRATEGY_LEAD_LAG_BENCHMARK_SUPPORT_H_
#define AQUILA_BENCHMARK_STRATEGY_LEAD_LAG_BENCHMARK_SUPPORT_H_

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string_view>

#include "nova/utils/log.h"

namespace aquila::strategy::leadlag::benchmarking {

inline constexpr std::int32_t kWideFreshnessGuardMs = 2'000'000'000;

[[nodiscard]] inline std::int64_t RealtimeNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline void EnsureLoggingStarted() {
  static const bool started = [] {
    nova::LogConfig config;
    const char* enable_console =
        std::getenv("AQUILA_LEAD_LAG_BENCHMARK_CONSOLE_LOG");
    if (enable_console != nullptr && std::string_view{enable_console} == "1") {
      config.set_console_sink_name("aquila_lead_lag_benchmark_console");
    } else {
      config.set_console_sink_name("");
    }
    config.set_file_sink_name((std::filesystem::temp_directory_path() /
                               "aquila_lead_lag_benchmark.log")
                                  .string());
    nova::InitializeLogging(config);
    return true;
  }();
  (void)started;
}

}  // namespace aquila::strategy::leadlag::benchmarking

#endif  // AQUILA_BENCHMARK_STRATEGY_LEAD_LAG_BENCHMARK_SUPPORT_H_
