#ifndef AQUILA_CORE_TRADING_ORDER_GATEWAY_CLIENT_H_
#define AQUILA_CORE_TRADING_ORDER_GATEWAY_CLIENT_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "core/common/result.h"
#include "core/trading/order_gateway_shm.h"
#include "core/trading/order_types.h"
#include "core/websocket/runtime_clock.h"

namespace aquila::core {

enum class OrderGatewaySendStatus : std::uint8_t {
  kOk,
  kInvalidRoute,
  kRouteNotReady,
  kCommandQueueFull,
  kRouteTableFull,
  kNotRunning,
};

struct OrderGatewaySendResult {
  OrderGatewaySendStatus status{OrderGatewaySendStatus::kNotRunning};
  std::uint64_t command_seq{0};
  std::int64_t send_local_ns{0};
};

struct OrderGatewayClientOptions {
  std::size_t route_table_capacity{16384};
  std::uint64_t max_events_per_poll_per_route{64};
};

struct OrderGatewayClientConfig {
  std::string shm_name;
  std::uint16_t route_count{0};
  std::uint32_t command_queue_capacity{0};
  std::uint32_t event_queue_capacity{0};
  std::uint32_t startup_ready_timeout_s{0};
  OrderGatewayClientOptions options{};
};

struct OrderGatewayClientStats {
  std::uint64_t commands_enqueued{0};
  std::uint64_t command_enqueue_failures{0};
  std::uint64_t route_table_full_rejections{0};
  std::uint64_t commands_skipped_route_not_ready{0};
  std::uint64_t invalid_routes{0};
  std::uint64_t ready_events{0};
  std::uint64_t not_ready_events{0};
  std::uint64_t order_response_events{0};
  std::uint64_t command_rejected_events{0};
  std::uint64_t stopped_events{0};
};

class OrderGatewayClient {
 public:
  OrderGatewayClient() noexcept = default;

  OrderGatewayClient(const OrderGatewayClient&) = delete;
  OrderGatewayClient& operator=(const OrderGatewayClient&) = delete;
  OrderGatewayClient(OrderGatewayClient&&) noexcept = default;
  OrderGatewayClient& operator=(OrderGatewayClient&&) noexcept = default;

  [[nodiscard]] static Result<OrderGatewayClient> Open(
      const OrderGatewayClientConfig& config) {
    OrderGatewayShmConfig shm_config{
        .shm_name = config.shm_name,
        .create = false,
        .remove_existing = false,
        .route_count = config.route_count,
        .command_queue_capacity = config.command_queue_capacity,
        .event_queue_capacity = config.event_queue_capacity,
        .startup_ready_timeout_s = config.startup_ready_timeout_s,
    };
    auto shm_result = OrderGatewayShmManager::Open(shm_config);
    if (!shm_result.ok) {
      Result<OrderGatewayClient> result;
      result.error = std::move(shm_result.error);
      return result;
    }
    return Attach(std::move(shm_result.value), config.options);
  }

  [[nodiscard]] static Result<OrderGatewayClient> Attach(
      OrderGatewayShmManager manager, OrderGatewayClientOptions options = {}) {
    Result<OrderGatewayClient> result;
    if (options.route_table_capacity == 0) {
      result.error =
          "order_gateway_client.route_table_capacity must be positive";
      return result;
    }
    if (options.max_events_per_poll_per_route == 0) {
      result.error =
          "order_gateway_client.max_events_per_poll_per_route must be positive";
      return result;
    }
    OrderGatewayClient client;
    client.shm_ = std::move(manager);
    client.options_ = options;
    client.route_table_capacity_ = options.route_table_capacity;
    if (!client.InitializeFromHeader()) {
      result.error = "order_gateway_client invalid shm header";
      return result;
    }
    try {
      client.route_table_.reserve(options.route_table_capacity);
    } catch (...) {
      result.error = "order_gateway_client route table reserve failed";
      return result;
    }
    result.value = std::move(client);
    result.ok = true;
    return result;
  }

