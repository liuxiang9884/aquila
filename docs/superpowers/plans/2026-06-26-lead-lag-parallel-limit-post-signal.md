# LeadLag Parallel Limit Post-Signal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move C++ LeadLag `parallel-limit` from pre-signal rejection to a Go-aligned post-signal open guard that emits triggered signal and rejected intent diagnostics.

**Architecture:** Keep close-first behavior and keep `new_entries_paused()` as a pre-signal production safety gate. Remove only the pre-open capacity rejection from `SignalEngine::OnLeadTick()`, then add a post-signal open-only `RejectOpenForParallelLimit()` in `Strategy::FinalizeActiveSignal()` before drift guard, synthetic accounting, and external order submission.

**Tech Stack:** C++20, CMake, GTest, Python unittest, existing LeadLag live log/report parsers.

---

## File Map

- Modify: `strategy/lead_lag/signal.h`
  - Remove the pre-open `active_group_count() >= capacity()` rejection.
  - Keep close-first and `new_entries_paused()` behavior unchanged.
- Modify: `strategy/lead_lag/strategy.h`
  - Add `RejectOpenForParallelLimit()`.
  - Call it after `RecordTriggeredSignal()` and before `RejectOpenForDriftGuard()`.
  - Emit `lead_lag_order_intent_rejected reason=parallel_limit`.
- Modify: `test/strategy/lead_lag_signal_test.cpp`
  - Add signal-engine regression test proving full capacity still allows an open signal to form.
- Modify: `test/strategy/lead_lag_strategy_interface_test.cpp`
  - Add external-mode regression test proving full capacity emits triggered signal, rejected intent, and no order.
- Modify: `scripts/test/lead_lag/analyze_order_detail_test.py`
  - Add parser regression test for `reason=parallel_limit`.
- Modify: `scripts/test/lead_lag/generate_live_report_test.py`
  - Add report regression test proving parallel-limit rejected intent joins signal without `missing_order`.
- Modify: `docs/diagnostic_fields.md`
  - Add `parallel_limit` to the documented rejected intent reasons.
- Modify: `docs/lead_lag_live_report_csv_schema.md`
  - Add `parallel_limit` to `reject_reason` semantics for `intent_rejected_v1`.

---

### Task 1: Signal Engine Allows Open Opportunity When Capacity Is Full

**Files:**
- Modify: `test/strategy/lead_lag_signal_test.cpp`
- Modify: `strategy/lead_lag/signal.h`

- [ ] **Step 1: Add the failing signal-engine test**

Add this test after `LeadTickAllowsOpenWhenAlignmentDriftIsHigh` in `test/strategy/lead_lag_signal_test.cpp`:

```cpp
TEST(LeadLagSignalTest, LeadTickFormsOpenSignalWhenParallelCapacityFull) {
  const leadlag::PairConfig pair = PairConfigForSignal();
  leadlag::ExecutionState execution;
  execution.Init(pair.execute.parallel);
  ASSERT_NE(execution.AddHoldGroup(/*signed_position_quantity=*/1,
                                   /*trailing_price=*/100.0),
            nullptr);
  ASSERT_EQ(execution.active_group_count(), execution.capacity());

  const leadlag::SignalDecision decision = leadlag::SignalEngine::OnLeadTick(
      pair, execution, OpenLongMarket(), ThresholdForSignal(),
      leadlag::AlignmentSnapshot{
          .drift_ready = true,
          .drift_deviation = 0.0,
      });

  ASSERT_TRUE(decision.triggered);
  EXPECT_EQ(decision.action, leadlag::SignalAction::kOpenLong);
  EXPECT_FALSE(decision.intent.reduce_only);
  EXPECT_EQ(decision.reject_reason, leadlag::SignalRejectReason::kNone);
}
```

- [ ] **Step 2: Run the focused test and verify it fails**

Run:

```bash
ctest --test-dir build/debug -R lead_lag_signal_test --output-on-failure
```

Expected before implementation: `LeadTickFormsOpenSignalWhenParallelCapacityFull` fails because the decision is not triggered and `reject_reason` is `kParallelLimit`.

- [ ] **Step 3: Remove only the pre-open capacity rejection**

In `strategy/lead_lag/signal.h`, change `SignalEngine::OnLeadTick()` from:

```cpp
    if (execution.active_group_count() >= execution.capacity()) {
      return Reject(SignalRejectReason::kParallelLimit);
    }
    if (execution.new_entries_paused()) {
      return Reject(SignalRejectReason::kDegraded);
    }
    SignalDecision open_long = TryOpenLong(pair, market, threshold);
```

to:

```cpp
    if (execution.new_entries_paused()) {
      return Reject(SignalRejectReason::kDegraded);
    }
    SignalDecision open_long = TryOpenLong(pair, market, threshold);
```

