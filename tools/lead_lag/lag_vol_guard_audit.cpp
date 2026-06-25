#include "tools/lead_lag/lag_vol_guard_audit.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "strategy/lead_lag/raw_market_state.h"
#include "strategy/lead_lag/strategy.h"

namespace aquila::tools::leadlag {
namespace {

[[nodiscard]] double MidPrice(const BookTicker& ticker) noexcept {
  return (ticker.bid_price + ticker.ask_price) * 0.5;
}

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    *error = std::string(message);
  }
}

[[nodiscard]] std::int64_t SaturatingWindowLowerBound(
    std::int64_t now_ns, std::uint64_t window_ns) noexcept {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::uint64_t kNegativeRange =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
      1ULL;
  const std::uint64_t distance_from_min =
      static_cast<std::uint64_t>(now_ns) - static_cast<std::uint64_t>(kMin);
  if (window_ns >= distance_from_min) {
    return kMin;
  }

  const std::uint64_t offset_from_min = distance_from_min - window_ns;
  if (offset_from_min < kNegativeRange) {
    return kMin + static_cast<std::int64_t>(offset_from_min);
  }
  return static_cast<std::int64_t>(offset_from_min - kNegativeRange);
}

[[nodiscard]] std::uint64_t SaturatingAdd(std::uint64_t lhs,
                                          std::uint64_t rhs) noexcept {
  constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
  if (lhs > kMax - rhs) {
    return kMax;
  }
  return lhs + rhs;
}

}  // namespace

void LagVolGuardAuditState::Init(const LagVolGuardAuditConfig& config) {
  config_ = config;
  jumps_.clear();
  mids_.clear();
  previous_mid_ = 0.0;
  has_previous_mid_ = false;
  last_event_ns_ = 0;
  cooldown_until_ns_ = 0;
  skipped_update_count_ = 0;
  non_monotonic_event_time_count_ = 0;
}

void LagVolGuardAuditState::OnLagBookTicker(const BookTicker& ticker) {
  const std::int64_t event_ns =
      strategy::leadlag::BookTickerEventTimeNs(ticker);
  const double current_mid = MidPrice(ticker);
  if (event_ns <= 0 || ticker.bid_price <= 0.0 || ticker.ask_price <= 0.0 ||
      current_mid <= 0.0) {
    ++skipped_update_count_;
    return;
  }
  if (last_event_ns_ > 0 && event_ns < last_event_ns_) {
    ++non_monotonic_event_time_count_;
  }
  last_event_ns_ = event_ns;
  if (!has_previous_mid_) {
    previous_mid_ = current_mid;
    has_previous_mid_ = true;
    return;
  }
  if (previous_mid_ == current_mid) {
    return;
  }
  mids_.push_back(MidEntry{.event_ns = event_ns, .mid = current_mid});
  jumps_.push_back(JumpEntry{
      .event_ns = event_ns,
      .abs_return = std::abs(current_mid / previous_mid_ - 1.0),
  });
  previous_mid_ = current_mid;
  Trim(event_ns);
}

LagVolGuardEvaluation LagVolGuardAuditState::EvaluateAndAdvanceOpenSignal(
    std::int64_t signal_time_ns) {
  Trim(signal_time_ns);

  LagVolGuardEvaluation eval;
  eval.cooldown_until_ns = cooldown_until_ns_;
  if (signal_time_ns <= 0) {
    return eval;
  }

  eval.jump_count = CurrentJumpCount(signal_time_ns);
  eval.amplitude = CurrentAmplitude(signal_time_ns);
  eval.hot = eval.jump_count >= config_.jump_count ||
             eval.amplitude > config_.amplitude_threshold;
  eval.cooldown_active =
      static_cast<std::uint64_t>(signal_time_ns) < cooldown_until_ns_;
  if (eval.cooldown_active) {
    eval.would_block = true;
    eval.reason = LagVolGuardBlockReason::kCooldown;
    return eval;
  }
  if (eval.hot) {
    eval.would_block = true;
    eval.reason = LagVolGuardBlockReason::kTrigger;
    cooldown_until_ns_ = SaturatingAdd(
        static_cast<std::uint64_t>(signal_time_ns), config_.cooldown_ns);
    eval.cooldown_until_ns = cooldown_until_ns_;
    return eval;
  }
  return eval;
}

std::uint64_t LagVolGuardAuditState::skipped_update_count() const noexcept {
  return skipped_update_count_;
}

