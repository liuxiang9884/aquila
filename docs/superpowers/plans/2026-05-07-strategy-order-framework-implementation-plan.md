# Strategy Order Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现第一版交易所无关 Strategy 订单框架，并把 Strategy 订单对象直接交给 `aquila::gate::OrderSession` 的 place/cancel 接口。

**Architecture:** 本计划记录原第一版边界，已被 `docs/superpowers/plans/2026-05-07-order-session-struct-flow-implementation-plan.md` 调整。当前实现是 Strategy 负责风控、订单对象、订单状态和交易所无关执行流程，直接把订单 struct 交给 Gate `OrderSession`；Gate `OrderSession` 在发送路径完成 JSON 序列化，不再使用 Gate adapter 缓存 wire fields。

**Tech Stack:** C++20, CMake, GoogleTest, benchmark, Abseil flat_hash_map, fmtlib header-only, existing `aquila::gate::OrderSession`.

---

## 执行规则

- 所有实现任务使用 subagent-driven 模式时，`spawn_agent` 必须显式设置 `reasoning_effort = "xhigh"`。
- 子 agent 不允许再派生下级 subagent。
- 每个任务只修改自己的文件集合；如果发现用户或其他 agent 已修改同一文件，先读现状再做最小增量修改，不回滚他人改动。
- 每个任务按 TDD 执行：先让目标测试失败，再补实现，再跑目标测试。
- 每个任务完成后先由主会话审查，再进入下一任务；最终统一跑 debug / release 对应验证并自动提交。

## 当前起点

历史起点曾包含 Strategy 测试草稿；当前实现已被后续 struct-flow plan 调整：

- `test/core/common/order_pool_test.cpp`
- `test/strategy/strategy_test.cpp`
- `test/strategy/CMakeLists.txt`
- `test/CMakeLists.txt`

早期 RED 命令曾使用 `strategy_order_store_test` / `strategy_gate_order_gateway_test`；当前验证入口见本文末尾 final verification。

当前实现不再保留 `strategy/order_store.h`、`strategy/order_pool.h`、`exchange/gate/trading/gate_order_gateway.h` 或 Strategy 侧 Gate wire cache。

## 文件结构

- Create `strategy/CMakeLists.txt`: 定义 header-only `aquila_strategy` target。
- Create `strategy/order_types.h`: Strategy 订单侧基础枚举、薄 `OrderCreateRequest`、`StrategyOrder`、send/create/cancel/result event 类型。
- Create `core/common/order_pool.h`: 通用固定容量订单池，维护 `local_order_id -> slot` 索引；不维护 exchange order id 索引。
- Create `strategy/strategy.h`: 模板化 `Strategy<GatewayT>`，提供 create/submit/cancel/response apply 的交易所无关执行流程。
- Modify `CMakeLists.txt`: 加入 `add_subdirectory(strategy)`。
- Modify `test/CMakeLists.txt`: 加入 `add_subdirectory(strategy)`。
- Create or keep `test/strategy/CMakeLists.txt`: 增加 Strategy gtest target。
- Create or keep `test/core/common/order_pool_test.cpp`: 验证本地订单 ID 分配、容量限制、slot 复用、指针稳定和 zero capacity。
- Create or keep `test/strategy/strategy_test.cpp`: 验证 create/submit/cancel/response 状态推进。
- Create `benchmark/strategy/order_gateway_benchmark.cpp`: 最小 benchmark，测 Strategy direct-send fake order session 调用成本。
- Modify `benchmark/CMakeLists.txt`: 加入 `add_subdirectory(strategy)`。
- Create `benchmark/strategy/CMakeLists.txt`: 增加 `strategy_order_gateway_benchmark`。
- Modify `doc/project_onboarding_guide.md`: 更新最近已完成、代码入口、验证命令和下一步建议。
- Modify `doc/agent-handoff-gate-trade-architecture.md`: 记录 Strategy / Gate `OrderSession` 直接 struct flow 第一版边界和未覆盖项。

## 第一版 API 约束

`strategy/order_types.h` 第一版保留薄对象和固定语义：

