# Strategy Order Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现第一版交易所无关 Strategy 订单框架，并通过 Gate adapter 把 Strategy 订单对象、Gate wire fields 缓存和 `aquila::gate::OrderSession` 的 place/cancel 接口连起来。

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

当前工作区已有未提交的 Strategy 测试草稿：

- `test/strategy/order_store_test.cpp`
- `test/strategy/strategy_test.cpp`
- `test/strategy/gate_order_gateway_test.cpp`
- `test/strategy/CMakeLists.txt`
- `test/CMakeLists.txt`

这些测试已经形成第一版 API 形状。主会话已运行：

```bash
cmake --build build/debug --target strategy_order_store_test strategy_test strategy_gate_order_gateway_test -j8
```

当前 RED 结果是编译失败，原因是 `strategy/order_store.h` 不存在。后续任务可以保留这些测试，也可以在不降低覆盖面的前提下微调命名和断言。

## 文件结构

- Create `strategy/CMakeLists.txt`: 定义 header-only `aquila_strategy` 和 `aquila_strategy_gate` target。
- Create `strategy/order_types.h`: Strategy 订单侧基础枚举、薄 `OrderDraft`、`StrategyOrder`、send/create/submit/cancel/result event 类型。
- Create `strategy/order_store.h`: 固定容量订单存储，维护 `local_order_id -> order` 和 `exchange_order_id -> local_order_id` 两个索引。
- Create `strategy/strategy.h`: 模板化 `Strategy<GatewayT>`，提供 create/submit/cancel/response apply 的交易所无关执行流程。
- Create `exchange/gate/trading/gate_order_gateway.h`: `GateStrategyOrder`、Gate wire fields 缓存、Gate TIF/文本编码、Gate `OrderResponse` 到 Strategy event 映射。
- Modify `CMakeLists.txt`: 加入 `add_subdirectory(strategy)`。
- Modify `test/CMakeLists.txt`: 加入 `add_subdirectory(strategy)`。
- Create or keep `test/strategy/CMakeLists.txt`: 增加 Strategy 三个 gtest target。
- Create or keep `test/strategy/order_store_test.cpp`: 验证本地订单 ID 分配、容量限制和 exchange order id 绑定。
- Create or keep `test/strategy/strategy_test.cpp`: 验证 create/submit/cancel/response 状态推进。
- Create or keep `test/strategy/gate_order_gateway_test.cpp`: 验证 Gate wire fields 缓存、place/cancel request 传递和 response mapping。
- Create `benchmark/strategy/order_gateway_benchmark.cpp`: 最小 benchmark，测 Strategy cached Gate wire fields 的 submit/cancel adapter 调用成本。
- Modify `benchmark/CMakeLists.txt`: 加入 `add_subdirectory(strategy)`。
- Create `benchmark/strategy/CMakeLists.txt`: 增加 `strategy_order_gateway_benchmark`。
- Modify `doc/project_onboarding_guide.md`: 更新最近已完成、代码入口、验证命令和下一步建议。
- Modify `doc/agent-handoff-gate-trade-architecture.md`: 记录 Strategy / Gate adapter 第一版边界和未覆盖项。

## 第一版 API 约束

`strategy/order_types.h` 第一版保留薄对象和固定语义：

```cpp
namespace aquila::strategy {

enum class OrderSide : std::uint8_t { kBuy, kSell };
enum class OrderType : std::uint8_t { kLimit, kMarket };
enum class TimeInForce : std::uint8_t { kGoodTillCancel, kImmediateOrCancel };

enum class OrderStatus : std::uint8_t {
  kCreated,
  kSubmitted,
  kAcked,
  kAccepted,
  kRejected,
  kCancelSubmitted,
  kCancelAccepted,
  kCancelRejected,
};

struct OrderDraft {
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
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
  std::uint64_t exchange_order_id{0};
  bool reduce_only{false};
  OrderStatus status{OrderStatus::kCreated};
  std::uint64_t error_label_hash{0};
};

}  // namespace aquila::strategy
```

`exchange/gate/trading/gate_order_gateway.h` 第一版让 Strategy 订单对象缓存 Gate wire fields：

```cpp
struct GateOrderCache {
  std::array<char, 64> contract_buffer{};
  std::array<char, 32> price_buffer{};
  std::array<char, 8> tif_buffer{};
  std::array<char, 32> text_buffer{};
  gate::OrderWireFields wire{};
};

struct GateStrategyOrder : StrategyOrder {
  GateOrderCache gate{};
};
```

