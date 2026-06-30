# Gate Multi OrderSession Baseline Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 Gate `n=4` 多路 `OrderSession` 的最小 baseline backend：单线程拥有 4 条 session，按 route hint / auto policy 路由 place、cancel、cache 和 forget。

**Architecture:** `Strategy` / OMS 仍负责 duplicate / split、winner、overfill 和 cancel trigger；本计划只实现 gateway 基础设施。`core::OrderCreateRequest` 和 `core::StrategyOrder` 增加通用 `gateway_route_id`，Gate 侧新增 header-only `MultiOrderSessionGateway<SessionT>`，内部固定支持 4 条 session、显式 `RouteTable(local_order_id -> session_index)`、auto round-robin 和 `min_ready_sessions`。不引入 worker thread、配置解析、live smoke 或策略 fanout 行为。

**Tech Stack:** C++20、CMake、GTest、Abseil `absl::flat_hash_map`、现有 `core::OrderManager` gateway contract、Gate `OrderSendResult`。

---

## Scope

本计划只做：

- `n=4` 固定 session count。
- `1 thread : 4 OrderSession` baseline backend。
- 显式 route hint：策略 / OMS 可以指定 child order 发往 session `0..3`。
- auto route：未指定 route 时按 ready session round-robin。
- cancel / cache / forget 回原 session。
- 单元测试覆盖 route semantics。

本计划不做：

- per-session worker thread。
- `gate_order_session_rtt_probe` fanout batch。
- config TOML。
- LeadLag 策略 fanout。
- live 下单或任何 fillability 结论。

## File Structure

- Modify: `core/trading/order_types.h`
  - 增加 `kAutoGatewayRoute` 和 `gateway_route_id` 字段。
- Modify: `core/trading/order_manager.h`
  - `CreateOrder()` 复制 route hint。
- Create: `exchange/gate/trading/multi_order_session_gateway.h`
  - Gate baseline multi-session gateway，固定 `kGateMultiOrderSessionCount = 4`。
- Modify: `exchange/gate/trading/order_types.h`
  - 增加 `OrderSendStatus::kInvalidRoute`，用于 route 越界或 route table 缺失。
- Modify: `exchange/gate/CMakeLists.txt`
  - 把新 header 纳入 `aquila_gate` target source list。
- Create: `test/core/trading/order_manager_route_test.cpp`
  - 验证 route hint 从 request 进入 `StrategyOrder`。
- Modify: `test/core/trading/CMakeLists.txt`
  - 增加 `core_order_manager_route_test`。
- Create: `test/exchange/gate/trading/multi_order_session_gateway_test.cpp`
  - 验证 4 session gateway 的 route/cancel/cache/forget/ready 行为。
- Modify: `test/exchange/gate/trading/CMakeLists.txt`
  - 增加 `gate_multi_order_session_gateway_test`。
- Modify: `docs/agent-handoff-gate-trade-architecture.md`
  - 记录最小实现入口和 baseline 边界。
- Modify: `docs/strategy_order_component_model.md`
  - 记录 `gateway_route_id` 和 `n=4` baseline。
- Modify: `docs/project_onboarding_guide.md`
  - 更新当前事实和验证命令。

---

### Task 1: Core Route Hint Contract

**Files:**
- Modify: `core/trading/order_types.h`
- Modify: `core/trading/order_manager.h`
- Create: `test/core/trading/order_manager_route_test.cpp`
- Modify: `test/core/trading/CMakeLists.txt`

- [ ] **Step 1: Write the failing core test**

Create `test/core/trading/order_manager_route_test.cpp`:

```cpp
#include "core/trading/order_manager.h"

#include <cstdint>
#include <vector>

#include "core/trading/order_types.h"
#include "gtest/gtest.h"

namespace aquila::core {
namespace {

enum class FakeSendStatus : std::uint8_t {
  kOk,
  kRejected,
};

struct FakeSendResult {
  FakeSendStatus status{FakeSendStatus::kOk};
  std::int64_t send_local_ns{0};
};

class CapturingGateway {
 public:
  FakeSendResult PlaceOrder(const StrategyOrder& order) noexcept {
    placed_routes.push_back(order.gateway_route_id);
    placed_ids.push_back(order.local_order_id);
    return {.status = FakeSendStatus::kOk, .send_local_ns = 123};
  }

  FakeSendResult CancelOrder(const StrategyOrder& order) noexcept {
    cancelled_routes.push_back(order.gateway_route_id);
    cancelled_ids.push_back(order.local_order_id);
    return {.status = FakeSendStatus::kOk, .send_local_ns = 0};
  }

  std::vector<std::uint16_t> placed_routes;
  std::vector<std::uint64_t> placed_ids;
  std::vector<std::uint16_t> cancelled_routes;
  std::vector<std::uint64_t> cancelled_ids;
};

OrderCreateRequest MakeRequest(std::uint16_t route_id) noexcept {
  OrderCreateRequest request;
  request.symbol_id = 1;
  request.symbol = "BTC_USDT";
  request.quantity = 1.0;
  request.quantity_text = "1";
  request.price_text = "50000";
  request.gateway_route_id = route_id;
  return request;
}

TEST(OrderManagerRouteTest, CopiesExplicitGatewayRouteToStrategyOrder) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  const OrderPlaceResult placed = manager.PlaceOrder(MakeRequest(3));

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(gateway.placed_routes.size(), 1U);
  EXPECT_EQ(gateway.placed_routes[0], 3U);
  EXPECT_EQ(manager.FindOrder(placed.local_order_id)->gateway_route_id, 3U);
}

TEST(OrderManagerRouteTest, DefaultsGatewayRouteToAuto) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  OrderCreateRequest request = MakeRequest(kAutoGatewayRoute);
  const OrderPlaceResult placed = manager.PlaceOrder(request);

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(gateway.placed_routes.size(), 1U);
  EXPECT_EQ(gateway.placed_routes[0], kAutoGatewayRoute);
  EXPECT_EQ(manager.FindOrder(placed.local_order_id)->gateway_route_id,
            kAutoGatewayRoute);
}

TEST(OrderManagerRouteTest, ChildOrdersGetUniqueLocalIdsWithRoutes) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  const OrderPlaceResult first = manager.PlaceOrder(MakeRequest(0));
  const OrderPlaceResult second = manager.PlaceOrder(MakeRequest(3));

  ASSERT_EQ(first.status, OrderPlaceStatus::kOk);
  ASSERT_EQ(second.status, OrderPlaceStatus::kOk);
  EXPECT_NE(first.local_order_id, second.local_order_id);
  ASSERT_EQ(gateway.placed_routes.size(), 2U);
  EXPECT_EQ(gateway.placed_routes[0], 0U);
  EXPECT_EQ(gateway.placed_routes[1], 3U);
}

TEST(OrderManagerRouteTest, CancelPreservesStoredGatewayRoute) {
  CapturingGateway gateway;
  OrderManager<CapturingGateway> manager(gateway, 8, 7);

  const OrderPlaceResult placed = manager.PlaceOrder(MakeRequest(2));
  const OrderCancelResult cancelled = manager.CancelOrder(placed.local_order_id);

  ASSERT_EQ(cancelled.status, OrderCancelStatus::kOk);
  ASSERT_EQ(gateway.cancelled_routes.size(), 1U);
  EXPECT_EQ(gateway.cancelled_routes[0], 2U);
  EXPECT_EQ(gateway.cancelled_ids[0], placed.local_order_id);
}

}  // namespace
}  // namespace aquila::core
```

- [ ] **Step 2: Add the test target**

Modify `test/core/trading/CMakeLists.txt`:

```cmake
add_executable(core_order_manager_route_test
    order_manager_route_test.cpp
)

target_link_libraries(core_order_manager_route_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME core_order_manager_route_test
         COMMAND core_order_manager_route_test)
```

- [ ] **Step 3: Run the failing test**

Run:

```bash
cmake --build build/debug --target core_order_manager_route_test -j8
```

Expected: compile failure mentioning `gateway_route_id` and `kAutoGatewayRoute` are not members of `aquila::core::OrderCreateRequest` / `StrategyOrder`.

- [ ] **Step 4: Add the route fields**

Modify `core/trading/order_types.h`:

```cpp
inline constexpr std::uint16_t kAutoGatewayRoute =
    static_cast<std::uint16_t>(0xFFFF);

struct OrderCreateRequest {
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  OrderType order_type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  double quantity{0.0};
  std::string_view quantity_text{};
  std::string_view price_text{};
  bool reduce_only{false};
  std::uint16_t gateway_route_id{kAutoGatewayRoute};
};

struct StrategyOrder {
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  Exchange exchange{Exchange::kGate};
  std::int32_t symbol_id{0};
  std::string_view symbol{};
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  double quantity{0.0};
  std::string_view quantity_text{};
  std::string_view price_text{};
  bool reduce_only{false};
  std::uint16_t gateway_route_id{kAutoGatewayRoute};
  OrderStatus status{OrderStatus::kCreated};
  double cumulative_filled_quantity{0.0};
  double cumulative_filled_value{0.0};
  double last_fill_price{0.0};
  std::int64_t request_send_local_ns{0};
  std::int64_t ack_local_receive_ns{0};
  std::int64_t response_local_receive_ns{0};
  std::int64_t ack_exchange_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t accepted_exchange_ns{0};
  std::int64_t finish_exchange_ns{0};
  std::int64_t exchange_update_ns{0};
  OrderFinishReason finish_reason{OrderFinishReason::kUnknown};
  OrderRole role{OrderRole::kNone};
  OrderRejectReason reject_reason{OrderRejectReason::kUnknown};
  bool is_finished{false};
```

Modify `core/trading/order_manager.h` in `CreateOrder()`:

```cpp
    order->price_text = request.price_text;
    order->reduce_only = request.reduce_only;
    order->gateway_route_id = request.gateway_route_id;
    order->status = OrderStatus::kCreated;
```

- [ ] **Step 5: Run the test**

Run:

```bash
cmake --build build/debug --target core_order_manager_route_test -j8
./build/debug/test/core/trading/core_order_manager_route_test
```

Expected: all tests pass.

- [ ] **Step 6: Commit Task 1**

```bash
git add core/trading/order_types.h core/trading/order_manager.h \
  test/core/trading/order_manager_route_test.cpp test/core/trading/CMakeLists.txt
git commit -m "feat: add order gateway route hint"
```

---

### Task 2: Gate MultiOrderSessionGateway Baseline

**Files:**
- Modify: `exchange/gate/trading/order_types.h`
- Create: `exchange/gate/trading/multi_order_session_gateway.h`
- Modify: `exchange/gate/CMakeLists.txt`
- Create: `test/exchange/gate/trading/multi_order_session_gateway_test.cpp`
- Modify: `test/exchange/gate/trading/CMakeLists.txt`

- [ ] **Step 1: Write the failing Gate gateway test**

Create `test/exchange/gate/trading/multi_order_session_gateway_test.cpp`:

```cpp
#include "exchange/gate/trading/multi_order_session_gateway.h"

#include <array>
#include <cstdint>
#include <vector>

#include "core/trading/order_types.h"
#include "exchange/gate/trading/order_types.h"
#include "gtest/gtest.h"

namespace aquila::gate {
namespace {

class FakeSession {
 public:
  explicit FakeSession(bool ready = true) : ready_(ready) {}

  [[nodiscard]] bool Ready() const noexcept { return ready_; }
  void set_ready(bool ready) noexcept { ready_ = ready; }

  OrderSendResult PlaceOrder(const core::StrategyOrder& order) noexcept {
    placed.push_back(order.local_order_id);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = next_sequence++,
            .encoded_request_id = order.local_order_id,
            .send_local_ns = static_cast<std::int64_t>(1000 + placed.size())};
  }

  OrderSendResult CancelOrder(const core::StrategyOrder& order) noexcept {
    cancelled.push_back(order.local_order_id);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = next_sequence++,
            .encoded_request_id = order.local_order_id,
            .send_local_ns = 0};
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    cached_local_ids.push_back(local_order_id);
    cached_exchange_ids.push_back(exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    forgotten_local_ids.push_back(local_order_id);
  }

  bool ready_{true};
  std::uint64_t next_sequence{1};
  std::vector<std::uint64_t> placed;
  std::vector<std::uint64_t> cancelled;
  std::vector<std::uint64_t> cached_local_ids;
  std::vector<std::uint64_t> cached_exchange_ids;
  std::vector<std::uint64_t> forgotten_local_ids;
};

core::StrategyOrder MakeOrder(std::uint64_t local_order_id,
                              std::uint16_t route_id) noexcept {
  core::StrategyOrder order;
  order.local_order_id = local_order_id;
  order.symbol_id = 1;
  order.symbol = "BTC_USDT";
  order.quantity = 1.0;
  order.quantity_text = "1";
  order.price_text = "50000";
  order.gateway_route_id = route_id;
  return order;
}

using Gateway = MultiOrderSessionGateway<FakeSession>;

Gateway MakeGateway(std::array<FakeSession, kGateMultiOrderSessionCount>&
                        sessions) {
  return Gateway({&sessions[0], &sessions[1], &sessions[2], &sessions[3]});
}

TEST(MultiOrderSessionGatewayTest, ExplicitRouteSendsToSelectedSession) {
  std::array<FakeSession, kGateMultiOrderSessionCount> sessions{
      FakeSession{}, FakeSession{}, FakeSession{}, FakeSession{}};
  Gateway gateway = MakeGateway(sessions);

  const OrderSendResult sent = gateway.PlaceOrder(MakeOrder(101, 2));

  EXPECT_EQ(sent.status, OrderSendStatus::kOk);
  EXPECT_TRUE(sessions[0].placed.empty());
  EXPECT_TRUE(sessions[1].placed.empty());
  ASSERT_EQ(sessions[2].placed.size(), 1U);
  EXPECT_EQ(sessions[2].placed[0], 101U);
  EXPECT_TRUE(sessions[3].placed.empty());
}

TEST(MultiOrderSessionGatewayTest, AutoRouteRoundRobinsAcrossReadySessions) {
  std::array<FakeSession, kGateMultiOrderSessionCount> sessions{
      FakeSession{}, FakeSession{}, FakeSession{}, FakeSession{}};
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(101, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(102, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(103, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(104, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);

  EXPECT_EQ(sessions[0].placed, std::vector<std::uint64_t>({101}));
  EXPECT_EQ(sessions[1].placed, std::vector<std::uint64_t>({102}));
  EXPECT_EQ(sessions[2].placed, std::vector<std::uint64_t>({103}));
  EXPECT_EQ(sessions[3].placed, std::vector<std::uint64_t>({104}));
}

TEST(MultiOrderSessionGatewayTest, CancelReturnsToOriginalRoute) {
  std::array<FakeSession, kGateMultiOrderSessionCount> sessions{
      FakeSession{}, FakeSession{}, FakeSession{}, FakeSession{}};
  Gateway gateway = MakeGateway(sessions);

  ASSERT_EQ(gateway.PlaceOrder(MakeOrder(201, 3)).status,
            OrderSendStatus::kOk);
  const OrderSendResult cancelled = gateway.CancelOrder(MakeOrder(201, 0));

  EXPECT_EQ(cancelled.status, OrderSendStatus::kOk);
  EXPECT_TRUE(sessions[0].cancelled.empty());
  EXPECT_TRUE(sessions[1].cancelled.empty());
  EXPECT_TRUE(sessions[2].cancelled.empty());
  EXPECT_EQ(sessions[3].cancelled, std::vector<std::uint64_t>({201}));
}

TEST(MultiOrderSessionGatewayTest, CacheAndForgetUseOriginalRoute) {
  std::array<FakeSession, kGateMultiOrderSessionCount> sessions{
      FakeSession{}, FakeSession{}, FakeSession{}, FakeSession{}};
  Gateway gateway = MakeGateway(sessions);

  ASSERT_EQ(gateway.PlaceOrder(MakeOrder(301, 1)).status,
            OrderSendStatus::kOk);
  gateway.CacheExchangeOrderId(301, 9001);
  gateway.ForgetExchangeOrderId(301);

  EXPECT_EQ(sessions[1].cached_local_ids, std::vector<std::uint64_t>({301}));
  EXPECT_EQ(sessions[1].cached_exchange_ids, std::vector<std::uint64_t>({9001}));
  EXPECT_EQ(sessions[1].forgotten_local_ids, std::vector<std::uint64_t>({301}));
  EXPECT_EQ(gateway.CancelOrder(MakeOrder(301, 1)).status,
            OrderSendStatus::kInvalidRoute);
}

TEST(MultiOrderSessionGatewayTest, InvalidRouteRejectsWithoutSending) {
  std::array<FakeSession, kGateMultiOrderSessionCount> sessions{
      FakeSession{}, FakeSession{}, FakeSession{}, FakeSession{}};
  Gateway gateway = MakeGateway(sessions);

  const OrderSendResult sent = gateway.PlaceOrder(MakeOrder(401, 4));

  EXPECT_EQ(sent.status, OrderSendStatus::kInvalidRoute);
  for (const FakeSession& session : sessions) {
    EXPECT_TRUE(session.placed.empty());
  }
}

TEST(MultiOrderSessionGatewayTest, ReadyRequiresConfiguredMinimum) {
  std::array<FakeSession, kGateMultiOrderSessionCount> sessions{
      FakeSession{}, FakeSession{}, FakeSession{false}, FakeSession{false}};
  Gateway::Config config;
  config.min_ready_sessions = 3;
  Gateway gateway({&sessions[0], &sessions[1], &sessions[2], &sessions[3]},
                  config);

  EXPECT_FALSE(gateway.Ready());
  sessions[2].set_ready(true);
  EXPECT_TRUE(gateway.Ready());
}

TEST(MultiOrderSessionGatewayTest, AutoRouteSkipsNotReadySessions) {
  std::array<FakeSession, kGateMultiOrderSessionCount> sessions{
      FakeSession{}, FakeSession{false}, FakeSession{}, FakeSession{false}};
  Gateway gateway = MakeGateway(sessions);

  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(501, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);
  EXPECT_EQ(gateway.PlaceOrder(MakeOrder(502, core::kAutoGatewayRoute)).status,
            OrderSendStatus::kOk);

  EXPECT_EQ(sessions[0].placed, std::vector<std::uint64_t>({501}));
  EXPECT_TRUE(sessions[1].placed.empty());
  EXPECT_EQ(sessions[2].placed, std::vector<std::uint64_t>({502}));
  EXPECT_TRUE(sessions[3].placed.empty());
}

}  // namespace
}  // namespace aquila::gate
```

