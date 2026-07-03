# LeadLag Fixed Slot Group Metadata Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate LeadLag multi-group execution to `FixedOrderedSlotPool<ExecutionGroup, 16>` and replace `parent_id` with `group_id` / `group_index` across core order contracts, order gateway SHM, logs, report scripts, docs, and tests.

**Architecture:** `group_id` is a per-`symbol_id` execution group identity. `group_index` is the runtime slot index in `FixedOrderedSlotPool` and is used for O(1) strategy feedback lookup after `local_order_id -> StrategyOrder`; every lookup must validate `order.group_id == group.group_id` before applying terminal state. The migration is end-to-end and not historical-log compatible: old `parent_id` logs require the old scripts.

**Tech Stack:** C++20, CMake, GoogleTest, Python report scripts, Gate order gateway SHM, `FixedOrderedSlotPool`.

---

## Scope

This plan is one migration with multiple atomic commits. Do not stop the branch after only replacing `parent_id` in one layer.

Locked decisions:

- `kMaxLeadLagExecutionGroups = 16`.
- `parallel > 16` fails config parsing or runtime init.
- `group_id` is per `symbol_id`; cross-symbol keys use `symbol_id + group_id`.
- `group_index` is diagnostic and runtime-only; it is not a stable report join key.
- `parent_id` is deleted from production order contracts, SHM types, LeadLag logs, Gate gateway logs, current report scripts, current schema docs, and tests.
- No new parser compatibility for old `parent_id` logs.

## Files

- Modify: `core/trading/order_types.h`
- Modify: `core/trading/order_manager.h`
- Modify: `core/trading/order_gateway_shm_types.h`
- Modify: `core/trading/order_gateway_client.h`
- Modify: `exchange/gate/trading/order_types.h`
- Modify: `exchange/gate/trading/order_session.h`
- Modify: `exchange/gate/trading/order_session_runtime_adapter.h`
- Modify: `exchange/gate/trading/order_gateway_worker.h`
- Modify: `strategy/lead_lag/execution_state.h`
- Modify: `strategy/lead_lag/signal.h`
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `strategy/lead_lag/strategy_test_hooks.h`
- Modify: `strategy/lead_lag/config.cpp`
- Modify: `scripts/lead_lag/analyze_order_detail.py`
- Modify: `scripts/lead_lag/generate_live_report.py`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/lead_lag_live_report_csv_schema.md`
- Modify: `docs/gate_order_gateway_shm_design.md`
- Modify: `docs/strategy_order_component_model.md`
- Modify tests under `test/core/trading`, `test/exchange/gate/trading`, `test/strategy`, and `scripts/test/lead_lag`.

## Task 1: Core Order Group Metadata Contract

**Files:**
- Modify: `core/trading/order_types.h`
- Modify: `core/trading/order_manager.h`
- Modify: `test/core/trading/order_manager_route_test.cpp`

- [ ] **Step 1: Update the order manager route test first**

Change `CapturingGateway` from parent tracking to group metadata:

```cpp
FakeSendResult PlaceOrder(const StrategyOrder& order) noexcept {
  placed_routes.push_back(order.gateway_route_id);
  placed_ids.push_back(order.local_order_id);
  placed_group_ids.push_back(order.group_id);
  placed_group_indices.push_back(order.group_index);
  return {.status = FakeSendStatus::kOk, .send_local_ns = 123};
}

std::vector<std::uint64_t> placed_group_ids;
std::vector<std::uint16_t> placed_group_indices;
```

Update the request assertions:

```cpp
request.group_id = 9001;
request.group_index = 7;
request.gateway_route_id = 3;
const core::OrderPlaceResult placed = manager.PlaceOrder(request);
ASSERT_EQ(placed.status, core::OrderPlaceStatus::kOk);
ASSERT_EQ(gateway.placed_group_ids.size(), 1U);
ASSERT_EQ(gateway.placed_group_indices.size(), 1U);
EXPECT_EQ(gateway.placed_group_ids[0], 9001U);
EXPECT_EQ(gateway.placed_group_indices[0], 7U);
EXPECT_EQ(manager.FindOrder(placed.local_order_id)->group_id, 9001U);
EXPECT_EQ(manager.FindOrder(placed.local_order_id)->group_index, 7U);
```

- [ ] **Step 2: Run the focused test and verify it fails**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_trading_order_manager_route_test -j8
./build/debug/test/core/trading/core_trading_order_manager_route_test
```

