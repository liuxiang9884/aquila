#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <magic_enum/magic_enum.hpp>

#include "core/common/order_ack_diagnostic_level.h"
#include "core/trading/order_latency.h"
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/socket_diagnostics.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/trading/decimal_size_header.h"
#include "exchange/gate/trading/order_latency_diagnostics.h"
#include "exchange/gate/trading/order_request_encoder.h"
#include "exchange/gate/trading/submit_response_parser.h"
#include "nova/utils/log.h"
#include <simdjson.h>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>

#include <sched.h>
#endif

namespace aquila::gate {

struct LoginCredentials {
  std::string api_key;
  std::string api_secret;
};

template <typename OrderT>
[[nodiscard]] std::string_view SignedOrderSizeTextForGate(
    const OrderT& order, std::span<char> buffer) noexcept {
  const std::string_view quantity_text = order.quantity_text;
  if (quantity_text.empty()) {
    return {};
  }
  if (quantity_text.front() == '-') {
    return order.side == OrderSide::kSell ? quantity_text : std::string_view{};
  }
  if (order.side == OrderSide::kBuy) {
    return quantity_text;
  }
  if (buffer.size() < quantity_text.size() + 1) {
    return {};
  }
  buffer[0] = '-';
  std::memcpy(buffer.data() + 1, quantity_text.data(), quantity_text.size());
  return std::string_view(buffer.data(), quantity_text.size() + 1);
}

class NoopOrderSessionDiagnostics {
 public:
  static constexpr bool kEnabled = false;

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return kStats;
  }

 private:
  inline static constexpr OrderSessionStats kStats{};
};

class OrderSessionDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  void RecordTextMessage() noexcept {
    ++stats_.text_messages;
  }
  void RecordParseError() noexcept {
    ++stats_.parse_errors;
  }
  void RecordIgnoredMessage() noexcept {
    ++stats_.ignored_messages;
  }
  void RecordLoginSent() noexcept {
    ++stats_.login_sent;
  }
  void RecordLoginAccepted() noexcept {
    ++stats_.login_accepted;
  }
  void RecordLoginRejected() noexcept {
    ++stats_.login_rejected;
  }
  void RecordPlaceSent() noexcept {
    ++stats_.place_sent;
  }
  void RecordCancelSent() noexcept {
    ++stats_.cancel_sent;
  }
  void RecordResponse() noexcept {
    ++stats_.responses;
  }
  void RecordUnknownRequestId() noexcept {
    ++stats_.unknown_request_ids;
  }
  void RecordLocalSendFailure() noexcept {
    ++stats_.local_send_failures;
  }

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  OrderSessionStats stats_{};
};

struct OrderSessionDefaultTlsWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::TlsSocket;
};

struct OrderSessionDefaultPlainWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::PlainSocket;
};

struct OrderSessionSocketDiagnosticsConfig {
  bool enable_tcp_info{false};
  OrderLatencyDiagnosticConfig ack_latency{};
};