- [ ] **Step 2: Add the test target**

Modify `test/exchange/gate/trading/CMakeLists.txt`:

```cmake
add_executable(gate_multi_order_session_gateway_test
    multi_order_session_gateway_test.cpp
)

target_link_libraries(gate_multi_order_session_gateway_test
    PRIVATE
        aquila_gate
        aquila_core
        GTest::gtest_main
)

add_test(NAME gate_multi_order_session_gateway_test
         COMMAND gate_multi_order_session_gateway_test)
```

- [ ] **Step 3: Run the failing test**

Run:

```bash
cmake --build build/debug --target gate_multi_order_session_gateway_test -j8
```

Expected: compile failure because `exchange/gate/trading/multi_order_session_gateway.h`, `kGateMultiOrderSessionCount`, `MultiOrderSessionGateway`, and `OrderSendStatus::kInvalidRoute` do not exist.

- [ ] **Step 4: Add `kInvalidRoute`**

Modify `exchange/gate/trading/order_types.h`:

```cpp
enum class OrderSendStatus : std::uint8_t {
  kOk,
  kNotLoggedIn,
  kNotActive,
  kInflightFull,
  kEncodeBufferTooSmall,
  kInvalidLocalOrderId,
  kInvalidRoute,
  kUnsupportedOrderType,
  kInvalidQuantityText,
  kSignatureFailed,
  kNoPreparedWriteSlot,
  kWriteUnavailable,
};
```

- [ ] **Step 5: Implement the baseline gateway**

Create `exchange/gate/trading/multi_order_session_gateway.h`:

```cpp
#ifndef AQUILA_EXCHANGE_GATE_TRADING_MULTI_ORDER_SESSION_GATEWAY_H_
#define AQUILA_EXCHANGE_GATE_TRADING_MULTI_ORDER_SESSION_GATEWAY_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "absl/container/flat_hash_map.h"
#include "core/trading/order_types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {

inline constexpr std::size_t kGateMultiOrderSessionCount = 4;

template <typename SessionT>
class MultiOrderSessionGateway {
 public:
  struct Config {
    std::size_t min_ready_sessions{1};
    std::size_t route_table_capacity{16384};
  };

  explicit MultiOrderSessionGateway(
      std::array<SessionT*, kGateMultiOrderSessionCount> sessions,
      Config config = {})
      : sessions_(sessions), config_(config) {
    route_table_.reserve(config_.route_table_capacity);
  }

  MultiOrderSessionGateway(const MultiOrderSessionGateway&) = delete;
  MultiOrderSessionGateway& operator=(const MultiOrderSessionGateway&) = delete;
  MultiOrderSessionGateway(MultiOrderSessionGateway&&) noexcept = default;
  MultiOrderSessionGateway& operator=(MultiOrderSessionGateway&&) noexcept =
      default;

  [[nodiscard]] bool Ready() const noexcept {
    return ReadySessionCount() >= config_.min_ready_sessions;
  }

  template <typename OrderT>
  [[nodiscard]] OrderSendResult PlaceOrder(const OrderT& order) noexcept {
    const std::size_t route = ResolvePlaceRoute(order.gateway_route_id);
    if (route >= kGateMultiOrderSessionCount || sessions_[route] == nullptr ||
        !SessionReady(route)) {
      return Failure(OrderSendStatus::kInvalidRoute);
    }

    OrderSendResult sent = sessions_[route]->PlaceOrder(order);
    if (sent.status == OrderSendStatus::kOk) {
      route_table_[order.local_order_id] = static_cast<std::uint8_t>(route);
    }
    return sent;
  }

  template <typename OrderT>
  [[nodiscard]] OrderSendResult CancelOrder(const OrderT& order) noexcept {
    const auto route = route_table_.find(order.local_order_id);
    if (route == route_table_.end()) {
      return Failure(OrderSendStatus::kInvalidRoute);
    }
    SessionT* session = sessions_[route->second];
    if (session == nullptr || !SessionReady(route->second)) {
      return Failure(OrderSendStatus::kNotActive);
    }
    return session->CancelOrder(order);
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    const auto route = route_table_.find(local_order_id);
    if (route == route_table_.end()) {
      return;
    }
    SessionT* session = sessions_[route->second];
    if (session != nullptr) {
      session->CacheExchangeOrderId(local_order_id, exchange_order_id);
    }
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    const auto route = route_table_.find(local_order_id);
    if (route == route_table_.end()) {
      return;
    }
    SessionT* session = sessions_[route->second];
    if (session != nullptr) {
      session->ForgetExchangeOrderId(local_order_id);
    }
    route_table_.erase(route);
  }

  [[nodiscard]] std::size_t ReadySessionCount() const noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < kGateMultiOrderSessionCount; ++i) {
      if (sessions_[i] != nullptr && SessionReady(i)) {
        ++count;
      }
    }
    return count;
  }

 private:
  [[nodiscard]] bool SessionReady(std::size_t index) const noexcept {
    if (index >= kGateMultiOrderSessionCount || sessions_[index] == nullptr) {
      return false;
    }
    if constexpr (requires(const SessionT& session) { session.Ready(); }) {
      return static_cast<bool>(sessions_[index]->Ready());
    }
    return true;
  }

  [[nodiscard]] std::size_t ResolvePlaceRoute(
      std::uint16_t route_id) noexcept {
    if (route_id != core::kAutoGatewayRoute) {
      return route_id;
    }
    for (std::size_t attempt = 0; attempt < kGateMultiOrderSessionCount;
         ++attempt) {
      const std::size_t candidate =
          (next_auto_route_ + attempt) % kGateMultiOrderSessionCount;
      if (sessions_[candidate] != nullptr && SessionReady(candidate)) {
        next_auto_route_ = (candidate + 1) % kGateMultiOrderSessionCount;
        return candidate;
      }
    }
    return kGateMultiOrderSessionCount;
  }

  [[nodiscard]] static OrderSendResult Failure(
      OrderSendStatus status) noexcept {
    return {.status = status,
            .request_sequence = 0,
            .encoded_request_id = 0,
            .send_local_ns = 0};
  }

  std::array<SessionT*, kGateMultiOrderSessionCount> sessions_{};
  Config config_{};
  std::size_t next_auto_route_{0};
  absl::flat_hash_map<std::uint64_t, std::uint8_t> route_table_;
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_MULTI_ORDER_SESSION_GATEWAY_H_
```

- [ ] **Step 6: Add the header to the Gate target**

