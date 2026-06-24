#include "tools/lead_lag/freshness_preflight.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "core/websocket/runtime_clock.h"

namespace aquila::tools::leadlag {
namespace {

[[nodiscard]] double Percentile(std::vector<double> samples,
                                double percentile) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const double rank =
      (percentile / 100.0) * static_cast<double>(samples.size() - 1);
  const auto lower_index = static_cast<std::size_t>(std::floor(rank));
  const auto upper_index = static_cast<std::size_t>(std::ceil(rank));
  if (lower_index == upper_index) {
    return samples[lower_index];
  }
  const double weight = rank - static_cast<double>(lower_index);
  return samples[lower_index] * (1.0 - weight) + samples[upper_index] * weight;
}

[[nodiscard]] std::string NormalizeExchangeName(std::string_view text) {
  std::string normalized;
  normalized.reserve(text.size());
  for (const char c : text) {
    if (c >= 'A' && c <= 'Z') {
      normalized.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c != '_' && c != '-') {
      normalized.push_back(c);
    }
  }
  if (!normalized.empty() && normalized.front() == 'k') {
    normalized.erase(normalized.begin());
  }
  return normalized;
}

[[nodiscard]] std::optional<Exchange> ParseExchange(std::string_view text) {
  const std::string normalized = NormalizeExchangeName(text);
  if (normalized == "binance") {
    return Exchange::kBinance;
  }
  if (normalized == "okx") {
    return Exchange::kOkx;
  }
  if (normalized == "gate" || normalized == "gateio") {
    return Exchange::kGate;
  }
  if (normalized == "bybit") {
    return Exchange::kBybit;
  }
  if (normalized == "bitget") {
    return Exchange::kBitget;
  }
  if (normalized == "coinbase") {
    return Exchange::kCoinbase;
  }
  return std::nullopt;
}

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    *error = message;
  }
}

[[nodiscard]] std::string JsonExchangeName(Exchange exchange) {
  std::string name{magic_enum::enum_name(exchange)};
  if (!name.empty() && name.front() == 'k') {
    name.erase(name.begin());
  }
  for (char& c : name) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return name;
}

void AppendStatsJson(std::ostringstream& out,
                     const FreshnessLatencyStats& stats) {
  out << "\"sample_count\":" << stats.sample_count
      << ",\"negative_latency_count\":" << stats.negative_latency_count
      << ",\"mean_ms\":" << fmt::format("{:.12g}", stats.mean_ms)
      << ",\"std_ms\":" << fmt::format("{:.12g}", stats.std_ms)
      << ",\"min_ms\":" << fmt::format("{:.12g}", stats.min_ms)
      << ",\"p50_ms\":" << fmt::format("{:.12g}", stats.p50_ms)
      << ",\"p95_ms\":" << fmt::format("{:.12g}", stats.p95_ms)
      << ",\"p99_ms\":" << fmt::format("{:.12g}", stats.p99_ms)
      << ",\"max_ms\":" << fmt::format("{:.12g}", stats.max_ms)
      << ",\"threshold_ms\":" << stats.threshold_ms;
}

}  // namespace

void FreshnessLatencyAccumulator::ObserveLatencyNs(std::int64_t latency_ns) {
  if (latency_ns < 0) {
    ++negative_latency_count_;
    return;
  }

  const double latency_ms = static_cast<double>(latency_ns) / 1'000'000.0;
  samples_ms_.push_back(latency_ms);
  ++sample_count_;
  if (sample_count_ == 1) {
    mean_ms_ = latency_ms;
    min_ms_ = latency_ms;
    max_ms_ = latency_ms;
    return;
  }

  if (latency_ms < min_ms_) {
    min_ms_ = latency_ms;
  }
  if (latency_ms > max_ms_) {
    max_ms_ = latency_ms;
  }

  const double delta = latency_ms - mean_ms_;
  mean_ms_ += delta / static_cast<double>(sample_count_);
  m2_ms_ += delta * (latency_ms - mean_ms_);
}

std::optional<FreshnessLatencyStats> FreshnessLatencyAccumulator::Build()
    const {
  if (sample_count_ == 0) {
    return std::nullopt;
  }

  const double std_ms = std::sqrt(m2_ms_ / static_cast<double>(sample_count_));
  const double threshold_ms = std::ceil(mean_ms_ + 3.0 * std_ms);
  return FreshnessLatencyStats{
      .sample_count = sample_count_,
      .negative_latency_count = negative_latency_count_,
      .mean_ms = mean_ms_,
      .std_ms = std_ms,
      .min_ms = min_ms_,
      .p50_ms = Percentile(samples_ms_, 50.0),
      .p95_ms = Percentile(samples_ms_, 95.0),
      .p99_ms = Percentile(samples_ms_, 99.0),
      .max_ms = max_ms_,
      .threshold_ms = static_cast<std::int32_t>(threshold_ms),
  };
}