Do not change the close-first loop above this block. Do not move `new_entries_paused()`; it remains a pre-signal production safety gate.

- [ ] **Step 4: Re-run the focused signal test**

Run:

```bash
ctest --test-dir build/debug -R lead_lag_signal_test --output-on-failure
```

Expected after implementation: all `lead_lag_signal_test` cases pass.

- [ ] **Step 5: Commit Task 1**

```bash
git add strategy/lead_lag/signal.h test/strategy/lead_lag_signal_test.cpp
git commit -m "fix: form LeadLag open signal before parallel guard"
```

---

### Task 2: Add Post-Signal Parallel-Limit Guard In Strategy

**Files:**
- Modify: `test/strategy/lead_lag_strategy_interface_test.cpp`
- Modify: `strategy/lead_lag/strategy.h`

- [ ] **Step 1: Add the failing strategy-interface test**

Add this test near `DriftGuardBlocksOpenAfterSignalTriggered` in `test/strategy/lead_lag_strategy_interface_test.cpp`:

```cpp
TEST(LeadLagStrategyInterfaceTest,
     ParallelLimitBlocksOpenAfterSignalTriggered) {
  leadlag::Strategy strategy{SignalOnlyConfig()};
  FakeOrderSession order_session;
  OrderManagerT order_manager{order_session, 8, 4};
  ContextT context{order_manager};
  StrategySignalTriggeredLogCaptureGuard signal_log_capture;
  StrategyOrderIntentRejectedLogCaptureGuard rejected_log_capture;

  FeedOpenLongSignal(&strategy, &context);
  ASSERT_EQ(order_session.placed_orders.size(), 1U);
  ASSERT_EQ(order_manager.order_count(), 1U);

  strategy.OnBookTicker(Ticker(3, aquila::Exchange::kBinance, 201, 113.0,
                               114.0),
                        context);

  EXPECT_EQ(order_session.placed_orders.size(), 1U);
  EXPECT_EQ(order_manager.order_count(), 1U);
  EXPECT_FALSE(strategy.last_signal_decision().triggered);
  EXPECT_EQ(strategy.last_signal_decision().reject_reason,
            leadlag::SignalRejectReason::kParallelLimit);
  ASSERT_GE(g_signal_triggered_log_count, 2U);
  EXPECT_EQ(g_signal_triggered_logs[1].action,
            leadlag::SignalAction::kOpenLong);
  EXPECT_FALSE(g_signal_triggered_logs[1].reduce_only);
  ASSERT_EQ(g_order_intent_rejected_log_count, 1U);
  const leadlag::detail::StrategyOrderIntentRejectedLogRecordForTest& reject =
      g_order_intent_rejected_logs[0];
  EXPECT_EQ(reject.reason, "parallel_limit");
  EXPECT_EQ(reject.symbol, "BTC_USDT_GATE");
  EXPECT_EQ(reject.symbol_id, 3);
  EXPECT_EQ(reject.action, leadlag::SignalAction::kOpenLong);
  EXPECT_FALSE(reject.reduce_only);
}
```

This uses the first `FeedOpenLongSignal()` to occupy the single `parallel` slot with a pending open order. The second Binance lead tick creates another open opportunity, which must be rejected after `lead_lag_signal_triggered`.

- [ ] **Step 2: Run the focused strategy-interface test and verify it fails**

Run:

```bash
ctest --test-dir build/debug -R lead_lag_strategy_interface_test --output-on-failure
```

Expected before implementation: the new test fails because no `parallel_limit` rejected intent is emitted and the second signal is not guarded in `FinalizeActiveSignal()`.

- [ ] **Step 3: Add `RejectOpenForParallelLimit()`**

In `strategy/lead_lag/strategy.h`, add this helper before `RejectOpenForFreshness()`:

```cpp
  [[nodiscard]] bool RejectOpenForParallelLimit(
      PairRuntimeState* runtime) noexcept {
    if (!AppliesOpenFreshnessGuard(last_signal_decision_)) {
      return false;
    }
    if (runtime->execution.active_group_count() <
        runtime->execution.capacity()) {
      return false;
    }
    const InstrumentMetadata& instrument = runtime->pair.lag_instrument;
    const std::string_view symbol = LagOrderSymbol(runtime->pair, instrument);
    LogOrderIntentRejectedForSignal(
        "parallel_limit", runtime, symbol, 0.0,
        last_signal_decision_.intent.price,
        last_signal_decision_.intent.price, 0, instrument.price_tick, 0.0);
    RejectSignal(SignalRejectReason::kParallelLimit);
    return true;
  }
```

`AppliesOpenFreshnessGuard()` is already open-only and non-reduce-only, so reuse it for now to keep the guard boundary identical to freshness and drift guard. If a later cleanup renames that helper to a more general `AppliesOpenIntentGuard()`, do it in a separate commit.

- [ ] **Step 4: Call the new guard before drift guard**

