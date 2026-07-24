#ifndef AQUILA_CORE_TRADING_STRATEGY_CONTEXT_H_
#define AQUILA_CORE_TRADING_STRATEGY_CONTEXT_H_

#include <cstdint>
#include <utility>

#include "core/trading/order_manager.h"

namespace aquila::core {

template <typename OrderSessionT>
class StrategyContext {
 public:
  using OrderManagerT = OrderManager<OrderSessionT>;

  explicit StrategyContext(OrderManagerT& order_manager) noexcept
      : order_manager_(order_manager) {}

  OrderPlaceResult PlaceOrder(const OrderPlaceRequest& request,
                              OrderLocalMetadata local_metadata = {}) noexcept {
    return order_manager_.PlaceOrder(request, local_metadata);
  }

  OrderPlaceResult PlaceLimitOrder(
      OrderPlaceRequest request,
      OrderLocalMetadata local_metadata = {}) noexcept {
    return order_manager_.PlaceLimitOrder(std::move(request), local_metadata);
  }

  OrderCancelResult CancelOrder(const OrderCancelRequest& request) noexcept {
    return order_manager_.CancelOrder(request);
  }

  [[nodiscard]] const StrategyOrder* FindOrder(
      std::uint64_t local_order_id) const noexcept {
    return order_manager_.FindOrder(local_order_id);
  }

  bool RetireFinishedOrder(std::uint64_t local_order_id) noexcept {
    return order_manager_.RetireFinishedOrder(local_order_id);
  }

  [[nodiscard]] std::uint16_t MaxOrderSessionFanout() const noexcept {
    return order_manager_.MaxOrderSessionFanout();
  }

  void RefreshOrderRoutes() noexcept {
    order_manager_.RefreshOrderRoutes();
  }

  [[nodiscard]] bool OrderRouteReady(std::uint16_t route_id) const noexcept {
    return order_manager_.OrderRouteReady(route_id);
  }

 private:
  OrderManagerT& order_manager_;
};

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_STRATEGY_CONTEXT_H_
