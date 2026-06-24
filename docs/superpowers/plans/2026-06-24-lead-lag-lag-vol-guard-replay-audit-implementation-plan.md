# LeadLag Lag Vol Guard Replay Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a replay-only `lag_vol_guard` audit path that evaluates Go-like post-signal filtering for LeadLag open signals without changing live strategy behavior.

**Architecture:** Add a focused C++ helper under `tools/lead_lag/` that owns guard state, CLI parsing support, and CSV output. Wire that helper into `lead_lag_replay` only; keep `strategy/lead_lag/Strategy` and live runner untouched. Add a Python summary script that joins audit rows with existing `order_detail.csv` and optional `position.csv`.

**Tech Stack:** C++20, CMake, GTest, CLI11, quill CSV writer, fmt, magic_enum, Python 3 stdlib `csv/json/unittest`, existing LeadLag replay/report scripts.

---

## File Structure

- Create `tools/lead_lag/lag_vol_guard_audit.h`: replay-only config, state, evaluation result, CSV row, collector, duration parser, writer declarations.
- Create `tools/lead_lag/lag_vol_guard_audit.cpp`: guard state logic, CSV writer, pair config builder, CLI duration parsing.
- Modify `tools/lead_lag/replay.cpp`: add CLI options, instantiate collector/writer, update guard state on each tick, write audit rows on open signals.
- Modify `tools/CMakeLists.txt`: compile `lag_vol_guard_audit.cpp` into `lead_lag_replay`.
- Create `test/tools/lead_lag/lag_vol_guard_audit_test.cpp`: unit tests for guard math, cooldown, duration parsing, CSV output.
- Modify `test/tools/lead_lag/CMakeLists.txt`: add `lead_lag_lag_vol_guard_audit_test`.
- Create `scripts/lead_lag/summarize_guard_audit.py`: joins audit CSV with order and optional position CSV, emits JSON and Markdown summary.
- Create `scripts/test/lead_lag/summarize_guard_audit_test.py`: Python unit tests for matching, grouping, unmatched rows, and optional position handling.
- Modify `strategy/lead_lag/README.md`: document replay audit usage and boundary.
- Modify `docs/diagnostic_fields.md`: register `lag_vol_guard_audit.csv` fields.

## Task 1: C++ Guard State

**Files:**
- Create: `tools/lead_lag/lag_vol_guard_audit.h`
- Create: `tools/lead_lag/lag_vol_guard_audit.cpp`
- Create: `test/tools/lead_lag/lag_vol_guard_audit_test.cpp`
- Modify: `test/tools/lead_lag/CMakeLists.txt`

- [ ] **Step 1: Add failing guard-state tests**

Create `test/tools/lead_lag/lag_vol_guard_audit_test.cpp` with tests covering default config, jump count, amplitude, cooldown trigger, cooldown block, invalid BBO skip, and duration parsing.

```cpp
#include "tools/lead_lag/lag_vol_guard_audit.h"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace aquila::tools::leadlag {
namespace {

BookTicker Ticker(std::int64_t id, std::int64_t exchange_ns, double bid,
                  double ask) {
  return BookTicker{
      .id = id,
      .symbol_id = 4,
      .exchange = Exchange::kGate,
      .exchange_ns = exchange_ns,
      .local_ns = exchange_ns + 1000,
      .bid_price = bid,
      .bid_volume = 1.0,
      .ask_price = ask,
      .ask_volume = 1.0,
  };
}

TEST(LeadLagLagVolGuardAuditTest, DefaultsMatchGoReference) {
  const LagVolGuardAuditConfig config;
  EXPECT_DOUBLE_EQ(config.jump_threshold, 0.005);
  EXPECT_EQ(config.jump_count, 3U);
  EXPECT_EQ(config.jump_window_ns, 300'000'000'000ULL);
  EXPECT_DOUBLE_EQ(config.amplitude_threshold, 0.025);
  EXPECT_EQ(config.amplitude_window_ns, 1'000'000'000ULL);
  EXPECT_EQ(config.cooldown_ns, 900'000'000'000ULL);
}

TEST(LeadLagLagVolGuardAuditTest, TriggersOnJumpCountAndStartsCooldown) {
  LagVolGuardAuditState state;
  state.Init(LagVolGuardAuditConfig{});
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 100.7, 100.9));
  state.OnLagBookTicker(Ticker(3, 3'000'000'000, 101.5, 101.7));
  state.OnLagBookTicker(Ticker(4, 4'000'000'000, 102.3, 102.5));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(5'000'000'000);

  EXPECT_TRUE(eval.would_block);
  EXPECT_EQ(eval.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_EQ(eval.jump_count, 3U);
  EXPECT_TRUE(eval.hot);
  EXPECT_EQ(eval.cooldown_until_ns, 905'000'000'000ULL);
}

TEST(LeadLagLagVolGuardAuditTest, BlocksDuringCooldownWithoutExtendingIt) {
  LagVolGuardAuditState state;
  state.Init(LagVolGuardAuditConfig{});
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 100.7, 100.9));
  state.OnLagBookTicker(Ticker(3, 3'000'000'000, 101.5, 101.7));
  state.OnLagBookTicker(Ticker(4, 4'000'000'000, 102.3, 102.5));
  const LagVolGuardEvaluation first =
      state.EvaluateAndAdvanceOpenSignal(5'000'000'000);

  const LagVolGuardEvaluation second =
      state.EvaluateAndAdvanceOpenSignal(6'000'000'000);

  EXPECT_EQ(first.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_TRUE(second.would_block);
  EXPECT_EQ(second.reason, LagVolGuardBlockReason::kCooldown);
  EXPECT_EQ(second.cooldown_until_ns, first.cooldown_until_ns);
}

TEST(LeadLagLagVolGuardAuditTest, TriggersOnAmplitudeWithoutEnoughJumps) {
  LagVolGuardAuditConfig config;
  config.jump_threshold = 0.50;
  LagVolGuardAuditState state;
  state.Init(config);
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 1'100'000'000, 103.0, 103.2));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(1'200'000'000);

  EXPECT_TRUE(eval.would_block);
  EXPECT_EQ(eval.reason, LagVolGuardBlockReason::kTrigger);
  EXPECT_EQ(eval.jump_count, 0U);
  EXPECT_GT(eval.amplitude, config.amplitude_threshold);
}

TEST(LeadLagLagVolGuardAuditTest, SkipsInvalidAndUnchangedMidUpdates) {
  LagVolGuardAuditState state;
  state.Init(LagVolGuardAuditConfig{});
  state.OnLagBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(2, 2'000'000'000, 100.0, 100.2));
  state.OnLagBookTicker(Ticker(3, 3'000'000'000, -1.0, 100.2));

  const LagVolGuardEvaluation eval =
      state.EvaluateAndAdvanceOpenSignal(4'000'000'000);

  EXPECT_FALSE(eval.would_block);
  EXPECT_EQ(eval.jump_count, 0U);
  EXPECT_DOUBLE_EQ(eval.amplitude, 0.0);
  EXPECT_EQ(state.skipped_update_count(), 1U);
}

TEST(LeadLagLagVolGuardAuditTest, ParsesDurationTextToNanoseconds) {
  std::string error;
  std::uint64_t value = 0;
  EXPECT_TRUE(ParseLagVolGuardAuditDurationNs("5m", &value, &error)) << error;
  EXPECT_EQ(value, 300'000'000'000ULL);
  EXPECT_TRUE(ParseLagVolGuardAuditDurationNs("1500ms", &value, &error))
      << error;
  EXPECT_EQ(value, 1'500'000'000ULL);
  EXPECT_FALSE(ParseLagVolGuardAuditDurationNs("1d", &value, &error));
  EXPECT_NE(error.find("unit must be ns, us, ms, s, m, or h"),
            std::string::npos);
}

}  // namespace
}  // namespace aquila::tools::leadlag
```

