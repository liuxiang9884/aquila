# Gate Order Gateway SHM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the production Gate multi-`OrderSession` order gateway using cross-process SHM command/event queues and wire LeadLag fanout through it.

**Architecture:** The strategy process owns `OrderManager`, execution state, route table, and ready flags. A separate order-gateway process owns N `OrderSessionWorker` threads. Strategy sends fixed-size POD `OrderGatewayCommand` records through `command_queue[i]`; workers return fixed-size POD `OrderGatewayEvent` records through `event_queue[i]`.

**Tech Stack:** C++20, CMake, POSIX shared memory, fixed-size SPSC queues, existing `core/trading`, `exchange/gate/trading`, `strategy/lead_lag`, and `tools/lead_lag` runtime patterns.

---

## File Structure

- Create `core/trading/order_gateway_shm_types.h`: shared POD command/event/header types and constants.
- Create `core/trading/order_gateway_shm.h`: SHM open/create/attach helpers and fixed-size SPSC queue accessors.
- Create `core/trading/order_gateway_client.h`: strategy-side gateway contract implementation for `OrderManager`.
- Create `exchange/gate/trading/order_gateway_worker.h`: worker-side command consumer that owns one `OrderSessionRuntimeAdapter` or raw `OrderSession`.
- Create `tools/gate/gate_order_gateway.cpp`: standalone order gateway process entrypoint.
- Create `core/config/order_gateway_config.h` and `.cpp`: parser for `[order_gateway]`.
- Modify `core/config/strategy_config.h` and `.cpp`: add mutually exclusive `[strategy.order_gateway]` support.
- Modify `tools/lead_lag/live_strategy.cpp`: choose single-session runtime or SHM order-gateway runtime from config.
- Modify `strategy/lead_lag/config.h` and `.cpp`: parse `execute.order_session_fanout`.
- Modify `strategy/lead_lag/execution_state.h` and `strategy/lead_lag/strategy.h`: track parent fanout groups and aggregate child fills.
- Add tests under `test/core/trading/`, `test/config/`, `test/exchange/gate/trading/`, and `test/strategy/`.
- Add configs under `config/order_gateways/` and update live strategy config examples after tests pass.

## Task 1: Shared SHM Types

**Files:**
- Create: `core/trading/order_gateway_shm_types.h`
- Test: `test/core/trading/order_gateway_shm_types_test.cpp`

- [ ] **Step 1: Write the failing ABI test**

```cpp
#include "core/trading/order_gateway_shm_types.h"

#include <type_traits>

#include <gtest/gtest.h>

namespace aquila::core {
namespace {

TEST(OrderGatewayShmTypesTest, PayloadTypesArePodLike) {
  EXPECT_TRUE(std::is_standard_layout_v<OrderGatewayCommand>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderGatewayCommand>);
  EXPECT_TRUE(std::is_standard_layout_v<OrderGatewayEvent>);
  EXPECT_TRUE(std::is_trivially_copyable_v<OrderGatewayEvent>);
}

TEST(OrderGatewayShmTypesTest, ConstantsMatchDesign) {
  EXPECT_EQ(kMaxOrderGatewayRoutes, 16U);
  EXPECT_EQ(kOrderGatewaySymbolBytes, 32U);
  EXPECT_EQ(kOrderGatewayQuantityTextBytes, 32U);
  EXPECT_EQ(kOrderGatewayPriceTextBytes, 32U);
}

}  // namespace
}  // namespace aquila::core
```

- [ ] **Step 2: Run the test and verify it fails before implementation**

