# LeadLag REST Reconcile / Feedback Recovery Design and Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a conservative read-only recovery path for LeadLag after Gate private feedback continuity is lost, so the runner can prove local order and position state before any real-order smoke is enabled.

**Architecture:** Keep the first recovery component read-only: it snapshots local `OrderManager` / LeadLag execution state, queries Gate REST order and position facts, maps remote facts to local orders, and only clears `needs_reconcile` when identity, cumulative fills, open orders, and REST position are unambiguous. Ambiguous or incomplete facts enter `ManualIntervention`; the helper must not submit replacement orders, auto-close positions, or open the currently disabled `RunLiveOrders()` path.

**Tech Stack:** C++20 strategy/runtime code, Gate WebSocket order and feedback sessions, Gate APIv4 REST helper scripts, CTest, Python unit tests, `ctest`, `git diff --check`.

---

## Scope

### Goals

- Restore confidence after feedback continuity loss by comparing local in-memory state with Gate REST facts.
- Start with read-only reconcile: no automatic补单, no automatic平仓, no cancel-all, and no order placement from the reconcile helper.
- Keep new opens paused from the first degraded signal until recovery succeeds and the strategy state is explicitly cleared.
- Route all uncertain cases to `ManualIntervention` with enough structured evidence for a human to decide.
- Make the document detailed enough for later subagents to implement the helper, tests, strategy recovery API, runner gate, smoke ladder, and benchmarks in separate commits.

### Non-goals

- Do not enable `tools/lead_lag/live_strategy.cpp::RunLiveOrders()`.
- Do not add automatic hedge, flatten, cancel-all, or backfill behavior in the first version.
- Do not treat Gate REST as a low-latency hot-path dependency; all REST use is cold recovery / smoke verification.
- Do not infer fills from broad time-window matches alone. Time-window matching can only produce human-readable candidates unless another identity signal confirms the order.
- Do not claim performance or recovery reliability without fresh tests, smoke output, or benchmark evidence.

## Current Facts

- `OrderManager` owns live local order state in memory: `StrategyOrder` includes `local_order_id`, optional `exchange_order_id`, side, quantity, `price_text`, `reduce_only`, cumulative filled quantity/value, status, finish reason, and `is_finished`.
- LeadLag owns strategy execution state in memory: `ExecutionGroup` stores stage, pending `local_order_id`, signed position quantity, trailing price, and group id. `ExecutionState::OnFeedbackContinuityLost()` sets `degraded=true` and `needs_reconcile=true`; `new_entries_paused()` is true while either flag is set.
- Gate private feedback continuity is represented by `OrderFeedbackKind::kContinuityLost` from SHM. It means the order fact stream is no longer provably continuous; it is not itself a terminal order fact.
- Gate REST facts currently available through `scripts/gate/query_gate_account.py`:
  - `orders --status open` for open futures orders by contract.
  - `orders --status finished` for finished futures orders by contract.
  - `orders --order-id <id-or-text>` for a single Gate order id or client text id.
  - `positions --contract <contract>` for the current futures position.
  - `account` for futures account and wallet / fee summaries.
- `local_order_id` identity:
  - `core/trading/order_id.h` encodes `local_order_id` as high 8 bits `strategy_id` plus low 56 bits `strategy_order_id`.
  - `exchange/gate/trading/order_codecs.h::OrderTextCodec` formats Gate client text as `t-<positive local_order_id>` and parses only that form.
  - `exchange/gate/trading/order_request_encoder.h::EncodePlaceOrderRequest()` always includes this `text` field in place requests.
  - `EncodeCancelOrderRequest()` uses `exchange_order_id` when known; otherwise it falls back to the same `text` value.
  - `RequestIdCodec` encodes request type in the high 8 bits and request sequence in the low 56 bits. Request id is a WebSocket correlation id, not durable order identity.
  - `OrderSession` caches `local_order_id -> exchange_order_id` after successful place final result / accepted feedback and clears it on disconnect or terminal order.
  - Gate `futures.orders` feedback is parsed by `OrderTextCodec::Parse(raw.text)` and routed by `LocalOrderIdCodec::StrategyId(local_order_id)`.