  [[nodiscard]] bool Start() noexcept {
    running_ = true;
    NullRuntime runtime;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(startup_ready_timeout_s_);
    while (!AllRoutesReady()) {
      SyncRouteStatesFromHeader();
      (void)PollOrderResponses(runtime);
      SyncRouteStatesFromHeader();
      if (AllRoutesReady()) {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        running_ = false;
        return false;
      }
      std::this_thread::yield();
    }
    running_ = true;
    return true;
  }

  void Stop() noexcept {
    for (std::uint16_t route = 0; route < route_count_; ++route) {
      OrderGatewayCommand command{};
      command.kind = OrderGatewayCommandKind::kStop;
      command.command_seq = NextCommandSeq();
      command.owner_enqueue_ns = NowNs();
      (void)command_queues_[route].TryPush(command);
    }
    running_ = false;
  }

  [[nodiscard]] bool Ready() const noexcept {
    return ready_route_count_ > 0;
  }

  [[nodiscard]] bool Running() const noexcept {
    return running_;
  }

  [[nodiscard]] bool RouteReady(std::uint16_t route_id) const noexcept {
    return route_id < route_count_ && route_ready_[route_id];
  }

  [[nodiscard]] std::uint16_t route_count() const noexcept {
    return route_count_;
  }

  [[nodiscard]] std::uint16_t MaxOrderSessionFanout() const noexcept {
    return route_count_;
  }

  void RefreshRouteStates() noexcept {
    SyncRouteStatesFromHeader();
  }

  [[nodiscard]] std::uint16_t ready_route_count() const noexcept {
    return ready_route_count_;
  }

  [[nodiscard]] const OrderGatewayClientStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] bool HasRouteForLocalOrderForTest(
      std::uint64_t local_order_id) const noexcept {
    return route_table_.contains(local_order_id);
  }

  [[nodiscard]] std::uint16_t RouteForLocalOrderForTest(
      std::uint64_t local_order_id) const noexcept {
    const auto it = route_table_.find(local_order_id);
    if (it == route_table_.end()) {
      return kAutoGatewayRoute;
    }
    return it->second;
  }

  [[nodiscard]] OrderGatewaySendResult PlaceOrder(
      const OrderPlaceRequest& request) noexcept {
    SyncRouteStatesFromHeader();
    if (!running_) {
      return Failure(OrderGatewaySendStatus::kNotRunning);
    }
    if (request.gateway_route_id == kAutoGatewayRoute &&
        ready_route_count_ == 0) {
      ++stats_.commands_skipped_route_not_ready;
      return Failure(OrderGatewaySendStatus::kRouteNotReady);
    }
    const std::uint16_t route = ResolvePlaceRoute(request.gateway_route_id);
    if (route >= route_count_) {
      ++stats_.invalid_routes;
      return Failure(OrderGatewaySendStatus::kInvalidRoute);
    }
    if (!RouteReady(route)) {
      ++stats_.commands_skipped_route_not_ready;
      return Failure(OrderGatewaySendStatus::kRouteNotReady);
    }

    OrderGatewayCommand command{};
    command.kind = OrderGatewayCommandKind::kPlace;
    command.command_seq = NextCommandSeq();
    command.owner_enqueue_ns = NowNs();
    command.payload.place = request;
    command.payload.place.parent_id =
        request.parent_id == 0 ? request.local_order_id : request.parent_id;
    command.payload.place.gateway_route_id = route;
    return EnqueueOrderCommand(command, route, request.local_order_id, true);
  }