Expected: compile failure mentioning missing `group_id`, `group_index`, or `kInvalidOrderGroupIndex`.

- [ ] **Step 3: Implement core order fields**

In `core/trading/order_types.h`, include `<limits>` and add:

```cpp
inline constexpr std::uint16_t kInvalidOrderGroupIndex =
    std::numeric_limits<std::uint16_t>::max();
```

Replace `parent_id` in `OrderCreateRequest`, `StrategyOrder`, and `OrderResponseEvent` with:

```cpp
std::uint64_t group_id{0};
std::uint16_t group_index{kInvalidOrderGroupIndex};
```

In `core/trading/order_manager.h::CreateOrder`, copy:

```cpp
order->group_id = request.group_id;
order->group_index = request.group_index;
```

Remove the old `order->parent_id = request.parent_id;` assignment.

- [ ] **Step 4: Run the focused test and verify it passes**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_trading_order_manager_route_test -j8
./build/debug/test/core/trading/core_trading_order_manager_route_test
```

Expected: test passes.

- [ ] **Step 5: Commit**

```bash
git add core/trading/order_types.h core/trading/order_manager.h test/core/trading/order_manager_route_test.cpp
git commit -m "Replace core order parent id with group metadata"
```

## Task 2: Order Gateway SHM And Gate Logging

**Files:**
- Modify: `core/trading/order_gateway_shm_types.h`
- Modify: `core/trading/order_gateway_client.h`
- Modify: `exchange/gate/trading/order_types.h`
- Modify: `exchange/gate/trading/order_session.h`
- Modify: `exchange/gate/trading/order_session_runtime_adapter.h`
- Modify: `exchange/gate/trading/order_gateway_worker.h`
- Modify: `test/core/trading/order_gateway_client_test.cpp`
- Modify: `test/core/trading/order_gateway_shm_test.cpp`
- Modify: `test/exchange/gate/trading/order_gateway_worker_test.cpp`

- [ ] **Step 1: Update SHM and client tests first**

Use this expectation style in `order_gateway_client_test.cpp`:

```cpp
core::StrategyOrder order;
order.local_order_id = 1001;
order.group_id = 77;
order.group_index = 4;
order.gateway_route_id = 2;

ASSERT_TRUE(client.FillOrderCommandForTest(
    core::OrderGatewayCommandKind::kPlace, 2, order, &command));
EXPECT_EQ(command.group_id, 77U);
EXPECT_EQ(command.group_index, 4U);
EXPECT_EQ(command.local_order_id, 1001U);
```

Use this expectation style in `order_gateway_shm_test.cpp` and worker tests:

```cpp
command.group_id = 42;
command.group_index = 5;
EXPECT_EQ(actual.group_id, expected.group_id);
EXPECT_EQ(actual.group_index, expected.group_index);
```

- [ ] **Step 2: Run gateway focused tests and verify they fail**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_order_gateway_client_test core_order_gateway_shm_test gate_order_gateway_worker_test -j8
ctest --test-dir build/debug --output-on-failure -R 'order_gateway_client|order_gateway_shm|gate_order_gateway_worker'
```

Expected: compile failures for missing SHM and Gate response group fields.

- [ ] **Step 3: Replace SHM fields**

In `core/trading/order_gateway_shm_types.h`, replace `parent_id` in `OrderGatewayCommand` and `OrderGatewayEvent`:

```cpp
std::uint64_t group_id{0};
std::uint16_t group_index{kInvalidOrderGroupIndex};
```

Keep alignment deterministic by placing `group_index` near `route_id` if that minimizes padding after compiling; do not add dynamic storage.

- [ ] **Step 4: Propagate fields in gateway client and worker**

In `core/trading/order_gateway_client.h::FillOrderCommand`, write:

```cpp
command->group_id = order.group_id;
command->group_index = order.group_index;
```

In `ToOrderResponseEvent`, write:

```cpp
.group_id = event.group_id,
.group_index = event.group_index,
```

In `exchange/gate/trading/order_gateway_worker.h`, replace cached metadata and event propagation:

```cpp
event.group_id = command.group_id;
event.group_index = command.group_index;
order.group_id = command.group_id;
order.group_index = command.group_index;
```

