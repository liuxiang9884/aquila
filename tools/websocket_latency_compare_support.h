#ifndef AQUILA_TOOLS_WEBSOCKET_LATENCY_COMPARE_SUPPORT_H_
#define AQUILA_TOOLS_WEBSOCKET_LATENCY_COMPARE_SUPPORT_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <fmt/compile.h>
#include <fmt/core.h>

#include "core/utils/numeric.h"
#include <simdjson.h>

namespace aquila::tools {

enum class EndpointSide : std::uint8_t {
  kPublic,
  kPrivate,
};

enum class WarmupPrimary : std::uint8_t {
  kNone,
  kPublic,
  kPrivate,
};

struct WarmupSelectionInput {
  bool public_healthy{false};
  bool private_healthy{false};
  size_t matched{0};
  size_t private_faster{0};
  size_t public_faster{0};
  size_t ties{0};
  size_t pending_public{0};
  size_t pending_private{0};
  std::int64_t private_lead_p50_ns{0};
  std::int64_t private_lead_p99_ns{0};
};

struct WarmupSelection {
  WarmupPrimary selected{WarmupPrimary::kNone};
  std::string reason;
};

struct GateBookTickerKey {
  std::string symbol;
  std::uint64_t update_id{0};
  std::uint64_t exchange_time_ms{0};

  [[nodiscard]] std::string MatchKey() const {
    return fmt::format(FMT_COMPILE("{}:{}"), symbol, update_id);
  }
};

struct MatchedLatency {
  GateBookTickerKey key;
  std::uint64_t public_arrival_ns{0};
  std::uint64_t private_arrival_ns{0};
  std::int64_t private_lead_ns{0};
};

struct LatencyCollectorSnapshot {
  std::vector<MatchedLatency> matched;
  size_t pending_public{0};
  size_t pending_private{0};
};

namespace detail {

inline bool ReadString(simdjson::ondemand::value value,
                       std::string_view* output) noexcept {
  simdjson::simdjson_result<std::string_view> result = value.get_string();
  std::string_view text{};
  if (std::move(result).get(text) != simdjson::SUCCESS) {
    return false;
  }
  *output = text;
  return true;
}

inline std::optional<std::uint64_t> ReadUint(
    simdjson::ondemand::value value) noexcept {
  std::uint64_t unsigned_value = 0;
  if (value.get_uint64().get(unsigned_value) == simdjson::SUCCESS) {
    return unsigned_value;
  }

  std::int64_t signed_value = 0;
  if (value.get_int64().get(signed_value) == simdjson::SUCCESS) {
    if (signed_value < 0) {
      return std::nullopt;
    }
    return static_cast<std::uint64_t>(signed_value);
  }

  std::string_view text{};
  if (ReadString(value, &text) && !text.empty()) {
    return aquila::ToUint64(text);
  }
  return std::nullopt;
}

inline bool FindField(simdjson::ondemand::object object, std::string_view key,
                      simdjson::ondemand::value* output) noexcept {
  simdjson::ondemand::value value;
  if (object.find_field_unordered(key).get(value) != simdjson::SUCCESS) {
    return false;
  }
  *output = value;
  return true;
}

inline bool FindObject(simdjson::ondemand::object object, std::string_view key,
                       simdjson::ondemand::object* output) noexcept {
  simdjson::ondemand::value value;
  if (!FindField(object, key, &value)) {
    return false;
  }
  simdjson::ondemand::object nested;
  if (value.get_object().get(nested) != simdjson::SUCCESS) {
    return false;
  }
  *output = nested;
  return true;
}

}  // namespace detail

inline std::optional<GateBookTickerKey> TryParseGateBookTicker(
    std::string_view payload) {
  if (payload.empty()) {
    return std::nullopt;
  }

  simdjson::padded_string padded(payload);
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return std::nullopt;
  }

  simdjson::ondemand::object root;
  if (document.get_object().get(root) != simdjson::SUCCESS) {
    return std::nullopt;
  }

  simdjson::ondemand::object result;
  if (!detail::FindObject(root, "result", &result)) {
    return std::nullopt;
  }

  simdjson::ondemand::value value;
  std::string_view symbol;
  if (!detail::FindField(result, "s", &value) ||
      !detail::ReadString(value, &symbol) || symbol.empty()) {
    return std::nullopt;
  }

  const auto update_id = detail::FindField(result, "u", &value)
                             ? detail::ReadUint(value)
                             : std::nullopt;
  const auto exchange_time_ms = detail::FindField(result, "t", &value)
                                    ? detail::ReadUint(value)
                                    : std::nullopt;
  if (!update_id.has_value() || !exchange_time_ms.has_value()) {
    return std::nullopt;
  }

  return GateBookTickerKey{
      .symbol = std::string(symbol),
      .update_id = *update_id,
      .exchange_time_ms = *exchange_time_ms,
  };
}

