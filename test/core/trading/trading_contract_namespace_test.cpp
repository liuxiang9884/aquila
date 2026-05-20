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

  SendResult PlaceOrder(StrategyOrder&) noexcept {
    return {};
  }

  SendResult CancelOrder(StrategyOrder&) noexcept {
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

  const OrderPlaceResult placed = context.PlaceLimitOrder(OrderCreateRequest{
      .symbol_id = 42,
      .symbol = "BTC_USDT",
      .quantity = 1,
      .price_text = "50000",
  });

  ASSERT_EQ(placed.status, OrderPlaceStatus::kOk);
  const StrategyOrder* order = context.FindOrder(placed.local_order_id);
  ASSERT_NE(order, nullptr);
  EXPECT_EQ(order->status, OrderStatus::kSent);
}

}  // namespace
}  // namespace aquila::core