In `FinalizeActiveSignal()` in `strategy/lead_lag/strategy.h`, change:

```cpp
    RecordTriggeredSignal(runtime, market, drifted_lead, recorder, alignment,
                          threshold, trigger_ticker, signal_role,
                          on_book_ticker_entry_ns);
    if (RejectOpenForDriftGuard(runtime, market)) {
      return;
    }
```

to:

```cpp
    RecordTriggeredSignal(runtime, market, drifted_lead, recorder, alignment,
                          threshold, trigger_ticker, signal_role,
                          on_book_ticker_entry_ns);
    if (RejectOpenForParallelLimit(runtime)) {
      return;
    }
    if (RejectOpenForDriftGuard(runtime, market)) {
      return;
    }
```

- [ ] **Step 5: Re-run the focused strategy-interface test**

Run:

```bash
ctest --test-dir build/debug -R lead_lag_strategy_interface_test --output-on-failure
```

Expected after implementation: all `lead_lag_strategy_interface_test` cases pass.

- [ ] **Step 6: Commit Task 2**

```bash
git add strategy/lead_lag/strategy.h test/strategy/lead_lag_strategy_interface_test.cpp
git commit -m "fix: reject LeadLag parallel limit after triggered signal"
```

---

### Task 3: Cover Parallel-Limit Rejected Intent In Python Report Parsers

**Files:**
- Modify: `scripts/test/lead_lag/analyze_order_detail_test.py`
- Modify: `scripts/test/lead_lag/generate_live_report_test.py`

- [ ] **Step 1: Add `analyze_order_detail` parser fixture**

In `scripts/test/lead_lag/analyze_order_detail_test.py`, add this test after `test_parses_drift_guard_rejected_order_intent`:

```python
    def test_parses_parallel_limit_rejected_order_intent(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            write_file(
                log_path,
                """
                I2026-06-25 09:00:00.000000100 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=parallel_limit trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = orders.analyze_order_detail(log_path)

        self.assertEqual(len(result.rows), 1)
        row = result.rows[0]
        self.assertEqual(row["source_schema"], "intent_rejected_v1")
        self.assertEqual(row["status"], "kRejected")
        self.assertEqual(row["reject_reason"], "parallel_limit")
        self.assertEqual(row["local_order_id"], "0")
        self.assertEqual(row["symbol"], "PROVE_USDT")
        self.assertEqual(row["symbol_id"], "4")
        self.assertEqual(row["action"], "kOpenLong")
        self.assertEqual(row["side"], "kBuy")
        self.assertEqual(row["reduce_only"], "false")
        self.assertNotIn("missing_submitted_log", row["warnings"])
        self.assertNotIn("missing_symbol", row["warnings"])
```

- [ ] **Step 2: Add `generate_live_report` fixture**

In `scripts/test/lead_lag/generate_live_report_test.py`, add this test after `test_drift_guard_rejected_intent_joins_signal_without_missing_order`:

```python
    def test_parallel_limit_rejected_intent_joins_signal_without_missing_order(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-06-25 09:00:00.000000100 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                W2026-06-25 09:00:00.000000200 1:1 strategy.h:LogStrategyOrderIntentRejected:407] lead_lag_order_intent_rejected reason=parallel_limit trigger_exchange_ns=1782360000000000000 trigger_local_ns=1782360000000010000 on_book_ticker_entry_ns=1782360000000015000 signal_decision_ns=1782360000000020000 lead_exchange_ns=1782360000000000000 lead_local_ns=1782360000000010000 signal_lead_id=7001 lead_freshness_ns=20000 lag_exchange_ns=1782359999999990000 lag_local_ns=1782360000000005000 signal_lag_id=7002 lag_freshness_ns=30000 symbol=PROVE_USDT symbol_id=4 action=kOpenLong side=kBuy reduce_only=false position_id=0 quantity=0 price=0.2711 raw_price=0.2711 order_price=0.2711 slippage_ticks=0 price_tick=0.0001 target_open_notional=100 estimated_notional=0 gross_before=0 gross_after=0 max_gross_notional=0 local_order_id=0 place_status=-
                """,
            )

            result = report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
            )

            report_dir = output_root / "run-1"
            with (report_dir / "signal.csv").open(newline="", encoding="utf-8") as input_file:
                signal_rows = list(csv.DictReader(input_file))
            with (report_dir / "order_detail.csv").open(newline="", encoding="utf-8") as input_file:
                order_rows = list(csv.DictReader(input_file))
            report_text = (report_dir / "report.md").read_text(encoding="utf-8")

        self.assertEqual(result.latency_rows, 0)
        self.assertEqual(len(signal_rows), 1)
        self.assertNotIn("missing_order", signal_rows[0]["warnings"])
        self.assertEqual(signal_rows[0]["status"], "kRejected")
        self.assertEqual(signal_rows[0]["local_order_id"], "0")
        self.assertEqual(len(order_rows), 1)
        self.assertEqual(order_rows[0]["source_schema"], "intent_rejected_v1")
        self.assertEqual(order_rows[0]["reject_reason"], "parallel_limit")
        self.assertIn("- submitted order: `0`", report_text)
```