- [ ] **Step 5: Update Gate order session response/log types**

Replace `parent_id` in `exchange/gate/trading/order_types.h`, `order_session_runtime_adapter.h`, and `order_session.h` with `group_id` / `group_index`. The key log fragments should become:

```text
gate_order_send_ok type=place local_order_id={} group_id={} group_index={} route_id={}
gate_order_response kind={} local_order_id={} group_id={} group_index={} route_id={}
gate_order_ack_latency_diagnostic reason={} local_order_id={} group_id={} group_index={}
```

- [ ] **Step 6: Run gateway focused tests and verify they pass**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_order_gateway_client_test core_order_gateway_shm_test gate_order_gateway_worker_test gate_order_session_test -j8
ctest --test-dir build/debug --output-on-failure -R 'order_gateway_client|order_gateway_shm|gate_order_gateway_worker|gate_order_session'
```

Expected: all selected tests pass.

- [ ] **Step 7: Commit**

```bash
git add core/trading/order_gateway_shm_types.h core/trading/order_gateway_client.h exchange/gate/trading/order_types.h exchange/gate/trading/order_session.h exchange/gate/trading/order_session_runtime_adapter.h exchange/gate/trading/order_gateway_worker.h test/core/trading/order_gateway_client_test.cpp test/core/trading/order_gateway_shm_test.cpp test/exchange/gate/trading/order_gateway_worker_test.cpp
git commit -m "Propagate order group metadata through gateway"
```

## Task 3: LeadLag FixedOrderedSlotPool ExecutionState

**Files:**
- Modify: `strategy/lead_lag/execution_state.h`
- Modify: `strategy/lead_lag/config.cpp`
- Modify: `strategy/lead_lag/signal.h`
- Modify: `test/strategy/lead_lag_feedback_state_test.cpp`
- Modify: `test/strategy/lead_lag_config_test.cpp`

- [ ] **Step 1: Add failing execution-state tests**

Add tests covering 16-capacity, slot reuse, and stale slot rejection:

```cpp
TEST(LeadLagFeedbackStateTest, GroupSlotReuseRequiresMatchingGroupId) {
  leadlag::ExecutionState state;
  state.Init(/*parallel=*/2);
  leadlag::ExecutionGroup* first = state.StartOpenGroup();
  ASSERT_NE(first, nullptr);
  const std::uint64_t first_group_id = first->group_id;
  const std::uint16_t first_index = first->group_index;
  EXPECT_TRUE(state.ClearGroupById(first_group_id));

  leadlag::ExecutionGroup* second = state.StartOpenGroup();
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->group_index, first_index);
  EXPECT_NE(second->group_id, first_group_id);
  EXPECT_EQ(state.GroupAt(first_index, first_group_id), nullptr);
  EXPECT_EQ(state.GroupAt(first_index, second->group_id), second);
}
```

Add a config test using the existing `MinimalConfigTomlWithRisk("")` helper and string replacement:

```cpp
TEST(LeadLagConfigTest, RejectsParallelAboveFixedGroupCapacity) {
  const aquila::config::InstrumentCatalog catalog =
      CatalogWithLagQuantityMetadata(/*quantity_step=*/1.0,
                                     /*quantity_decimal_places=*/0);
  std::string text = MinimalConfigTomlWithRisk("");
  const std::string needle = "parallel = 1";
  const std::size_t pos = text.find(needle);
  ASSERT_NE(pos, std::string::npos);
  text.replace(pos, needle.size(), "parallel = 17");

  const auto result = ParseConfigToml(text, catalog);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("execute.parallel"), std::string::npos);
}
```

Use the existing config-test helper style in `lead_lag_config_test.cpp`; do not introduce a new parser path.

- [ ] **Step 2: Run focused tests and verify they fail**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_feedback_state_test lead_lag_config_test -j8
ctest --test-dir build/debug --output-on-failure -R 'lead_lag_feedback_state|lead_lag_config'
```

Expected: compile or assertion failure for missing `group_index`, `GroupAt`, and max parallel validation.

- [ ] **Step 3: Replace vector storage**

In `strategy/lead_lag/execution_state.h`, include `core/base/fixed_ordered_slot_pool.h`, remove `<vector>`, add:

```cpp
inline constexpr std::uint32_t kMaxLeadLagExecutionGroups = 16;
using ExecutionGroupIndex = std::uint16_t;
inline constexpr ExecutionGroupIndex kInvalidExecutionGroupIndex =
    core::kInvalidOrderGroupIndex;
```

Add `ExecutionGroup::group_index` and remove `ExecutionGroup::parent_id`:

```cpp
ExecutionGroupIndex group_index{kInvalidExecutionGroupIndex};
```

Replace storage:

```cpp
FixedOrderedSlotPool<ExecutionGroup, kMaxLeadLagExecutionGroups,
                     ExecutionGroupIndex>
    groups_;
```

Update `Init(parallel)` to use `Initialize(parallel)` and store the actual capacity. Because config rejects `parallel > 16`, initialized capacity should equal requested capacity in production.

- [ ] **Step 4: Add slot-aware helpers**

Use helpers with these signatures:

```cpp
[[nodiscard]] ExecutionGroup* GroupAt(ExecutionGroupIndex index,
                                      std::uint64_t group_id) noexcept;
[[nodiscard]] const ExecutionGroup* GroupAt(ExecutionGroupIndex index,
                                            std::uint64_t group_id) const noexcept;

template <typename Fn>
void ForEachActiveGroup(Fn&& fn) noexcept;

template <typename Fn>
void ForEachMutableActiveGroup(Fn&& fn) noexcept;
```

`GroupAt()` returns `nullptr` unless the slot is occupied and `At(index).group_id == group_id`.

- [ ] **Step 5: Update group creation and clearing**

`StartOpenGroup()` and `AddHoldGroup()` should use `EmplaceBack()` and stamp `group_index`:

```cpp
const ExecutionGroupIndex index = groups_.EmplaceBack(ExecutionGroup{
    .stage = ExecutionStage::kOpen,
    .group_id = next_group_id_++,
});
if (index == decltype(groups_)::kInvalidIndex) {
  return nullptr;
}
ExecutionGroup& group = groups_.At(index);
group.group_index = index;
return &group;
```

`ClearGroup()` should erase by `group.group_index` after clearing unknown tracking. Do not keep a pointer after erase.

- [ ] **Step 6: Replace active iteration callers**

In `signal.h`, replace range-for over `groups()` / `mutable_groups()` with `ForEachActiveGroup` / `ForEachMutableActiveGroup`. For early-return decisions, use a local `SignalDecision result` and set it inside the callback.

In `strategy.h::CurrentGlobalRiskTotals()`, use active iteration helper and skip zero positions as before.

- [ ] **Step 7: Run focused tests and commit**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_feedback_state_test lead_lag_config_test lead_lag_signal_test -j8
ctest --test-dir build/debug --output-on-failure -R 'lead_lag_feedback_state|lead_lag_config|lead_lag_signal'
```

Expected: selected tests pass.

Commit:

```bash
git add strategy/lead_lag/execution_state.h strategy/lead_lag/config.cpp strategy/lead_lag/signal.h test/strategy/lead_lag_feedback_state_test.cpp test/strategy/lead_lag_config_test.cpp
git commit -m "Move LeadLag execution groups to fixed slot pool"
```

## Task 4: LeadLag Strategy Order Flow And Logs

**Files:**
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `strategy/lead_lag/strategy_test_hooks.h`
- Modify: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [ ] **Step 1: Update strategy interface tests first**

Replace parent assertions with group metadata assertions:

```cpp
const auto& open_order = order_session.placed_orders[0];
EXPECT_EQ(open_order.group_id, 1U);
EXPECT_NE(open_order.group_index, core::kInvalidOrderGroupIndex);

const auto& close_order = order_session.placed_orders.back();
EXPECT_EQ(close_order.group_id, open_order.group_id);
EXPECT_EQ(close_order.group_index, open_order.group_index);
```

For two different open groups on the same symbol, assert:

```cpp
EXPECT_EQ(first_open.group_id, 1U);
EXPECT_EQ(second_open.group_id, 2U);
```

- [ ] **Step 2: Run strategy test and verify it fails**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_strategy_interface_test -j8
./build/debug/test/strategy/lead_lag_strategy_interface_test
```

Expected: compile failures for parent log fields and order request fields.

- [ ] **Step 3: Remove parent allocation and write group fields to orders**

