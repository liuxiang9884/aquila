# Order Session Struct Flow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Strategy 直接创建并保存订单 struct，发单/撤单时把该 struct 交给 Gate `OrderSession`，由 `OrderSession` 在发送路径完成 Gate JSON 序列化。

**Architecture:** Strategy 负责风控、建单、订单池和状态推进；Gate `OrderSession` 负责登录、request correlation、Gate place/cancel 编码和发送。删除 Strategy 侧 Gate wire fields 缓存、`PrepareOrder()` 和两阶段 `CreateLimitOrder()` / `SubmitOrder()` 发单接口。

**Tech Stack:** C++20、CMake、GoogleTest、Google Benchmark、Abseil flat_hash_map、fmt header-only。

---

### Task 1: Strategy API And OrderPool

**Files:**
- Modify: `core/common/types.h`
- Create: `strategy/order_pool.h`
- Delete: `strategy/order_store.h`
- Modify: `strategy/order_types.h`
- Modify: `strategy/strategy.h`
- Move/modify: `test/strategy/order_store_test.cpp` -> `test/strategy/order_pool_test.cpp`
- Modify: `test/strategy/strategy_test.cpp`
- Modify: `test/strategy/CMakeLists.txt`

- [x] **Step 1: Write failing Strategy tests**

Update tests to expect:
- `Strategy::PlaceLimitOrder(request)` creates an order, stores it in `OrderPool`, calls `order_session.PlaceOrder(order)` immediately, and marks status `kSent` on local send success.
- invalid request is rejected before allocation and before session send.
- session send failure stores the attempted order as `kRejected`.
- no test calls `PrepareOrder()` or `SubmitOrder()`.

- [x] **Step 2: Verify RED**

Run:

```bash
cmake --build build/debug --target strategy_test strategy_order_pool_test -j8
```

Expected: compile fails because current `Strategy` still expects `PrepareOrder()` and `SubmitOrder()`, and `strategy/order_pool.h` does not exist.

- [x] **Step 3: Implement Strategy/OrderPool**

Implementation requirements:
- Move generic order enums (`OrderSide`、`OrderType`、`TimeInForce`) to `core/common/types.h` so Gate code can use them without depending on `strategy`.
- Replace `OrderDraft` with `OrderCreateRequest`.
- Add `symbol` and `price_text` fields to `StrategyOrder`; do not add Gate wire buffer/cache fields.
- Replace `OrderStore` with simple `OrderPool` backed by a reserved vector. Local lookup uses monotonic `local_order_id` as vector index; exchange id lookup can be a linear scan in v1.
- `Strategy::PlaceLimitOrder()` privately creates the order, sends it directly through `order_session_.PlaceOrder(*order)`, and returns the local id.
- `Strategy::CancelOrder()` sends the stored order directly through `order_session_.CancelOrder(*order)`.

- [x] **Step 4: Verify GREEN**

Run:

```bash
cmake --build build/debug --target strategy_test strategy_order_pool_test -j8
./build/debug/test/strategy/strategy_test
./build/debug/test/strategy/strategy_order_pool_test
```

Expected: all Strategy tests pass.

### Task 2: Gate OrderSession Encodes From Order Struct

**Files:**
- Modify: `exchange/gate/trading/order_types.h`
- Modify: `exchange/gate/trading/order_request_encoder.h`
- Modify: `exchange/gate/trading/order_session.h`
- Delete: `exchange/gate/trading/gate_order_gateway.h`
- Modify: `test/exchange/gate/trading/order_request_encoder_test.cpp`
- Modify: `test/exchange/gate/trading/order_session_test.cpp`
- Delete: `test/strategy/gate_order_gateway_test.cpp`
- Modify: `test/strategy/CMakeLists.txt`

- [x] **Step 1: Write failing Gate tests**

Update tests to expect:
- `EncodePlaceOrderRequest()` receives business order fields (`local_order_id`、`contract`、`signed_size`、`price_text`、`time_in_force`、`reduce_only`) and creates Gate `text`/`tif` inside the encoder.
- `OrderSession::PlaceOrder(order)` accepts an order struct directly; no `PlaceOrderRequest` / `OrderWireFields` is passed from Strategy.
- invalid local order id is not prechecked by `OrderSession`; if it fails, it fails through encoder status.

- [x] **Step 2: Verify RED**

Run:

```bash
cmake --build build/debug --target gate_order_request_encoder_test gate_order_session_test -j8
```

Expected: compile fails because current `OrderSession::PlaceOrder()` still expects `PlaceOrderRequest`.

- [x] **Step 3: Implement Gate direct struct send**

Implementation requirements:
- Remove `OrderWireFields` and `PlaceOrderRequest`.
- `PlaceOrderEncodeFields` owns the fields needed for Gate JSON serialization and computes `text = t-<local_order_id>` and `tif` in `EncodePlaceOrderRequest()`.
- `OrderSession::PlaceOrder(const OrderT& order)` is templated and reads `order.local_order_id`、`order.symbol`、`order.side`、`order.quantity`、`order.price_text`、`order.time_in_force`、`order.reduce_only`；Gate signed size is derived inside OrderSession from `side + quantity`.
- `OrderSession::CancelOrder(const OrderT& order)` reads `order.local_order_id` and `order.exchange_order_id`.
- Keep session-owned checks only: active, logged-in, request map capacity, send/encode failure. Do not add extra order semantic validation.

- [x] **Step 4: Verify GREEN**

Run:

```bash
cmake --build build/debug --target gate_order_request_encoder_test gate_order_session_test -j8
./build/debug/test/exchange/gate/trading/gate_order_request_encoder_test
./build/debug/test/exchange/gate/trading/gate_order_session_test
```

Expected: all Gate order session tests pass.

### Task 3: Benchmarks And Docs

**Files:**
- Modify: `benchmark/strategy/order_gateway_benchmark.cpp`
- Modify: `benchmark/exchange/gate/trading/order_session_benchmark.cpp`
- Modify: `doc/project_onboarding_guide.md`
- Modify: `doc/agent-handoff-gate-trade-architecture.md`
- Modify: `docs/superpowers/plans/2026-05-07-strategy-order-framework-implementation-plan.md`

- [x] **Step 1: Update benchmarks**

Benchmark Strategy direct send and Gate order session direct struct encode. Remove cached-wire benchmark names.

- [x] **Step 2: Update docs**

Document the new boundary:
- Strategy creates/stores order struct and does risk/state.
- OrderSession serializes order struct on send.
- No Strategy-side Gate wire cache and no `PrepareOrder()`.

- [x] **Step 3: Final verification**

Run:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target strategy_order_pool_test strategy_test gate_order_request_encoder_test gate_order_session_test -j8
./build/debug/test/strategy/strategy_order_pool_test
./build/debug/test/strategy/strategy_test
./build/debug/test/exchange/gate/trading/gate_order_request_encoder_test
./build/debug/test/exchange/gate/trading/gate_order_session_test
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target strategy_order_gateway_benchmark gate_order_session_benchmark -j8
./build/release/benchmark/strategy/strategy_order_gateway_benchmark --benchmark_filter='BM_StrategyPlaceLimitOrder|BM_StrategyCancelAcceptedOrder' --benchmark_min_time=0.01s
./build/release/benchmark/exchange/gate/trading/gate_order_session_benchmark --benchmark_filter='BM_EncodePlaceOrder|BM_EncodeCancelOrder' --benchmark_min_time=0.01s
git diff --check
```

Expected: all commands pass.