void FreshnessLatencyAccumulator::Observe(const BookTicker& ticker) {
  ObserveLatencyNs(ticker.local_ns - ticker.exchange_ns);
}

FreshnessPreflightCollector::FreshnessPreflightCollector(
    std::vector<FreshnessPairConfig> pairs) {
  groups_.reserve(pairs.size());
  for (const FreshnessPairConfig& pair : pairs) {
    groups_.push_back(Group{
        .symbol_id = pair.symbol_id,
        .lead_exchange = pair.lead_exchange,
        .lag_exchange = pair.lag_exchange,
    });
  }
}

void FreshnessPreflightCollector::OnBookTicker(const BookTicker& ticker) {
  OnBookTickerAt(ticker,
                 static_cast<std::int64_t>(websocket::RealtimeClockNowNs()));
}

void FreshnessPreflightCollector::OnBookTickerAt(const BookTicker& ticker,
                                                 std::int64_t decision_ns) {
  for (Group& group : groups_) {
    if (group.symbol_id != ticker.symbol_id) {
      continue;
    }
    if (ticker.exchange == group.lead_exchange) {
      group.latest_lead = ticker;
      if (group.has_lag) {
        group.lead_accumulator.ObserveLatencyNs(decision_ns -
                                                ticker.exchange_ns);
        group.lag_accumulator.ObserveLatencyNs(decision_ns -
                                               group.latest_lag.exchange_ns);
      }
      return;
    }
    if (ticker.exchange == group.lag_exchange) {
      group.latest_lag = ticker;
      group.has_lag = true;
      return;
    }
  }
}

std::vector<FreshnessGroupSummary> FreshnessPreflightCollector::BuildSummaries()
    const {
  std::vector<FreshnessGroupSummary> summaries;
  summaries.reserve(groups_.size() * 2);
  for (const Group& group : groups_) {
    if (const std::optional<FreshnessLatencyStats> stats =
            group.lead_accumulator.Build();
        stats.has_value()) {
      summaries.push_back(FreshnessGroupSummary{
          .symbol_id = group.symbol_id,
          .exchange = group.lead_exchange,
          .stats = *stats,
      });
    }
    if (const std::optional<FreshnessLatencyStats> stats =
            group.lag_accumulator.Build();
        stats.has_value()) {
      summaries.push_back(FreshnessGroupSummary{
          .symbol_id = group.symbol_id,
          .exchange = group.lag_exchange,
          .stats = *stats,
      });
    }
  }
  std::sort(
      summaries.begin(), summaries.end(),
      [](const FreshnessGroupSummary& lhs, const FreshnessGroupSummary& rhs) {
        if (lhs.symbol_id != rhs.symbol_id) {
          return lhs.symbol_id < rhs.symbol_id;
        }
        return static_cast<std::uint8_t>(lhs.exchange) <
               static_cast<std::uint8_t>(rhs.exchange);
      });
  return summaries;
}

std::optional<std::vector<FreshnessPairConfig>>
BuildFreshnessPairConfigsFromLeadLagConfig(const toml::table& config,
                                           std::string* error) {
  const toml::table* lead_lag = config["lead_lag"].as_table();
  if (lead_lag == nullptr) {
    SetError(error, "missing [lead_lag] table");
    return std::nullopt;
  }
  const toml::array* pairs = (*lead_lag)["pairs"].as_array();
  if (pairs == nullptr) {
    SetError(error, "missing [[lead_lag.pairs]] array");
    return std::nullopt;
  }

  std::vector<FreshnessPairConfig> result;
  result.reserve(pairs->size());
  for (const toml::node& pair_node : *pairs) {
    const toml::table* pair = pair_node.as_table();
    if (pair == nullptr) {
      SetError(error, "lead_lag.pairs contains a non-table entry");
      return std::nullopt;
    }
    const std::optional<std::int64_t> symbol_id_value =
        (*pair)["symbol_id"].value<std::int64_t>();
    const std::optional<std::string> symbol_value =
        (*pair)["symbol"].value<std::string>();
    const std::optional<std::string> lead_exchange_value =
        (*pair)["lead_exchange"].value<std::string>();
    const std::optional<std::string> lag_exchange_value =
        (*pair)["lag_exchange"].value<std::string>();
    if (!symbol_id_value.has_value() || !lead_exchange_value.has_value() ||
        !lag_exchange_value.has_value()) {
      SetError(error,
               "each lead_lag pair must define symbol_id, lead_exchange, and "
               "lag_exchange");
      return std::nullopt;
    }

    const auto lead_exchange = ParseExchange(*lead_exchange_value);
    const auto lag_exchange = ParseExchange(*lag_exchange_value);
    if (!lead_exchange.has_value() || !lag_exchange.has_value()) {
      SetError(error, fmt::format("unsupported exchange in pair symbol={}",
                                  symbol_value.value_or("-")));
      return std::nullopt;
    }
    result.push_back(FreshnessPairConfig{
        .symbol_id = static_cast<std::int32_t>(*symbol_id_value),
        .lead_exchange = *lead_exchange,
        .lag_exchange = *lag_exchange,
    });
  }
  return result;
}