std::uint64_t LagVolGuardAuditState::non_monotonic_event_time_count()
    const noexcept {
  return non_monotonic_event_time_count_;
}

void LagVolGuardAuditState::Trim(std::int64_t now_ns) {
  // Keep Go reference semantics: arrival-ordered replay state is trimmed only
  // by the front lower bound, without excluding future ticks or older
  // out-of-order entries behind them.
  const std::int64_t jump_cutoff =
      SaturatingWindowLowerBound(now_ns, config_.jump_window_ns);
  while (!jumps_.empty() && jumps_.front().event_ns < jump_cutoff) {
    jumps_.pop_front();
  }

  const std::int64_t amplitude_cutoff =
      SaturatingWindowLowerBound(now_ns, config_.amplitude_window_ns);
  while (!mids_.empty() && mids_.front().event_ns < amplitude_cutoff) {
    mids_.pop_front();
  }
}

std::uint32_t LagVolGuardAuditState::CurrentJumpCount(std::int64_t now_ns) {
  Trim(now_ns);
  std::uint32_t count = 0;
  for (const JumpEntry& entry : jumps_) {
    if (entry.abs_return >= config_.jump_threshold) {
      ++count;
    }
  }
  return count;
}

double LagVolGuardAuditState::CurrentAmplitude(std::int64_t now_ns) {
  Trim(now_ns);
  if (mids_.size() < 2) {
    return 0.0;
  }

  double min_mid = mids_.front().mid;
  double max_mid = mids_.front().mid;
  for (const MidEntry& entry : mids_) {
    min_mid = std::min(min_mid, entry.mid);
    max_mid = std::max(max_mid, entry.mid);
  }
  return min_mid > 0.0 ? max_mid / min_mid - 1.0 : 0.0;
}

bool ParseLagVolGuardAuditDurationNs(std::string_view text,
                                     std::uint64_t* output,
                                     std::string* error) {
  if (output == nullptr) {
    SetError(error, "duration output pointer must not be null");
    return false;
  }

  std::uint64_t value = 0;
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed = std::from_chars(begin, end, value);
  if (parsed.ec != std::errc{} || parsed.ptr == begin) {
    SetError(error, "duration must be an integer duration with unit");
    return false;
  }

  const std::string_view unit{parsed.ptr,
                              static_cast<std::size_t>(end - parsed.ptr)};
  std::uint64_t multiplier = 0;
  if (unit == "ns") {
    multiplier = 1;
  } else if (unit == "us") {
    multiplier = 1'000ULL;
  } else if (unit == "ms") {
    multiplier = 1'000'000ULL;
  } else if (unit == "s") {
    multiplier = 1'000'000'000ULL;
  } else if (unit == "m") {
    multiplier = 60'000'000'000ULL;
  } else if (unit == "h") {
    multiplier = 3'600'000'000'000ULL;
  } else {
    SetError(error, "duration unit must be ns, us, ms, s, m, or h");
    return false;
  }

  if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    SetError(error, "duration overflows uint64 nanoseconds");
    return false;
  }
  *output = value * multiplier;
  return true;
}

std::string LagVolGuardBlockReasonText(LagVolGuardBlockReason reason) {
  switch (reason) {
    case LagVolGuardBlockReason::kNone:
      return "none";
    case LagVolGuardBlockReason::kCooldown:
      return "lag-vol-guard-cooldown";
    case LagVolGuardBlockReason::kTrigger:
      return "lag-vol-guard-trigger";
  }
  return "none";
}

bool LagVolGuardAuditCsvWriter::Open(const std::filesystem::path& path,
                                     std::string* error) {
  try {
    writer_ = std::make_unique<Writer>(path.string());
  } catch (const std::exception& ex) {
    writer_.reset();
    if (error != nullptr) {
      *error = fmt::format("failed to open lag vol guard audit '{}': {}",
                           path.string(), ex.what());
    }
    return false;
  }
  return true;
}