```cpp
namespace aquila::strategy {

enum class OrderSide : std::uint8_t { kBuy, kSell };
enum class OrderType : std::uint8_t { kLimit, kMarket };
enum class TimeInForce : std::uint8_t { kGoodTillCancel, kImmediateOrCancel };

enum class OrderStatus : std::uint8_t {
  kCreated,
  kSent,
  kAccepted,
  kPartialFilled,
  kFilled,
  kCancelSent,
  kCancelled,
  kRejected,
};

struct OrderCreateRequest {
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::int64_t quantity{0};
  std::string_view price_text{};
  bool reduce_only{false};
};

struct StrategyOrder {
  std::int64_t local_order_id{0};
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::int64_t quantity{0};
  std::string_view price_text{};
  bool reduce_only{false};
  OrderStatus status{OrderStatus::kCreated};
  std::uint64_t error_label_hash{0};
};

}  // namespace aquila::strategy
```

当前约束：

- `OrderCreateRequest::symbol` 和 `price_text` 生命周期由 Strategy / symbol metadata 设计保证；当前第一版不在 Strategy 中缓存 Gate wire fields。
- Gate `OrderSession` 接收订单 struct，并在发送路径读取 `symbol`、`side`、`quantity`、`price_text`、`time_in_force`、`reduce_only` 完成编码。
- Strategy 不维护 exchange order id 索引；Gate `OrderSession` 在内部缓存 local/exchange order id 供 cancel 编码使用。

---

### Task 1: Strategy Core Order Types And Store

**Files:**
- Create: `strategy/CMakeLists.txt`
- Create: `strategy/order_types.h`
- Create: `core/common/order_pool.h`
- Modify: `CMakeLists.txt`
- Keep or modify: `test/core/common/order_pool_test.cpp`
- Keep or modify: `test/strategy/CMakeLists.txt`
- Keep or modify: `test/CMakeLists.txt`

- [x] **Step 1: 确认 RED**

Run:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target core_order_pool_test -j8
```

Expected: compile fails before `core/common/order_pool.h` exists.

- [x] **Step 2: 实现 CMake target**

Create `strategy/CMakeLists.txt`:

```cmake
add_library(aquila_strategy INTERFACE)

target_include_directories(aquila_strategy
    INTERFACE
        ${PROJECT_SOURCE_DIR}
)

target_link_libraries(aquila_strategy
    INTERFACE
        aquila_core
        absl::flat_hash_map
)

add_library(aquila_strategy_gate INTERFACE)

target_link_libraries(aquila_strategy_gate
    INTERFACE
        aquila_strategy
        aquila_gate
)
```

Modify root `CMakeLists.txt`:

```cmake
add_subdirectory(strategy)
```

Place it after `add_subdirectory(exchange)` and before `add_subdirectory(tools)`.

- [x] **Step 3: 实现订单基础类型**

Create `strategy/order_types.h` using the API constraints above. Add result enums and structs:

```cpp
enum class GatewaySendStatus : std::uint8_t { kOk, kRejected };
enum class OrderCreateStatus : std::uint8_t {
  kOk,
  kInvalidOrder,
  kStoreFull,
  kGatewayRejected,
};
enum class OrderSubmitStatus : std::uint8_t {
  kOk,
  kOrderNotFound,
  kInvalidStatus,
  kGatewayRejected,
};
enum class OrderCancelStatus : std::uint8_t {
  kOk,
  kOrderNotFound,
  kInvalidStatus,
  kGatewayRejected,
};
enum class OrderResponseKind : std::uint8_t {
  kAck,
  kAccepted,
  kRejected,
  kCancelAccepted,
  kCancelRejected,
};