Modify `exchange/gate/CMakeLists.txt`:

```cmake
    trading/multi_order_session_gateway.h
```

Place it near the other `trading/order_*` headers in the `add_library(aquila_gate STATIC ...)` list.

- [ ] **Step 7: Run the Gate gateway test**

Run:

```bash
cmake --build build/debug --target gate_multi_order_session_gateway_test -j8
./build/debug/test/exchange/gate/trading/gate_multi_order_session_gateway_test
```

Expected: all tests pass.

- [ ] **Step 8: Commit Task 2**

```bash
git add exchange/gate/trading/order_types.h \
  exchange/gate/trading/multi_order_session_gateway.h exchange/gate/CMakeLists.txt \
  test/exchange/gate/trading/multi_order_session_gateway_test.cpp \
  test/exchange/gate/trading/CMakeLists.txt
git commit -m "feat: add gate multi order session gateway"
```

---

### Task 3: Documentation Sync

**Files:**
- Modify: `docs/agent-handoff-gate-trade-architecture.md`
- Modify: `docs/strategy_order_component_model.md`
- Modify: `docs/project_onboarding_guide.md`

- [ ] **Step 1: Update Gate handoff**

In `docs/agent-handoff-gate-trade-architecture.md`, add a short implementation note under “多路 OrderSession 讨论结论”:

```markdown
当前最小实现先落地 `1 thread : 4 OrderSession` baseline：`exchange/gate/trading/multi_order_session_gateway.h`
固定管理 4 条 session，使用 `core::StrategyOrder::gateway_route_id` 做显式 route hint，未指定时按 ready session round-robin。
该 baseline 只验证 route / cancel / cache / forget 语义，不代表 per-session worker fanout 或 fillability 收益。
```

- [ ] **Step 2: Update component model**

In `docs/strategy_order_component_model.md`, add:

```markdown
最小 baseline 使用 `core::kAutoGatewayRoute` 表示 gateway 自选 route；显式 `gateway_route_id=0..3`
表示发往固定 Gate order session。`OrderManager` 只复制该字段，不解释策略 fanout 语义。
```

- [ ] **Step 3: Update onboarding**

In `docs/project_onboarding_guide.md`, update the Gate Trading / TUI bullet to mention:

```markdown
`1 thread : 4 OrderSession` baseline gateway 已落地后，下一步才是 `gate_order_session_rtt_probe`
fanout timing 和 per-session worker 对照；baseline 不宣称 fillability 改善。
```

- [ ] **Step 4: Run documentation check**

Run:

```bash
git diff --check
```

Expected: no output, exit 0.

- [ ] **Step 5: Commit Task 3**

```bash
git add docs/agent-handoff-gate-trade-architecture.md \
  docs/strategy_order_component_model.md docs/project_onboarding_guide.md
git commit -m "docs: record gate multi order session baseline"
```

---

### Task 4: Final Verification

**Files:**
- No new files.

- [ ] **Step 1: Run focused tests**

Run:

```bash
ctest --test-dir build/debug -R '(core_order_manager_route|gate_multi_order_session_gateway|core_trading_runtime)' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 2: Run broader trading checks**

Run:

```bash
ctest --test-dir build/debug -R '(core_order_pool|core_trading_strategy_context|core_trading_runtime|gate_order_session|gate_order_session_runtime_adapter)' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 3: Run whitespace check**

Run:

```bash
git diff --check
```

Expected: no output, exit 0.

- [ ] **Step 4: Confirm no evaluation boundary change**

This plan does not touch `evaluation/`. If implementation accidentally changes `evaluation/` or target links, run:

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: no output.

- [ ] **Step 5: Inspect final status**

Run:

```bash
git status --short --branch
git log --oneline -5
```

Expected: current branch contains the task commits; no unrelated dirty files.

## Self-Review

- Spec coverage: `n=4` baseline, route hint, explicit route, auto route, cancel original session, single account feedback boundary and no threaded backend are covered by Tasks 1-4.
- Placeholder scan: no placeholder or vague implementation step is required for the baseline.
- Type consistency: `gateway_route_id`, `kAutoGatewayRoute`, `kGateMultiOrderSessionCount`, `MultiOrderSessionGateway`, and `OrderSendStatus::kInvalidRoute` are defined before use.