inline std::string BuildGateSubscribeRequest(std::string_view channel,
                                             std::string_view contract,
                                             std::uint64_t epoch_seconds) {
  return fmt::format(
      FMT_COMPILE(
          R"({{"time":{},"channel":"{}","event":"subscribe","payload":["{}"]}})"),
      epoch_seconds, channel, contract);
}

inline WarmupSelection SelectWarmupPrimary(const WarmupSelectionInput& input) {
  const auto reason = [&](std::string_view health) {
    return fmt::format(
        FMT_COMPILE("health={},gap=pending_public:{},pending_private:{},"
                    "matched={},private_faster={},public_faster={},ties={},"
                    "p50_private_lead_ns={},p99_private_lead_ns={}"),
        health, input.pending_public, input.pending_private, input.matched,
        input.private_faster, input.public_faster, input.ties,
        input.private_lead_p50_ns, input.private_lead_p99_ns);
  };

  if (!input.public_healthy && !input.private_healthy) {
    return WarmupSelection{
        .selected = WarmupPrimary::kNone,
        .reason = reason("both_unhealthy"),
    };
  }
  if (input.public_healthy && !input.private_healthy) {
    return WarmupSelection{
        .selected = WarmupPrimary::kPublic,
        .reason = reason("private_unhealthy"),
    };
  }
  if (!input.public_healthy && input.private_healthy) {
    return WarmupSelection{
        .selected = WarmupPrimary::kPrivate,
        .reason = reason("public_unhealthy"),
    };
  }
  if (input.matched == 0) {
    return WarmupSelection{
        .selected = WarmupPrimary::kNone,
        .reason = reason("no_matched_samples"),
    };
  }
  if (input.private_lead_p50_ns > 0) {
    return WarmupSelection{
        .selected = WarmupPrimary::kPrivate,
        .reason = reason("ok"),
    };
  }
  if (input.private_lead_p50_ns < 0) {
    return WarmupSelection{
        .selected = WarmupPrimary::kPublic,
        .reason = reason("ok"),
    };
  }
  if (input.private_faster > input.public_faster) {
    return WarmupSelection{
        .selected = WarmupPrimary::kPrivate,
        .reason = reason("ok"),
    };
  }
  if (input.public_faster > input.private_faster) {
    return WarmupSelection{
        .selected = WarmupPrimary::kPublic,
        .reason = reason("ok"),
    };
  }
  return WarmupSelection{
      .selected = WarmupPrimary::kNone,
      .reason = reason("tie"),
  };
}

class LatencyPairCollector {
 public:
  explicit LatencyPairCollector(size_t max_pending)
      : max_pending_(max_pending == 0 ? 1 : max_pending) {}

  void Observe(EndpointSide side, const GateBookTickerKey& key,
               std::uint64_t arrival_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string match_key = key.MatchKey();
    auto [it, inserted] = pending_.try_emplace(match_key);
    if (inserted) {
      it->second.key = key;
      pending_order_.push_back(match_key);
    }

    PendingPair& pair = it->second;
    if (side == EndpointSide::kPublic) {
      if (!pair.has_public) {
        pair.public_arrival_ns = arrival_ns;
        pair.has_public = true;
      }
    } else {
      if (!pair.has_private) {
        pair.private_arrival_ns = arrival_ns;
        pair.has_private = true;
      }
    }

    if (pair.has_public && pair.has_private) {
      matched_.push_back(MatchedLatency{
          .key = pair.key,
          .public_arrival_ns = pair.public_arrival_ns,
          .private_arrival_ns = pair.private_arrival_ns,
          .private_lead_ns = static_cast<std::int64_t>(pair.public_arrival_ns) -
                             static_cast<std::int64_t>(pair.private_arrival_ns),
      });
      pending_.erase(it);
    }
    PrunePending();
  }

  [[nodiscard]] LatencyCollectorSnapshot Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    LatencyCollectorSnapshot snapshot;
    snapshot.matched = matched_;
    for (const auto& [_, pair] : pending_) {
      if (pair.has_public && !pair.has_private) {
        ++snapshot.pending_public;
      } else if (pair.has_private && !pair.has_public) {
        ++snapshot.pending_private;
      }
    }
    return snapshot;
  }

 private:
  struct PendingPair {
    GateBookTickerKey key;
    std::uint64_t public_arrival_ns{0};
    std::uint64_t private_arrival_ns{0};
    bool has_public{false};
    bool has_private{false};
  };

  void PrunePending() {
    while (pending_.size() > max_pending_ && !pending_order_.empty()) {
      const std::string oldest = pending_order_.front();
      pending_order_.pop_front();
      pending_.erase(oldest);
    }
  }

  size_t max_pending_{0};
  mutable std::mutex mutex_;
  absl::flat_hash_map<std::string, PendingPair> pending_;
  std::deque<std::string> pending_order_;
  std::vector<MatchedLatency> matched_;
};

}  // namespace aquila::tools

#endif  // AQUILA_TOOLS_WEBSOCKET_LATENCY_COMPARE_SUPPORT_H_