struct GatewaySendResult {
  GatewaySendStatus status{GatewaySendStatus::kRejected};
};
struct OrderCreateResult {
  OrderCreateStatus status{OrderCreateStatus::kInvalidOrder};
  std::int64_t local_order_id{0};
};
struct OrderSubmitResult {
  OrderSubmitStatus status{OrderSubmitStatus::kOrderNotFound};
  std::int64_t local_order_id{0};
};
struct OrderCancelResult {
  OrderCancelStatus status{OrderCancelStatus::kOrderNotFound};
  std::int64_t local_order_id{0};
};
struct OrderResponseEvent {
  OrderResponseKind kind{OrderResponseKind::kAck};
  std::int64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t error_label_hash{0};
};
```

- [x] **Step 4: 实现固定容量订单存储**

Create `core/common/order_pool.h`:

- `OrderPool<OrderT>(std::size_t max_live_orders)` 在构造期固定 resize `max_live_orders * 2` 个 slot，并 reserve `max_live_orders * 4` 的 `absl::flat_hash_map`。
- `Create()` 从 free list 取 slot，默认重置 order，分配递增 `local_order_id`，容量满或 free list 为空时返回 `nullptr`。
- `Find(local_order_id)` 返回订单指针，找不到返回 `nullptr`。
- `Erase(local_order_id)` 删除 local id 索引、重置 order，并把 slot 放回 free list。
- `size()`、`capacity()` 和 `slot_capacity()` 返回当前 live 数、最大 live 容量和内部 slot 数。
- 不实现 `BindExchangeOrderId()` / `FindByExchangeOrderId()`；Strategy 不维护 exchange order id 索引。

- [x] **Step 5: 运行 Task 1 验证**

Run:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target core_order_pool_test -j8
./build/debug/test/core/common/core_order_pool_test
```

Expected: all `OrderPoolTest` tests pass.

### Task 2: Exchange-Neutral Strategy State Machine

**Files:**
- Create: `strategy/strategy.h`
- Keep or modify: `test/strategy/strategy_test.cpp`

- [x] **Step 1: 确认 RED**

Run:

```bash
cmake --build build/debug --target strategy_test -j8
```

Expected: compile fails because `strategy/strategy.h` does not exist or required methods are missing.

- [x] **Step 2: 实现 `Strategy<GatewayT>`**

Create `strategy/strategy.h`:

- Template parameter `GatewayT` must expose `PlaceOrder(Order&)` and `CancelOrder(Order&)`。
- Constructor stores `GatewayT& gateway` and `OrderPool<Order> orders`。
- `PlaceLimitOrder(OrderCreateRequest request)` validates `request.symbol` non-empty, `request.price_text` non-empty and `request.quantity > 0` before allocation。
- Valid request allocates an order, copies exchange-neutral fields, calls `gateway.PlaceOrder()` once and changes status to `kSent` on local send ok。
- `CancelOrder(local_order_id)` only accepts `kSent`、`kAccepted` 或 `kPartialFilled`; gateway send ok changes status to `kCancelSent`。
- `OnOrderResponse(event)` updates status from accepted / cancel accepted responses without building an exchange order id index。
- `FindOrder()` 和 `order_count()` delegate to `OrderPool`。

- [x] **Step 3: 运行 Task 2 验证**

Run:

```bash
cmake --build build/debug --target strategy_test -j8
./build/debug/test/strategy/strategy_test
```

Expected: all `StrategyTest` tests pass.

### Task 3: Gate Direct Struct Flow

**Files:**
- Modify: `exchange/gate/trading/order_session.h`
- Modify: `test/exchange/gate/trading/order_session_test.cpp`

- [x] **Step 1: 确认 RED**

Run:

```bash
cmake --build build/debug --target strategy_test gate_order_session_test -j8
```

Expected: compile fails while `Strategy` / `OrderSession` still expect old prepared request / cached wire fields.

- [x] **Step 2: 实现 Gate direct struct send**

- `OrderSession::PlaceOrder(const OrderT&)` 读取订单 struct 并现场编码 Gate place JSON。
- `OrderSession::CancelOrder(const OrderT&)` 优先使用内部 capped `local_order_id -> exchange_order_id` cache；没有缓存时 fallback 到 `text="t-<local_order_id>"`。
- place final result 在 `OrderSession` 内缓存 exchange order id，cancel accepted / disconnect / 显式 forget 时清理。
- `OrderResponse` 仍透传 `exchange_order_id`，但 Strategy 不建立 exchange id 索引。

- [x] **Step 3: 运行 Task 3 验证**

Run:

```bash
cmake --build build/debug --target strategy_test gate_order_session_test -j8
./build/debug/test/strategy/strategy_test
./build/debug/test/exchange/gate/trading/gate_order_session_test
```

Expected: Strategy 和 Gate `OrderSession` tests pass.

### Task 4: Strategy To Gate OrderSession Path Benchmark

**Files:**
- Create: `benchmark/strategy/CMakeLists.txt`
- Create: `benchmark/strategy/order_gateway_benchmark.cpp`
- Modify: `benchmark/CMakeLists.txt`

- [x] **Step 1: 写 benchmark**