- [ ] **Step 2: Register failing test target**

Modify `test/tools/lead_lag/CMakeLists.txt` by adding this block after `lead_lag_freshness_preflight_cli_test`:

```cmake
add_executable(lead_lag_lag_vol_guard_audit_test
    lag_vol_guard_audit_test.cpp
    ${PROJECT_SOURCE_DIR}/tools/lead_lag/lag_vol_guard_audit.cpp
)

target_link_libraries(lead_lag_lag_vol_guard_audit_test
    PRIVATE
        aquila_core
        aquila_strategy
        fmt::fmt-header-only
        GTest::gtest_main
        magic_enum::magic_enum
        nova
        quill::quill
)

add_test(NAME lead_lag_lag_vol_guard_audit_test
         COMMAND lead_lag_lag_vol_guard_audit_test)
```

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R lead_lag_lag_vol_guard_audit_test --output-on-failure
```

Expected: build fails because `tools/lead_lag/lag_vol_guard_audit.h` does not exist, or the test fails because the symbols are undefined.

- [ ] **Step 3: Implement guard-state header**

Create `tools/lead_lag/lag_vol_guard_audit.h` with these public types and methods:

```cpp
#ifndef AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_
#define AQUILA_TOOLS_LEAD_LAG_LAG_VOL_GUARD_AUDIT_H_

#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <quill/CsvWriter.h>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/strategy.h"

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
    double abs_r_tick{0.0};
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
```

- [ ] **Step 4: Implement guard-state source**

Create `tools/lead_lag/lag_vol_guard_audit.cpp` with the state logic. Use `strategy::leadlag::BookTickerEventTimeNs(ticker)` for event time. Keep invalid mid updates out of the windows.

```cpp
#include "tools/lead_lag/lag_vol_guard_audit.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/core.h>

#include "strategy/lead_lag/raw_market_state.h"

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
      .abs_r_tick = std::abs(current_mid / previous_mid_ - 1.0),
  });
  previous_mid_ = current_mid;
  Trim(event_ns);
}