Delete `next_execution_parent_id_`, `AllocateExecutionParentId()`, and `EnsureExecutionParentId()`.

Change `PlacePreparedExternalOrder()` to accept `const ExecutionGroup& group` and populate:

```cpp
.group_id = group.group_id,
.group_index = group.group_index,
```

Change submit-stage test hook parameter names from `parent_id` to `group_id`, and keep `local_order_id`, `route_id`, `route_index`, and `route_count`.

- [ ] **Step 4: Apply terminal orders by group metadata**

Change `ApplyFinishedOrder()` to fetch `order` once, then:

```cpp
ExecutionApplyResult applied =
    runtime->execution.ApplyTerminalOrder(*order, runtime->pair.lag_instrument);
```

Inside `ExecutionState::ApplyTerminalOrder`, use `GroupAt(order.group_index, order.group_id)` as the normal path. If it returns `nullptr`, return `kIgnoredUnknownOrder`; the `Strategy::ApplyFinishedOrder()` caller must then call `runtime->execution.MarkNeedsReconcile()` before logging the finished order. Do not scan active groups in the normal terminal path.

For submit rejected flow, fetch `const core::StrategyOrder* order = context.FindOrder(local_order_id)` and call an overload:

```cpp
runtime->execution.ApplySubmitRejected(*order);
```

- [ ] **Step 5: Replace LeadLag log fields**

Update log strings:

```text
lead_lag_order_submitted local_order_id={} group_id={} group_index={} route_id={}
lead_lag_order_response kind={} local_order_id={} group_id={} group_index={} route_id={}
lead_lag_order_feedback kind={} local_order_id={} group_id={} group_index={} route_id={}
lead_lag_order_finished local_order_id={} group_id={} group_index={} route_id={}
```

Update `StrategyOrderSubmittedLogRecordForTest`, `StrategyOrderFinishedLogRecordForTest`, `StrategyOrderResponseLogRecordForTest`, `StrategyOrderFeedbackLogRecordForTest`, and submit-stage hook records to use `group_id` / `group_index`.

- [ ] **Step 6: Run strategy focused tests and commit**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target lead_lag_strategy_interface_test lead_lag_feedback_state_test -j8
ctest --test-dir build/debug --output-on-failure -R 'lead_lag_strategy_interface|lead_lag_feedback_state'
```

Expected: selected tests pass.

Commit:

```bash
git add strategy/lead_lag/strategy.h strategy/lead_lag/strategy_test_hooks.h test/strategy/lead_lag_strategy_interface_test.cpp
git commit -m "Use group metadata in LeadLag order flow"
```

## Task 5: Report Scripts And Current Schema

**Files:**
- Modify: `scripts/lead_lag/analyze_order_detail.py`
- Modify: `scripts/lead_lag/generate_live_report.py`
- Modify: `scripts/test/lead_lag/analyze_order_detail_test.py`
- Modify: `scripts/test/lead_lag/generate_live_report_test.py`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/lead_lag_live_report_csv_schema.md`

- [ ] **Step 1: Update script fixtures to new log fields**

Replace sample log snippets:

```text
gate_order_send_ok type=place local_order_id=288230376151711749 group_id=777 group_index=3 route_id=3 ...
lead_lag_order_submitted local_order_id=288230376151711749 group_id=777 group_index=3 route_id=3 ...
gate_order_response kind=kAck local_order_id=288230376151711749 group_id=777 group_index=3 route_id=3 ...
lead_lag_order_response kind=kAck local_order_id=288230376151711749 group_id=777 group_index=3 route_id=3 ...
lead_lag_order_feedback kind=kFilled local_order_id=288230376151711749 group_id=777 group_index=3 route_id=3 ...
lead_lag_order_finished local_order_id=288230376151711749 group_id=777 group_index=3 route_id=3 ...
```

Change assertions:

```python
self.assertEqual(row["group_id"], "777")
self.assertEqual(row["group_index"], "3")
self.assertNotIn("parent_id", row)
```