void LagVolGuardAuditCsvWriter::Write(
    const LagVolGuardAuditRow& row) noexcept {
  if (writer_ == nullptr) {
    return;
  }
  writer_->append_row(
      row.open_signal_index, row.symbol, row.symbol_id, row.action, row.side,
      row.trigger_exchange_ns, row.lead_exchange_ns, row.lag_exchange_ns,
      row.signal_lead_id, row.signal_lag_id, row.raw_price,
      row.would_block ? "true" : "false", row.would_block_reason,
      row.lag_vol_jump_count, row.lag_vol_amplitude,
      row.lag_vol_hot ? "true" : "false",
      row.lag_vol_cooldown_active ? "true" : "false",
      row.lag_vol_cooldown_until_ns, row.config.jump_threshold,
      row.config.jump_count, row.config.jump_window_ns,
      row.config.amplitude_threshold, row.config.amplitude_window_ns,
      row.config.cooldown_ns, row.drift_guard_outcome);
}

void LagVolGuardAuditCsvWriter::Close() {
  writer_.reset();
}

LagVolGuardAuditCollector::LagVolGuardAuditCollector(
    std::vector<LagVolGuardAuditPairConfig> pairs,
    LagVolGuardAuditConfig config)
    : config_(config) {
  pairs_.reserve(pairs.size());
  for (LagVolGuardAuditPairConfig& pair : pairs) {
    PairState state;
    state.pair = std::move(pair);
    state.state.Init(config_);
    pairs_.push_back(std::move(state));
  }
}

void LagVolGuardAuditCollector::OnBookTicker(const BookTicker& ticker) {
  PairState* pair = FindPair(ticker.symbol_id);
  if (pair == nullptr || ticker.exchange != pair->pair.lag_exchange) {
    return;
  }
  pair->state.OnLagBookTicker(ticker);
}

bool LagVolGuardAuditCollector::BuildOpenSignalRow(
    const BookTicker& trigger_ticker,
    const strategy::leadlag::SignalDecision& decision,
    const strategy::leadlag::SignalDiagnostics& diagnostics,
    LagVolGuardAuditRow* row) {
  if (row == nullptr || !decision.triggered || decision.intent.reduce_only ||
      (decision.action != strategy::leadlag::SignalAction::kOpenLong &&
       decision.action != strategy::leadlag::SignalAction::kOpenShort)) {
    return false;
  }

  PairState* pair = FindPair(decision.intent.symbol_id);
  if (pair == nullptr) {
    return false;
  }

  const LagVolGuardEvaluation eval =
      pair->state.EvaluateAndAdvanceOpenSignal(diagnostics.event_ns);
  *row = LagVolGuardAuditRow{
      .open_signal_index = next_open_signal_index_++,
      .symbol = pair->pair.symbol,
      .symbol_id = pair->pair.symbol_id,
      .action = std::string(magic_enum::enum_name(decision.action)),
      .side = std::string(magic_enum::enum_name(decision.intent.side)),
      .trigger_exchange_ns = trigger_ticker.exchange_ns,
      .lead_exchange_ns = diagnostics.lead_raw.exchange_ns,
      .lag_exchange_ns = diagnostics.lag.exchange_ns,
      .signal_lead_id = diagnostics.lead_raw.id,
      .signal_lag_id = diagnostics.lag.id,
      .raw_price = decision.intent.price,
      .would_block = eval.would_block,
      .would_block_reason = LagVolGuardBlockReasonText(eval.reason),
      .lag_vol_jump_count = eval.jump_count,
      .lag_vol_amplitude = eval.amplitude,
      .lag_vol_hot = eval.hot,
      .lag_vol_cooldown_active = eval.cooldown_active,
      .lag_vol_cooldown_until_ns = eval.cooldown_until_ns,
      .config = config_,
      .drift_guard_outcome = "not_evaluated",
  };
  return true;
}

LagVolGuardAuditCollector::PairState* LagVolGuardAuditCollector::FindPair(
    std::int32_t symbol_id) noexcept {
  for (PairState& pair : pairs_) {
    if (pair.pair.symbol_id == symbol_id) {
      return &pair;
    }
  }
  return nullptr;
}

std::vector<LagVolGuardAuditPairConfig> BuildLagVolGuardAuditPairs(
    const strategy::leadlag::Config& config) {
  std::vector<LagVolGuardAuditPairConfig> pairs;
  pairs.reserve(config.pairs.size());
  for (const strategy::leadlag::PairConfig& pair : config.pairs) {
    pairs.push_back(LagVolGuardAuditPairConfig{
        .symbol = pair.symbol,
        .symbol_id = pair.symbol_id,
        .lag_exchange = pair.lag_exchange,
    });
  }
  return pairs;
}

}  // namespace aquila::tools::leadlag
