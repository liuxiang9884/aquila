#ifndef AQUILA_TOOLS_LEAD_LAG_FRESHNESS_PREFLIGHT_H_
#define AQUILA_TOOLS_LEAD_LAG_FRESHNESS_PREFLIGHT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace aquila::tools::leadlag {

struct FreshnessLatencyStats {
  std::uint64_t sample_count{0};
  std::uint64_t negative_latency_count{0};
  double mean_ms{0.0};
  double std_ms{0.0};
  double min_ms{0.0};
  double p50_ms{0.0};
  double p95_ms{0.0};
  double p99_ms{0.0};
  double max_ms{0.0};
  std::int32_t threshold_ms{0};
};

class FreshnessLatencyAccumulator {
 public:
  void Observe(const BookTicker& ticker);

  [[nodiscard]] std::optional<FreshnessLatencyStats> Build() const;

 private:
  std::uint64_t sample_count_{0};
  std::uint64_t negative_latency_count_{0};
  double mean_ms_{0.0};
  double m2_ms_{0.0};
  double min_ms_{0.0};
  double max_ms_{0.0};
  std::vector<double> samples_ms_;
};

struct FreshnessGroupSummary {
  std::int32_t symbol_id{0};
  Exchange exchange{Exchange::kBinance};
  FreshnessLatencyStats stats;
};

class FreshnessPreflightCollector {
 public:
  void OnBookTicker(const BookTicker& ticker);

  [[nodiscard]] std::vector<FreshnessGroupSummary> BuildSummaries() const;

 private:
  struct Group {
    std::int32_t symbol_id{0};
    Exchange exchange{Exchange::kBinance};
    FreshnessLatencyAccumulator accumulator;
  };

  std::vector<Group> groups_;
};

[[nodiscard]] const FreshnessLatencyStats* FindStats(
    const std::vector<FreshnessGroupSummary>& summaries, std::int32_t symbol_id,
    Exchange exchange) noexcept;

[[nodiscard]] bool ApplyFreshnessThresholdsToLeadLagConfig(
    toml::table* config, const std::vector<FreshnessGroupSummary>& summaries,
    std::string* error);

[[nodiscard]] std::string RenderSummaryJson(
    const std::vector<FreshnessGroupSummary>& summaries);

}  // namespace aquila::tools::leadlag

#endif  // AQUILA_TOOLS_LEAD_LAG_FRESHNESS_PREFLIGHT_H_