缓存约束：

- `OrderDraft::symbol` 和 `price_text` 在 `PrepareOrder()` 阶段拷贝进固定数组，避免 submit 热路径依赖外部 `string_view` 生命周期。
- `gate::OrderWireFields` 中的 `string_view` 必须指向 `GateStrategyOrder::gate` 内部 buffer。
- `OrderSession` 仍只接收 wire-ready request，不理解 side、order type、symbol metadata 或风险逻辑。

---

### Task 1: Strategy Core Order Types And Store

**Files:**
- Create: `strategy/CMakeLists.txt`
- Create: `strategy/order_types.h`
- Create: `strategy/order_store.h`
- Modify: `CMakeLists.txt`
- Keep or modify: `test/strategy/order_store_test.cpp`
- Keep or modify: `test/strategy/CMakeLists.txt`
- Keep or modify: `test/CMakeLists.txt`

- [x] **Step 1: 确认 RED**

Run:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target strategy_order_store_test -j8
```

Expected: compile fails because `strategy/order_store.h` or `aquila_strategy_gate` does not exist.

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

Create `strategy/order_store.h`:

- `OrderStore<OrderT>(std::size_t capacity)` 在构造期 `reserve()` vector 和两个 `absl::flat_hash_map`。
- `Create()` 分配递增 `local_order_id`，容量满时返回 `nullptr`。
- `Find(local_order_id)` 返回订单指针，找不到返回 `nullptr`。
- `BindExchangeOrderId(local_order_id, exchange_order_id)` 设置订单上的 `exchange_order_id` 并写入 exchange id 索引；`local_order_id <= 0`、`exchange_order_id == 0` 或本地订单不存在时返回 `false`。
- `FindByExchangeOrderId(exchange_order_id)` 先查 exchange 索引，再查本地索引。
- `size()` 和 `capacity()` 返回当前数量和固定容量。

- [x] **Step 5: 运行 Task 1 验证**

Run:

```bash
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug --target strategy_order_store_test -j8
./build/debug/test/strategy/strategy_order_store_test
```

Expected: all `OrderStoreTest` tests pass.

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

- Template parameter `GatewayT` must expose `using Order`, `PrepareOrder(Order&, const OrderDraft&)`, `PlaceOrder(Order&)` and `CancelOrder(Order&)`。
- Constructor stores `GatewayT& gateway` and `OrderStore<typename GatewayT::Order> orders`。
- `CreateLimitOrder(OrderDraft draft)` validates `draft.symbol` non-empty, `draft.price_text` non-empty and `draft.quantity > 0` before allocation。
- Valid draft allocates an order, copies exchange-neutral fields, calls `gateway.PrepareOrder()` once and leaves status at `kCreated`。
- `SubmitOrder(local_order_id)` only accepts `kCreated`; gateway send ok changes status to `kSubmitted`。
- `CancelOrder(local_order_id)` only accepts `kSubmitted`、`kAcked` 或 `kAccepted`; gateway send ok changes status to `kCancelSubmitted`。
- `OnOrderResponse(event)` updates status and binds non-zero exchange order id on accepted / cancel accepted responses。
- `FindOrder()`、`FindOrderByExchangeOrderId()` 和 `order_count()` delegate to `OrderStore`。

- [x] **Step 3: 运行 Task 2 验证**

Run:

```bash
cmake --build build/debug --target strategy_test -j8
./build/debug/test/strategy/strategy_test
```

Expected: all `StrategyTest` tests pass.

### Task 3: Gate Order Gateway Adapter

**Files:**
- Create: `exchange/gate/trading/gate_order_gateway.h`
- Keep or modify: `test/strategy/gate_order_gateway_test.cpp`

- [x] **Step 1: 确认 RED**

Run:

```bash
cmake --build build/debug --target strategy_gate_order_gateway_test -j8
```

Expected: compile fails because `exchange/gate/trading/gate_order_gateway.h` does not exist or required adapter methods are missing.

- [x] **Step 2: 实现 Gate adapter**

Create `exchange/gate/trading/gate_order_gateway.h`:

- `GateOrderCache` owns fixed buffers and `gate::OrderWireFields wire`。
- `GateStrategyOrder : StrategyOrder` owns one `GateOrderCache gate`。
- `GateOrderGateway<OrderSessionT>` stores `OrderSessionT& session` and exposes `using Order = GateStrategyOrder`。
- `PrepareOrder()` copies generic fields into `GateStrategyOrder` and copies `symbol`、`price_text`、TIF token、`OrderTextCodec::Format(local_order_id)` output into owned buffers。
- TIF mapping: `TimeInForce::kGoodTillCancel -> "gtc"`，`TimeInForce::kImmediateOrCancel -> "ioc"`。
- Buffer overflow returns `false` and does not call `OrderSession`。
- `PlaceOrder()` calls `session.PlaceOrder(gate::PlaceOrderRequest{.wire = order.gate.wire})`。
- `CancelOrder()` calls `session.CancelOrder(gate::CancelOrderRequest{.local_order_id = order.local_order_id, .exchange_order_id = order.exchange_order_id})`。
- `ToStrategyOrderResponse(const gate::OrderResponse&)` maps Gate response kinds one-to-one and preserves `local_order_id`、`exchange_order_id`、`error_label_hash`。

- [x] **Step 3: 运行 Task 3 验证**

Run:

```bash
cmake --build build/debug --target strategy_gate_order_gateway_test -j8
./build/debug/test/strategy/strategy_gate_order_gateway_test
```

Expected: all `GateOrderGatewayTest` tests pass.

### Task 4: Strategy To Gate OrderSession Path Benchmark

**Files:**
- Create: `benchmark/strategy/CMakeLists.txt`
- Create: `benchmark/strategy/order_gateway_benchmark.cpp`
- Modify: `benchmark/CMakeLists.txt`

- [x] **Step 1: 写 benchmark**

Create `benchmark/strategy/order_gateway_benchmark.cpp` with a fake Gate order session that only records `PlaceOrderRequest` / `CancelOrderRequest` and returns `gate::OrderSendStatus::kOk`。

Benchmark cases:

- `BM_GateStrategyPrepareLimitOrder`: constructs `GateStrategyOrder` and calls `GateOrderGateway::PrepareOrder()`。
- `BM_GateStrategyPlaceCachedOrder`: prepares once outside the loop and calls `GateOrderGateway::PlaceOrder()` in the loop。
- `BM_GateStrategyCancelCachedOrder`: prepares once outside the loop, sets `exchange_order_id`, and calls `GateOrderGateway::CancelOrder()` in the loop。

- [x] **Step 2: 接入 benchmark target**

Create `benchmark/strategy/CMakeLists.txt` and add root benchmark subdirectory:

```cmake
add_executable(strategy_order_gateway_benchmark
    order_gateway_benchmark.cpp
)