- [ ] **Step 2: Run script tests and verify they fail**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
```

Expected: failures because `ORDER_DETAIL_FIELDS` and `LATENCY_FIELDS` still contain `parent_id`.

- [ ] **Step 3: Update output fields and parsers**

In `scripts/lead_lag/analyze_order_detail.py`, replace `parent_id` in order detail and latency fields with:

```python
"group_id",
"group_index",
```

Update merge loops from:

```python
for key in ("parent_id", "route_id"):
```

to:

```python
for key in ("group_id", "group_index", "route_id"):
```

In latency rows, write:

```python
"group_id": order.get("group_id", ""),
"group_index": order.get("group_index", ""),
```

Do not add fallback from `parent_id`.

In `generate_live_report.py`, preserve the new columns when copying order and latency CSV rows. If `parent_id` appears in generated CSV fields, remove it and add `group_id` / `group_index`.

- [ ] **Step 4: Update current schema docs**

In `docs/diagnostic_fields.md` and `docs/lead_lag_live_report_csv_schema.md`, replace stable/current `parent_id` entries with `group_id` / `group_index`. State:

- `group_id`: per-symbol execution group id.
- `group_index`: runtime slot index diagnostic; not a stable join key.
- Cross-symbol grouping key: `symbol_id + group_id`.

- [ ] **Step 5: Run script tests and commit**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
```

Expected: both pass.

Commit:

```bash
git add scripts/lead_lag/analyze_order_detail.py scripts/lead_lag/generate_live_report.py scripts/test/lead_lag/analyze_order_detail_test.py scripts/test/lead_lag/generate_live_report_test.py docs/diagnostic_fields.md docs/lead_lag_live_report_csv_schema.md
git commit -m "Migrate LeadLag reports to group metadata"
```

## Task 6: Active Documentation And Final Cleanup

**Files:**
- Modify: `docs/gate_order_gateway_shm_design.md`
- Modify: `docs/strategy_order_component_model.md`
- Modify: `docs/project_onboarding_guide.md`
- Modify: `docs/lead_lag_fixed_ordered_slot_pool_parallel.md`

- [ ] **Step 1: Update active design docs**

Replace current `parent_id` semantics with:

```text
group_id is the per-symbol execution group identity. group_index is a runtime
slot index used for O(1) feedback lookup and diagnostics; it is not a stable
report join key.
```

When the document describes fanout child orders, use:

```text
Fanout child orders share group_id and have unique local_order_id plus route_id.
```

- [ ] **Step 2: Search active code for parent_id**

Run:

```bash
rg -n "parent_id" core exchange tools strategy scripts test docs/diagnostic_fields.md docs/lead_lag_live_report_csv_schema.md docs/gate_order_gateway_shm_design.md docs/strategy_order_component_model.md docs/project_onboarding_guide.md docs/lead_lag_fixed_ordered_slot_pool_parallel.md
```

Expected: no hits in production code, current scripts, current tests, or current active docs listed above. Hits in historical plan files under `docs/superpowers/plans/` are not part of this command.

- [ ] **Step 3: Run final focused verification**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_order_gateway_client_test core_order_gateway_shm_test gate_order_gateway_worker_test gate_order_session_test lead_lag_feedback_state_test lead_lag_config_test lead_lag_signal_test lead_lag_strategy_interface_test -j8
ctest --test-dir build/debug --output-on-failure -R 'order_gateway_client|order_gateway_shm|gate_order_gateway_worker|gate_order_session|lead_lag_feedback_state|lead_lag_config|lead_lag_signal|lead_lag_strategy_interface'
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
git diff --check
```

Expected: all pass.

- [ ] **Step 4: Run performance checks before claiming latency improvement**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target lead_lag_group_container_benchmark lead_lag_submit_breakdown_benchmark -j8
taskset -c 16 ./build/release/benchmark/strategy/lead_lag_group_container_benchmark --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
taskset -c 16 ./build/release/benchmark/strategy/lead_lag_submit_breakdown_benchmark --benchmark_filter='BM_OrderGatewayFanout(CurrentPlaceOrder|BatchModel)4Routes(/|$)' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

Expected: benchmarks complete. Do not claim a latency improvement unless the new numbers support it.

- [ ] **Step 5: Commit docs and cleanup**

```bash
git add docs/gate_order_gateway_shm_design.md docs/strategy_order_component_model.md docs/project_onboarding_guide.md docs/lead_lag_fixed_ordered_slot_pool_parallel.md
git commit -m "Document order group metadata migration"
```

## Final Verification

Before handing back:

```bash
git status --short --branch
git log --oneline -8
```

The expected final state is a clean branch with several atomic commits and no remaining current-code `parent_id` references.
