# LeadLag Drift Guard Go Parity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 用 Go current 的 `drift_guard` 取代 C++ 现有 pre-signal `drift_limit` gate，使 drift 风控统一为 post-signal open guard。

**Architecture:** `drift_period` / `drift_min_samples` / `drift_warmup` 继续负责 alignment readiness 和 drifted lead price；`drift_limit` 从配置和信号热路径删除。新增共享 `DriftGuardState` 维护 `lag_mid / lead_mid` 的 instant、std 和 mean 窗口，在 open signal 形成后、下单或 synthetic accounting 前进行 open-only block。

**Tech Stack:** C++20、TOML parser、现有 `MeanStdWindow` / `MeanWindow`、GoogleTest、CMake / CTest、LeadLag replay。

---

## Scope

本计划只处理 `drift_guard` 与 Go current 的配置和 live/replay 信号路径统一。

不处理：

- `lag_vol_guard` live 化。
- freshness auto / taker buffer auto 热路径迁移。
- `normal_close_retry` 进一步调整。
- 同进程 shadow / A/B test。

## Go Current Fact Source

`reference/leadlag-current-strategy-package.zip` 中 Go current 的 `DriftGuardConfig` 字段为：

```go
type DriftGuardConfig struct {
    Enabled         *bool             `json:"enabled"`
    DriftInstant    float64           `json:"drift_instant" default:"0.015"`
    RatioStd        float64           `json:"ratio_std" default:"0.008"`
    RatioStdWindow  jsontype.Duration `json:"ratio_std_window" default:"1m"`
    DriftMean       float64           `json:"drift_mean" default:"0.02"`
    DriftMeanWindow jsontype.Duration `json:"drift_mean_window" default:"1m"`
}
```

Go current 没有独立 `drift_limit`。`drift_guard` 在 open signal 形成后作为 post-signal guard 执行；normal close 和 stoploss 不受影响。

## Recommended C++ Config Shape

C++ 字段名与 Go current 保持一致，但不采用“section 缺失时隐式启用”的上线语义。所有策略 TOML 必须显式写出 `enabled`，配置文件迁移时由旧 `drift_limit = 0.02` 转成 `drift_guard.drift_mean = 0.02`。

```toml
[lead_lag.pairs.trigger]
lead = 0.0025
close = 0.0005
lag_part = 0.5
target_profit_rate = 0.0
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"

[lead_lag.pairs.trigger.drift_guard]
enabled = true
drift_instant = 0.015
ratio_std = 0.008
ratio_std_window = "1m"
drift_mean = 0.02
drift_mean_window = "1m"
```

Parser 规则：

- `drift_limit` 不再支持，遇到即 fail fast。
- `[lead_lag.pairs.trigger.drift_guard]` 存在时必须显式包含 `enabled`。
- `enabled = false` 时仍解析字段，便于配置审计；缺失阈值使用 Go 默认值。
- `enabled = true` 时 `drift_instant`、`ratio_std`、`ratio_std_window`、`drift_mean`、`drift_mean_window` 必须为正值。
- `mode` / `shadow` / `enforce` 不再是 `drift_guard` 的合法字段。

## File Structure

- Modify `strategy/lead_lag/config.h`: `DriftGuardConfig` 改为 Go-like bool enabled + threshold/window 字段；从 `TriggerConfig` 删除 `drift_limit`。
- Modify `strategy/lead_lag/config.cpp`: 删除 required `drift_limit`，解析 `drift_guard.enabled` 和 Go 默认值，拒绝旧 `drift_limit` / `drift_guard.mode`。
- Create `strategy/lead_lag/drift_guard.h`: 生产路径共享 evaluator，封装 ratio 更新、snapshot 和 block 判断。
- Modify `strategy/lead_lag/strategy.h`: 每个 pair 增加 `DriftGuardState`，在 paired raw BBO 更新时喂数据，在 open signal 形成后执行 post-signal guard。
- Modify `strategy/lead_lag/signal.h`: 删除 pre-signal `alignment.drift_deviation > drift_limit` 拦截；新增或保留 `SignalRejectReason::kDriftGuard` 用于 blocked open。
- Modify `tools/lead_lag/drift_guard_audit.*` if present on implementation branch: 复用生产 `DriftGuardState`，避免 live 与 replay 口径分叉。
- Modify `config/strategies/*.toml` and `test/tools/lead_lag/*.toml`: 删除 `drift_limit`，增加 `[trigger.drift_guard]`。
- Modify `test/strategy/lead_lag_config_test.cpp`: 更新 minimal config helper 和 parser tests。
- Modify `test/strategy/lead_lag_signal_test.cpp`: 覆盖 `SignalEngine` 不再 pre-signal drift block。
- Modify `test/strategy/lead_lag_strategy_interface_test.cpp`: 覆盖 post-signal `drift_guard` block open、close/stoploss 不受影响。
- Modify `docs/diagnostic_fields.md`: 登记新增 live/replay log 字段和 reject reason。
- Modify `strategy/lead_lag/README.md` and `docs/project_onboarding_guide.md`: 更新 Go parity、配置迁移和上线边界。

