#ifndef AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_
#define AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include "core/market_data/types.h"

namespace aquila::tools::leadlag {

struct LagVolGuardAuditConfig {
  double jump_threshold{0.005};
  std::uint32_t jump_count{3};
  std::uint64_t jump_window_ns{300'000'000'000ULL};
  double amplitude_threshold{0.025};
  std::uint64_t amplitude_window_ns{1'000'000'000ULL};
  std::uint64_t cooldown_ns{900'000'000'000ULL};
};

enum class LagVolGuardBlockReason : std::uint8_t {
  kNone,
  kCooldown,
  kTrigger,
};

struct LagVolGuardEvaluation {
  bool would_block{false};
  LagVolGuardBlockReason reason{LagVolGuardBlockReason::kNone};
  std::uint32_t jump_count{0};
  double amplitude{0.0};
  bool hot{false};
  bool cooldown_active{false};
  std::uint64_t cooldown_until_ns{0};
};

class LagVolGuardAuditState {
 public:
  void Init(const LagVolGuardAuditConfig& config);
  void OnLagBookTicker(const BookTicker& ticker);
  [[nodiscard]] LagVolGuardEvaluation EvaluateAndAdvanceOpenSignal(
      std::int64_t signal_time_ns);
  [[nodiscard]] std::uint64_t skipped_update_count() const noexcept;
  [[nodiscard]] std::uint64_t non_monotonic_event_time_count() const noexcept;

 private:
  struct JumpEntry {
    std::int64_t event_ns{0};
    double abs_return{0.0};
  };

  struct MidEntry {
    std::int64_t event_ns{0};
    double mid{0.0};
  };

  void Trim(std::int64_t now_ns);
  [[nodiscard]] std::uint32_t CurrentJumpCount(std::int64_t now_ns);
  [[nodiscard]] double CurrentAmplitude(std::int64_t now_ns);

  LagVolGuardAuditConfig config_;
  std::deque<JumpEntry> jumps_;
  std::deque<MidEntry> mids_;
  double previous_mid_{0.0};
  bool has_previous_mid_{false};
  std::int64_t last_event_ns_{0};
  std::uint64_t cooldown_until_ns_{0};
  std::uint64_t skipped_update_count_{0};
  std::uint64_t non_monotonic_event_time_count_{0};
};

[[nodiscard]] bool ParseLagVolGuardAuditDurationNs(std::string_view text,
                                                   std::uint64_t* output,
                                                   std::string* error);

[[nodiscard]] std::string LagVolGuardBlockReasonText(
    LagVolGuardBlockReason reason);

}  // namespace aquila::tools::leadlag

#endif  // AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_