  [[nodiscard]] OrderGatewaySendResult CancelOrder(
      const OrderCancelRequest& request) noexcept {
    SyncRouteStatesFromHeader();
    if (!running_) {
      return Failure(OrderGatewaySendStatus::kNotRunning);
    }
    const auto route_it = route_table_.find(request.local_order_id);
    if (route_it == route_table_.end() || route_it->second >= route_count_) {
      ++stats_.invalid_routes;
      return Failure(OrderGatewaySendStatus::kInvalidRoute);
    }
    const std::uint16_t route = route_it->second;
    if (!RouteReady(route)) {
      ++stats_.commands_skipped_route_not_ready;
      return Failure(OrderGatewaySendStatus::kRouteNotReady);
    }

    OrderGatewayCommand command{};
    command.kind = OrderGatewayCommandKind::kCancel;
    command.command_seq = NextCommandSeq();
    command.owner_enqueue_ns = NowNs();
    command.payload.cancel = request;
    command.payload.cancel.parent_id =
        request.parent_id == 0 ? request.local_order_id : request.parent_id;
    command.payload.cancel.gateway_route_id = route;
    return EnqueueOrderCommand(command, route, request.local_order_id, false);
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    SyncRouteStatesFromHeader();
    const auto route_it = route_table_.find(local_order_id);
    if (route_it == route_table_.end() || route_it->second >= route_count_) {
      return;
    }
    const std::uint16_t route = route_it->second;
    if (!RouteReady(route)) {
      ++stats_.commands_skipped_route_not_ready;
      return;
    }
    OrderGatewayCommand command{};
    command.kind = OrderGatewayCommandKind::kCacheExchangeOrderId;
    command.command_seq = NextCommandSeq();
    command.owner_enqueue_ns = NowNs();
    command.payload.order_id.local_order_id = local_order_id;
    command.payload.order_id.exchange_order_id = exchange_order_id;
    command.payload.order_id.gateway_route_id = route;
    if (command_queues_[route].TryPush(command)) {
      ++stats_.commands_enqueued;
    } else {
      ++stats_.command_enqueue_failures;
    }
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    SyncRouteStatesFromHeader();
    const auto route_it = route_table_.find(local_order_id);
    if (route_it == route_table_.end() || route_it->second >= route_count_) {
      return;
    }
    const std::uint16_t route = route_it->second;
    if (RouteReady(route)) {
      OrderGatewayCommand command{};
      command.kind = OrderGatewayCommandKind::kForgetExchangeOrderId;
      command.command_seq = NextCommandSeq();
      command.owner_enqueue_ns = NowNs();
      command.payload.order_id.local_order_id = local_order_id;
      command.payload.order_id.gateway_route_id = route;
      if (command_queues_[route].TryPush(command)) {
        ++stats_.commands_enqueued;
      } else {
        ++stats_.command_enqueue_failures;
      }
    } else {
      ++stats_.commands_skipped_route_not_ready;
    }
    route_table_.erase(route_it);
  }

  template <typename RuntimeT>
  [[nodiscard]] std::uint64_t PollOrderResponses(RuntimeT& runtime) noexcept {
    SyncRouteStatesFromHeader();
    std::uint64_t handled = 0;
    for (std::uint16_t route = 0; route < route_count_; ++route) {
      const DrainRouteResult drained =
          DrainRouteEvents(route, options_.max_events_per_poll_per_route,
                           /*defer_stopped_unknown=*/true, runtime);
      handled += drained.handled;
      if (drained.stopped_seen) {
        handled += DrainAndHandleStoppedRoute(route, runtime);
      }
    }
    handled += SyncRouteStatesFromHeader(runtime);
    return handled;
  }

 private:
  class NullRuntime {
   public:
    void OnOrderResponse(const OrderResponseEvent&) noexcept {}
  };

  [[nodiscard]] bool InitializeFromHeader() noexcept {
    const OrderGatewayShmHeader& header = shm_.header();
    if (header.route_count == 0 ||
        header.route_count > kMaxOrderGatewayRoutes ||
        header.startup_ready_timeout_s == 0) {
      return false;
    }
    route_count_ = header.route_count;
    startup_ready_timeout_s_ = header.startup_ready_timeout_s;
    for (std::uint16_t route = 0; route < route_count_; ++route) {
      command_queues_[route] = shm_.CommandQueue(route);
      event_queues_[route] = shm_.EventQueue(route);
    }
    running_ = true;
    return true;
  }

  [[nodiscard]] bool AllRoutesReady() const noexcept {
    return route_count_ != 0 && ready_route_count_ == route_count_;
  }