Run:

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R order_gateway_shm_types --output-on-failure
```

Expected: build fails because `core/trading/order_gateway_shm_types.h` does not exist.

- [ ] **Step 3: Add the shared type header**

Implement the constants, enums, `OrderGatewayCommand`, `OrderGatewayEvent`, queue descriptor, and header exactly from `docs/gate_order_gateway_shm_design.md`. Keep comments in English. Include `core/common/types.h` and `core/trading/order_types.h`.

- [ ] **Step 4: Register and run the test**

Add the test to the existing CMake test target pattern used by nearby `test/core/trading/*` tests.

Run:

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R order_gateway_shm_types --output-on-failure
```

Expected: test passes.

- [ ] **Step 5: Commit**

```bash
git add core/trading/order_gateway_shm_types.h test/core/trading/order_gateway_shm_types_test.cpp
git commit -m "feat: add order gateway shm types"
```

## Task 2: SHM Queue Runtime

**Files:**
- Create: `core/trading/order_gateway_shm.h`
- Test: `test/core/trading/order_gateway_shm_test.cpp`

- [ ] **Step 1: Write queue lifecycle tests**

Cover:

- create with `route_count=4`, command capacity `4096`, event capacity `8192`
- attach existing SHM by name
- reject `route_count=0`
- reject `route_count=17`
- push/pop one `OrderGatewayCommand`
- push/pop one `OrderGatewayEvent`
- full command queue returns false without overwriting unread data

- [ ] **Step 2: Implement fixed-size SPSC queue helpers**

Use POSIX SHM with one object containing all queues. Use atomic head/tail in each queue header. Align queue headers and slots to cacheline boundaries. Do not allocate per push/pop.

- [ ] **Step 3: Run queue tests**

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R order_gateway_shm --output-on-failure
```

Expected: all SHM queue tests pass.

- [ ] **Step 4: Commit**

```bash
git add core/trading/order_gateway_shm.h test/core/trading/order_gateway_shm_test.cpp
git commit -m "feat: add order gateway shm queues"
```

## Task 3: Order Gateway Config

**Files:**
- Create: `core/config/order_gateway_config.h`
- Create: `core/config/order_gateway_config.cpp`
- Modify: `core/config/strategy_config.h`
- Modify: `core/config/strategy_config.cpp`
- Test: `test/config/order_gateway_config_test.cpp`
- Test: `test/config/strategy_config_test.cpp`

- [ ] **Step 1: Write parser tests**

Test this TOML:

```toml
[order_gateway]
name = "gate_order_gateway_test"
route_count = 4
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30

[[order_gateway.routes]]
name = "route0"
order_session_config = "config/order_sessions/gate_order_session.toml"
worker_cpu_id = 4
```

Assert:

- `route_count == 4`
- `startup_ready_timeout_s == 30`
- route count must match `routes.size()`
- route count must be in `[1, 16]`
- capacities must be positive
- timeout must be positive

- [ ] **Step 2: Add strategy config mutual exclusion tests**

Add tests for:

- `[strategy.order_gateway] config = "..."`
- legacy `[strategy.order_session] config = "..."`
- both sections present fails
- live-orders runtime creation fails if neither section is present; non-order / validation-only modes keep existing behavior

- [ ] **Step 3: Implement parser**

Follow existing parser style in `core/config/strategy_config.cpp` and `exchange/gate/trading/order_session_config.cpp`. Resolve relative paths from the owning config file.

- [ ] **Step 4: Run config tests**

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R '(order_gateway_config|strategy_config)' --output-on-failure
```

Expected: config tests pass.

- [ ] **Step 5: Commit**

```bash
git add core/config/order_gateway_config.* core/config/strategy_config.* test/config/order_gateway_config_test.cpp test/config/strategy_config_test.cpp
git commit -m "feat: add order gateway config"
```

## Task 4: Worker Process

**Files:**
- Create: `exchange/gate/trading/order_gateway_worker.h`
- Create: `tools/gate/gate_order_gateway.cpp`
- Modify: relevant CMake files for the new tool
- Test: `test/exchange/gate/trading/order_gateway_worker_test.cpp`

- [ ] **Step 1: Write worker tests with fake session**

Fake session must expose:

```cpp
OrderSendResult PlaceOrder(const core::StrategyOrder& order) noexcept;
OrderSendResult CancelOrder(const core::StrategyOrder& order) noexcept;
void CacheExchangeOrderId(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept;
void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept;
bool Ready() const noexcept;
```

Tests:

- `kPlace` command converts payload to `StrategyOrder` and calls fake session once.
- wrong `route_id` emits `kCommandRejected`.
- fake session send failure emits `kCommandRejected`.
- fake session send ok emits no immediate order response; real Ack comes from response handler.
- login ready callback emits `kReady`; login not-ready emits `kNotReady`.

- [ ] **Step 2: Implement worker command consumer**

Worker loop:

```text
poll command_queue[i]
dispatch kPlace / kCancel / kCacheExchangeOrderId / kForgetExchangeOrderId / kStop
call only the worker-owned OrderSession
push event_queue[i] for local rejects and state events
```

- [ ] **Step 3: Add `gate_order_gateway` tool**

The tool must:

- load order gateway config
- create SHM
- load each route order session config
- start one worker thread per route
- keep running while workers reconnect
- stop workers on signal / process shutdown

- [ ] **Step 4: Run worker tests**

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R order_gateway_worker --output-on-failure
```

Expected: worker tests pass.

- [ ] **Step 5: Commit**

```bash
git add exchange/gate/trading/order_gateway_worker.h tools/gate/gate_order_gateway.cpp test/exchange/gate/trading/order_gateway_worker_test.cpp
git commit -m "feat: add gate order gateway worker"
```

## Task 5: Strategy-Side Gateway Client

**Files:**
- Create: `core/trading/order_gateway_client.h`
- Test: `test/core/trading/order_gateway_client_test.cpp`

- [ ] **Step 1: Write client tests**

Tests:

- attach existing SHM and initialize `route_ready` false.
- drain `kReady` increments ready count only once.
- duplicate `kReady` does not over-count.
- `kNotReady` decrements ready count only on true-to-false transition.
- startup wait succeeds when all N ready events arrive.
- startup wait fails after configured timeout.
- place to ready route writes command and route table.
- place to not-ready route skips route and reports degraded fanout.
- command queue full returns session rejected.
- event `kCommandRejected` converts to `core::OrderResponseKind::kRejected`.

- [ ] **Step 2: Implement client gateway contract**

Implement:

```cpp
OrderSendResult PlaceOrder(const core::StrategyOrder& order) noexcept;
OrderSendResult CancelOrder(const core::StrategyOrder& order) noexcept;
void CacheExchangeOrderId(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept;
void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept;
std::uint64_t PollOrderResponses(RuntimeT& runtime) noexcept;
bool Ready() const noexcept;
```

`PlaceOrder()` returning ok means command queue accepted the command, not socket write complete.

- [ ] **Step 3: Run client tests**

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R order_gateway_client --output-on-failure
```

Expected: client tests pass.

- [ ] **Step 4: Commit**

```bash
git add core/trading/order_gateway_client.h test/core/trading/order_gateway_client_test.cpp
git commit -m "feat: add order gateway client"
```

## Task 6: LeadLag Fanout Config and Submission

**Files:**
- Modify: `strategy/lead_lag/config.h`
- Modify: `strategy/lead_lag/config.cpp`
- Modify: `strategy/lead_lag/execution_state.h`
- Modify: `strategy/lead_lag/strategy.h`
- Test: `test/strategy/lead_lag_config_test.cpp`
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [ ] **Step 1: Add failing config tests**

Add `order_session_fanout = 4` under `[lead_lag.pairs.execute]` and assert:

```cpp
EXPECT_EQ(config.pairs[0].execute.order_session_fanout, 4U);
```

Also test default `1` and reject `0`.

- [ ] **Step 2: Add fanout strategy tests**

Using fake order session / fake gateway:

- open signal with fanout 4 places 4 orders.
- all 4 child orders share `parent_id`.
- all 4 child orders have unique `local_order_id`.
- routes are `0, 1, 2, 3`.
- one signal consumes one `parallel` slot.
- partial fills across two child orders aggregate into one execution group quantity.
- close signal places reduce-only child orders with quantity equal to aggregate position quantity.

- [ ] **Step 3: Implement config field**

Add:

```cpp
std::uint32_t order_session_fanout{1};
```

Parse with positive integer semantics and default `1`.

- [ ] **Step 4: Implement fanout submission**

For each external signal:

```text
prepare price / quantity text once
allocate stable parent_id for this fanout batch
for ready route 0..N-1 until requested fanout reached:
  set gateway_route_id = route
  submit child order
  child local_order_id stays unique
  child parent_id uses the batch parent_id
```

The batch `parent_id` can be the first preallocated child `local_order_id`, but it must not depend on the first child that is accepted
by a queue or by the exchange. Do not change `LocalOrderIdCodec`.

- [ ] **Step 5: Update execution state**

Change execution state from a single pending `local_order_id` per group to a bounded pending child set keyed by `parent_id`. Keep scans bounded by `execute.parallel * order_session_fanout`.

- [ ] **Step 6: Run LeadLag tests**

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R '(lead_lag_config|lead_lag_strategy|lead_lag_feedback_state)' --output-on-failure
```

Expected: LeadLag tests pass.

- [ ] **Step 7: Commit**

```bash
git add strategy/lead_lag/config.* strategy/lead_lag/execution_state.h strategy/lead_lag/strategy.h test/strategy/lead_lag_config_test.cpp test/strategy/lead_lag_strategy_interface_test.cpp
git commit -m "feat: add leadlag order session fanout"
```

## Task 7: Live Strategy Integration

**Files:**
- Modify: `tools/lead_lag/live_strategy.cpp`
- Modify: `tools/lead_lag/live_strategy.h` if stats need gateway state
- Test: `test/tools/lead_lag/live_strategy_test.cpp`

- [ ] **Step 1: Add live config mode tests**

Tests:

- single-session config still uses existing `OrderSessionRuntimeAdapter`.
- order-gateway config attaches `OrderGatewayClient`.
- both single session and order gateway config fail fast.
- startup timeout is surfaced as runtime create / startup failure.

- [ ] **Step 2: Implement runtime selection**

In live orders mode:

```text
if strategy.order_gateway.config_path is set:
  load order_gateway config
  attach OrderGatewayClient
  build TradingRuntime<LiveOrdersStrategy, OrderGatewayClient, DataReader>
else:
  use existing single OrderSessionRuntimeAdapter runtime
```

Smoke modes can stay single-session until explicitly migrated.

- [ ] **Step 3: Run tool tests**

```bash
./build/debug/build_tests.sh
ctest --test-dir build/debug -R live_strategy --output-on-failure
```

Expected: live strategy tests pass.

- [ ] **Step 4: Commit**

```bash
git add tools/lead_lag/live_strategy.* test/tools/lead_lag/live_strategy_test.cpp
git commit -m "feat: wire leadlag live order gateway"
```

## Task 8: Configs, Docs, and Smoke Validation

**Files:**
- Create: `config/order_gateways/gate_order_gateway_30symbols_private_plain_20260627.toml`
- Create: `config/strategies/lead_lag_30symbols_fusion_order_gateway_live_strategy_20260627.toml`
- Modify: `docs/gate_order_gateway_shm_design.md`
- Modify: `docs/agent-handoff-gate-trade-architecture.md`
- Modify: `docs/strategy_order_component_model.md`
- Modify: `docs/project_onboarding_guide.md`

- [ ] **Step 1: Add 30-symbol gateway config**

Use `route_count = 4`, capacities `4096 / 8192`, and `startup_ready_timeout_s = 30`. Point all four routes at the existing 30-symbol private plain order session config first; later experiments can split per-route config files if IP / CPU differs.

- [ ] **Step 2: Update strategy live config**

Create a new multi-session live strategy config variant that replaces `[strategy.order_session]` with `[strategy.order_gateway]`.
Keep legacy single-session configs unchanged.

- [ ] **Step 3: Validate config-only run**

```bash
./build/debug/tools/lead_lag_strategy \
  --config config/strategies/lead_lag_30symbols_fusion_order_gateway_live_strategy_20260627.toml \
  --duration-sec 0
```

Expected: config validates and exits without starting real orders.

- [ ] **Step 4: Run full targeted tests**

```bash
ctest --test-dir build/debug -R '(order_gateway|lead_lag_config|lead_lag_strategy|live_strategy|strategy_config)' --output-on-failure
```

Expected: all targeted tests pass.

- [ ] **Step 5: Run diff checks**

```bash
git diff --check
```

Expected: no whitespace errors.

- [ ] **Step 6: Commit**

```bash
git add config/order_gateways config/strategies/lead_lag_30symbols_fusion_order_gateway_live_strategy_20260627.toml docs/gate_order_gateway_shm_design.md docs/agent-handoff-gate-trade-architecture.md docs/strategy_order_component_model.md docs/project_onboarding_guide.md
git commit -m "docs: document gate order gateway shm design"
```

## Self-Review Checklist

- The plan preserves unique `local_order_id` per child order.
- The plan keeps feedback on the existing account-level feedback SHM path.
- The plan uses runtime `route_count` with compile-time max 16.
- The plan uses `command_queue` / `event_queue` naming, not up/down/ring.
- The plan keeps write-path diagnostics out of the main event payload.
- The plan defines startup all-route-ready wait with `startup_ready_timeout_s = 30`.
- The plan does not require changing `LocalOrderIdCodec`.
- Every task has concrete files, tests, commands, and commit boundaries.