namespace detail {

[[nodiscard]] inline std::uint64_t NextOrderSessionIdForDiagnostics() noexcept {
  static std::atomic_uint64_t next_id{1};
  return next_id.fetch_add(1, std::memory_order_relaxed);
}

[[nodiscard]] inline int CurrentCpuForOrderSessionDiagnostics() noexcept {
#if defined(__linux__)
  const int cpu = ::sched_getcpu();
  return cpu >= 0 ? cpu : -1;
#else
  return -1;
#endif
}

[[nodiscard]] inline int CurrentTidForOrderSessionDiagnostics() noexcept {
#if defined(__linux__)
  const long tid = ::syscall(SYS_gettid);
  return tid > 0 && tid <= static_cast<long>(std::numeric_limits<int>::max())
             ? static_cast<int>(tid)
             : -1;
#else
  return -1;
#endif
}

#if defined(AQUILA_GATE_ORDER_SESSION_ENABLE_TEST_HOOKS)
struct OrderSessionConnectionLogRecordForTest {
  std::uint64_t order_session_id{0};
  int owner_thread_cpu{-1};
  int owner_thread_tid{-1};
  bool endpoint_available{false};
  std::string local_ip;
  std::uint16_t local_port{0};
  std::string remote_ip;
  std::uint16_t remote_port{0};
};

struct OrderSessionSendLogRecordForTest {
  std::uint64_t order_session_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  int send_cpu{-1};
};

struct OrderSessionResponseLogRecordForTest {
  std::uint64_t order_session_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  int ack_cpu{-1};
  std::int64_t exchange_x_in_ns{0};
  std::int64_t exchange_x_out_ns{0};
  std::int64_t exchange_x_in_to_x_out_ns{0};
  bool tcp_info_requested{false};
  bool tcp_info_available{false};
  std::uint32_t tcp_info_rtt_us{0};
  std::uint32_t tcp_info_rttvar_us{0};
  std::uint32_t tcp_info_retrans{0};
  std::uint32_t tcp_info_total_retrans{0};
  std::uint32_t tcp_info_unacked{0};
  std::uint32_t tcp_info_snd_cwnd{0};
};

struct OrderSessionLatencyDiagnosticLogRecordForTest {
  std::uint64_t order_session_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  int diagnostic_cpu{-1};
  bool tcp_info_requested{false};
  bool tcp_info_available{false};
  std::uint32_t tcp_info_rtt_us{0};
  std::uint32_t tcp_info_rttvar_us{0};
  std::uint32_t tcp_info_retrans{0};
  std::uint32_t tcp_info_total_retrans{0};
  std::uint32_t tcp_info_unacked{0};
  std::uint32_t tcp_info_snd_cwnd{0};
  bool ts_available{false};
  std::int64_t ts_write_complete_ns{0};
  std::int64_t ts_tx_sched_ns{0};
  std::int64_t ts_tx_software_ns{0};
  std::int64_t ts_tx_ack_ns{0};
  std::int64_t ts_rx_software_ns{0};
  std::int64_t ts_ack_receive_local_ns{0};
  std::int64_t ts_write_to_tx_software_ns{-1};
  std::int64_t ts_tx_software_to_tx_ack_ns{-1};
  std::int64_t ts_tx_ack_to_rx_software_ns{-1};
  std::int64_t ts_rx_software_to_ack_receive_ns{-1};
};

using OrderSessionConnectionLogObserverForTest =
    void (*)(const OrderSessionConnectionLogRecordForTest& record) noexcept;
using OrderSessionSendLogObserverForTest =
    void (*)(const OrderSessionSendLogRecordForTest& record) noexcept;
using OrderSessionResponseLogObserverForTest =
    void (*)(const OrderSessionResponseLogRecordForTest& record) noexcept;
using OrderSessionLatencyDiagnosticLogObserverForTest = void (*)(
    const OrderSessionLatencyDiagnosticLogRecordForTest& record) noexcept;

[[nodiscard]] inline OrderSessionConnectionLogObserverForTest&
OrderSessionConnectionLogObserverSlotForTest() noexcept {
  static OrderSessionConnectionLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline OrderSessionSendLogObserverForTest&
OrderSessionSendLogObserverSlotForTest() noexcept {
  static OrderSessionSendLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline OrderSessionResponseLogObserverForTest&
OrderSessionResponseLogObserverSlotForTest() noexcept {
  static OrderSessionResponseLogObserverForTest observer = nullptr;
  return observer;
}

[[nodiscard]] inline OrderSessionLatencyDiagnosticLogObserverForTest&
OrderSessionLatencyDiagnosticLogObserverSlotForTest() noexcept {
  static OrderSessionLatencyDiagnosticLogObserverForTest observer = nullptr;
  return observer;
}

inline void SetOrderSessionConnectionLogObserverForTest(
    OrderSessionConnectionLogObserverForTest observer) noexcept {
  OrderSessionConnectionLogObserverSlotForTest() = observer;
}

inline void SetOrderSessionSendLogObserverForTest(
    OrderSessionSendLogObserverForTest observer) noexcept {
  OrderSessionSendLogObserverSlotForTest() = observer;
}

inline void SetOrderSessionResponseLogObserverForTest(
    OrderSessionResponseLogObserverForTest observer) noexcept {
  OrderSessionResponseLogObserverSlotForTest() = observer;
}

inline void SetOrderSessionLatencyDiagnosticLogObserverForTest(
    OrderSessionLatencyDiagnosticLogObserverForTest observer) noexcept {
  OrderSessionLatencyDiagnosticLogObserverSlotForTest() = observer;
}

inline void NotifyOrderSessionConnectionLogObserverForTest(
    std::uint64_t order_session_id, int owner_thread_cpu, int owner_thread_tid,
    const websocket::SocketEndpointDiagnostics& endpoints) noexcept {
  OrderSessionConnectionLogObserverForTest observer =
      OrderSessionConnectionLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(OrderSessionConnectionLogRecordForTest{
      .order_session_id = order_session_id,
      .owner_thread_cpu = owner_thread_cpu,
      .owner_thread_tid = owner_thread_tid,
      .endpoint_available = endpoints.available,
      .local_ip = endpoints.local_ip.data(),
      .local_port = endpoints.local_port,
      .remote_ip = endpoints.remote_ip.data(),
      .remote_port = endpoints.remote_port,
  });
}

inline void NotifyOrderSessionSendLogObserverForTest(
    std::uint64_t order_session_id, std::uint64_t local_order_id,
    std::uint64_t request_sequence, int send_cpu) noexcept {
  OrderSessionSendLogObserverForTest observer =
      OrderSessionSendLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(OrderSessionSendLogRecordForTest{
      .order_session_id = order_session_id,
      .local_order_id = local_order_id,
      .request_sequence = request_sequence,
      .send_cpu = send_cpu,
  });
}

inline void NotifyOrderSessionResponseLogObserverForTest(
    std::uint64_t order_session_id, std::uint64_t local_order_id,
    std::uint64_t request_sequence, int ack_cpu, std::int64_t exchange_x_in_ns,
    std::int64_t exchange_x_out_ns, std::int64_t exchange_x_in_to_x_out_ns,
    bool tcp_info_requested,
    const websocket::TcpInfoDiagnostics& tcp_info) noexcept {
  OrderSessionResponseLogObserverForTest observer =
      OrderSessionResponseLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(OrderSessionResponseLogRecordForTest{
      .order_session_id = order_session_id,
      .local_order_id = local_order_id,
      .request_sequence = request_sequence,
      .ack_cpu = ack_cpu,
      .exchange_x_in_ns = exchange_x_in_ns,
      .exchange_x_out_ns = exchange_x_out_ns,
      .exchange_x_in_to_x_out_ns = exchange_x_in_to_x_out_ns,
      .tcp_info_requested = tcp_info_requested,
      .tcp_info_available = tcp_info.available,
      .tcp_info_rtt_us = tcp_info.rtt_us,
      .tcp_info_rttvar_us = tcp_info.rttvar_us,
      .tcp_info_retrans = tcp_info.retrans,
      .tcp_info_total_retrans = tcp_info.total_retrans,
      .tcp_info_unacked = tcp_info.unacked,
      .tcp_info_snd_cwnd = tcp_info.snd_cwnd,
  });
}

inline void NotifyOrderSessionLatencyDiagnosticLogObserverForTest(
    std::uint64_t order_session_id,
    const OrderLatencyDiagnosticLogRecord& record, int diagnostic_cpu,
    bool tcp_info_requested,
    const websocket::TcpInfoDiagnostics& tcp_info) noexcept {
  OrderSessionLatencyDiagnosticLogObserverForTest observer =
      OrderSessionLatencyDiagnosticLogObserverSlotForTest();
  if (observer == nullptr) {
    return;
  }
  observer(OrderSessionLatencyDiagnosticLogRecordForTest{
      .order_session_id = order_session_id,
      .local_order_id = record.local_order_id,
      .request_sequence = record.request_sequence,
      .diagnostic_cpu = diagnostic_cpu,
      .tcp_info_requested = tcp_info_requested,
      .tcp_info_available = tcp_info.available,
      .tcp_info_rtt_us = tcp_info.rtt_us,
      .tcp_info_rttvar_us = tcp_info.rttvar_us,
      .tcp_info_retrans = tcp_info.retrans,
      .tcp_info_total_retrans = tcp_info.total_retrans,
      .tcp_info_unacked = tcp_info.unacked,
      .tcp_info_snd_cwnd = tcp_info.snd_cwnd,
      .ts_available = record.socket_timestamps.available,
      .ts_write_complete_ns = record.socket_timestamps.write_complete_ns,
      .ts_tx_sched_ns = record.socket_timestamps.tx_sched_ns,
      .ts_tx_software_ns = record.socket_timestamps.tx_software_ns,
      .ts_tx_ack_ns = record.socket_timestamps.tx_ack_ns,
      .ts_rx_software_ns = record.socket_timestamps.rx_software_ns,
      .ts_ack_receive_local_ns = record.socket_timestamps.ack_receive_local_ns,
      .ts_write_to_tx_software_ns =
          record.socket_timestamp_stages.write_complete_to_tx_software_ns,
      .ts_tx_software_to_tx_ack_ns =
          record.socket_timestamp_stages.tx_software_to_tx_ack_ns,
      .ts_tx_ack_to_rx_software_ns =
          record.socket_timestamp_stages.tx_ack_to_rx_software_ns,
      .ts_rx_software_to_ack_receive_ns =
          record.socket_timestamp_stages.rx_software_to_ack_receive_ns,
  });
}
#endif

}  // namespace detail

template <typename ResponseHandler,
          typename WebSocketPolicy = OrderSessionDefaultTlsWebSocketPolicy,
          typename Diagnostics = NoopOrderSessionDiagnostics>
class OrderSession {
 public:
  using TransportSocket = typename WebSocketPolicy::TransportSocket;
  using MessageHandler = websocket::MessageHandlerRef<OrderSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocket, MessageHandler>;
  static constexpr bool DiagnosticsEnabled = Diagnostics::kEnabled;
  static constexpr websocket::ClockSource kClockSource =
      WebSocketPolicy::kClockSource;

  OrderSession(
      websocket::ConnectionConfig config, LoginCredentials credentials,
      ResponseHandler& response_handler,
      std::size_t request_map_capacity = kDefaultOrderRequestMapCapacity,
      OrderSessionSocketDiagnosticsConfig socket_diagnostics_config = {})
      : connection_(ApplyOptions(std::move(config))),
        quote_order_size_(HasGateSizeDecimalHeader(connection_)),
        credentials_(std::move(credentials)),
        socket_diagnostics_config_(socket_diagnostics_config),
        ack_latency_diagnostics_(socket_diagnostics_config_.ack_latency),
        response_handler_(response_handler),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_),
        request_map_capacity_(request_map_capacity == 0
                                  ? kDefaultOrderRequestMapCapacity
                                  : request_map_capacity) {
    request_id_to_local_order_id_.reserve(request_map_capacity_);
    local_order_id_to_exchange_order_id_.reserve(request_map_capacity_);
    ack_latency_diagnostics_.reserve(request_map_capacity_);
    client_.SetStateHook(this, &HandleState);
    client_.SetRuntimeLoopProbe(this, &HandleRuntimeLoopProbe);
  }

  bool Start() noexcept {
    return client_.Start();
  }

  void Stop() noexcept {
    client_.Stop();
  }

  void Wakeup() noexcept {
    client_.Wakeup();
  }

  void SetRuntimeHook(void* context, websocket::RuntimeHook handler) noexcept {
    client_.SetRuntimeHook(context, handler);
  }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    if (view.kind != websocket::PayloadKind::kText) {
      return websocket::DeliveryResult::kAccepted;
    }
    return HandleText(view);
  }