LagVolGuardEvaluation LagVolGuardAuditState::EvaluateAndAdvanceOpenSignal(
    std::int64_t signal_time_ns) {
  Trim(signal_time_ns);
  LagVolGuardEvaluation eval;
  eval.jump_count = CurrentJumpCount(signal_time_ns);
  eval.amplitude = CurrentAmplitude(signal_time_ns);
  eval.hot = eval.jump_count >= config_.jump_count ||
             eval.amplitude > config_.amplitude_threshold;
  eval.cooldown_until_ns = cooldown_until_ns_;
  eval.cooldown_active =
      signal_time_ns > 0 &&
      static_cast<std::uint64_t>(signal_time_ns) < cooldown_until_ns_;
  if (eval.cooldown_active) {
    eval.would_block = true;
    eval.reason = LagVolGuardBlockReason::kCooldown;
    return eval;
  }
  if (eval.hot) {
    eval.would_block = true;
    eval.reason = LagVolGuardBlockReason::kTrigger;
    cooldown_until_ns_ =
        static_cast<std::uint64_t>(std::max<std::int64_t>(signal_time_ns, 0)) +
        config_.cooldown_ns;
    eval.cooldown_until_ns = cooldown_until_ns_;
    return eval;
  }
  eval.reason = LagVolGuardBlockReason::kNone;
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
  const auto trim_jumps_before =
      now_ns - static_cast<std::int64_t>(config_.jump_window_ns);
  while (!jumps_.empty() && jumps_.front().event_ns < trim_jumps_before) {
    jumps_.pop_front();
  }
  const auto trim_mids_before =
      now_ns - static_cast<std::int64_t>(config_.amplitude_window_ns);
  while (!mids_.empty() && mids_.front().event_ns < trim_mids_before) {
    mids_.pop_front();
  }
}

std::uint32_t LagVolGuardAuditState::CurrentJumpCount(std::int64_t now_ns) {
  Trim(now_ns);
  std::uint32_t count = 0;
  for (const JumpEntry& entry : jumps_) {
    if (entry.abs_r_tick >= config_.jump_threshold) {
      ++count;
    }
  }
  return count;
}

