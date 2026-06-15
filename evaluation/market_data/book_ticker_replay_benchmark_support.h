#ifndef AQUILA_EVALUATION_MARKET_DATA_BOOK_TICKER_REPLAY_BENCHMARK_SUPPORT_H_
#define AQUILA_EVALUATION_MARKET_DATA_BOOK_TICKER_REPLAY_BENCHMARK_SUPPORT_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/common/result.h"
#include "core/market_data/types.h"

namespace aquila::market_data::evaluation {

struct LatencySummary {
  std::uint64_t count{0};
  std::int64_t min_ns{0};
  std::int64_t p50_ns{0};
  std::int64_t p95_ns{0};
  std::int64_t p99_ns{0};
  std::int64_t p999_ns{0};
  std::int64_t max_ns{0};
};

[[nodiscard]] inline Result<std::vector<BookTicker>> LoadBookTickerDump(
    const std::filesystem::path& path, std::uint64_t max_records) {
  Result<std::vector<BookTicker>> result;
  if (path.empty()) {
    result.error = "input path must not be empty";
    return result;
  }
  if (!std::filesystem::exists(path)) {
    result.error = "input path does not exist: " + path.string();
    return result;
  }

  const std::uintmax_t file_size = std::filesystem::file_size(path);
  if (file_size % sizeof(BookTicker) != 0) {
    result.error = "input size must be a multiple of BookTicker";
    return result;
  }

  std::uint64_t record_count =
      static_cast<std::uint64_t>(file_size / sizeof(BookTicker));
  if (max_records != 0 && record_count > max_records) {
    record_count = max_records;
  }

  std::vector<BookTicker> records(record_count);
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    result.error = "failed to open input path: " + path.string();
    return result;
  }
  if (!records.empty()) {
    input.read(
        reinterpret_cast<char*>(records.data()),
        static_cast<std::streamsize>(records.size() * sizeof(BookTicker)));
    if (input.gcount() !=
        static_cast<std::streamsize>(records.size() * sizeof(BookTicker))) {
      result.error = "failed to read requested BookTicker records";
      return result;
    }
  }

  result.value = std::move(records);
  result.ok = true;
  return result;
}

[[nodiscard]] inline std::int64_t ReplayDelayNs(
    std::int64_t previous_exchange_ns, std::int64_t exchange_ns,
    double speed) noexcept {
  if (exchange_ns <= previous_exchange_ns || speed <= 0.0) {
    return 0;
  }
  return static_cast<std::int64_t>(
      static_cast<double>(exchange_ns - previous_exchange_ns) / speed);
}

[[nodiscard]] inline std::int64_t NearestRankPercentile(
    const std::vector<std::int64_t>& sorted_values,
    double percentile) noexcept {
  if (sorted_values.empty()) {
    return 0;
  }
  if (percentile <= 0.0) {
    return sorted_values.front();
  }
  if (percentile >= 1.0) {
    return sorted_values.back();
  }
  const double rank =
      std::ceil(percentile * static_cast<double>(sorted_values.size()));
  const std::size_t index =
      static_cast<std::size_t>(rank <= 1.0 ? 0.0 : rank - 1.0);
  return sorted_values[std::min(index, sorted_values.size() - 1)];
}

[[nodiscard]] inline LatencySummary SummarizeLatencies(
    std::vector<std::int64_t> latencies_ns) {
  LatencySummary summary;
  summary.count = latencies_ns.size();
  if (latencies_ns.empty()) {
    return summary;
  }

  std::sort(latencies_ns.begin(), latencies_ns.end());
  summary.min_ns = latencies_ns.front();
  summary.p50_ns = NearestRankPercentile(latencies_ns, 0.50);
  summary.p95_ns = NearestRankPercentile(latencies_ns, 0.95);
  summary.p99_ns = NearestRankPercentile(latencies_ns, 0.99);
  summary.p999_ns = NearestRankPercentile(latencies_ns, 0.999);
  summary.max_ns = latencies_ns.back();
  return summary;
}

}  // namespace aquila::market_data::evaluation

#endif  // AQUILA_EVALUATION_MARKET_DATA_BOOK_TICKER_REPLAY_BENCHMARK_SUPPORT_H_