const FreshnessLatencyStats* FindStats(
    const std::vector<FreshnessGroupSummary>& summaries, std::int32_t symbol_id,
    Exchange exchange) noexcept {
  for (const FreshnessGroupSummary& summary : summaries) {
    if (summary.symbol_id == symbol_id && summary.exchange == exchange) {
      return &summary.stats;
    }
  }
  return nullptr;
}

bool ApplyFreshnessThresholdsToLeadLagConfig(
    toml::table* config, const std::vector<FreshnessGroupSummary>& summaries,
    std::string* error) {
  if (config == nullptr) {
    SetError(error, "lead_lag config table is null");
    return false;
  }
  toml::table* lead_lag = (*config)["lead_lag"].as_table();
  if (lead_lag == nullptr) {
    SetError(error, "missing [lead_lag] table");
    return false;
  }
  toml::array* pairs = (*lead_lag)["pairs"].as_array();
  if (pairs == nullptr) {
    SetError(error, "missing [[lead_lag.pairs]] array");
    return false;
  }

  for (toml::node& pair_node : *pairs) {
    toml::table* pair = pair_node.as_table();
    if (pair == nullptr) {
      SetError(error, "lead_lag.pairs contains a non-table entry");
      return false;
    }
    const std::optional<std::int64_t> symbol_id_value =
        (*pair)["symbol_id"].value<std::int64_t>();
    const std::optional<std::string> symbol_value =
        (*pair)["symbol"].value<std::string>();
    const std::optional<std::string> lead_exchange_value =
        (*pair)["lead_exchange"].value<std::string>();
    const std::optional<std::string> lag_exchange_value =
        (*pair)["lag_exchange"].value<std::string>();
    if (!symbol_id_value.has_value() || !lead_exchange_value.has_value() ||
        !lag_exchange_value.has_value()) {
      SetError(error,
               "each lead_lag pair must define symbol_id, lead_exchange, and "
               "lag_exchange");
      return false;
    }

    const auto lead_exchange = ParseExchange(*lead_exchange_value);
    const auto lag_exchange = ParseExchange(*lag_exchange_value);
    if (!lead_exchange.has_value() || !lag_exchange.has_value()) {
      SetError(error, fmt::format("unsupported exchange in pair symbol={}",
                                  symbol_value.value_or("-")));
      return false;
    }

    const auto symbol_id = static_cast<std::int32_t>(*symbol_id_value);
    const FreshnessLatencyStats* lead_stats =
        FindStats(summaries, symbol_id, *lead_exchange);
    const FreshnessLatencyStats* lag_stats =
        FindStats(summaries, symbol_id, *lag_exchange);
    if (lead_stats == nullptr || lag_stats == nullptr) {
      SetError(error, fmt::format("missing freshness samples for symbol={} "
                                  "symbol_id={} lead_exchange={} "
                                  "lag_exchange={}",
                                  symbol_value.value_or("-"), symbol_id,
                                  *lead_exchange_value, *lag_exchange_value));
      return false;
    }

    pair->insert_or_assign("max_lead_freshness_ms", lead_stats->threshold_ms);
    pair->insert_or_assign("max_lag_freshness_ms", lag_stats->threshold_ms);
  }

  return true;
}

std::string RenderSummaryJson(
    const std::vector<FreshnessGroupSummary>& summaries) {
  std::ostringstream out;
  out << "{\n  \"method\":\"lead_decision_ns_minus_exchange_ns_mean_plus_"
         "3std\","
      << "\n  \"groups\":[";
  for (std::size_t i = 0; i < summaries.size(); ++i) {
    const FreshnessGroupSummary& summary = summaries[i];
    out << (i == 0 ? "\n    {" : ",\n    {")
        << "\"symbol_id\":" << summary.symbol_id << ",\"exchange\":\""
        << JsonExchangeName(summary.exchange) << "\",";
    AppendStatsJson(out, summary.stats);
    out << "}";
  }
  out << "\n  ]\n}\n";
  return out.str();
}

}  // namespace aquila::tools::leadlag