  void SetRouteReady(std::uint16_t route, bool ready) noexcept {
    if (route >= route_count_) {
      return;
    }
    if (route_ready_[route] == ready) {
      return;
    }
    route_ready_[route] = ready;
    if (ready) {
      ++ready_route_count_;
    } else if (ready_route_count_ > 0) {
      --ready_route_count_;
    }
  }

  void ApplyRouteState(std::uint16_t route,
                       OrderGatewayRouteState state) noexcept {
    switch (state) {
      case OrderGatewayRouteState::kReady:
        route_stopped_[route] = false;
        SetRouteReady(route, true);
        running_ = !AllRoutesStopped();
        return;
      case OrderGatewayRouteState::kNotReady:
        route_stopped_[route] = false;
        SetRouteReady(route, false);
        running_ = !AllRoutesStopped();
        return;
      case OrderGatewayRouteState::kStopped:
        SetRouteReady(route, false);
        route_stopped_[route] = true;
        running_ = !AllRoutesStopped();
        return;
      case OrderGatewayRouteState::kUnknown:
        return;
    }
  }

  void SyncRouteStatesFromHeader() noexcept {
    OrderGatewayShmHeader& header = shm_.header();
    for (std::uint16_t route = 0; route < route_count_; ++route) {
      ApplyRouteState(route, LoadOrderGatewayRouteState(header, route));
    }
  }

  template <typename RuntimeT>
  [[nodiscard]] std::uint64_t SyncRouteStatesFromHeader(
      RuntimeT& runtime) noexcept {
    std::uint64_t handled = 0;
    OrderGatewayShmHeader& header = shm_.header();
    for (std::uint16_t route = 0; route < route_count_; ++route) {
      const OrderGatewayRouteState state =
          LoadOrderGatewayRouteState(header, route);
      if (state == OrderGatewayRouteState::kStopped) {
        handled += DrainAndHandleStoppedRoute(route, runtime);
      } else {
        ApplyRouteState(route, state);
      }
    }
    return handled;
  }

  [[nodiscard]] std::uint16_t ResolvePlaceRoute(
      std::uint16_t route_id) noexcept {
    if (route_id != kAutoGatewayRoute) {
      return route_id;
    }
    if (route_count_ == 0) {
      return std::numeric_limits<std::uint16_t>::max();
    }
    for (std::uint16_t attempt = 0; attempt < route_count_; ++attempt) {
      const std::uint16_t candidate = static_cast<std::uint16_t>(
          (next_auto_route_ + attempt) % route_count_);
      if (RouteReady(candidate)) {
        next_auto_route_ =
            static_cast<std::uint16_t>((candidate + 1U) % route_count_);
        return candidate;
      }
    }
    return route_count_;
  }

  [[nodiscard]] OrderGatewaySendResult EnqueueOrderCommand(
      const OrderGatewayCommand& command, std::uint16_t route,
      std::uint64_t local_order_id, bool record_route) noexcept {
    bool route_recorded = false;
    bool had_previous_route = false;
    std::uint16_t previous_route = 0;
    if (record_route &&
        !RecordRouteBeforeEnqueue(local_order_id, route, &route_recorded,
                                  &had_previous_route, &previous_route)) {
      return Failure(OrderGatewaySendStatus::kRouteTableFull);
    }
    if (!command_queues_[route].TryPush(command)) {
      if (route_recorded) {
        RollbackRecordedRoute(local_order_id, had_previous_route,
                              previous_route);
      }
      ++stats_.command_enqueue_failures;
      return Failure(OrderGatewaySendStatus::kCommandQueueFull);
    }
    ++stats_.commands_enqueued;
    return OrderGatewaySendResult{
        .status = OrderGatewaySendStatus::kOk,
        .command_seq = command.command_seq,
        .send_local_ns = command.owner_enqueue_ns,
    };
  }

