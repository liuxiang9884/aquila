#include <type_traits>

#include <gtest/gtest.h>

#include "core/trading/order_manager.h"
#include "core/trading/order_types.h"
#include "core/trading/strategy_context.h"
#include "core/trading/trading_runtime.h"

namespace aquila::core {
namespace {

struct FakeOrderSession {
  enum class SendStatus { kOk };

  struct SendResult {
    SendStatus status{SendStatus::kOk};
  };

  SendResult PlaceOrder(const OrderPlaceRequest&) noexcept {
    return {};
  }

  SendResult CancelOrder(const OrderCancelRequest&) noexcept {
    return {};
  }
};

struct FakeStrategy {};

static_assert(
    std::is_same_v<TradingRuntime<FakeStrategy, FakeOrderSession>::ContextT,
                   StrategyContext<FakeOrderSession>>);

TEST(CoreTradingContractNamespaceTest, PublicContractsLiveInAquilaCore) {
  FakeOrderSession order_session;
  OrderManager<FakeOrderSession> order_manager(order_session, 2, 1);
  StrategyContext<FakeOrderSession> context(order_manager);

  OrderPlaceRequest request{.price = 50000.0, .quantity = 1.0, .symbol_id = 42};
  SetOrderSymbol(&request, "BTC_USDT");
  const OrderPlaceResult placed = context.PlaceLimitOrder(request);

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
}

}  // namespace
}  // namespace aquila::core