- [ ] **Step 3: Run Python tests**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
```

Expected: both scripts pass. These tests may already pass before C++ implementation because the parser is reason-agnostic; keep them as schema coverage for the new reason.

- [ ] **Step 4: Commit Task 3**

```bash
git add scripts/test/lead_lag/analyze_order_detail_test.py scripts/test/lead_lag/generate_live_report_test.py
git commit -m "test: cover LeadLag parallel limit rejected intents"
```

---

### Task 4: Document New Rejected Intent Reason

**Files:**
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/lead_lag_live_report_csv_schema.md`

- [ ] **Step 1: Update diagnostic field docs**

In `docs/diagnostic_fields.md`, update the `lead_lag_order_intent_rejected` / rejected intent reason description near the LeadLag live log fields so it includes:

```text
`parallel_limit` 表示 open signal 已触发，但当前 pair 的 active execution group 数量已达到 `execute.parallel`，策略未提交订单。
```

- [ ] **Step 2: Update live report schema docs**

In `docs/lead_lag_live_report_csv_schema.md`, update the `reject_reason` row so it states:

```text
`intent_rejected_v1` 行的 `reject_reason` 可包括 `drift_guard`、`parallel_limit`、`risk_limit`、`stale_lead_quote`、`stale_lag_quote` 和本地下单准备阶段拒绝原因。
```

Keep existing freshness wording if the table has a freshness-specific row; add `parallel_limit` only to the general rejected-intent semantics.

- [ ] **Step 3: Run docs whitespace check**

Run:

```bash
git diff --check
```

Expected: no output and exit code 0.

- [ ] **Step 4: Commit Task 4**

```bash
git add docs/diagnostic_fields.md docs/lead_lag_live_report_csv_schema.md
git commit -m "docs: document LeadLag parallel limit rejected intent"
```

---

### Task 5: Focused Verification And Performance Boundary

**Files:**
- No source edits unless verification exposes a defect.

- [ ] **Step 1: Run focused C++ strategy tests**

Run:

```bash
ctest --test-dir build/debug -R '(lead_lag_signal_test|lead_lag_strategy_interface_test)' --output-on-failure
```

Expected: both test binaries pass.

- [ ] **Step 2: Run focused Python report tests**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
```

Expected: both Python test scripts pass.

- [ ] **Step 3: Run broader LeadLag focused tests**

Run:

```bash
ctest --test-dir build/debug -R '(lead_lag|signal_csv_writer)' --output-on-failure
```

Expected: all matching tests pass.

- [ ] **Step 4: Run diff whitespace check**

Run:

```bash
git diff --check
```

Expected: no output and exit code 0.

- [ ] **Step 5: Performance boundary**

If this implementation is about to be used for live trading, run the relevant release benchmark before claiming no hot-path regression:

```bash
./build.sh release
./build/release/benchmark/strategy/lead_lag_strategy_benchmark
```

Expected: benchmark completes. Record command, CPU context, and observed output in the final implementation summary. Do not claim latency impact without this benchmark evidence.

- [ ] **Step 6: Final implementation commit if verification required fixups**

If verification required additional source edits, commit them:

```bash
git add \
  strategy/lead_lag/signal.h \
  strategy/lead_lag/strategy.h \
  test/strategy/lead_lag_signal_test.cpp \
  test/strategy/lead_lag_strategy_interface_test.cpp \
  scripts/test/lead_lag/analyze_order_detail_test.py \
  scripts/test/lead_lag/generate_live_report_test.py \
  docs/diagnostic_fields.md \
  docs/lead_lag_live_report_csv_schema.md
git commit -m "fix: complete LeadLag parallel limit guard verification"
```

If no additional source edits were needed, do not create an empty commit.

---

## Self-Review

- Spec coverage: This plan implements item 1 from `docs/lead_lag_go_cpp_strategy_alignment.md`: post-signal `parallel-limit`, `lead_lag_signal_triggered`, `lead_lag_order_intent_rejected reason=parallel_limit`, no synthetic/external order when full, report parser coverage, schema docs, and focused verification.
- Placeholder scan: No placeholder steps remain; every code change step includes the target file and concrete snippet.
- Type consistency: The plan uses existing names from the codebase: `SignalRejectReason::kParallelLimit`, `AppliesOpenFreshnessGuard()`, `LogOrderIntentRejectedForSignal()`, `PositionAccountingMode::kSyntheticSignals`, `lead_lag_order_intent_rejected`, and `intent_rejected_v1`.