- Process restart currently loses `OrderManager` and LeadLag execution state unless a future local recovery snapshot is added. Without a local snapshot or journal, restart recovery is read-only evidence gathering plus manual intervention, not automatic resume.

## Trigger Scenarios

- Feedback SHM reports `OrderFeedbackKind::kContinuityLost`.
- Gate private feedback WebSocket reconnect has an unknown window, including disconnect, reconnect backoff, producer restart, queue full, or unrecoverable decode.
- Strategy process restarts while there may be live orders or positions.
- Local state has any pending order: `ExecutionGroup.local_order_id != 0` or `OrderManager` order status is sent / accepted / partial / cancel-sent.
- Any required REST query fails, times out, returns rate-limit errors, or returns partial data.
- REST position does not match the reconstructed LeadLag execution position.
- REST open orders contain an order that cannot be mapped to a local order by safe identity rules.

## Recovery State Machine

| State | Meaning | New opens | Close / stoploss | Exit rule |
| --- | --- | --- | --- | --- |
| `Normal` | Feedback stream is continuous enough for current policy; no reconcile required. | Allowed when `OrderSession::Ready()` and strategy limits allow it. | Allowed through normal reduce-only order path. | Enter `DegradedNeedsReconcile` on continuity lost, unknown reconnect window, restart requiring recovery, local pending order at unsafe startup, or REST/local mismatch. |
| `DegradedNeedsReconcile` | A continuity or state-integrity signal was observed; local facts may still be useful but are not enough to resume opens. | Forbidden. | Only risk-reducing reduce-only close / stoploss may be considered when there is a locally known hold, no local pending order, no unmapped remote open order, and policy has not started a read-only snapshot. First LeadLag runner implementation may choose the stricter rule and pause all automatic order submission. | Enter `Reconciling` when a read-only reconcile attempt starts; enter `ManualIntervention` if a required precondition is missing. |
| `Reconciling` | The recovery component is taking local and REST snapshots and mapping facts. | Forbidden. | Forbidden for the automatic runner; the reconcile helper is read-only. Emergency manual actions are outside this strategy path and must be documented separately. | Enter `Recovered` only if every guardrail passes; otherwise enter `ManualIntervention`. |
| `Recovered` | The helper proved local state and REST state agree. | Allowed only after `needs_reconcile` is cleared and the runner logs the recovery summary. | Allowed. | Transition immediately back to `Normal` after clearing the degraded/reconcile flags. |
| `ManualIntervention` | Facts are incomplete, conflicting, stale, or ambiguous. | Forbidden. | Forbidden for the automatic runner. Human-directed reduce-only or cancel actions must happen outside this first read-only flow, followed by a new REST verification. | Exit only after a human documents the account/order/position state and an explicit recovery command or future reviewed API resets the strategy state. |

State transition notes:

- `Recovered` is not a trading mode for long-running operation; it is an auditable transition point.
- `ManualIntervention` must include the reason, local snapshot, REST summary, unmatched local ids, unmatched remote order ids/texts, and REST position.
- A reconnect alone is not recovery. The system must still reconcile the unknown window before reopening new entries.

## Reconcile Algorithm

1. Freeze new opens.
   - Set or observe the LeadLag degraded / `needs_reconcile` state.
   - Stop creating new open intents from `SignalEngine`.
   - Keep `RunLiveOrders()` disabled until a later reviewed task explicitly opens it.
2. Snapshot local state.
   - Capture every live `OrderManager` order: local id, exchange id, contract, side, quantity, price text, reduce-only, status, cumulative fill, and finished flag.
   - Capture every LeadLag execution group: group id, stage, pending local order id, signed position quantity, and trailing price.
   - Record the snapshot time and the feedback continuity reason / sequence.
3. Query open orders by contract.
   - Use `scripts/gate/query_gate_account.py orders --contract <CONTRACT> --status open --limit <N>`.
   - Any open order must either map to a local pending order or block resume.