  void OnConnectionPhase(websocket::ConnectionPhase phase) noexcept {
    if (phase == websocket::ConnectionPhase::kActive) {
      active_ = true;
      const int owner_thread_cpu =
          detail::CurrentCpuForOrderSessionDiagnostics();
      const int owner_thread_tid =
          detail::CurrentTidForOrderSessionDiagnostics();
      const websocket::SocketEndpointDiagnostics endpoints =
          SnapshotSocketEndpointDiagnostics();
      LogGateOrderSessionConnected(order_session_id_, owner_thread_cpu,
                                   owner_thread_tid, endpoints);
      NotifyOrderSessionConnected(owner_thread_cpu, owner_thread_tid,
                                  endpoints);
      (void)SendLogin();
      return;
    }
    if (phase == websocket::ConnectionPhase::kDisconnected ||
        phase == websocket::ConnectionPhase::kReconnectBackoff ||
        phase == websocket::ConnectionPhase::kClosing ||
        phase == websocket::ConnectionPhase::kClosed) {
      active_ = false;
      login_ready_ = false;
      login_request_sequence_ = 0;
      request_id_to_local_order_id_.clear();
      local_order_id_to_exchange_order_id_.clear();
      ack_latency_diagnostics_.clear();
      NotifyLoginNotReady();
    }
  }