## Task 1: Create Worktree

**Files:**
- No repository file changes.

- [ ] **Step 1: Verify main state**

Run:

```bash
git status --short --branch
git log --oneline -8
```

Expected: current `main` is clean or only contains unrelated user changes that are not touched.

- [ ] **Step 2: Create implementation worktree**

Run:

```bash
git worktree add .worktrees/lead-lag-drift-guard-go-parity -b feature/lead-lag-drift-guard-go-parity main
cd .worktrees/lead-lag-drift-guard-go-parity
```

Expected: a clean worktree on `feature/lead-lag-drift-guard-go-parity`.

## Task 2: Config Parser TDD

**Files:**
- Modify: `test/strategy/lead_lag_config_test.cpp`
- Modify later: `strategy/lead_lag/config.h`
- Modify later: `strategy/lead_lag/config.cpp`

- [ ] **Step 1: Update minimal TOML fixture to remove `drift_limit` and allow trigger extras**

Change the helper signature to:

```cpp
std::string MinimalConfigTomlWithRisk(
    std::string_view risk_section, std::string_view execute_extra = {},
    std::string_view freshness_lines = R"toml(max_lead_freshness_ms = 5
max_lag_freshness_ms = 20
)toml",
    std::string_view trigger_extra = {}) {
```

In the helper body, remove:

```toml
drift_limit = 0.02
```

Keep:

```toml
drift_period = "1m"
drift_min_samples = 20
drift_warmup = "30s"
```

Append `trigger_extra` immediately after the `drift_warmup` line:

```cpp
drift_warmup = "30s"
)toml"} + std::string{trigger_extra} +
         std::string{R"toml(

[lead_lag.pairs.trigger.quantile]
```

- [ ] **Step 2: Add parser test for Go-like disabled config**

Add a test near `ReferenceMigrationDefaultsStayDisabled`:

```cpp
TEST(LeadLagConfigTest, ParsesGoLikeDriftGuardConfig) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
enabled = false
drift_instant = 0.015
ratio_std = 0.008
ratio_std_window = "1m"
drift_mean = 0.02
drift_mean_window = "1m"
)toml",
                                      catalog);

  ASSERT_TRUE(result.ok) << result.error;
  const leadlag::DriftGuardConfig& guard =
      result.value.pairs[0].trigger.drift_guard;
  EXPECT_FALSE(guard.enabled);
  EXPECT_DOUBLE_EQ(guard.drift_instant, 0.015);
  EXPECT_DOUBLE_EQ(guard.ratio_std, 0.008);
  EXPECT_EQ(guard.ratio_std_window_ns, 60'000'000'000ULL);
  EXPECT_DOUBLE_EQ(guard.drift_mean, 0.02);
  EXPECT_EQ(guard.drift_mean_window_ns, 60'000'000'000ULL);
}
```

- [ ] **Step 3: Add parser test rejecting old `drift_limit`**

```cpp
TEST(LeadLagConfigTest, RejectsDeprecatedDriftLimit) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(
      MinimalConfigTomlWithRisk("", "", R"toml(max_lead_freshness_ms = 5
max_lag_freshness_ms = 20
)toml",
                                "drift_limit = 0.02\n"),
      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_limit"), std::string::npos);
  EXPECT_NE(result.error.find("drift_guard.drift_mean"), std::string::npos);
}
```