4. Query finished order facts.
   - Prefer direct single-order lookup by Gate `exchange_order_id`.
   - Query by encoded text `t-<local_order_id>` when exchange id is missing or the order session cache was cleared.
   - Query the recent finished list for a constrained time window around the local order creation / feedback gap. The time-window list is candidate evidence only unless identity is also confirmed.
5. Query current position.
   - Use `scripts/gate/query_gate_account.py positions --contract <CONTRACT>`.
   - Query account summary when smoke evidence requires margin, balance, or pending order diagnostics.
6. Map remote facts to local orders.
   - Build maps by `local_order_id` from text, by `exchange_order_id`, and by contract.
   - Detect duplicates, conflicting remote records, stale feedback, and local orders that have no remote fact.
7. Apply terminal facts only when identity and cumulative fill are unambiguous.
   - A terminal fact is safe when it maps by text or exchange id, belongs to the expected contract, matches local side/size/price constraints, and has cumulative filled quantity that is equal to or greater than the local cumulative fill.
   - Do not decrease local cumulative fill from REST or feedback.
   - Do not apply terminal status from broad time-window candidate matching.
8. Reconstruct LeadLag position from mapped facts.
   - Start from the local execution snapshot.
   - Apply only safe terminal facts.
   - Derive expected signed position by group and contract.
9. Compare reconstructed position with REST position.
   - REST position size and direction must equal reconstructed execution state before resuming.
   - For the first LeadLag real-order smoke, require no residual remote open orders and zero position unless the smoke explicitly expects an open hold.
10. Finish with one of two outcomes.
   - Clear `needs_reconcile`, emit a recovery summary, and return to `Normal` only if identity, open orders, terminal facts, and REST position all agree.
   - Enter `ManualIntervention` if any fact is missing, ambiguous, contradictory, stale beyond the recovery window, or only matched by time-window heuristics.

## Identity / Matching Rules

1. Primary identity: Gate text `t-<local_order_id>`.
   - This is the durable client identity written by the place encoder.
   - It also carries strategy lane routing through `local_order_id >> 56`.
   - If a remote order has invalid or missing text, it cannot be auto-applied to LeadLag state.
2. Secondary identity: cached or REST-returned `exchange_order_id`.
   - Use this when it is present in local `StrategyOrder.exchange_order_id`, `OrderSession` cache, accepted feedback, or REST order facts.
   - If the same exchange id maps to multiple local orders, enter `ManualIntervention`.
3. Tertiary candidate match: constrained time window plus contract, side, size, and price.
   - This may help a human identify an order after restart or missing text.
   - It must not auto-apply terminal facts because two IOC orders can share contract, side, size, price, and a close time window.

## Account / Position Guardrails

- Open orders must be empty or safely mapped.
  - Empty is required for the first small real-order smoke ladder unless that smoke explicitly expects a live pending order.
  - Mapped open orders may keep the strategy in a tracked pending state, but they must not reopen new entries until feedback continuity and position checks are clean.
- REST position must equal reconstructed LeadLag execution state before resuming.
  - If reconstructed state is flat, REST position must be flat and `pending_orders` must be zero or explained.
  - If reconstructed state holds a position, side and integer contract size must match the sum of active execution groups.
- Position drift handling:
  - Drift in side, size, or unexplained pending order count enters `ManualIntervention`.
  - Drift caused by an unmapped filled or partially-cancelled order is not auto-corrected in the first version.
- Reduce-only behavior:
  - LeadLag close / stoploss orders must stay reduce-only.
  - The read-only reconcile helper must never submit reduce-only orders.
  - Any future automatic risk-reducing action needs a separate reviewed design and smoke evidence.

## Failure Policy

- REST timeout, 5xx, network failure, or rate limit:
  - Do not mutate local execution state.
  - Stay in `DegradedNeedsReconcile` or move to `ManualIntervention` after the configured retry budget.
  - Keep new opens forbidden.
- Partial data:
  - If any required endpoint is missing, treat the whole reconcile attempt as inconclusive.
  - Keep successful endpoint responses in the summary for diagnosis, but do not clear `needs_reconcile`.
- Unknown local order:
  - Feedback or REST facts for a local id not present in the local snapshot are not auto-applied.
  - If the strategy id lane matches LeadLag, enter `ManualIntervention`; otherwise record it as out-of-scope remote account noise.
