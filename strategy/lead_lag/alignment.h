#ifndef AQUILA_STRATEGY_LEAD_LAG_ALIGNMENT_H_
#define AQUILA_STRATEGY_LEAD_LAG_ALIGNMENT_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "strategy/lead_lag/raw_market_state.h"
#include "strategy/lead_lag/types.h"
#include "strategy/lead_lag/window_stats.h"

namespace aquila::strategy::leadlag {

enum class AlignmentPhase : std::uint8_t {
  kBootstrap,
  kAligning,
  kActive,
};

struct AlignmentConfig {
  std::uint64_t drift_period_ns{0};
  std::uint64_t stats_window_ns{0};
  std::uint64_t drift_warmup_ns{0};
  std::uint32_t drift_min_samples{0};
  std::size_t initial_capacity{0};
};

struct AlignmentSnapshot {
  AlignmentPhase phase{AlignmentPhase::kBootstrap};
  bool active{false};
  bool drift_ready{false};
  std::uint32_t drift_samples{0};
  std::int64_t first_paired_drift_ns{0};
  double drift_mean{1.0};
  double drift_std{0.0};
  double drift_std_ema{0.0};
  double drift_deviation{0.0};
  bool resume_lead_tick{false};
};

struct ActiveTransition {
  bool valid{false};
  PairRole trigger_role{PairRole::kNone};
  QuoteSnapshot lead_seed_raw;
  QuoteSnapshot lead_seed_drifted;
  QuoteSnapshot lag_seed;
  bool resume_lead_tick{false};
  AlignmentSnapshot alignment;
};

class AlignmentState {
 public:
  void Init(const AlignmentConfig& config) {
    config_ = config;
    drift_window_.Init(config_.drift_period_ns, config_.initial_capacity);
    drift_std_window_.Init(config_.stats_window_ns, config_.initial_capacity);
    Reset();
  }

  void Reset() noexcept {
    drift_window_.Clear();
    drift_std_window_.Clear();
    phase_ = AlignmentPhase::kBootstrap;
    drift_ready_ = false;
    drift_samples_ = 0;
    first_paired_drift_ns_ = 0;
    resume_lead_tick_ = false;
  }

  void OnPairedRawBbo(std::int64_t now_ns, const QuoteSnapshot& raw_lead,
                      const QuoteSnapshot& raw_lag) {
    const double lead_sum = raw_lead.bid_price + raw_lead.ask_price;
    const double lag_sum = raw_lag.bid_price + raw_lag.ask_price;
    const double drift = lag_sum / lead_sum;

    if (!drift_ready_) {
      first_paired_drift_ns_ = now_ns;
      drift_ready_ = true;
    }
    ++drift_samples_;
    drift_window_.Update(now_ns, drift);
    drift_std_window_.Update(now_ns, drift_window_.stddev());
  }

  [[nodiscard]] AlignmentPhase UpdatePhase(std::int64_t now_ns, bool lead_valid,
                                           bool lag_valid) noexcept {
    if (phase_ == AlignmentPhase::kActive) {
      return phase_;
    }
    if (!lead_valid || !lag_valid) {
      phase_ = AlignmentPhase::kBootstrap;
      return phase_;
    }
    phase_ =
        Ready(now_ns) ? AlignmentPhase::kActive : AlignmentPhase::kAligning;
    return phase_;
  }

  [[nodiscard]] ActiveTransition EnterActive(std::int64_t,
                                             const ActiveSeed& seed,
                                             PairRole trigger_role) noexcept {
    ActiveTransition transition;
    if (!seed.valid) {
      return transition;
    }
    phase_ = AlignmentPhase::kActive;
    resume_lead_tick_ = seed.resume_lead_tick && trigger_role == PairRole::kLag;
    transition = ActiveTransition{
        .valid = true,
        .trigger_role = trigger_role,
        .lead_seed_raw = seed.lead,
        .lead_seed_drifted = DriftLead(seed.lead),
        .lag_seed = seed.lag,
        .resume_lead_tick = resume_lead_tick_,
        .alignment = Snapshot(),
    };
    return transition;
  }

  [[nodiscard]] QuoteSnapshot DriftLead(
      const QuoteSnapshot& raw_lead) const noexcept {
    const double drift = drift_ready_ ? drift_window_.mean() : 1.0;
    return QuoteSnapshot{
        .local_ns = raw_lead.local_ns,
        .bid_price = raw_lead.bid_price * drift,
        .ask_price = raw_lead.ask_price * drift,
    };
  }

  [[nodiscard]] bool ConsumeResumeLeadTick(PairRole role) noexcept {
    if (role != PairRole::kLead || !resume_lead_tick_) {
      return false;
    }
    resume_lead_tick_ = false;
    return true;
  }

  [[nodiscard]] AlignmentSnapshot Snapshot() const noexcept {
    const double drift_mean = drift_ready_ ? drift_window_.mean() : 1.0;
    return AlignmentSnapshot{
        .phase = phase_,
        .active = phase_ == AlignmentPhase::kActive,
        .drift_ready = drift_ready_,
        .drift_samples = drift_samples_,
        .first_paired_drift_ns = first_paired_drift_ns_,
        .drift_mean = drift_mean,
        .drift_std = drift_ready_ ? drift_window_.stddev() : 0.0,
        .drift_std_ema = drift_ready_ ? drift_std_window_.mean() : 0.0,
        .drift_deviation = std::abs(drift_mean - 1.0),
        .resume_lead_tick = resume_lead_tick_,
    };
  }

  [[nodiscard]] AlignmentPhase phase() const noexcept {
    return phase_;
  }

  [[nodiscard]] bool active() const noexcept {
    return phase_ == AlignmentPhase::kActive;
  }

 private:
  [[nodiscard]] bool Ready(std::int64_t now_ns) const noexcept {
    const std::uint64_t warmup_ns = config_.drift_warmup_ns == 0
                                        ? config_.stats_window_ns
                                        : config_.drift_warmup_ns;
    return drift_ready_ && drift_samples_ >= config_.drift_min_samples &&
           now_ns >= first_paired_drift_ns_ &&
           static_cast<std::uint64_t>(now_ns - first_paired_drift_ns_) >=
               warmup_ns;
  }

  AlignmentConfig config_;
  MeanStdWindow drift_window_;
  MeanWindow drift_std_window_;
  AlignmentPhase phase_{AlignmentPhase::kBootstrap};
  bool drift_ready_{false};
  std::uint32_t drift_samples_{0};
  std::int64_t first_paired_drift_ns_{0};
  bool resume_lead_tick_{false};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_ALIGNMENT_H_
