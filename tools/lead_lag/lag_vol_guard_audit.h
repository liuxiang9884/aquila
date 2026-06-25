#ifndef AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_
#define AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <quill/CsvWriter.h>

#include "core/common/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/drift_guard.h"

namespace aquila {

struct BookTicker;

namespace strategy::leadlag {

struct Config;
struct SignalDecision;
struct SignalDiagnostics;

}  // namespace strategy::leadlag

}  // namespace aquila

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

struct LagVolGuardAuditRow {
  std::uint64_t open_signal_index{0};
  std::string symbol;
  std::int32_t symbol_id{0};
  std::string action;
  std::string side;
  std::int64_t trigger_exchange_ns{0};
  std::int64_t lead_exchange_ns{0};
  std::int64_t lag_exchange_ns{0};
  std::int64_t signal_lead_id{0};
  std::int64_t signal_lag_id{0};
  double raw_price{0.0};
  bool would_block{false};
  std::string would_block_reason{"none"};
  std::uint32_t lag_vol_jump_count{0};
  double lag_vol_amplitude{0.0};
  bool lag_vol_hot{false};
  bool lag_vol_cooldown_active{false};
  std::uint64_t lag_vol_cooldown_until_ns{0};
  LagVolGuardAuditConfig config;
  double drift_instant{std::numeric_limits<double>::quiet_NaN()};
  double ratio_std{std::numeric_limits<double>::quiet_NaN()};
  double drift_mean{std::numeric_limits<double>::quiet_NaN()};
  std::string drift_guard_outcome{"not_evaluated"};
};

struct LagVolGuardAuditPairConfig {
  std::string symbol;
  std::int32_t symbol_id{0};
  Exchange lead_exchange{Exchange::kBinance};
  Exchange lag_exchange{Exchange::kGate};
  strategy::leadlag::DriftGuardConfig drift_guard;
  std::size_t drift_guard_initial_capacity{
      strategy::leadlag::kDefaultDriftGuardWindowCapacity};
};

struct LagVolGuardAuditCsvSchema {
  static constexpr char const* header =
      "open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,"
      "lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,"
      "raw_price,would_block,would_block_reason,lag_vol_jump_count,"
      "lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,"
      "lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,"
      "jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,"
      "drift_instant,ratio_std,drift_mean,drift_guard_outcome";
  static constexpr char const* format =
      "{},{},{},{},{},{},{},{},{},{},{:.12g},{},{},{},{:.12g},{},{},{},"
      "{:.12g},{},{},{:.12g},{},{},{:.12g},{:.12g},{:.12g},{}";
};

class LagVolGuardAuditCsvWriter {
 public:
  using Writer = quill::CsvWriter<LagVolGuardAuditCsvSchema,
                                  nova::LogManager::NovaFrontendOptions>;

  LagVolGuardAuditCsvWriter() = default;
  ~LagVolGuardAuditCsvWriter() = default;

  LagVolGuardAuditCsvWriter(const LagVolGuardAuditCsvWriter&) = delete;
  LagVolGuardAuditCsvWriter& operator=(const LagVolGuardAuditCsvWriter&) =
      delete;

  [[nodiscard]] bool Open(const std::filesystem::path& path,
                          std::string* error);
  void Write(const LagVolGuardAuditRow& row) noexcept;
  void Close();

 private:
  std::unique_ptr<Writer> writer_;
};

class LagVolGuardAuditCollector {
 public:
  LagVolGuardAuditCollector(std::vector<LagVolGuardAuditPairConfig> pairs,
                            LagVolGuardAuditConfig config);

  void OnBookTicker(const BookTicker& ticker);
  [[nodiscard]] bool BuildOpenSignalRow(
      const BookTicker& trigger_ticker,
      const strategy::leadlag::SignalDecision& decision,
      const strategy::leadlag::SignalDiagnostics& diagnostics,
      LagVolGuardAuditRow* row);

 private:
  struct PairState {
    LagVolGuardAuditPairConfig pair;
    LagVolGuardAuditState state;
    strategy::leadlag::DriftGuardState drift_guard;
    strategy::leadlag::QuoteSnapshot latest_lead;
    strategy::leadlag::QuoteSnapshot latest_lag;
    bool has_latest_lead{false};
    bool has_latest_lag{false};
  };

  [[nodiscard]] PairState* FindPair(std::int32_t symbol_id) noexcept;

  LagVolGuardAuditConfig config_;
  std::vector<PairState> pairs_;
  std::uint64_t next_open_signal_index_{0};
};

[[nodiscard]] std::vector<LagVolGuardAuditPairConfig>
BuildLagVolGuardAuditPairs(const strategy::leadlag::Config& config);

}  // namespace aquila::tools::leadlag

#endif  // AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_