  template <typename RuntimeT>
  void HandleEvent(std::uint16_t route, const OrderGatewayEvent& event,
                   bool clear_unknown_result_route,
                   RuntimeT& runtime) noexcept {
    switch (event.kind) {
      case OrderGatewayEventKind::kReady:
        ++stats_.ready_events;
        ApplyRouteState(route, OrderGatewayRouteState::kReady);
        return;
      case OrderGatewayEventKind::kNotReady:
        ++stats_.not_ready_events;
        ApplyRouteState(route, OrderGatewayRouteState::kNotReady);
        return;
      case OrderGatewayEventKind::kStopped:
        ++stats_.stopped_events;
        (void)HandleRouteStopped(route, runtime);
        return;
      case OrderGatewayEventKind::kCommandRejected:
        ++stats_.command_rejected_events;
        if (event.command_kind == OrderGatewayCommandKind::kPlace) {
          route_table_.erase(event.local_order_id);
        }
        runtime.OnOrderResponse(ToOrderResponseEvent(
            event, CommandRejectedResponseKind(event.command_kind)));
        return;
      case OrderGatewayEventKind::kOrderResponse:
        ++stats_.order_response_events;
        if (OrderResponseClearsRoute(event.response_kind,
                                     clear_unknown_result_route)) {
          route_table_.erase(event.local_order_id);
        }
        runtime.OnOrderResponse(ToOrderResponseEvent(event));
        return;
      case OrderGatewayEventKind::kNone:
        return;
    }
  }

  [[nodiscard]] static bool OrderResponseClearsRoute(
      OrderResponseKind kind, bool clear_unknown_result_route) noexcept {
    return kind == OrderResponseKind::kRejected ||
           (clear_unknown_result_route &&
            kind == OrderResponseKind::kUnknownResult);
  }

  struct DrainRouteResult {
    std::uint64_t handled{0};
    bool stopped_seen{false};
  };

  template <typename RuntimeT>
  [[nodiscard]] DrainRouteResult DrainRouteEvents(std::uint16_t route,
                                                  std::uint64_t max_events,
                                                  bool defer_stopped_unknown,
                                                  RuntimeT& runtime) noexcept {
    DrainRouteResult result;
    OrderGatewayEvent event{};
    bool clear_unknown_result_route =
        defer_stopped_unknown && route_stopped_[route];
    while (result.handled < max_events && event_queues_[route].TryPop(&event)) {
      ++result.handled;
      if (defer_stopped_unknown &&
          event.kind == OrderGatewayEventKind::kStopped) {
        ++stats_.stopped_events;
        ApplyRouteState(route, OrderGatewayRouteState::kStopped);
        result.stopped_seen = true;
        clear_unknown_result_route = true;
        continue;
      }
      HandleEvent(route, event, clear_unknown_result_route, runtime);
    }
    return result;
  }

  template <typename RuntimeT>
  [[nodiscard]] std::uint64_t DrainAndHandleStoppedRoute(
      std::uint16_t route, RuntimeT& runtime) noexcept {
    std::uint64_t handled = 0;
    ApplyRouteState(route, OrderGatewayRouteState::kStopped);
    DrainRouteResult drained =
        DrainRouteEvents(route, event_queues_[route].capacity(),
                         /*defer_stopped_unknown=*/true, runtime);
    handled += drained.handled;
    handled += HandleRouteStopped(route, runtime);
    return handled;
  }

  [[nodiscard]] bool AllRoutesStopped() const noexcept {
    for (std::uint16_t route = 0; route < route_count_; ++route) {
      if (!route_stopped_[route]) {
        return false;
      }
    }
    return route_count_ != 0;
  }

  template <typename RuntimeT>
  [[nodiscard]] std::uint64_t HandleRouteStopped(std::uint16_t route,
                                                 RuntimeT& runtime) noexcept {
    ApplyRouteState(route, OrderGatewayRouteState::kStopped);
    std::uint64_t synthetic_count = 0;
    while (true) {
      auto route_order = route_table_.end();
      for (auto it = route_table_.begin(); it != route_table_.end(); ++it) {
        if (it->second == route) {
          route_order = it;
          break;
        }
      }
      if (route_order == route_table_.end()) {
        return synthetic_count;
      }
      const std::uint64_t local_order_id = route_order->first;
      route_table_.erase(route_order);
      ++synthetic_count;
      runtime.OnOrderResponse(OrderResponseEvent{
          .kind = OrderResponseKind::kUnknownResult,
          .local_order_id = local_order_id,
          .route_id = route,
          .local_receive_ns = NowNs(),
      });
    }
  }