  template <typename OrderT>
  OrderSendResult PlaceOrder(const OrderT& order) noexcept {
    if (!active_) {
      LogGateOrderSendFailed("place", OrderSendStatus::kNotActive,
                             order.local_order_id, active_, login_ready_,
                             inflight_count(), request_map_capacity_);
      return EarlyLocalReject(OrderSendStatus::kNotActive, true);
    }
    if (!login_ready_) {
      LogGateOrderSendFailed("place", OrderSendStatus::kNotLoggedIn,
                             order.local_order_id, active_, login_ready_,
                             inflight_count(), request_map_capacity_);
      return EarlyLocalReject(OrderSendStatus::kNotLoggedIn, false);
    }
    if (request_id_to_local_order_id_.size() >= request_map_capacity_) {
      LogGateOrderSendFailed("place", OrderSendStatus::kInflightFull,
                             order.local_order_id, active_, login_ready_,
                             inflight_count(), request_map_capacity_);
      return EarlyLocalReject(OrderSendStatus::kInflightFull, true);
    }
    const OrderType order_type = OrderTypeForGate(order);
    if (order_type != OrderType::kLimit) {
      LogGateOrderSendFailed("place", OrderSendStatus::kUnsupportedOrderType,
                             order.local_order_id, active_, login_ready_,
                             inflight_count(), request_map_capacity_);
      return EarlyLocalReject(OrderSendStatus::kUnsupportedOrderType, true);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
    std::array<char, 64> signed_size_buffer{};
    const std::string_view signed_size_text = SignedOrderSizeTextForGate(
        order,
        std::span<char>(signed_size_buffer.data(), signed_size_buffer.size()));
    std::array<char, kPlaceOrderRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodePlaceOrderRequest(
        PlaceOrderEncodeFields{.timestamp = NowSeconds(),
                               .encoded_request_id = encoded_request_id,
                               .local_order_id = order.local_order_id,
                               .order_type = order_type,
                               .contract = order.symbol,
                               .signed_size_text = signed_size_text,
                               .price_text = order.price_text,
                               .time_in_force = order.time_in_force,
                               .reduce_only = order.reduce_only,
                               .quote_size = quote_order_size_},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      const OrderSendStatus status = MapEncodeStatus(encoded.status);
      LogGateOrderSendFailed("place", status, order.local_order_id, active_,
                             login_ready_, inflight_count(),
                             request_map_capacity_);
      return SendFailure(status, sequence, encoded_request_id);
    }

    websocket::WritePathDiagnostics write_path{};
    if constexpr (core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      write_path.order_encode_done_ns = RealtimeNowNsInt64();
    }
    const int send_cpu = detail::CurrentCpuForOrderSessionDiagnostics();
    const std::int64_t send_local_ns = RealtimeNowNsInt64();
    const bool socket_timestamping_probe_started =
        core::kOrderAckDiagnosticSocketTimestampingEnabled
            ? client_.Core().StartSocketTimestampingProbe(sequence)
            : false;
    const OrderSendStatus status = MapSendStatus(
        SendText(encoded.text, core::kOrderAckDiagnosticRuntimeWritePathEnabled
                                   ? &write_path
                                   : nullptr));
    const bool socket_timestamping_probe_active =
        UpdateSocketTimestampingProbeAfterSend(
            sequence, status, write_path, socket_timestamping_probe_started);
    if (status != OrderSendStatus::kOk) {
      LogGateOrderSendFailed("place", status, order.local_order_id, active_,
                             login_ready_, inflight_count(),
                             request_map_capacity_);
      return SendFailure(status, sequence, encoded_request_id);
    }
    const websocket::SocketSendQueueDiagnostics socket_send_queue =
        SnapshotSocketSendQueueDiagnostics();
    request_id_to_local_order_id_.emplace(sequence, order.local_order_id);
    ArmAckLatencyDiagnostic(order.local_order_id, sequence, send_local_ns,
                            write_path, socket_send_queue,
                            MakeSocketTimestampingSendSnapshot(
                                write_path, socket_timestamping_probe_active));
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPlaceSent();
    }
    LogGatePlaceOrderSent(order, sequence, encoded_request_id, inflight_count(),
                          send_local_ns, order_session_id_, send_cpu);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id,
            .send_local_ns = send_local_ns};
  }

  template <typename OrderT>
  OrderSendResult CancelOrder(const OrderT& order) noexcept {
    if (!active_) {
      LogGateOrderSendFailed("cancel", OrderSendStatus::kNotActive,
                             order.local_order_id, active_, login_ready_,
                             inflight_count(), request_map_capacity_);
      return EarlyLocalReject(OrderSendStatus::kNotActive, true);
    }
    if (!login_ready_) {
      LogGateOrderSendFailed("cancel", OrderSendStatus::kNotLoggedIn,
                             order.local_order_id, active_, login_ready_,
                             inflight_count(), request_map_capacity_);
      return EarlyLocalReject(OrderSendStatus::kNotLoggedIn, false);
    }
    if (request_id_to_local_order_id_.size() >= request_map_capacity_) {
      LogGateOrderSendFailed("cancel", OrderSendStatus::kInflightFull,
                             order.local_order_id, active_, login_ready_,
                             inflight_count(), request_map_capacity_);
      return EarlyLocalReject(OrderSendStatus::kInflightFull, true);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kCancelOrder, sequence);
    const std::uint64_t exchange_order_id = ExchangeOrderIdForCancel(order);
    std::array<char, kCancelOrderRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(
        CancelOrderEncodeFields{.timestamp = NowSeconds(),
                                .encoded_request_id = encoded_request_id,
                                .local_order_id = order.local_order_id,
                                .exchange_order_id = exchange_order_id},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      const OrderSendStatus status = MapEncodeStatus(encoded.status);
      LogGateOrderSendFailed("cancel", status, order.local_order_id, active_,
                             login_ready_, inflight_count(),
                             request_map_capacity_);
      return SendFailure(status, sequence, encoded_request_id);
    }

    websocket::WritePathDiagnostics write_path{};
    if constexpr (core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      write_path.order_encode_done_ns = RealtimeNowNsInt64();
    }
    const int send_cpu = detail::CurrentCpuForOrderSessionDiagnostics();
    const std::int64_t send_local_ns = RealtimeNowNsInt64();
    const bool socket_timestamping_probe_started =
        core::kOrderAckDiagnosticSocketTimestampingEnabled
            ? client_.Core().StartSocketTimestampingProbe(sequence)
            : false;
    const OrderSendStatus status = MapSendStatus(
        SendText(encoded.text, core::kOrderAckDiagnosticRuntimeWritePathEnabled
                                   ? &write_path
                                   : nullptr));
    const bool socket_timestamping_probe_active =
        UpdateSocketTimestampingProbeAfterSend(
            sequence, status, write_path, socket_timestamping_probe_started);
    if (status != OrderSendStatus::kOk) {
      LogGateOrderSendFailed("cancel", status, order.local_order_id, active_,
                             login_ready_, inflight_count(),
                             request_map_capacity_);
      return SendFailure(status, sequence, encoded_request_id);
    }
    const websocket::SocketSendQueueDiagnostics socket_send_queue =
        SnapshotSocketSendQueueDiagnostics();
    request_id_to_local_order_id_.emplace(sequence, order.local_order_id);
    ArmAckLatencyDiagnostic(order.local_order_id, sequence, send_local_ns,
                            write_path, socket_send_queue,
                            MakeSocketTimestampingSendSnapshot(
                                write_path, socket_timestamping_probe_active));
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordCancelSent();
    }
    LogGateCancelOrderSent(order.local_order_id, exchange_order_id, sequence,
                           encoded_request_id, inflight_count(), send_local_ns,
                           order_session_id_, send_cpu);
    return {.status = OrderSendStatus::kOk,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id,
            .send_local_ns = send_local_ns};
  }

  [[nodiscard]] bool login_ready() const noexcept {
    return login_ready_;
  }

  [[nodiscard]] std::size_t inflight_count() const noexcept {
    return request_id_to_local_order_id_.size();
  }

  [[nodiscard]] std::size_t request_map_capacity() const noexcept {
    return request_map_capacity_;
  }

  [[nodiscard]] std::uint64_t order_session_id() const noexcept {
    return order_session_id_;
  }

  [[nodiscard]] std::uint64_t exchange_order_id_for_local_order(
      std::uint64_t local_order_id) const noexcept {
    const auto it = local_order_id_to_exchange_order_id_.find(local_order_id);
    if (it == local_order_id_to_exchange_order_id_.end()) {
      return 0;
    }
    return it->second;
  }

  [[nodiscard]] bool forget_exchange_order_id_for_local_order(
      std::uint64_t local_order_id) noexcept {
    return local_order_id_to_exchange_order_id_.erase(local_order_id) != 0;
  }

  void CacheExchangeOrderId(std::uint64_t local_order_id,
                            std::uint64_t exchange_order_id) noexcept {
    auto it = local_order_id_to_exchange_order_id_.find(local_order_id);
    if (it != local_order_id_to_exchange_order_id_.end()) {
      it->second = exchange_order_id;
      return;
    }
    if (local_order_id_to_exchange_order_id_.size() >= request_map_capacity_) {
      return;
    }
    local_order_id_to_exchange_order_id_.emplace(local_order_id,
                                                 exchange_order_id);
  }

  void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept {
    (void)local_order_id_to_exchange_order_id_.erase(local_order_id);
  }

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return diagnostics_.stats();
  }

 private:
  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<OrderSession*>(context)->OnConnectionPhase(phase);
  }

  static void HandleRuntimeLoopProbe(
      void* context, websocket::RuntimeLoopProbePoint point) noexcept {
    static_cast<OrderSession*>(context)->OnRuntimeLoopProbe(point);
  }

  static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config) {
    config.runtime_policy.clock_source = kClockSource;
    return config;
  }

  [[nodiscard]] std::int64_t NowSeconds() const noexcept {
    return static_cast<std::int64_t>(std::time(nullptr));
  }

  [[nodiscard]] static std::int64_t RealtimeNowNsInt64() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
  }

  [[nodiscard]] std::uint64_t NextRequestSequence() noexcept {
    return request_sequence_++;
  }

  [[nodiscard]] OrderSendStatus MapSendStatus(
      websocket::SendStatus status) noexcept {
    switch (status) {
      case websocket::SendStatus::kOk:
        return OrderSendStatus::kOk;
      case websocket::SendStatus::kNoPreparedWriteSlot:
        return OrderSendStatus::kNoPreparedWriteSlot;
      case websocket::SendStatus::kWriteUnavailable:
        return OrderSendStatus::kWriteUnavailable;
      case websocket::SendStatus::kEncodeFailed:
      case websocket::SendStatus::kPayloadTooLarge:
        return OrderSendStatus::kEncodeBufferTooSmall;
    }
    return OrderSendStatus::kWriteUnavailable;
  }

  [[nodiscard]] OrderSendStatus MapEncodeStatus(
      OrderEncodeStatus status) noexcept {
    switch (status) {
      case OrderEncodeStatus::kOk:
        return OrderSendStatus::kOk;
      case OrderEncodeStatus::kBufferTooSmall:
        return OrderSendStatus::kEncodeBufferTooSmall;
      case OrderEncodeStatus::kInvalidOrderText:
        return OrderSendStatus::kInvalidLocalOrderId;
      case OrderEncodeStatus::kUnsupportedOrderType:
        return OrderSendStatus::kUnsupportedOrderType;
      case OrderEncodeStatus::kInvalidQuantityText:
        return OrderSendStatus::kInvalidQuantityText;
      case OrderEncodeStatus::kSignatureFailed:
        return OrderSendStatus::kSignatureFailed;
    }
    return OrderSendStatus::kEncodeBufferTooSmall;
  }

  template <typename OrderT>
  [[nodiscard]] static OrderType OrderTypeForGate(
      const OrderT& order) noexcept {
    if constexpr (requires { order.type; }) {
      return order.type;
    }
    return OrderType::kLimit;
  }

  [[nodiscard]] websocket::SendStatus SendText(
      std::string_view payload_text,
      websocket::WritePathDiagnostics* write_path = nullptr) noexcept {
    auto& core = client_.Core();
    const auto payload = std::as_bytes(
        std::span<const char>(payload_text.data(), payload_text.size()));
    return core.SendText(payload, websocket::WriteFlushMode::kTryFlushOne,
                         write_path);
  }

  template <typename OrderT>
  static void LogGatePlaceOrderSent(const OrderT& order,
                                    std::uint64_t request_sequence,
                                    std::uint64_t encoded_request_id,
                                    std::size_t inflight,
                                    std::int64_t request_send_local_ns,
                                    std::uint64_t order_session_id,
                                    int send_cpu) noexcept {
#if defined(AQUILA_GATE_ORDER_SESSION_ENABLE_TEST_HOOKS)
    detail::NotifyOrderSessionSendLogObserverForTest(
        order_session_id, order.local_order_id, request_sequence, send_cpu);
#endif
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_INFO(
        "gate_order_send_ok type=place local_order_id={} "
        "request_sequence={} encoded_request_id={} contract={} side={} "
        "quantity={} price={} tif={} reduce_only={} inflight={} "
        "request_send_local_ns={} order_session_id={} send_cpu={}",
        order.local_order_id, request_sequence, encoded_request_id,
        order.symbol, magic_enum::enum_name(order.side), order.quantity_text,
        order.price_text, magic_enum::enum_name(order.time_in_force),
        order.reduce_only ? "true" : "false", inflight, request_send_local_ns,
        order_session_id, send_cpu);
  }

  static void LogGateCancelOrderSent(
      std::uint64_t local_order_id, std::uint64_t exchange_order_id,
      std::uint64_t request_sequence, std::uint64_t encoded_request_id,
      std::size_t inflight, std::int64_t request_send_local_ns,
      std::uint64_t order_session_id, int send_cpu) noexcept {
#if defined(AQUILA_GATE_ORDER_SESSION_ENABLE_TEST_HOOKS)
    detail::NotifyOrderSessionSendLogObserverForTest(
        order_session_id, local_order_id, request_sequence, send_cpu);
#endif
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_INFO(
        "gate_order_send_ok type=cancel local_order_id={} "
        "exchange_order_id={} request_sequence={} encoded_request_id={} "
        "inflight={} request_send_local_ns={} order_session_id={} "
        "send_cpu={}",
        local_order_id, exchange_order_id, request_sequence, encoded_request_id,
        inflight, request_send_local_ns, order_session_id, send_cpu);
  }

  static void LogGateOrderSendFailed(std::string_view type,
                                     OrderSendStatus status,
                                     std::uint64_t local_order_id, bool active,
                                     bool login_ready, std::size_t inflight,
                                     std::size_t capacity) noexcept {
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_WARNING(
        "gate_order_send_failed type={} status={} local_order_id={} "
        "active={} login_ready={} inflight={} capacity={}",
        type, magic_enum::enum_name(status), local_order_id,
        active ? "true" : "false", login_ready ? "true" : "false", inflight,
        capacity);
  }

  static void LogGateOrderResponse(
      const GateSubmitResponse& parsed, std::uint64_t local_order_id,
      std::uint64_t exchange_order_id, std::int64_t local_receive_ns,
      std::uint64_t order_session_id, int ack_cpu, bool tcp_info_requested,
      const websocket::TcpInfoDiagnostics& tcp_info) noexcept {
#if defined(AQUILA_GATE_ORDER_SESSION_ENABLE_TEST_HOOKS)
    detail::NotifyOrderSessionResponseLogObserverForTest(
        order_session_id, local_order_id, parsed.request_id.sequence, ack_cpu,
        parsed.exchange_x_in_ns, parsed.exchange_x_out_ns,
        parsed.exchange_x_in_to_x_out_ns, tcp_info_requested, tcp_info);
#endif
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    const std::int64_t exchange_to_local_ns =
        ::aquila::core::LatencyDeltaNs(local_receive_ns, parsed.exchange_ns);
    NOVA_INFO(
        "gate_order_response kind={} local_order_id={} exchange_order_id={} "
        "request_sequence={} channel={} http_status={} error_label_hash={} "
        "error_label={} error_message={} local_receive_ns={} exchange_ns={} "
        "exchange_x_in_ns={} exchange_x_out_ns={} "
        "exchange_x_in_to_x_out_ns={} exchange_to_local_ns={} "
        "order_session_id={} ack_cpu={} "
        "tcp_info_requested={} tcp_info_available={} tcp_info_rtt_us={} "
        "tcp_info_rttvar_us={} tcp_info_retrans={} "
        "tcp_info_total_retrans={} tcp_info_unacked={} tcp_info_snd_cwnd={}",
        magic_enum::enum_name(parsed.kind), local_order_id, exchange_order_id,
        parsed.request_id.sequence, static_cast<int>(parsed.channel),
        parsed.http_status, parsed.error_label_hash, parsed.error_label,
        parsed.error_message, local_receive_ns, parsed.exchange_ns,
        parsed.exchange_x_in_ns, parsed.exchange_x_out_ns,
        parsed.exchange_x_in_to_x_out_ns, exchange_to_local_ns,
        order_session_id, ack_cpu, tcp_info_requested ? "true" : "false",
        tcp_info.available ? "true" : "false", tcp_info.rtt_us,
        tcp_info.rttvar_us, tcp_info.retrans, tcp_info.total_retrans,
        tcp_info.unacked, tcp_info.snd_cwnd);
  }

  static void LogGateOrderResponseIgnored(
      std::string_view reason, const GateSubmitResponse& parsed) noexcept {
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_WARNING(
        "gate_order_response_ignored reason={} request_sequence={} "
        "channel={} kind={} http_status={} error_label_hash={} "
        "error_label={} error_message={}",
        reason, parsed.request_id.sequence, static_cast<int>(parsed.channel),
        magic_enum::enum_name(parsed.kind), parsed.http_status,
        parsed.error_label_hash, parsed.error_label, parsed.error_message);
  }

  static void LogGateOrderResponseUnknownRequestId(
      const GateSubmitResponse& parsed) noexcept {
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_WARNING(
        "gate_order_response_unknown_request_id request_sequence={} "
        "channel={} kind={} http_status={} error_label_hash={} "
        "error_label={} error_message={}",
        parsed.request_id.sequence, static_cast<int>(parsed.channel),
        magic_enum::enum_name(parsed.kind), parsed.http_status,
        parsed.error_label_hash, parsed.error_label, parsed.error_message);
  }

  static void LogOrderLatencyDiagnostic(
      const OrderLatencyDiagnosticLogRecord& record,
      std::uint64_t order_session_id, int diagnostic_cpu,
      bool tcp_info_requested,
      const websocket::TcpInfoDiagnostics& tcp_info) noexcept {
#if defined(AQUILA_GATE_ORDER_SESSION_ENABLE_TEST_HOOKS)
    detail::NotifyOrderSessionLatencyDiagnosticLogObserverForTest(
        order_session_id, record, diagnostic_cpu, tcp_info_requested, tcp_info);
#endif
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_WARNING(
        "gate_order_ack_latency_diagnostic reason={} local_order_id={} "
        "request_sequence={} request_send_local_ns={} "
        "ack_local_receive_ns={} ack_exchange_ns={} ack_rtt_ns={} "
        "send_to_first_after_hook_ns={} send_to_first_drive_read_ns={} "
        "drive_read_duration_ns={} max_observed_drive_read_duration_ns={} "
        "inflight_at_send={} order_session_id={} diagnostic_cpu={} "
        "max_runtime_loop_gap_ns={} runtime_loop_iterations_before_ack={} "
        "owner_thread_tid={} order_encode_done_ns={} "
        "ws_frame_encode_done_ns={} write_enqueue_ns={} "
        "drive_write_enter_ns={} write_some_enter_ns={} "
        "write_some_return_ns={} write_complete_ns={} "
        "write_some_bytes={} write_complete_bytes={} write_errno={} "
        "write_eagain={} pending_write_count_after={} "
        "socket_send_queue_available={} tcp_sendq_bytes={} "
        "tcp_notsent_bytes={} "
        "ts_available={} ts_write_complete_ns={} ts_tx_sched_ns={} "
        "ts_tx_software_ns={} ts_tx_ack_ns={} ts_rx_software_ns={} "
        "ts_ack_receive_local_ns={} ts_write_to_tx_software_ns={} "
        "ts_tx_software_to_tx_ack_ns={} "
        "ts_tx_ack_to_rx_software_ns={} "
        "ts_rx_software_to_ack_receive_ns={} "
        "tcp_info_requested={} tcp_info_available={} tcp_info_rtt_us={} "
        "tcp_info_rttvar_us={} tcp_info_retrans={} "
        "tcp_info_total_retrans={} tcp_info_unacked={} tcp_info_snd_cwnd={}",
        magic_enum::enum_name(record.reason), record.local_order_id,
        record.request_sequence, record.request_send_local_ns,
        record.ack_local_receive_ns, record.ack_exchange_ns, record.ack_rtt_ns,
        record.send_to_first_after_hook_ns, record.send_to_first_drive_read_ns,
        record.drive_read_duration_ns,
        record.max_observed_drive_read_duration_ns, record.inflight_at_send,
        order_session_id, diagnostic_cpu, record.max_runtime_loop_gap_ns,
        record.runtime_loop_iterations_before_ack, record.owner_thread_tid,
        record.order_encode_done_ns, record.ws_frame_encode_done_ns,
        record.write_enqueue_ns, record.drive_write_enter_ns,
        record.write_some_enter_ns, record.write_some_return_ns,
        record.write_complete_ns, record.write_some_bytes,
        record.write_complete_bytes, record.write_errno,
        record.write_eagain ? "true" : "false",
        record.pending_write_count_after,
        record.socket_send_queue_available ? "true" : "false",
        record.tcp_sendq_bytes, record.tcp_notsent_bytes,
        record.socket_timestamps.available ? "true" : "false",
        record.socket_timestamps.write_complete_ns,
        record.socket_timestamps.tx_sched_ns,
        record.socket_timestamps.tx_software_ns,
        record.socket_timestamps.tx_ack_ns,
        record.socket_timestamps.rx_software_ns,
        record.socket_timestamps.ack_receive_local_ns,
        record.socket_timestamp_stages.write_complete_to_tx_software_ns,
        record.socket_timestamp_stages.tx_software_to_tx_ack_ns,
        record.socket_timestamp_stages.tx_ack_to_rx_software_ns,
        record.socket_timestamp_stages.rx_software_to_ack_receive_ns,
        tcp_info_requested ? "true" : "false",
        tcp_info.available ? "true" : "false", tcp_info.rtt_us,
        tcp_info.rttvar_us, tcp_info.retrans, tcp_info.total_retrans,
        tcp_info.unacked, tcp_info.snd_cwnd);
  }

  static void LogGateOrderSessionConnected(
      std::uint64_t order_session_id, int owner_thread_cpu,
      int owner_thread_tid,
      const websocket::SocketEndpointDiagnostics& endpoints) noexcept {
#if defined(AQUILA_GATE_ORDER_SESSION_ENABLE_TEST_HOOKS)
    detail::NotifyOrderSessionConnectionLogObserverForTest(
        order_session_id, owner_thread_cpu, owner_thread_tid, endpoints);
#endif
    if (::nova::kLogManager.logger() == nullptr) {
      return;
    }
    NOVA_INFO(
        "gate_order_session_connected order_session_id={} "
        "owner_thread_cpu={} owner_thread_tid={} endpoint_available={} "
        "local_ip={} local_port={} remote_ip={} remote_port={}",
        order_session_id, owner_thread_cpu, owner_thread_tid,
        endpoints.available ? "true" : "false", endpoints.local_ip.data(),
        endpoints.local_port, endpoints.remote_ip.data(),
        endpoints.remote_port);
  }

  template <typename OrderT>
  [[nodiscard]] std::uint64_t ExchangeOrderIdForCancel(
      const OrderT& order) const noexcept {
    const std::uint64_t exchange_order_id =
        exchange_order_id_for_local_order(order.local_order_id);
    if (exchange_order_id != 0) {
      return exchange_order_id;
    }
    if constexpr (requires { order.exchange_order_id; }) {
      return order.exchange_order_id;
    }
    return 0;
  }

  [[nodiscard]] OrderSendResult SendLogin() noexcept {
    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kLogin, sequence);
    std::array<char, kLoginRequestBufferSize> buffer;
    const EncodedTextRequest encoded = EncodeLoginRequest(
        LoginRequestFields{.api_key = credentials_.api_key,
                           .api_secret = credentials_.api_secret,
                           .timestamp = NowSeconds(),
                           .encoded_request_id = encoded_request_id},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return {.status = MapEncodeStatus(encoded.status),
              .request_sequence = sequence,
              .encoded_request_id = encoded_request_id};
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status == OrderSendStatus::kOk) {
      login_request_sequence_ = sequence;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginSent();
      }
    }
    return {.status = status,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  [[nodiscard]] OrderSendResult EarlyLocalReject(OrderSendStatus status,
                                                 bool record_failure) noexcept {
    if constexpr (DiagnosticsEnabled) {
      if (record_failure) {
        diagnostics_.RecordLocalSendFailure();
      }
    }
    return {.status = status, .request_sequence = 0, .encoded_request_id = 0};
  }

  [[nodiscard]] OrderSendResult SendFailure(
      OrderSendStatus status, std::uint64_t sequence,
      std::uint64_t encoded_request_id) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLocalSendFailure();
    }
    return {.status = status,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  void ArmAckLatencyDiagnostic(
      std::uint64_t local_order_id, std::uint64_t sequence,
      std::int64_t send_local_ns,
      const websocket::WritePathDiagnostics& write_path,
      const websocket::SocketSendQueueDiagnostics& socket_send_queue,
      const websocket::SocketTimestampingSnapshot& socket_timestamps) noexcept {
    ack_latency_diagnostics_.Arm(OrderLatencyDiagnosticWindow{
        .local_order_id = local_order_id,
        .request_sequence = sequence,
        .request_send_local_ns = send_local_ns,
        .inflight_at_send = inflight_count(),
        .owner_thread_tid = detail::CurrentTidForOrderSessionDiagnostics(),
        .write_path = write_path,
        .socket_send_queue = socket_send_queue,
        .socket_timestamps = socket_timestamps,
        .socket_timestamp_stages =
            websocket::ComputeSocketTimestampingStages(socket_timestamps),
    });
  }

  [[nodiscard]] websocket::SocketTimestampingSnapshot
  MakeSocketTimestampingSendSnapshot(
      const websocket::WritePathDiagnostics& write_path,
      bool socket_timestamping_probe_active) const noexcept {
    websocket::SocketTimestampingSnapshot snapshot{};
    if constexpr (!core::kOrderAckDiagnosticSocketTimestampingEnabled) {
      (void)write_path;
      (void)socket_timestamping_probe_active;
      return snapshot;
    }
    if (!socket_timestamping_probe_active) {
      return snapshot;
    }
    snapshot.available = true;
    snapshot.write_complete_ns = write_path.write_complete_ns;
    return snapshot;
  }

  [[nodiscard]] bool UpdateSocketTimestampingProbeAfterSend(
      std::uint64_t sequence, OrderSendStatus status,
      const websocket::WritePathDiagnostics& write_path,
      bool socket_timestamping_probe_started) noexcept {
    if constexpr (!core::kOrderAckDiagnosticSocketTimestampingEnabled) {
      (void)sequence;
      (void)status;
      (void)write_path;
      (void)socket_timestamping_probe_started;
      return false;
    }
    if (!socket_timestamping_probe_started) {
      return false;
    }
    client_.Core().SetSocketTimestampingProbeWriteComplete(
        sequence, write_path.write_complete_ns);
    if (!connection_.socket_timestamping.enabled) {
      return false;
    }
    if (status == OrderSendStatus::kOk && write_path.write_complete_ns > 0) {
      return true;
    }
    (void)client_.Core().FinishSocketTimestampingProbe(sequence, 0);
    return false;
  }

  [[nodiscard]] OrderLatencyDiagnosticAckResult RecordAckLatencyDiagnostic(
      std::uint64_t sequence, std::int64_t ack_local_receive_ns,
      std::int64_t ack_exchange_ns, int diagnostic_cpu,
      const websocket::SocketTimestampingSnapshot& socket_timestamps,
      const websocket::TcpInfoDiagnostics& tcp_info) noexcept {
    const std::uint64_t order_session_id = order_session_id_;
    const bool tcp_info_requested = TcpInfoDiagnosticsEnabled();
    return ack_latency_diagnostics_.RecordAckWithRecord(
        sequence, ack_local_receive_ns, ack_exchange_ns,
        current_drive_read_start_ns_, socket_timestamps,
        [order_session_id, diagnostic_cpu, tcp_info_requested,
         tcp_info](const OrderLatencyDiagnosticLogRecord& record) noexcept {
          LogOrderLatencyDiagnostic(record, order_session_id, diagnostic_cpu,
                                    tcp_info_requested, tcp_info);
        });
  }

  [[nodiscard]] bool TcpInfoDiagnosticsEnabled() const noexcept {
    if constexpr (!core::kOrderAckDiagnosticTcpInfoEnabled) {
      return false;
    }
    return socket_diagnostics_config_.enable_tcp_info;
  }

  [[nodiscard]] websocket::SocketEndpointDiagnostics
  SnapshotSocketEndpointDiagnostics() noexcept {
    return websocket::SnapshotSocketEndpointDiagnostics(
        client_.Core().NativeFd());
  }

  [[nodiscard]] websocket::TcpInfoDiagnostics
  SnapshotTcpInfoDiagnostics() noexcept {
    if (!TcpInfoDiagnosticsEnabled()) {
      return {};
    }
    return websocket::SnapshotTcpInfoDiagnostics(client_.Core().NativeFd());
  }

  [[nodiscard]] websocket::SocketSendQueueDiagnostics
  SnapshotSocketSendQueueDiagnostics() noexcept {
    if constexpr (!core::kOrderAckDiagnosticTcpInfoEnabled) {
      return {};
    }
    if (!TcpInfoDiagnosticsEnabled()) {
      return {};
    }
    return websocket::SnapshotSocketSendQueueDiagnostics(
        client_.Core().NativeFd());
  }

  void OnRuntimeLoopProbe(websocket::RuntimeLoopProbePoint point) noexcept {
    if constexpr (!core::kOrderAckDiagnosticRuntimeWritePathEnabled) {
      if (point == websocket::RuntimeLoopProbePoint::kAfterDriveRead ||
          point == websocket::RuntimeLoopProbePoint::kBeforeDriveRead) {
        current_drive_read_start_ns_ = 0;
      }
      return;
    }
    if (ack_latency_diagnostics_.empty()) {
      if (point == websocket::RuntimeLoopProbePoint::kAfterDriveRead ||
          point == websocket::RuntimeLoopProbePoint::kBeforeDriveRead) {
        current_drive_read_start_ns_ = 0;
      }
      return;
    }
    const std::int64_t now_ns = RealtimeNowNsInt64();
    switch (point) {
      case websocket::RuntimeLoopProbePoint::kAfterRuntimeHook: {
        const std::uint64_t order_session_id = order_session_id_;
        (void)ack_latency_diagnostics_.RecordAfterRuntimeHook(
            now_ns,
            [this, order_session_id](
                const OrderLatencyDiagnosticLogRecord& record) noexcept {
              const int diagnostic_cpu =
                  detail::CurrentCpuForOrderSessionDiagnostics();
              const websocket::TcpInfoDiagnostics tcp_info =
                  SnapshotTcpInfoDiagnostics();
              LogOrderLatencyDiagnostic(record, order_session_id,
                                        diagnostic_cpu,
                                        TcpInfoDiagnosticsEnabled(), tcp_info);
            });
      }
        return;
      case websocket::RuntimeLoopProbePoint::kBeforeDriveRead:
        current_drive_read_start_ns_ = now_ns;
        {
          const std::uint64_t order_session_id = order_session_id_;
          (void)ack_latency_diagnostics_.RecordBeforeDriveRead(
              now_ns,
              [this, order_session_id](
                  const OrderLatencyDiagnosticLogRecord& record) noexcept {
                const int diagnostic_cpu =
                    detail::CurrentCpuForOrderSessionDiagnostics();
                const websocket::TcpInfoDiagnostics tcp_info =
                    SnapshotTcpInfoDiagnostics();
                LogOrderLatencyDiagnostic(
                    record, order_session_id, diagnostic_cpu,
                    TcpInfoDiagnosticsEnabled(), tcp_info);
              });
        }
        return;
      case websocket::RuntimeLoopProbePoint::kAfterDriveRead: {
        const std::uint64_t order_session_id = order_session_id_;
        (void)ack_latency_diagnostics_.RecordAfterDriveRead(
            now_ns,
            [this, order_session_id](
                const OrderLatencyDiagnosticLogRecord& record) noexcept {
              const int diagnostic_cpu =
                  detail::CurrentCpuForOrderSessionDiagnostics();
              const websocket::TcpInfoDiagnostics tcp_info =
                  SnapshotTcpInfoDiagnostics();
              LogOrderLatencyDiagnostic(record, order_session_id,
                                        diagnostic_cpu,
                                        TcpInfoDiagnosticsEnabled(), tcp_info);
            });
      }
        current_drive_read_start_ns_ = 0;
        return;
    }
  }

  websocket::DeliveryResult HandleText(
      const websocket::MessageView& view) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordTextMessage();
    }
    const std::int64_t local_receive_ns = RealtimeNowNsInt64();
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};

    const GateSubmitResponse parsed = ParseGateSubmitResponseForOrderSession(
        payload, view.readable_tail_bytes, text_parser_);
    if (parsed.parse_status != GateSubmitParseStatus::kOk ||
        !parsed.request_id.ok) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    if (parsed.request_id.type == OrderRequestType::kLogin) {
      HandleLoginResponse(parsed);
      return websocket::DeliveryResult::kAccepted;
    }

    auto it = request_id_to_local_order_id_.find(parsed.request_id.sequence);
    if (it == request_id_to_local_order_id_.end()) {
      LogGateOrderResponseUnknownRequestId(parsed);
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordUnknownRequestId();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const std::uint64_t local_order_id = it->second;
    if (!RequestTypeMatchesChannel(parsed)) {
      LogGateOrderResponseIgnored("request_type_channel_mismatch", parsed);
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.kind == GateSubmitResponseKind::kUnknown) {
      LogGateOrderResponseIgnored("unknown_kind", parsed);
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }
    if (!ResponseStatusMatchesKind(parsed)) {
      LogGateOrderResponseIgnored("status_kind_mismatch", parsed);
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }
    if (parsed.kind == GateSubmitResponseKind::kAck) {
      if (!AckReqIdMatchesRequestId(parsed)) {
        LogGateOrderResponseIgnored("ack_req_id_mismatch", parsed);
        RecordIgnoredMessage();
        return websocket::DeliveryResult::kAccepted;
      }
      const int ack_cpu = detail::CurrentCpuForOrderSessionDiagnostics();
      const websocket::TcpInfoDiagnostics tcp_info =
          SnapshotTcpInfoDiagnostics();
      const websocket::SocketTimestampingSnapshot socket_timestamps =
          core::kOrderAckDiagnosticSocketTimestampingEnabled
              ? client_.Core().FinishSocketTimestampingProbe(
                    parsed.request_id.sequence, local_receive_ns)
              : websocket::SocketTimestampingSnapshot{};
      const websocket::SocketTimestampingStages socket_timestamp_stages =
          websocket::ComputeSocketTimestampingStages(socket_timestamps);
      const OrderLatencyDiagnosticAckResult ack_latency_diagnostic =
          RecordAckLatencyDiagnostic(parsed.request_id.sequence,
                                     local_receive_ns, parsed.exchange_ns,
                                     ack_cpu, socket_timestamps, tcp_info);
      LogGateOrderResponse(parsed, local_order_id, 0, local_receive_ns,
                           order_session_id_, ack_cpu,
                           TcpInfoDiagnosticsEnabled(), tcp_info);
      response_handler_.OnOrderResponse(OrderResponse{
          .kind = OrderResponseKind::kAck,
          .local_order_id = local_order_id,
          .exchange_order_id = 0,
          .request_sequence = parsed.request_id.sequence,
          .http_status = parsed.http_status,
          .error_label_hash = 0,
          .local_receive_ns = local_receive_ns,
          .exchange_ns = parsed.exchange_ns,
          .exchange_x_in_ns = parsed.exchange_x_in_ns,
          .exchange_x_out_ns = parsed.exchange_x_out_ns,
          .exchange_x_in_to_x_out_ns = parsed.exchange_x_in_to_x_out_ns,
          .socket_timestamps = socket_timestamps,
          .socket_timestamp_stages = socket_timestamp_stages,
          .ack_latency_diagnostic_available = ack_latency_diagnostic.found,
          .ack_latency_diagnostic = ack_latency_diagnostic.record,
          .tcp_info_requested = TcpInfoDiagnosticsEnabled(),
          .tcp_info = tcp_info});
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordResponse();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const bool is_cancel =
        parsed.request_id.type == OrderRequestType::kCancelOrder;
    const bool is_error = parsed.kind == GateSubmitResponseKind::kError;
    if (!is_error && !FinalResultMatchesLocalOrder(parsed, local_order_id)) {
      request_id_to_local_order_id_.erase(it);
      ack_latency_diagnostics_.Erase(parsed.request_id.sequence);
      LogGateOrderResponseIgnored("final_result_local_order_mismatch", parsed);
      RecordIgnoredMessage();
      return websocket::DeliveryResult::kAccepted;
    }

    request_id_to_local_order_id_.erase(it);
    ack_latency_diagnostics_.Erase(parsed.request_id.sequence);
    if (!is_error && !is_cancel && parsed.exchange_order_id != 0) {
      CacheExchangeOrderId(local_order_id, parsed.exchange_order_id);
    }
    if (!is_error && is_cancel) {
      local_order_id_to_exchange_order_id_.erase(local_order_id);
    }
    const OrderResponseKind kind =
        is_cancel ? (is_error ? OrderResponseKind::kCancelRejected
                              : OrderResponseKind::kCancelAccepted)
                  : (is_error ? OrderResponseKind::kRejected
                              : OrderResponseKind::kAccepted);
    const int ack_cpu = detail::CurrentCpuForOrderSessionDiagnostics();
    const websocket::TcpInfoDiagnostics tcp_info = SnapshotTcpInfoDiagnostics();
    LogGateOrderResponse(parsed, local_order_id, parsed.exchange_order_id,
                         local_receive_ns, order_session_id_, ack_cpu,
                         TcpInfoDiagnosticsEnabled(), tcp_info);
    response_handler_.OnOrderResponse(OrderResponse{
        .kind = kind,
        .local_order_id = local_order_id,
        .exchange_order_id = parsed.exchange_order_id,
        .request_sequence = parsed.request_id.sequence,
        .http_status = parsed.http_status,
        .error_label_hash = parsed.error_label_hash,
        .local_receive_ns = local_receive_ns,
        .exchange_ns = parsed.exchange_ns,
        .exchange_x_in_ns = parsed.exchange_x_in_ns,
        .exchange_x_out_ns = parsed.exchange_x_out_ns,
        .exchange_x_in_to_x_out_ns = parsed.exchange_x_in_to_x_out_ns});
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordResponse();
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleLoginResponse(const GateSubmitResponse& parsed) noexcept {
    if (login_request_sequence_ == 0 ||
        parsed.request_id.sequence != login_request_sequence_) {
      RecordIgnoredMessage();
      return;
    }
    login_request_sequence_ = 0;
    if (parsed.channel != kFuturesLogin ||
        parsed.kind == GateSubmitResponseKind::kUnknown) {
      login_ready_ = false;
      RecordIgnoredMessage();
      return;
    }
    if (parsed.http_status == 200 &&
        parsed.kind == GateSubmitResponseKind::kResult &&
        parsed.has_login_uid) {
      login_ready_ = true;
      NotifyLoginReady();
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginAccepted();
      }
      return;
    }
    login_ready_ = false;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLoginRejected();
    }
  }

  [[nodiscard]] bool RequestTypeMatchesChannel(
      const GateSubmitResponse& parsed) const noexcept {
    if (parsed.request_id.type == OrderRequestType::kPlaceOrder) {
      return parsed.channel == kFuturesOrderPlace;
    }
    if (parsed.request_id.type == OrderRequestType::kCancelOrder) {
      return parsed.channel == kFuturesOrderCancel;
    }
    return false;
  }

  [[nodiscard]] bool AckReqIdMatchesRequestId(
      const GateSubmitResponse& parsed) const noexcept {
    return parsed.has_req_id && parsed.req_id.ok &&
           parsed.req_id.type == parsed.request_id.type &&
           parsed.req_id.sequence == parsed.request_id.sequence;
  }

  [[nodiscard]] bool ResponseStatusMatchesKind(
      const GateSubmitResponse& parsed) const noexcept {
    if (parsed.kind == GateSubmitResponseKind::kAck ||
        parsed.kind == GateSubmitResponseKind::kResult) {
      return parsed.http_status == 200;
    }
    return true;
  }

  [[nodiscard]] bool FinalResultMatchesLocalOrder(
      const GateSubmitResponse& parsed,
      std::uint64_t local_order_id) const noexcept {
    if (parsed.kind != GateSubmitResponseKind::kResult) {
      return false;
    }
    if (parsed.request_id.type == OrderRequestType::kPlaceOrder) {
      return parsed.exchange_order_id != 0 && parsed.has_local_order_id &&
             parsed.local_order_id == local_order_id;
    }
    if (parsed.request_id.type == OrderRequestType::kCancelOrder) {
      if (parsed.has_local_order_id) {
        return parsed.local_order_id == local_order_id;
      }
      return parsed.exchange_order_id != 0;
    }
    return false;
  }

  void RecordIgnoredMessage() noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordIgnoredMessage();
    }
  }

  void NotifyLoginReady() noexcept {
    if constexpr (requires { response_handler_.OnOrderSessionLoginReady(); }) {
      response_handler_.OnOrderSessionLoginReady();
    }
  }

  void NotifyLoginNotReady() noexcept {
    if constexpr (requires {
                    response_handler_.OnOrderSessionLoginNotReady();
                  }) {
      response_handler_.OnOrderSessionLoginNotReady();
    }
  }

  void NotifyOrderSessionConnected(
      int owner_thread_cpu, int owner_thread_tid,
      const websocket::SocketEndpointDiagnostics& endpoints) noexcept {
    if constexpr (requires(ResponseHandler& handler,
                           const OrderSessionConnectionInfo& info) {
                    handler.OnOrderSessionConnected(info);
                  }) {
      response_handler_.OnOrderSessionConnected(OrderSessionConnectionInfo{
          .order_session_id = order_session_id_,
          .owner_thread_cpu = owner_thread_cpu,
          .owner_thread_tid = owner_thread_tid,
          .endpoint_available = endpoints.available,
          .local_ip = endpoints.local_ip.data(),
          .local_port = endpoints.local_port,
          .remote_ip = endpoints.remote_ip.data(),
          .remote_port = endpoints.remote_port,
      });
    }
  }

  websocket::ConnectionConfig connection_;
  bool quote_order_size_{false};
  LoginCredentials credentials_;
  OrderSessionSocketDiagnosticsConfig socket_diagnostics_config_{};
  OrderAckLatencyDiagnostics ack_latency_diagnostics_;
  ResponseHandler& response_handler_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] Diagnostics diagnostics_{};
  simdjson::ondemand::parser text_parser_;
  absl::flat_hash_map<std::uint64_t, std::uint64_t>
      request_id_to_local_order_id_;
  absl::flat_hash_map<std::uint64_t, std::uint64_t>
      local_order_id_to_exchange_order_id_;
  std::int64_t current_drive_read_start_ns_{0};
  std::size_t request_map_capacity_{kDefaultOrderRequestMapCapacity};
  const std::uint64_t order_session_id_{
      detail::NextOrderSessionIdForDiagnostics()};
  std::uint64_t request_sequence_{1};
  std::uint64_t login_request_sequence_{0};
  bool active_{false};
  bool login_ready_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_