target_link_libraries(strategy_order_gateway_benchmark
    PRIVATE
        aquila_strategy_gate
        benchmark::benchmark_main
)
```

- [x] **Step 3: 运行 benchmark smoke**

Run:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target strategy_order_gateway_benchmark -j8
./build/release/benchmark/strategy/strategy_order_gateway_benchmark --benchmark_filter='BM_GateStrategyPrepareLimitOrder|BM_GateStrategyPlaceCachedOrder|BM_GateStrategyCancelCachedOrder' --benchmark_min_time=0.01s
```

Expected: benchmark executable runs all three benchmark cases without failures. Do not claim latency improvement from these numbers; they are first-version baseline evidence only.

### Task 5: Documentation And Onboarding Sync

**Files:**
- Modify: `doc/project_onboarding_guide.md`
- Modify: `doc/agent-handoff-gate-trade-architecture.md`

- [x] **Step 1: 更新 onboarding**

Update `doc/project_onboarding_guide.md`:

- “最近已完成”增加 Strategy 第一版订单框架、Gate adapter、tests 和 benchmark。
- “文档索引”增加 this plan。
- “代码入口”新增 `Strategy 订单框架` 小节，列出 `strategy/order_types.h`、`order_store.h`、`strategy.h`、`exchange/gate/trading/gate_order_gateway.h`、Strategy tests 和 benchmark。
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
cmake --build build/debug --target strategy_order_store_test strategy_test strategy_gate_order_gateway_test -j8
ctest --test-dir build/debug -R 'strategy|gate_(order|submit)' --output-on-failure

cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target strategy_order_gateway_benchmark -j8
./build/release/benchmark/strategy/strategy_order_gateway_benchmark --benchmark_filter='BM_GateStrategyPrepareLimitOrder|BM_GateStrategyPlaceCachedOrder|BM_GateStrategyCancelCachedOrder' --benchmark_min_time=0.01s

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