Create `benchmark/strategy/order_gateway_benchmark.cpp` with a fake order session that records the order struct and returns local OK。

Benchmark cases:

- `BM_StrategyPlaceLimitOrder`: creates and sends limit orders through the fake session。
- `BM_StrategyCancelAcceptedOrder`: creates an accepted order and measures Strategy cancel submission through the fake session。

- [x] **Step 2: 接入 benchmark target**

Create `benchmark/strategy/CMakeLists.txt` and add root benchmark subdirectory:

```cmake
add_executable(strategy_order_gateway_benchmark
    order_gateway_benchmark.cpp
)

target_link_libraries(strategy_order_gateway_benchmark
    PRIVATE
        aquila_strategy
        benchmark::benchmark_main
)
```

- [x] **Step 3: 运行 benchmark smoke**

Run:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target strategy_order_gateway_benchmark -j8
./build/release/benchmark/strategy/strategy_order_gateway_benchmark --benchmark_filter='BM_StrategyPlaceLimitOrder|BM_StrategyCancelAcceptedOrder' --benchmark_min_time=0.01s
```

Expected: benchmark executable runs all three benchmark cases without failures. Do not claim latency improvement from these numbers; they are first-version baseline evidence only.

### Task 5: Documentation And Onboarding Sync

**Files:**
- Modify: `doc/project_onboarding_guide.md`
- Modify: `doc/agent-handoff-gate-trade-architecture.md`

- [x] **Step 1: 更新 onboarding**

Update `doc/project_onboarding_guide.md`:

- “最近已完成”增加 Strategy 第一版订单框架、Gate direct struct flow、tests 和 benchmark。
- “文档索引”增加 this plan。
- “代码入口”新增 `Strategy 订单框架` 小节，列出 `core/common/order_pool.h`、`strategy/order_types.h`、`strategy/strategy.h`、Strategy tests 和 benchmark。
- “验证命令”增加 strategy gtest 和 benchmark smoke 命令。
- “下一步建议”增加私有 feedback session、REST reconcile、symbol metadata/risk check 接入和端到端 live smoke 的顺序。

- [x] **Step 2: 更新 Gate handoff**

Update `doc/agent-handoff-gate-trade-architecture.md`:

- 记录 Strategy 做订单对象和状态，Gate adapter 缓存 wire fields，Gate `OrderSession` 只做协议发送和轻量 response。
- 明确未覆盖项：private feedback、REST reconcile、batch/amend、真实成交回报状态合并。
- 增加 Strategy tests / benchmark 验证命令。

- [x] **Step 3: 文档检查**

Run:

```bash
git diff --check
```

Expected: no whitespace errors.

## 全量验证

所有任务完成后运行：

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target core_order_pool_test strategy_test -j8
ctest --test-dir build/debug -R 'strategy|gate_(order|submit)' --output-on-failure

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target core_order_pool_benchmark strategy_order_gateway_benchmark -j8
./build/release/benchmark/core/common/core_order_pool_benchmark --benchmark_min_time=0.01s
./build/release/benchmark/strategy/strategy_order_gateway_benchmark --benchmark_filter='BM_StrategyPlaceLimitOrder|BM_StrategyCancelAcceptedOrder' --benchmark_min_time=0.01s

git diff --check
```

如果 `evaluation/` 边界没有变更，不需要运行 evaluation 边界检查。若任何任务意外修改 `evaluation/`、`core/`、`exchange/` 或 `tools/` 的 evaluation 依赖边界，则额外运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: both commands have no matches.

## 提交策略

- 计划文档单独提交：`doc: add strategy order framework plan`。
- Task 1-3 可合并为一个代码提交：`feat: add strategy order framework`。
- Task 4 benchmark 单独提交：`bench: add strategy order gateway benchmark`。
- Task 5 文档同步单独提交：`doc: sync strategy order framework handoff`。

## 非本计划范围

- 私有订单 / 成交 / 仓位回报 `OrderFeedbackSession`。
- REST reconcile 和断线 gap 补偿。
- 策略级完整风控、仓位管理和真实成交合并。
- 多交易所 Binance / OKX 下单 adapter。
- 跨线程 order ring、SPSC feedback ring 和多线程 strategy runtime。
- 真实资金账户 live 下单 smoke；后续需要单独计划，默认 dry-run 或 fake session 验证。