double LagVolGuardAuditState::CurrentAmplitude(std::int64_t now_ns) {
  Trim(now_ns);
  if (mids_.empty()) {
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
  std::uint64_t value = 0;
  const char* begin = text.data();
  const char* end = text.data() + text.size();
  const auto parsed = std::from_chars(begin, end, value);
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
  if (value > UINT64_MAX / multiplier) {
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

}  // namespace aquila::tools::leadlag
```

- [ ] **Step 5: Run guard-state tests**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R lead_lag_lag_vol_guard_audit_test --output-on-failure
```

Expected: `100% tests passed` for `lead_lag_lag_vol_guard_audit_test`.

- [ ] **Step 6: Commit Task 1**

```bash
git add tools/lead_lag/lag_vol_guard_audit.h tools/lead_lag/lag_vol_guard_audit.cpp test/tools/lead_lag/lag_vol_guard_audit_test.cpp test/tools/lead_lag/CMakeLists.txt
git commit -m "feat: add LeadLag lag vol guard audit state"
```

## Task 2: Audit CSV Writer And Collector

**Files:**
- Modify: `tools/lead_lag/lag_vol_guard_audit.h`
- Modify: `tools/lead_lag/lag_vol_guard_audit.cpp`
- Modify: `test/tools/lead_lag/lag_vol_guard_audit_test.cpp`

- [ ] **Step 1: Add failing writer and collector tests**

Append tests to `test/tools/lead_lag/lag_vol_guard_audit_test.cpp` that verify header/row output and per-symbol lag exchange routing.

```cpp
#include <filesystem>
#include <fstream>
#include <sstream>

#include "nova/utils/log.h"

void EnsureLoggingStarted() {
  static const bool started = [] {
    nova::LogConfig config;
    config.set_console_sink_name("");
    config.set_file_sink_name((std::filesystem::temp_directory_path() /
                               "aquila_lag_vol_guard_audit_test.log")
                                  .string());
    nova::InitializeLogging(config);
    return true;
  }();
  (void)started;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

strategy::leadlag::SignalDiagnostics Diagnostics() {
  return strategy::leadlag::SignalDiagnostics{
      .event_ns = 5'000'000'000,
      .role = strategy::leadlag::PairRole::kLead,
      .lead_raw =
          strategy::leadlag::QuoteSnapshot{
              .id = 7001,
              .event_ns = 5'000'000'000,
              .exchange_ns = 5'000'000'000,
              .local_ns = 5'000'001'000,
              .bid_price = 10.0,
              .ask_price = 10.1,
          },
      .lag =
          strategy::leadlag::QuoteSnapshot{
              .id = 8002,
              .event_ns = 4'999'000'000,
              .exchange_ns = 4'999'000'000,
              .local_ns = 4'999'001'000,
              .bid_price = 9.9,
              .ask_price = 10.0,
          },
  };
}

strategy::leadlag::SignalDecision OpenLongDecision() {
  return strategy::leadlag::SignalDecision{
      .triggered = true,
      .action = strategy::leadlag::SignalAction::kOpenLong,
      .intent =
          strategy::leadlag::OrderIntent{
              .action = strategy::leadlag::SignalAction::kOpenLong,
              .exchange = Exchange::kGate,
              .symbol_id = 4,
              .side = OrderSide::kBuy,
              .price = 10.0,
              .reduce_only = false,
          },
  };
}

TEST(LeadLagLagVolGuardAuditTest, WriterOutputsHeaderAndOpenSignalRow) {
  EnsureLoggingStarted();
  const std::filesystem::path output_path =
      std::filesystem::temp_directory_path() /
      "aquila_lag_vol_guard_audit.csv";
  std::filesystem::remove(output_path);

  LagVolGuardAuditCsvWriter writer;
  std::string error;
  ASSERT_TRUE(writer.Open(output_path, &error)) << error;
  writer.Write(LagVolGuardAuditRow{
      .open_signal_index = 0,
      .symbol = "PROVE_USDT",
      .symbol_id = 4,
      .action = "kOpenLong",
      .side = "kBuy",
      .trigger_exchange_ns = 5'000'000'000,
      .lead_exchange_ns = 5'000'000'000,
      .lag_exchange_ns = 4'999'000'000,
      .signal_lead_id = 7001,
      .signal_lag_id = 8002,
      .raw_price = 10.0,
      .would_block = true,
      .would_block_reason = "lag-vol-guard-trigger",
      .lag_vol_jump_count = 3,
      .lag_vol_amplitude = 0.031,
      .lag_vol_hot = true,
      .lag_vol_cooldown_active = false,
      .lag_vol_cooldown_until_ns = 905'000'000'000ULL,
      .config = LagVolGuardAuditConfig{},
      .drift_guard_outcome = "not_evaluated",
  });
  writer.Close();

  EXPECT_EQ(ReadFile(output_path),
            "open_signal_index,symbol,symbol_id,action,side,"
            "trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,"
            "signal_lead_id,signal_lag_id,raw_price,would_block,"
            "would_block_reason,lag_vol_jump_count,lag_vol_amplitude,"
            "lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,"
            "jump_threshold,jump_count_threshold,jump_window_ns,"
            "amplitude_threshold,amplitude_window_ns,cooldown_ns,"
            "drift_instant,ratio_std,drift_mean,drift_guard_outcome\n"
            "0,PROVE_USDT,4,kOpenLong,kBuy,5000000000,5000000000,"
            "4999000000,7001,8002,10,true,lag-vol-guard-trigger,3,"
            "0.031,true,false,905000000000,0.005,3,300000000000,"
            "0.025,1000000000,900000000000,nan,nan,nan,"
            "not_evaluated\n");
}

TEST(LeadLagLagVolGuardAuditTest, CollectorRoutesOnlyConfiguredLagExchange) {
  LagVolGuardAuditCollector collector(
      {LagVolGuardAuditPairConfig{.symbol = "PROVE_USDT",
                                  .symbol_id = 4,
                                  .lag_exchange = Exchange::kGate}},
      LagVolGuardAuditConfig{});
  collector.OnBookTicker(Ticker(1, 1'000'000'000, 100.0, 100.2));
  BookTicker binance = Ticker(2, 2'000'000'000, 101.0, 101.2);
  binance.exchange = Exchange::kBinance;
  collector.OnBookTicker(binance);
  collector.OnBookTicker(Ticker(3, 3'000'000'000, 101.0, 101.2));
  collector.OnBookTicker(Ticker(4, 4'000'000'000, 102.0, 102.2));

  LagVolGuardAuditRow row;
  ASSERT_TRUE(collector.BuildOpenSignalRow(Ticker(5, 5'000'000'000, 0, 0),
                                           OpenLongDecision(), Diagnostics(),
                                           &row));

  EXPECT_EQ(row.symbol, "PROVE_USDT");
  EXPECT_EQ(row.symbol_id, 4);
  EXPECT_EQ(row.signal_lag_id, 8002);
  EXPECT_TRUE(row.would_block);
  EXPECT_EQ(row.would_block_reason, "lag-vol-guard-trigger");
}
```

Run:

```bash
ctest --test-dir build/debug -R lead_lag_lag_vol_guard_audit_test --output-on-failure
```

Expected: fails because writer and collector classes do not exist.

- [ ] **Step 2: Extend header with row, writer, and collector**

Add these declarations before the final namespace close in `tools/lead_lag/lag_vol_guard_audit.h`:

```cpp
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
  std::string drift_guard_outcome{"not_evaluated"};
};

struct LagVolGuardAuditPairConfig {
  std::string symbol;
  std::int32_t symbol_id{0};
  Exchange lag_exchange{Exchange::kGate};
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
      "{:.12g},{},{},{:.12g},{},{},nan,nan,nan,{}";
};

class LagVolGuardAuditCsvWriter {
 public:
  using Writer = quill::CsvWriter<LagVolGuardAuditCsvSchema,
                                  nova::LogManager::NovaFrontendOptions>;

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
  };

  [[nodiscard]] PairState* FindPair(std::int32_t symbol_id) noexcept;

  LagVolGuardAuditConfig config_;
  std::vector<PairState> pairs_;
  std::uint64_t next_open_signal_index_{0};
};

[[nodiscard]] std::vector<LagVolGuardAuditPairConfig>
BuildLagVolGuardAuditPairs(const strategy::leadlag::Config& config);
```

- [ ] **Step 3: Implement writer and collector**

Add to `tools/lead_lag/lag_vol_guard_audit.cpp`:

```cpp
#include <exception>

#include <magic_enum/magic_enum.hpp>

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
```

- [ ] **Step 4: Run writer/collector tests**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R lead_lag_lag_vol_guard_audit_test --output-on-failure
```

Expected: `100% tests passed` for `lead_lag_lag_vol_guard_audit_test`.

- [ ] **Step 5: Commit Task 2**

```bash
git add tools/lead_lag/lag_vol_guard_audit.h tools/lead_lag/lag_vol_guard_audit.cpp test/tools/lead_lag/lag_vol_guard_audit_test.cpp
git commit -m "feat: write LeadLag lag vol guard audit CSV"
```

## Task 3: Replay CLI Integration

**Files:**
- Modify: `tools/lead_lag/replay.cpp`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: Add audit source to replay target**

Modify `tools/CMakeLists.txt`:

```cmake
add_executable(lead_lag_replay
    lead_lag/replay.cpp
    lead_lag/lag_vol_guard_audit.cpp
)
```

Run:

```bash
cmake --build build/debug --target lead_lag_replay -j8
```

Expected: build still succeeds before replay wiring.

- [ ] **Step 2: Add replay CLI fields**

Modify `tools/lead_lag/replay.cpp`:

```cpp
#include <memory>

#include "tools/lead_lag/lag_vol_guard_audit.h"
```

Extend `CliOptions`:

```cpp
std::filesystem::path lag_vol_guard_audit_output_path;
double lag_vol_guard_jump_threshold{0.005};
std::uint32_t lag_vol_guard_jump_count{3};
std::string lag_vol_guard_jump_window{"5m"};
double lag_vol_guard_amplitude_threshold{0.025};
std::string lag_vol_guard_amplitude_window{"1s"};
std::string lag_vol_guard_cooldown{"15m"};
```

In `main()`, add options:

```cpp
app.add_option("--lag-vol-guard-audit-output",
               options.lag_vol_guard_audit_output_path,
               "Optional CSV path for replay-only lag vol guard audit");
app.add_option("--lag-vol-guard-jump-threshold",
               options.lag_vol_guard_jump_threshold,
               "Lag vol guard jump threshold ratio");
app.add_option("--lag-vol-guard-jump-count",
               options.lag_vol_guard_jump_count,
               "Lag vol guard jump count threshold");
app.add_option("--lag-vol-guard-jump-window",
               options.lag_vol_guard_jump_window,
               "Lag vol guard jump window duration");
app.add_option("--lag-vol-guard-amplitude-threshold",
               options.lag_vol_guard_amplitude_threshold,
               "Lag vol guard amplitude threshold ratio");
app.add_option("--lag-vol-guard-amplitude-window",
               options.lag_vol_guard_amplitude_window,
               "Lag vol guard amplitude window duration");
app.add_option("--lag-vol-guard-cooldown", options.lag_vol_guard_cooldown,
               "Lag vol guard cooldown duration");
```

- [ ] **Step 3: Add config builder with fail-fast validation**

Add a helper near `LoadConfig()` in `replay.cpp`:

```cpp
bool BuildLagVolGuardAuditConfig(
    const CliOptions& options,
    leadlag_tools::LagVolGuardAuditConfig* config) {
  if (config == nullptr) {
    return false;
  }
  if (options.lag_vol_guard_jump_threshold <= 0.0) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-jump-threshold must be positive\n");
    return false;
  }
  if (options.lag_vol_guard_jump_count == 0) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-jump-count must be positive\n");
    return false;
  }
  if (options.lag_vol_guard_amplitude_threshold <= 0.0) {
    fmt::print(stderr,
               "[FAIL] --lag-vol-guard-amplitude-threshold must be positive\n");
    return false;
  }
  std::string error;
  std::uint64_t jump_window_ns = 0;
  if (!leadlag_tools::ParseLagVolGuardAuditDurationNs(
          options.lag_vol_guard_jump_window, &jump_window_ns, &error)) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-jump-window {}\n", error);
    return false;
  }
  std::uint64_t amplitude_window_ns = 0;
  if (!leadlag_tools::ParseLagVolGuardAuditDurationNs(
          options.lag_vol_guard_amplitude_window, &amplitude_window_ns,
          &error)) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-amplitude-window {}\n", error);
    return false;
  }
  std::uint64_t cooldown_ns = 0;
  if (!leadlag_tools::ParseLagVolGuardAuditDurationNs(
          options.lag_vol_guard_cooldown, &cooldown_ns, &error)) {
    fmt::print(stderr, "[FAIL] --lag-vol-guard-cooldown {}\n", error);
    return false;
  }
  *config = leadlag_tools::LagVolGuardAuditConfig{
      .jump_threshold = options.lag_vol_guard_jump_threshold,
      .jump_count = options.lag_vol_guard_jump_count,
      .jump_window_ns = jump_window_ns,
      .amplitude_threshold = options.lag_vol_guard_amplitude_threshold,
      .amplitude_window_ns = amplitude_window_ns,
      .cooldown_ns = cooldown_ns,
  };
  return true;
}
```

Use this alias near existing namespace aliases:

```cpp
namespace leadlag_tools = aquila::tools::leadlag;
```

- [ ] **Step 4: Wire collector and writer into ReplayStrategy**

Extend `ReplayStrategy` constructor and member:

```cpp
ReplayStrategy(leadlag::Config config, leadlag::StrategyOptions options,
               ReplayStats* stats, leadlag::SignalCsvWriter* signal_writer,
               leadlag_tools::LagVolGuardAuditCollector* lag_vol_audit
#if defined(AQUILA_LEAD_LAG_ENABLE_MARKET_CALC_CSV)
               ,
               leadlag::MarketCalcCsvWriter* market_calc_writer
#endif
               )
    : inner_(std::move(config), options),
      stats_(stats),
      signal_writer_(signal_writer),
      lag_vol_audit_(lag_vol_audit) {
```

Add member:

```cpp
leadlag_tools::LagVolGuardAuditCollector* lag_vol_audit_{nullptr};
```

In `ReplayStrategy::OnBookTicker()` call audit state before `inner_.OnBookTicker()`:

```cpp
if (lag_vol_audit_ != nullptr) {
  lag_vol_audit_->OnBookTicker(ticker);
}
```

After signal writer call, write audit row:

```cpp
if (lag_vol_audit_ != nullptr && inner_.last_signal_diagnostics_valid()) {
  leadlag_tools::LagVolGuardAuditRow row;
  if (lag_vol_audit_->BuildOpenSignalRow(
          ticker, decision, inner_.last_signal_diagnostics(), &row)) {
    lag_vol_audit_writer_->Write(row);
  }
}
```

Add a `leadlag_tools::LagVolGuardAuditCsvWriter* lag_vol_audit_writer_`
member and constructor parameter together with the collector pointer.

- [ ] **Step 5: Open writer in RunReplay and pass collector**

In `RunReplay()`, before `Runtime::Create`, add:

```cpp
leadlag_tools::LagVolGuardAuditCsvWriter lag_vol_audit_writer;
leadlag_tools::LagVolGuardAuditCsvWriter* lag_vol_audit_writer_ptr = nullptr;
std::unique_ptr<leadlag_tools::LagVolGuardAuditCollector>
    lag_vol_audit_collector;
if (!options.lag_vol_guard_audit_output_path.empty()) {
  leadlag_tools::LagVolGuardAuditConfig audit_config;
  if (!BuildLagVolGuardAuditConfig(options, &audit_config)) {
    return 1;
  }
  std::string error;
  if (!lag_vol_audit_writer.Open(options.lag_vol_guard_audit_output_path,
                                 &error)) {
    fmt::print(stderr, "[FAIL] lag_vol_guard_audit_error={}\n", error);
    return 1;
  }
  lag_vol_audit_writer_ptr = &lag_vol_audit_writer;
  lag_vol_audit_collector =
      std::make_unique<leadlag_tools::LagVolGuardAuditCollector>(
          leadlag_tools::BuildLagVolGuardAuditPairs(lead_lag_config),
          audit_config);
}
```

Pass both pointers into `Runtime::Create`. After `Run()`, call:

```cpp
lag_vol_audit_writer.Close();
```

Extend summary print with:

```cpp
" lag_vol_guard_audit_output={}"
```

and value:

```cpp
options.lag_vol_guard_audit_output_path.empty()
    ? "-"
    : options.lag_vol_guard_audit_output_path.string()
```

- [ ] **Step 6: Run replay build and focused tests**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R 'lead_lag_lag_vol_guard_audit_test|signal_csv_writer_test|lead_lag_strategy_interface_test' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 7: Commit Task 3**

```bash
git add tools/CMakeLists.txt tools/lead_lag/replay.cpp
git commit -m "feat: add LeadLag replay lag vol guard audit output"
```

## Task 4: Python Guard Audit Summary

**Files:**
- Create: `scripts/lead_lag/summarize_guard_audit.py`
- Create: `scripts/test/lead_lag/summarize_guard_audit_test.py`

- [ ] **Step 1: Add failing Python tests**

Create `scripts/test/lead_lag/summarize_guard_audit_test.py`:

```python
#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import summarize_guard_audit as audit


def write_file(path: Path, text: str) -> None:
    path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")


class SummarizeGuardAuditTest(unittest.TestCase):
    def test_matches_audit_rows_to_entry_orders_and_groups_outcomes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            position_path = base / "positions.csv"
            json_path = base / "summary.json"
            md_path = base / "summary.md"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                1,PROVE_USDT,4,kOpenShort,kSell,200,200,190,5002,6002,11,false,none,0,0,false,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status,cumulative_filled_quantity,position_id
                10,PROVE_USDT,4,entry,kOpenLong,6001,kCancelled,0,1
                11,PROVE_USDT,4,entry,kOpenShort,6002,kFilled,10,2
                """,
            )
            write_file(
                position_path,
                """
                symbol,symbol_id,position_id,status,gross_pnl,net_pnl
                PROVE_USDT,4,1,closed,-1.2,-1.3
                PROVE_USDT,4,2,closed,0.5,0.4
                """,
            )

            summary = audit.summarize_guard_audit(
                guard_path, order_path, position_path
            )
            audit.write_summary_json(summary, json_path)
            audit.write_summary_markdown(summary, md_path)

            saved = json.loads(json_path.read_text(encoding="utf-8"))
        self.assertEqual(saved["totals"]["open_signal_count"], 2)
        self.assertEqual(saved["totals"]["would_block_count"], 1)
        self.assertEqual(saved["groups"]["blocked"]["order_count"], 1)
        self.assertEqual(saved["groups"]["blocked"]["zero_fill_cancelled"], 1)
        self.assertEqual(saved["groups"]["allowed"]["filled"], 1)
        self.assertEqual(saved["groups"]["blocked"]["net_pnl"], "-1.3")
        self.assertEqual(saved["groups"]["allowed"]["net_pnl"], "0.4")

    def test_missing_position_file_still_summarizes_orders(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status,cumulative_filled_quantity,position_id
                10,PROVE_USDT,4,entry,kOpenLong,6001,kCancelled,0,1
                """,
            )

            summary = audit.summarize_guard_audit(guard_path, order_path, None)

        self.assertEqual(summary["totals"]["open_signal_count"], 1)
        self.assertEqual(summary["groups"]["blocked"]["order_count"], 1)
        self.assertEqual(summary["warnings"], [])

    def test_counts_unmatched_audit_rows(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status,cumulative_filled_quantity,position_id
                11,PROVE_USDT,4,entry,kOpenShort,6002,kFilled,10,2
                """,
            )

            summary = audit.summarize_guard_audit(guard_path, order_path, None)

        self.assertEqual(summary["totals"]["unmatched_audit_rows"], 1)
        self.assertEqual(summary["totals"]["unmatched_order_rows"], 1)


if __name__ == "__main__":
    unittest.main()
```

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/summarize_guard_audit_test.py
```

Expected: fails with `ModuleNotFoundError: No module named 'summarize_guard_audit'`.

- [ ] **Step 2: Implement summary script**

Create `scripts/lead_lag/summarize_guard_audit.py`:

```python
#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import json
from collections import defaultdict, deque
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as input_file:
        reader = csv.DictReader(input_file)
        if reader.fieldnames is None:
            raise ValueError(f"missing CSV header: {path}")
        return [dict(row) for row in reader]


def decimal_value(text: str) -> Decimal:
    if text == "":
        return Decimal("0")
    try:
        return Decimal(text)
    except InvalidOperation:
        return Decimal("0")


def audit_key(row: dict[str, str]) -> tuple[str, str, str]:
    return (row.get("symbol_id", ""), row.get("signal_lag_id", ""), row.get("action", ""))


def order_key(row: dict[str, str]) -> tuple[str, str, str]:
    return (row.get("symbol_id", ""), row.get("signal_lag_id", ""), row.get("action", ""))


def empty_group() -> dict[str, Any]:
    return {
        "audit_count": 0,
        "order_count": 0,
        "filled": 0,
        "partially_filled": 0,
        "cancelled": 0,
        "zero_fill_cancelled": 0,
        "position_count": 0,
        "gross_pnl": "0",
        "net_pnl": "0",
    }


def classify_group(row: dict[str, str]) -> str:
    return "blocked" if row.get("would_block", "").lower() == "true" else "allowed"


def summarize_guard_audit(
    guard_audit_path: Path,
    order_detail_path: Path,
    position_path: Path | None,
) -> dict[str, Any]:
    audit_rows = read_csv_rows(guard_audit_path)
    order_rows = [row for row in read_csv_rows(order_detail_path) if row.get("order_role") == "entry"]
    position_rows = read_csv_rows(position_path) if position_path is not None else []

    orders_by_key: dict[tuple[str, str, str], deque[dict[str, str]]] = defaultdict(deque)
    for row in order_rows:
        orders_by_key[order_key(row)].append(row)

    positions_by_key = {
        (row.get("symbol_id", ""), row.get("position_id", "")): row
        for row in position_rows
    }

    groups = {"blocked": empty_group(), "allowed": empty_group()}
    unmatched_audit_rows = 0
    matched_order_ids: set[str] = set()
    warnings: list[str] = []

    for audit_row in audit_rows:
        group_name = classify_group(audit_row)
        group = groups[group_name]
        group["audit_count"] += 1
        order_queue = orders_by_key.get(audit_key(audit_row))
        if not order_queue:
            unmatched_audit_rows += 1
            continue
        order = order_queue.popleft()
        matched_order_ids.add(order.get("local_order_id", ""))
        group["order_count"] += 1
        status = order.get("status", "")
        filled_quantity = decimal_value(order.get("cumulative_filled_quantity", ""))
        if status == "kFilled":
            group["filled"] += 1
        elif status == "kPartiallyCancelled" or (
            status == "kCancelled" and filled_quantity > 0
        ):
            group["partially_filled"] += 1
        elif status == "kCancelled":
            group["cancelled"] += 1
            if filled_quantity == 0:
                group["zero_fill_cancelled"] += 1

        position = positions_by_key.get((order.get("symbol_id", ""), order.get("position_id", "")))
        if position is not None:
            group["position_count"] += 1
            group["gross_pnl"] = str(decimal_value(group["gross_pnl"]) + decimal_value(position.get("gross_pnl", "")))
            group["net_pnl"] = str(decimal_value(group["net_pnl"]) + decimal_value(position.get("net_pnl", "")))

    unmatched_order_rows = sum(
        1
        for row in order_rows
        if row.get("local_order_id", "") not in matched_order_ids
    )

    open_signal_count = len(audit_rows)
    would_block_count = groups["blocked"]["audit_count"]
    block_rate = (
        str(Decimal(would_block_count) / Decimal(open_signal_count))
        if open_signal_count
        else "0"
    )
    return {
        "totals": {
            "open_signal_count": open_signal_count,
            "would_block_count": would_block_count,
            "block_rate": block_rate,
            "unmatched_audit_rows": unmatched_audit_rows,
            "unmatched_order_rows": unmatched_order_rows,
        },
        "groups": groups,
        "warnings": warnings,
    }


def write_summary_json(summary: dict[str, Any], path: Path) -> None:
    path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def write_summary_markdown(summary: dict[str, Any], path: Path) -> None:
    lines = [
        "# LeadLag Guard Audit Summary",
        "",
        f"- open_signal_count: {summary['totals']['open_signal_count']}",
        f"- would_block_count: {summary['totals']['would_block_count']}",
        f"- block_rate: {summary['totals']['block_rate']}",
        f"- unmatched_audit_rows: {summary['totals']['unmatched_audit_rows']}",
        f"- unmatched_order_rows: {summary['totals']['unmatched_order_rows']}",
        "",
        "| group | audit_count | order_count | filled | partially_filled | cancelled | zero_fill_cancelled | gross_pnl | net_pnl |",
        "| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for name in ("blocked", "allowed"):
        group = summary["groups"][name]
        lines.append(
            f"| {name} | {group['audit_count']} | {group['order_count']} | "
            f"{group['filled']} | {group['partially_filled']} | "
            f"{group['cancelled']} | {group['zero_fill_cancelled']} | "
            f"{group['gross_pnl']} | {group['net_pnl']} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize LeadLag guard audit CSV.")
    parser.add_argument("--guard-audit", required=True, type=Path)
    parser.add_argument("--order-detail", required=True, type=Path)
    parser.add_argument("--position", type=Path)
    parser.add_argument("--summary-json", required=True, type=Path)
    parser.add_argument("--summary-md", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    summary = summarize_guard_audit(args.guard_audit, args.order_detail, args.position)
    write_summary_json(summary, args.summary_json)
    write_summary_markdown(summary, args.summary_md)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 3: Run Python tests**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/summarize_guard_audit_test.py
```

Expected: all three tests pass.

- [ ] **Step 4: Commit Task 4**

```bash
git add scripts/lead_lag/summarize_guard_audit.py scripts/test/lead_lag/summarize_guard_audit_test.py
git commit -m "feat: summarize LeadLag guard audit output"
```

## Task 5: Documentation

**Files:**
- Modify: `strategy/lead_lag/README.md`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: Update LeadLag README**

In `strategy/lead_lag/README.md`, after the `lead_lag_replay` examples, add:

````markdown
Go-like `lag_vol_guard` 评估只通过 replay audit 完成，不进入 live 策略热路径：

```bash
./build/debug/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --signals-output /home/liuxiang/tmp/lead_lag_signals.csv \
  --lag-vol-guard-audit-output /home/liuxiang/tmp/lag_vol_guard_audit.csv
```

默认参数对齐 Go reference：`jump_threshold=0.005`、`jump_count=3`、
`jump_window=5m`、`amplitude_threshold=0.025`、`amplitude_window=1s`、
`cooldown=15m`。audit 只在 replay 里维护独立 guard state；它不改变
`lead_lag_replay` 的 signal、synthetic position accounting 或 live 策略行为。
后续可用 `scripts/lead_lag/summarize_guard_audit.py` 把
`lag_vol_guard_audit.csv` 与正式 report 的 `order_detail.csv` / `position.csv`
对齐，评估 would-block 组和 allowed 组的成交、cancel 与 PnL 差异。
````

- [ ] **Step 2: Update diagnostic fields**

In `docs/diagnostic_fields.md`, add a row near existing LeadLag diagnostic fields:

```markdown
| `lag_vol_guard_audit.csv` fields | `lead_lag_replay --lag-vol-guard-audit-output` | experiment | mixed | replay-only Go-like `lag_vol_guard` audit。每行对应一个 replay open signal，包含 `open_signal_index`、`symbol`、`symbol_id`、`action`、`side`、`signal_lead_id`、`signal_lag_id`、`would_block`、`would_block_reason`、`lag_vol_jump_count`、`lag_vol_amplitude`、`lag_vol_hot`、`lag_vol_cooldown_active`、`lag_vol_cooldown_until_ns` 和 guard 参数。该 CSV 不来自 live hot path，不改变策略下单行为。 | 若 `lag_vol_guard` 后续进入正式 live shadow / enforce，需要重新登记 live log 或 report 字段，并说明与 `drift_limit`、freshness guard 的执行顺序。 |
```

- [ ] **Step 3: Run documentation checks**

Run:

```bash
git diff --check
```

Expected: no output and exit code 0.

- [ ] **Step 4: Commit Task 5**

```bash
git add strategy/lead_lag/README.md docs/diagnostic_fields.md
git commit -m "docs: document LeadLag lag vol guard audit"
```

## Task 6: Final Verification

**Files:**
- Read: all files changed by Tasks 1-5

- [ ] **Step 1: Run focused C++ verification**

```bash
./build.sh debug
ctest --test-dir build/debug -R 'lead_lag_lag_vol_guard_audit_test|signal_csv_writer_test|lead_lag_strategy_interface_test' --output-on-failure
```

Expected: build succeeds and selected CTest tests pass.

- [ ] **Step 2: Run Python verification**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/summarize_guard_audit_test.py
```

Expected: Python unittest reports `OK`.

- [ ] **Step 3: Run repository hygiene checks**

```bash
git diff --check
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: `git diff --check` has no output. Both `rg` commands have no output because this plan does not add production dependencies on `evaluation/`.

- [ ] **Step 4: Inspect commits and status**

```bash
git status --short --branch
git log --oneline -8
```

Expected: branch is ahead by the task commits, and no unrelated dirty files remain.

- [ ] **Step 5: If final verification required fixes, commit them**

Use this exact commit message for final fixups that only repair implementation/test/doc issues introduced by this plan:

```bash
git add tools/lead_lag/lag_vol_guard_audit.h tools/lead_lag/lag_vol_guard_audit.cpp tools/lead_lag/replay.cpp tools/CMakeLists.txt test/tools/lead_lag/lag_vol_guard_audit_test.cpp test/tools/lead_lag/CMakeLists.txt scripts/lead_lag/summarize_guard_audit.py scripts/test/lead_lag/summarize_guard_audit_test.py strategy/lead_lag/README.md docs/diagnostic_fields.md
git commit -m "fix: stabilize LeadLag guard audit verification"
```