  [[nodiscard]] static OrderResponseEvent ToOrderResponseEvent(
      const OrderGatewayEvent& event,
      OrderResponseKind kind_override = OrderResponseKind::kAck) noexcept {
    const OrderResponseKind kind = kind_override == OrderResponseKind::kAck
                                       ? event.response_kind
                                       : kind_override;
    return OrderResponseEvent{
        .kind = kind,
        .local_order_id = event.local_order_id,
        .parent_id = event.parent_id,
        .exchange_order_id = event.exchange_order_id,
        .route_id = event.route_id,
        .local_receive_ns = event.local_receive_ns != 0
                                ? event.local_receive_ns
                                : event.worker_event_enqueue_ns,
        .exchange_ns = event.exchange_ns,
    };
  }

  [[nodiscard]] std::uint64_t NextCommandSeq() noexcept {
    return ++command_seq_;
  }

  [[nodiscard]] bool CanRecordRoute(
      std::uint64_t local_order_id) const noexcept {
    if (route_table_.contains(local_order_id)) {
      return true;
    }
    return route_table_.size() < route_table_capacity_;
  }

  [[nodiscard]] bool RecordRouteBeforeEnqueue(
      std::uint64_t local_order_id, std::uint16_t route, bool* route_recorded,
      bool* had_previous_route, std::uint16_t* previous_route) noexcept {
    auto existing = route_table_.find(local_order_id);
    if (existing != route_table_.end()) {
      *route_recorded = true;
      *had_previous_route = true;
      *previous_route = existing->second;
      existing->second = route;
      return true;
    }
    if (route_table_.size() >= route_table_capacity_) {
      ++stats_.route_table_full_rejections;
      return false;
    }
    try {
      const auto insert_result =
          route_table_.try_emplace(local_order_id, route);
      if (!insert_result.second) {
        ++stats_.route_table_full_rejections;
        return false;
      }
    } catch (...) {
      ++stats_.route_table_full_rejections;
      return false;
    }
    *route_recorded = true;
    *had_previous_route = false;
    *previous_route = 0;
    return true;
  }

  void RollbackRecordedRoute(std::uint64_t local_order_id,
                             bool had_previous_route,
                             std::uint16_t previous_route) noexcept {
    auto existing = route_table_.find(local_order_id);
    if (existing == route_table_.end()) {
      return;
    }
    if (had_previous_route) {
      existing->second = previous_route;
      return;
    }
    route_table_.erase(existing);
  }

  [[nodiscard]] static OrderResponseKind CommandRejectedResponseKind(
      OrderGatewayCommandKind command_kind) noexcept {
    if (command_kind == OrderGatewayCommandKind::kCancel) {
      return OrderResponseKind::kCancelRejected;
    }
    return OrderResponseKind::kRejected;
  }

  [[nodiscard]] static OrderGatewaySendResult Failure(
      OrderGatewaySendStatus status) noexcept {
    return {.status = status, .command_seq = 0, .send_local_ns = 0};
  }

  [[nodiscard]] static std::int64_t NowNs() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
  }

  OrderGatewayShmManager shm_;
  OrderGatewayClientOptions options_{};
  std::array<OrderGatewayCommandQueue, kMaxOrderGatewayRoutes>
      command_queues_{};
  std::array<OrderGatewayEventQueue, kMaxOrderGatewayRoutes> event_queues_{};
  std::array<bool, kMaxOrderGatewayRoutes> route_ready_{};
  std::array<bool, kMaxOrderGatewayRoutes> route_stopped_{};
  absl::flat_hash_map<std::uint64_t, std::uint16_t> route_table_;
  OrderGatewayClientStats stats_{};
  std::size_t route_table_capacity_{0};
  std::uint64_t command_seq_{0};
  std::uint16_t route_count_{0};
  std::uint16_t ready_route_count_{0};
  std::uint16_t next_auto_route_{0};
  std::uint32_t startup_ready_timeout_s_{30};
  bool running_{false};
};

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_GATEWAY_CLIENT_H_
