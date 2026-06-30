#ifndef AQUILA_EXCHANGE_GATE_TRADING_MULTI_ORDER_SESSION_GATEWAY_H_
#define AQUILA_EXCHANGE_GATE_TRADING_MULTI_ORDER_SESSION_GATEWAY_H_

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "core/trading/order_types.h"
#include "exchange/gate/trading/order_types.h"

namespace aquila::gate {

template <typename SessionT>
class MultiOrderSessionGateway {
 public:
  struct Config {
    std::size_t min_ready_sessions{1};
    std::size_t route_table_capacity{16384};
  };

  explicit MultiOrderSessionGateway(std::vector<SessionT*> sessions,
                                    Config config = {})
      : sessions_(std::move(sessions)), config_(config) {
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
    if (route >= sessions_.size() || sessions_[route] == nullptr ||
        !SessionReady(route)) {
      return Failure(OrderSendStatus::kInvalidRoute);
    }

    OrderSendResult sent = sessions_[route]->PlaceOrder(order);
    if (sent.status == OrderSendStatus::kOk) {
      route_table_[order.local_order_id] = static_cast<std::uint16_t>(route);
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
    for (std::size_t i = 0; i < sessions_.size(); ++i) {
      if (sessions_[i] != nullptr && SessionReady(i)) {
        ++count;
      }
    }
    return count;
  }

 private:
  [[nodiscard]] bool SessionReady(std::size_t index) const noexcept {
    if (index >= sessions_.size() || sessions_[index] == nullptr) {
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
    if (sessions_.empty()) {
      return sessions_.size();
    }
    for (std::size_t attempt = 0; attempt < sessions_.size(); ++attempt) {
      const std::size_t candidate =
          (next_auto_route_ + attempt) % sessions_.size();
      if (sessions_[candidate] != nullptr && SessionReady(candidate)) {
        next_auto_route_ = (candidate + 1) % sessions_.size();
        return candidate;
      }
    }
    return sessions_.size();
  }

  [[nodiscard]] static OrderSendResult Failure(
      OrderSendStatus status) noexcept {
    return {.status = status,
            .request_sequence = 0,
            .encoded_request_id = 0,
            .send_local_ns = 0};
  }

  std::vector<SessionT*> sessions_;
  Config config_{};
  std::size_t next_auto_route_{0};
  absl::flat_hash_map<std::uint64_t, std::uint16_t> route_table_;
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_MULTI_ORDER_SESSION_GATEWAY_H_