- Duplicate or stale feedback:
  - Ignore duplicate cumulative quantity equal to local state.
  - Reject stale cumulative quantity lower than local state and keep the existing local state.
  - If stale or duplicate events conflict with REST terminal facts, enter `ManualIntervention`.
- Unmapped remote order:
  - Any open or recently finished Gate order for the contract with no safe text or exchange-id match blocks resume.
  - Time-window candidate matches can be printed for humans but cannot clear the block.
- Residual position:
  - Any REST position not explained by reconstructed execution groups blocks resume.
  - The helper must not flatten the position.
- Process restart with no local snapshot:
  - Query REST and print evidence.
  - Do not auto-rebuild LeadLag execution groups.
  - Require manual intervention or a future durable local recovery snapshot before resuming.

## Follow-up Implementation Tasks

### Task 1: Read-only REST Reconcile Helper

**Files:**
- Create: `scripts/gate/reconcile_futures_orders.py`
- Modify: `scripts/gate/query_gate_account.py`
- Test: `scripts/gate/reconcile_futures_orders_test.py`
- Test: `scripts/gate/query_gate_account_test.py`

- [ ] **Step 1: Add REST query builders for bounded order facts**

Add helper functions for open orders, finished orders, single order by id/text, position, and account summary. Preserve `query_gate_account.py` as read-only.

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/query_gate_account_test.py
```

Expected: existing query plan and signature tests pass.

- [ ] **Step 2: Implement read-only reconcile CLI**

Input fields: `--settle`, `--contract`, `--strategy-id`, `--local-state-json`, `--finished-limit`, `--window-sec`, `--allow-partial=false`, and credential env names. Output one JSON summary with `state`, `mapped_orders`, `unmapped_local_orders`, `unmapped_remote_orders`, `rest_position`, `position_match`, and `manual_intervention_reason`.

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/reconcile_futures_orders_test.py
```

Expected: tests cover flat/no-orders recovery, mapped open order, mapped filled order, ambiguous time-window candidate, REST partial failure, and residual position.

- [ ] **Step 3: Live read-only smoke**

Run with credentials after no real LeadLag order path is enabled:

```bash
scripts/gate/query_gate_account.py orders --contract BTC_USDT --status open --limit 50
scripts/gate/query_gate_account.py orders --contract BTC_USDT --status finished --limit 50
scripts/gate/query_gate_account.py positions --contract BTC_USDT
scripts/gate/query_gate_account.py account --contract BTC_USDT
```

Expected: commands return JSON with `ok=true`; no order placement occurs.

- [ ] **Step 4: Commit**

```bash
git add scripts/gate/reconcile_futures_orders.py scripts/gate/reconcile_futures_orders_test.py scripts/gate/query_gate_account.py scripts/gate/query_gate_account_test.py
git commit -m "Add lead lag read-only reconcile helper"
```

### Task 2: Reconcile Parser and Mapping Tests

**Files:**
- Modify: `scripts/gate/reconcile_futures_orders.py`
- Modify: `scripts/gate/reconcile_futures_orders_test.py`

- [ ] **Step 1: Cover identity rules**

Test cases:
- text `t-<local_order_id>` maps to the exact local order.
- exchange id maps when text is unavailable but exchange id is unique.
- duplicate exchange id or duplicate text enters manual intervention.
- constrained time-window candidate is reported but not applied.

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/reconcile_futures_orders_test.py
ctest --test-dir build/debug -R '(gate_order_request_encoder|gate_order_feedback_parser)' --output-on-failure
```

Expected: codec/parser tests pass; reconcile mapping rejects ambiguous matches.

- [ ] **Step 2: Cover terminal apply rules**

Test cases:
- terminal filled with cumulative fill greater than local cumulative fill is safe.
- terminal cancelled with partial cumulative fill is safe only when identity is confirmed.
- cumulative fill lower than local state is stale and blocks apply.
- unknown local order with LeadLag strategy id enters manual intervention.

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/reconcile_futures_orders_test.py
```

Expected: all reconcile parser/mapping cases pass.