- [ ] **Step 4: Add parser test rejecting `drift_guard.mode`**

Replace the existing `RejectsUnimplementedDriftGuardEnforceMode` / `ShadowMode` tests with:

```cpp
TEST(LeadLagConfigTest, RejectsDeprecatedDriftGuardMode) {
  const aquila::config::InstrumentCatalog catalog = LoadCatalog();

  const auto result = ParseConfigToml(MinimalConfigTomlWithRisk("") + R"toml(

[lead_lag.pairs.trigger.drift_guard]
mode = "enforce"
enabled = true
)toml",
                                      catalog);

  ASSERT_FALSE(result.ok);
  EXPECT_NE(result.error.find("drift_guard.mode"), std::string::npos);
}
```

- [ ] **Step 5: Run failing tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_config_test
ctest --test-dir build/debug -R lead_lag_config_test --output-on-failure
```

Expected: fails because implementation still requires `drift_limit` and old `DriftGuardConfig` uses `FeatureMode`.

## Task 3: Implement Go-Like Config

**Files:**
- Modify: `strategy/lead_lag/config.h`
- Modify: `strategy/lead_lag/config.cpp`
- Modify: `test/strategy/lead_lag_config_test.cpp`

- [ ] **Step 1: Update `DriftGuardConfig`**

Replace the old struct:

```cpp
struct DriftGuardConfig {
  FeatureMode mode{FeatureMode::kOff};
  double drift_instant{0.0};
  double ratio_std{0.0};
  std::uint64_t ratio_std_window_ns{0};
  double drift_mean{0.0};
  std::uint64_t drift_mean_window_ns{0};
};
```

with:

```cpp
struct DriftGuardConfig {
  bool enabled{false};
  double drift_instant{0.015};
  double ratio_std{0.008};
  std::uint64_t ratio_std_window_ns{60'000'000'000ULL};
  double drift_mean{0.02};
  std::uint64_t drift_mean_window_ns{60'000'000'000ULL};
};
```

Remove `double drift_limit{0.0};` from `TriggerConfig`.

- [ ] **Step 2: Reject old `drift_limit` before required-field parsing**

In `ParseTrigger`, add:

```cpp
if (table.contains("drift_limit")) {
  Fail(prefix + ".drift_limit",
       " is no longer supported; use drift_guard.drift_mean");
  return trigger;
}
```

Remove the old `RequiredDouble(table, "drift_limit", ...)` assignment.

- [ ] **Step 3: Parse Go-like `drift_guard`**

Implement `ParseDriftGuard` with:

First add these parser helpers near `UInt32Or` / `BoolOr`:

```cpp
[[nodiscard]] double DoubleOr(const toml::table& table, std::string_view key,
                              double fallback, std::string_view name) {
  if (!table.contains(key)) {
    return fallback;
  }
  const std::optional<double> double_value = table[key].value<double>();
  if (double_value) {
    return *double_value;
  }
  const std::optional<std::int64_t> int_value =
      table[key].value<std::int64_t>();
  if (int_value) {
    return static_cast<double>(*int_value);
  }
  Fail(name, " must be a number");
  return fallback;
}

[[nodiscard]] bool RequiredBool(const toml::table& table,
                                std::string_view key,
                                std::string_view name) {
  const std::optional<bool> value = table[key].value<bool>();
  if (!value) {
    Fail(name, " is required and must be a boolean");
    return false;
  }
  return *value;
}

[[nodiscard]] std::uint64_t DurationNsOr(const toml::table& table,
                                         std::string_view key,
                                         std::uint64_t fallback,
                                         std::string_view name) {
  if (!table.contains(key)) {
    return fallback;
  }
  return RequiredDurationNs(table, key, name);
}
```

Then implement `ParseDriftGuard` with:

```cpp
if (table.contains("mode")) {
  Fail(prefix + ".mode", " is no longer supported; use enabled");
  return guard;
}
guard.enabled = RequiredBool(table, "enabled", prefix + ".enabled");
guard.drift_instant =
    DoubleOr(table, "drift_instant", guard.drift_instant,
             prefix + ".drift_instant");
guard.ratio_std =
    DoubleOr(table, "ratio_std", guard.ratio_std, prefix + ".ratio_std");
guard.ratio_std_window_ns =
    DurationNsOr(table, "ratio_std_window", guard.ratio_std_window_ns,
                 prefix + ".ratio_std_window");
guard.drift_mean =
    DoubleOr(table, "drift_mean", guard.drift_mean, prefix + ".drift_mean");
guard.drift_mean_window_ns =
    DurationNsOr(table, "drift_mean_window", guard.drift_mean_window_ns,
                 prefix + ".drift_mean_window");
```

- [ ] **Step 4: Validate positive values when enabled**

After parsing:

```cpp
if (guard.enabled) {
  if (guard.drift_instant <= 0.0) Fail(prefix + ".drift_instant", " must be positive");
  if (guard.ratio_std <= 0.0) Fail(prefix + ".ratio_std", " must be positive");
  if (guard.ratio_std_window_ns == 0) Fail(prefix + ".ratio_std_window", " must be positive");
  if (guard.drift_mean <= 0.0) Fail(prefix + ".drift_mean", " must be positive");
  if (guard.drift_mean_window_ns == 0) Fail(prefix + ".drift_mean_window", " must be positive");
}
```

- [ ] **Step 5: Run config tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_config_test
ctest --test-dir build/debug -R lead_lag_config_test --output-on-failure
```

Expected: all config tests pass.

- [ ] **Step 6: Commit config parser**

```bash
git add strategy/lead_lag/config.h strategy/lead_lag/config.cpp test/strategy/lead_lag_config_test.cpp
git commit -m "feat: parse Go-like LeadLag drift guard config"
```

## Task 4: Add Production Drift Guard State

**Files:**
- Create: `strategy/lead_lag/drift_guard.h`
- Modify: `strategy/CMakeLists.txt` if the library target explicitly lists headers.
- Test: `test/strategy/lead_lag_drift_guard_test.cpp`

- [ ] **Step 1: Write drift guard unit tests**

Create `test/strategy/lead_lag_drift_guard_test.cpp` with tests covering:

```cpp
TEST(LeadLagDriftGuardTest, DisabledGuardNeverBlocks);
TEST(LeadLagDriftGuardTest, InvalidQuotesAreNotReadyAndDoNotBlock);
TEST(LeadLagDriftGuardTest, InstantRatioBlocksWhenDeviationExceedsThreshold);
TEST(LeadLagDriftGuardTest, RatioStdBlocksWhenWindowStdExceedsThreshold);
TEST(LeadLagDriftGuardTest, DriftMeanBlocksWhenWindowMeanDeviationExceedsThreshold);
TEST(LeadLagDriftGuardTest, NonMonotonicTimestampDoesNotEvictAllSamples);
```

Use `QuoteSnapshot` values where `lead_mid=100.0` and `lag_mid=102.0` to hit `drift_instant = 0.015`.

- [ ] **Step 2: Run failing drift guard tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_drift_guard_test
ctest --test-dir build/debug -R lead_lag_drift_guard_test --output-on-failure
```

Expected: target or header is missing.

- [ ] **Step 3: Implement `DriftGuardState`**

Create `strategy/lead_lag/drift_guard.h` with:

```cpp
struct DriftGuardSnapshot {
  bool enabled{false};
  bool ready{false};
  double instant_ratio{0.0};
  bool instant_hit{false};
  double ratio_std{0.0};
  bool ratio_std_hit{false};
  double drift_mean{0.0};
  bool drift_mean_hit{false};
  bool blocked{false};
};

class DriftGuardState {
 public:
  void Init(const DriftGuardConfig& config, std::size_t initial_capacity);
  void Reset() noexcept;
  void OnPairedRawBbo(std::int64_t event_ns, const QuoteSnapshot& lead,
                      const QuoteSnapshot& lag);
  [[nodiscard]] DriftGuardSnapshot Evaluate(const DriftGuardConfig& config,
                                            const QuoteSnapshot& lead,
                                            const QuoteSnapshot& lag) const noexcept;
};
```

Use `MeanStdWindow ratio_std_` and `MeanWindow ratio_mean_`. Ratio is:

```cpp
const double lead_mid = (lead.bid_price + lead.ask_price) * 0.5;
const double lag_mid = (lag.bid_price + lag.ask_price) * 0.5;
const double ratio = lag_mid / lead_mid;
```

Block if any condition is true:

```cpp
std::abs(instant_ratio - 1.0) > config.drift_instant
ratio_std_.stddev() > config.ratio_std
std::abs(ratio_mean_.mean() - 1.0) > config.drift_mean
```

- [ ] **Step 4: Wire test target**

Add `lead_lag_drift_guard_test` to `test/strategy/CMakeLists.txt` in the same section as the other LeadLag strategy tests.

- [ ] **Step 5: Run drift guard tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_drift_guard_test
ctest --test-dir build/debug -R lead_lag_drift_guard_test --output-on-failure
```

Expected: all drift guard unit tests pass.

- [ ] **Step 6: Commit drift guard state**

```bash
git add strategy/lead_lag/drift_guard.h test/strategy/lead_lag_drift_guard_test.cpp strategy/CMakeLists.txt test/strategy/CMakeLists.txt
git commit -m "feat: add LeadLag drift guard state"
```

If `strategy/CMakeLists.txt` did not change because headers are not listed, omit it from `git add`.

## Task 5: Move Drift Block To Post-Signal Guard

**Files:**
- Modify: `strategy/lead_lag/signal.h`
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `test/strategy/lead_lag_signal_test.cpp`
- Modify: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [ ] **Step 1: Add signal test that high alignment drift no longer blocks before open signal**

Update or add a `LeadLagSignalTest` case that calls `SignalEngine::OnLeadTick` with `alignment.drift_ready = true` and `alignment.drift_deviation = 0.5`, while open metrics satisfy long entry. Expected decision:

```cpp
EXPECT_TRUE(decision.triggered);
EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kNone);
```

- [ ] **Step 2: Remove pre-signal drift limit branch**

In `SignalEngine::OnLeadTick`, delete:

```cpp
if (alignment.drift_ready &&
    alignment.drift_deviation > pair.trigger.drift_limit) {
  return Reject(SignalRejectReason::kDriftLimit);
}
```

Add:

```cpp
kDriftGuard,
```

to `SignalRejectReason`. Remove `kDriftLimit` only if no diagnostics or tests still depend on it; otherwise leave it unused for compatibility.

- [ ] **Step 3: Feed `DriftGuardState` from paired raw BBO**

In `PairRuntimeState`, add:

```cpp
DriftGuardState drift_guard;
```

Initialize it beside `AlignmentState`:

```cpp
runtime.drift_guard.Init(pair.trigger.drift_guard,
                         pair.capacity.noise_window_capacity);
```

When both raw lead and lag BBO are valid and alignment receives paired drift, also call:

```cpp
runtime->drift_guard.OnPairedRawBbo(event_ns, raw_lead_quote, raw_lag_quote);
```

- [ ] **Step 4: Block only open actions after signal is recorded**

In `FinalizeActiveSignal`, after `RecordTriggeredSignal(...)` and before `SyntheticPositionAccounting()` / `SubmitExternalSignal(...)`, add:

```cpp
if (RejectOpenForDriftGuard(runtime, market)) {
  return;
}
```

Implement:

```cpp
[[nodiscard]] bool RejectOpenForDriftGuard(PairRuntimeState* runtime,
                                           const PairMarketState& market) noexcept {
  if (!AppliesOpenFreshnessGuard(last_signal_decision_)) {
    return false;
  }
  const DriftGuardSnapshot guard = runtime->drift_guard.Evaluate(
      runtime->pair.trigger.drift_guard,
      market.lead.latest_quote,
      market.lag.latest_quote);
  if (!guard.blocked) {
    return false;
  }
  LogOrderIntentRejectedForSignal("drift_guard", runtime, runtime->pair.symbol,
                                  0.0, last_signal_decision_.intent.price,
                                  last_signal_decision_.intent.price, 0,
                                  runtime->pair.lag_instrument.price_tick, 0.0);
  RejectSignal(SignalRejectReason::kDriftGuard);
  return true;
}
```

Adjust arguments to existing `LogOrderIntentRejectedForSignal` conventions; do not log dynamic strings in the hot path.

- [ ] **Step 5: Add strategy interface tests**

Add tests:

```cpp
TEST(LeadLagStrategyInterfaceTest, DriftGuardBlocksOpenAfterSignalTriggered);
TEST(LeadLagStrategyInterfaceTest, DriftGuardDoesNotBlockNormalClose);
TEST(LeadLagStrategyInterfaceTest, DriftGuardDoesNotBlockStoploss);
TEST(LeadLagStrategyInterfaceTest, DisabledDriftGuardKeepsOpenPathUnchanged);
```

For the block test, configure:

```toml
[lead_lag.pairs.trigger.drift_guard]
enabled = true
drift_instant = 0.001
ratio_std = 10.0
ratio_std_window = "1m"
drift_mean = 10.0
drift_mean_window = "1m"
```

Use lead mid `100.0` and lag mid `101.0` so `instant_drift` blocks.

- [ ] **Step 6: Run signal and strategy tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_signal_test lead_lag_strategy_interface_test
ctest --test-dir build/debug -R 'lead_lag_(signal|strategy_interface)_test' --output-on-failure
```

Expected: tests pass, and drift guard only affects open actions.

- [ ] **Step 7: Commit strategy path**

```bash
git add strategy/lead_lag/signal.h strategy/lead_lag/strategy.h test/strategy/lead_lag_signal_test.cpp test/strategy/lead_lag_strategy_interface_test.cpp
git commit -m "feat: enforce LeadLag drift guard after open signals"
```

## Task 6: Reuse Drift Guard In Replay Audit

**Files:**
- Modify: `tools/lead_lag/drift_guard_audit.h`
- Modify: `tools/lead_lag/drift_guard_audit.cpp`
- Modify: `test/tools/lead_lag/lead_lag_drift_guard_audit_test.cpp`

- [ ] **Step 1: Replace duplicate evaluator with `DriftGuardState`**

Use `strategy/lead_lag/drift_guard.h` in the audit tool. The audit output must preserve existing CSV field names:

```text
drift_instant,ratio_std,drift_mean,drift_guard_would_block,drift_guard_block_reason
```

- [ ] **Step 2: Keep audit behavior signal-only**

Confirm `lead_lag_replay --drift-guard-audit-output` still writes audit rows without changing replay synthetic accounting.

- [ ] **Step 3: Run replay audit tests**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_replay lead_lag_drift_guard_audit_test
ctest --test-dir build/debug -R 'lead_lag_(replay.*drift|drift_guard_audit)' --output-on-failure
```

Expected: audit tests pass and no live/replay evaluator divergence remains.

- [ ] **Step 4: Commit replay reuse**

```bash
git add tools/lead_lag/drift_guard_audit.h tools/lead_lag/drift_guard_audit.cpp test/tools/lead_lag/lead_lag_drift_guard_audit_test.cpp
git commit -m "refactor: share LeadLag drift guard evaluator"
```

## Task 7: Migrate Strategy Configs

**Files:**
- Modify: `config/strategies/*.toml`
- Modify: `test/tools/lead_lag/*.toml`
- Modify: report context configs only if they are active test fixtures.

- [ ] **Step 1: Replace each `drift_limit`**

For every pair:

```toml
drift_limit = 0.02
```

replace with:

```toml
[lead_lag.pairs.trigger.drift_guard]
enabled = true
drift_instant = 0.015
ratio_std = 0.008
ratio_std_window = "1m"
drift_mean = 0.02
drift_mean_window = "1m"
```

Use the old `drift_limit` value as the new `drift_mean` value if it differs from `0.02`.

- [ ] **Step 2: Preserve TOML table ordering**

Place `[lead_lag.pairs.trigger.drift_guard]` after `[lead_lag.pairs.trigger.quantile]` or immediately after `[lead_lag.pairs.trigger]`, matching the file’s existing style. Do not move unrelated pair sections.

- [ ] **Step 3: Verify no old config remains**

Run:

```bash
rg -n 'drift_limit|drift_guard\\.mode' config test docs strategy tools
```

Expected: no `drift_limit` in active configs or parser docs except migration notes saying it is deprecated.

- [ ] **Step 4: Commit config migration**

```bash
git add config/strategies test/tools/lead_lag
git commit -m "chore: migrate LeadLag configs to drift guard"
```

## Task 8: Documentation And Diagnostics

**Files:**
- Modify: `strategy/lead_lag/README.md`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/project_onboarding_guide.md`
- Modify: `docs/lead_lag_live_operations_pipeline.md` if startup checks reference `drift_limit`.

- [ ] **Step 1: Update strategy README**

Document:

- Go current has no independent `drift_limit`.
- C++ uses `[trigger.drift_guard]` with Go field names.
- `drift_period` / `drift_min_samples` / `drift_warmup` remain alignment readiness parameters.
- `drift_guard` is post-signal open-only; close and stoploss are unaffected.

- [ ] **Step 2: Update diagnostic fields**

Register live reject/log fields:

```text
drift_guard_enabled
drift_guard_ready
drift_instant_ratio
drift_instant_threshold
drift_instant_hit
ratio_std
ratio_std_threshold
ratio_std_hit
drift_mean
drift_mean_threshold
drift_mean_hit
drift_guard_outcome
```

If the first implementation only logs `order_intent_rejected reason=drift_guard`, document that the richer snapshot is not yet emitted and should be added before enabling broad live runs.

- [ ] **Step 3: Update onboarding**

In `docs/project_onboarding_guide.md`, replace “next step evaluate drift guard vs drift limit” with:

```text
LeadLag 已按 Go current 收敛到 post-signal drift_guard；旧 drift_limit 配置已废弃。drift_period / drift_min_samples / drift_warmup 只保留为 alignment readiness 参数。
```

- [ ] **Step 4: Commit docs**

```bash
git add strategy/lead_lag/README.md docs/diagnostic_fields.md docs/project_onboarding_guide.md docs/lead_lag_live_operations_pipeline.md
git commit -m "docs: document LeadLag drift guard parity"
```

Only add `docs/lead_lag_live_operations_pipeline.md` if it changed.

## Task 9: Verification

**Files:**
- No source changes expected.

- [ ] **Step 1: Build debug targets**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  lead_lag_config_test \
  lead_lag_drift_guard_test \
  lead_lag_signal_test \
  lead_lag_strategy_interface_test \
  lead_lag_replay
```

Expected: build succeeds.

- [ ] **Step 2: Run focused CTest**

```bash
ctest --test-dir build/debug -R 'lead_lag_(config|drift_guard|signal|strategy_interface|replay.*drift)' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 3: Build release replay**

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target lead_lag_replay
```

Expected: build succeeds.

- [ ] **Step 4: Re-run known fusion replay sample**

Use the existing 28-symbol fusion replay scratch data if available:

```bash
TMPDIR=/home/liuxiang/tmp ./build/release/tools/lead_lag_replay \
  --config /home/liuxiang/tmp/lead_lag_drift_guard_fusion31_20260625/lead_lag_fusion28_replay.toml \
  --data-reader-config /home/liuxiang/tmp/lead_lag_drift_guard_fusion31_20260625/lead_lag_fusion31_binary_replay.toml \
  --signals-output /home/liuxiang/tmp/lead_lag_drift_guard_go_parity_verify/signals.csv \
  --drift-guard-audit-output /home/liuxiang/tmp/lead_lag_drift_guard_go_parity_verify/drift_guard_audit.csv
```

Expected: replay completes. Compare open signal counts against the previous run and explain any difference as post-signal guard behavior.

- [ ] **Step 5: Run formatting and boundary checks**

```bash
git diff --check
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: `git diff --check` has no output; evaluation boundary checks have no matches.

- [ ] **Step 6: Final status**

```bash
git status --short --branch
git log --oneline -8
```

Expected: branch contains the drift guard parity commits and no uncommitted changes.

## Rollout Notes

- Do not enable `lag_vol_guard` as part of this change.
- Do not add `cooldown`, `min_samples`, or `warmup` to `drift_guard`; those are not Go current fields.
- Do not keep `drift_limit` and `drift_guard` both enforcing.
- If live configs enable `drift_guard`, run release smoke before longer live tests and inspect `reason=drift_guard` rejects.
- If A/B testing is needed, run separate processes or switch configs; do not add in-strategy shadow mode.