- [ ] **Step 3: Commit**

```bash
git add scripts/gate/reconcile_futures_orders.py scripts/gate/reconcile_futures_orders_test.py
git commit -m "Test lead lag reconcile mapping rules"
```

### Task 3: Strategy Recovery API

**Files:**
- Modify: `strategy/lead_lag/execution_state.h`
- Modify: `strategy/lead_lag/strategy.h`
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [ ] **Step 1: Add explicit recovery states**

Represent `Normal`, `DegradedNeedsReconcile`, `Reconciling`, `Recovered`, and `ManualIntervention` in LeadLag strategy state or a small recovery helper. Keep the hot signal path as cheap as the current `new_entries_paused()` check.

Run:

```bash
ctest --test-dir build/debug -R lead_lag_strategy_interface --output-on-failure
```

Expected: tests show new opens are paused on continuity lost and stay paused until recovery is explicitly cleared.

- [ ] **Step 2: Add read-only recovery apply API**

Expose a cold-path API that accepts mapped terminal facts and REST position result. It may clear `needs_reconcile` only when all guardrails pass. It must not submit orders.

Run:

```bash
ctest --test-dir build/debug -R lead_lag --output-on-failure
```

Expected: LeadLag tests pass; recovery failure cases leave new opens paused.

- [ ] **Step 3: Commit**

```bash
git add strategy/lead_lag/execution_state.h strategy/lead_lag/strategy.h test/strategy/lead_lag_strategy_interface_test.cpp
git commit -m "Add lead lag recovery state API"
```

### Task 4: Runner Gating and Recovery Wiring

**Files:**
- Modify: `tools/lead_lag/live_strategy.cpp`
- Modify: `test/tools/lead_lag/live_strategy_test.cpp`
- Modify: `test/tools/lead_lag/lead_lag_live_orders_disabled_smoke.cmake`
- Modify: `doc/lead_lag_live_runtime_plan.md`

- [ ] **Step 1: Keep live orders disabled**

Preserve the current `RunLiveOrders()` disabled return path while adding tests that recovery flags do not bypass it.

Run:

```bash
ctest --test-dir build/debug -R lead_lag_live_strategy --output-on-failure
./build/debug/tools/lead_lag_strategy --config test/tools/lead_lag/lead_lag_live_orders_disabled_strategy.toml --execute --duration-sec 1
```

Expected: CTest passes; direct runner exits with the disabled live-order error and does not create live orders.

- [ ] **Step 2: Wire recovery summary into signal-only diagnostics**

Expose degraded / needs-reconcile / manual-intervention state in low-frequency runner summary. Do not add hot-path logging.

Run:

```bash
ctest --test-dir build/debug -R '(lead_lag_live_strategy|lead_lag_strategy_interface)' --output-on-failure
```

Expected: summaries are stable and tests pass.

- [ ] **Step 3: Commit**

```bash
git add tools/lead_lag/live_strategy.cpp test/tools/lead_lag/live_strategy_test.cpp test/tools/lead_lag/lead_lag_live_orders_disabled_smoke.cmake doc/lead_lag_live_runtime_plan.md
git commit -m "Gate lead lag live runner on recovery state"
```

### Task 5: Live Smoke Ladder After Recovery

**Files:**
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Runtime evidence only: Gate REST summaries, runner summaries, feedback summaries.

- [ ] **Step 1: Feedback disconnect / reconnect recovery smoke**

Run Gate feedback session, force a controlled disconnect or unknown-window event, reconnect, and run read-only reconcile.

Expected: new opens remain paused during degraded/reconciling states; recovery succeeds only when REST facts and local state agree.

- [ ] **Step 2: REST reconcile smoke**

Run the read-only helper against BTC_USDT while the account is flat.

Expected: open orders are empty, position is flat, `manual_intervention_reason` is empty, and no live orders are submitted.

- [ ] **Step 3: Small real-order smoke remains behind recovery**

Only after Steps 1 and 2 have documented evidence and a separate reviewed commit may a future task enable tiny filled open / close, unfilled-cancel, and rejected / cancel-rejected smoke.

Verification after each smoke:

```bash
scripts/gate/query_gate_account.py orders --contract BTC_USDT --status open --limit 50
scripts/gate/query_gate_account.py positions --contract BTC_USDT
```

Expected: no unexplained open orders or residual position.

- [ ] **Step 4: Commit smoke evidence**

```bash
git add doc/lead_lag_live_runtime_plan.md
git commit -m "Document lead lag recovery smoke evidence"
```

### Task 6: Benchmarks

**Files:**
- Create: `benchmark/strategy/lead_lag_runtime_benchmark.cpp`
- Create: `benchmark/strategy/lead_lag_feedback_runtime_benchmark.cpp`
- Modify: `benchmark/strategy/CMakeLists.txt`
- Modify: `doc/lead_lag_live_runtime_plan.md` only for summarized evidence.

- [ ] **Step 1: Benchmark local recovery-state checks**

Measure the steady-state cost of the hot-path recovery gate in `OnBookTicker()` without REST and without logging.

Run:

```bash
./build.sh release
taskset -c 2 ./build/release/benchmark/strategy/lead_lag_strategy_benchmark
taskset -c 2 ./build/release/benchmark/strategy/lead_lag_runtime_benchmark
```

Expected: benchmarks complete; results are recorded as measured local-path evidence only.

- [ ] **Step 2: Benchmark feedback recovery ingress**

Measure `OrderFeedbackSession parser -> SHM -> TradingRuntime -> OrderManager -> LeadLag OnOrderFeedback()` with fixed local fixtures.

Run:

```bash
taskset -c 2 ./build/release/benchmark/strategy/lead_lag_feedback_runtime_benchmark
```

Expected: benchmark completes without network access; no production performance claim is made without live/profile evidence.

- [ ] **Step 3: Commit**

```bash
git add benchmark/strategy/lead_lag_runtime_benchmark.cpp benchmark/strategy/lead_lag_feedback_runtime_benchmark.cpp benchmark/strategy/CMakeLists.txt doc/lead_lag_live_runtime_plan.md
git commit -m "Benchmark lead lag recovery paths"
```

## Phase Verification Matrix

| Phase | Command | Expected high-level result |
| --- | --- | --- |
| Design / docs only | `git diff --check` | No whitespace errors. |
| Design / docs only | `rg -n "lead_lag_reconcile_design" doc/project_onboarding_guide.md doc/lead_lag_live_runtime_plan.md strategy/lead_lag/README.md` | New design document is indexed from the onboarding guide, runtime plan, and LeadLag README. |
| REST helper | `/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/query_gate_account_test.py` | Existing read-only Gate query tests pass. |
| REST helper | `/home/liuxiang/dev/pyenv/lx/bin/python scripts/gate/reconcile_futures_orders_test.py` | New helper covers mapped, ambiguous, partial, and residual-position cases. |
| Strategy API | `ctest --test-dir build/debug -R lead_lag_strategy_interface --output-on-failure` | New opens remain paused until recovery succeeds; failure cases enter manual intervention. |
| Runner gate | `ctest --test-dir build/debug -R lead_lag_live_strategy --output-on-failure` | `--execute` gating behavior is tested while real live orders remain disabled. |
| Focused integration | `ctest --test-dir build/debug -R '(lead_lag|trading_runtime|order_feedback|gate_order)' --output-on-failure` | Focused strategy/runtime/order feedback tests pass after code changes. |
| Live read-only REST | `scripts/gate/query_gate_account.py orders --contract BTC_USDT --status open --limit 50` | Returns read-only JSON with `ok=true`; no live order action. |
| Live read-only REST | `scripts/gate/query_gate_account.py positions --contract BTC_USDT` | Returns current position for manual/reconcile comparison. |
| Disabled live-order guard | `./build/debug/tools/lead_lag_strategy --config test/tools/lead_lag/lead_lag_live_orders_disabled_strategy.toml --execute --duration-sec 1` | Exits through disabled live-order path; no order submission. |
| Benchmark | `taskset -c 2 ./build/release/benchmark/strategy/lead_lag_runtime_benchmark` | Local benchmark completes; only measured evidence is documented. |
