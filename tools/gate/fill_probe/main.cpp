#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <absl/container/flat_hash_map.h>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/common/result.h"
#include "core/config/instrument_catalog.h"
#include "core/market_data/data_shm.h"
#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_gateway_client.h"
#include "core/trading/order_id.h"
#include "tools/gate/fill_probe/bbo_cache.h"
#include "tools/gate/fill_probe/config.h"
#include "tools/gate/fill_probe/csv_writer.h"
#include "tools/gate/fill_probe/node_budget.h"
#include "tools/gate/fill_probe/order_math.h"
#include "tools/gate/fill_probe/state_machine.h"
#include "tools/gate/fill_probe/trigger_quote.h"

namespace {

namespace fp = aquila::tools::gate::fill_probe;
namespace md = aquila::market_data;
namespace core = aquila::core;

std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);
inline constexpr std::uint64_t kMarketDataDrainCap = md::kBookTickerShmCapacity;

void HandleSignal(int) {
  stop_requested.store(true, std::memory_order_relaxed);
}

struct CliOptions {
  std::filesystem::path config_path;
  bool validate_config{false};
  bool preflight_only{false};
};

struct LoadedContext {
  fp::FillProbeConfig config;
  aquila::config::InstrumentCatalog catalog;
  const aquila::config::InstrumentInfo* instrument{nullptr};
};

struct OrderRecord {
  core::StrategyOrder order;
  fp::EntryKind lifecycle_kind{fp::EntryKind::kGtc};
  bool close_order{false};
  std::string quantity_text;
  std::string price_text;
};

struct NodeCsvContext {
  fp::TriggerMode trigger_mode{fp::TriggerMode::kGateDirect};
  fp::BboSnapshot trigger_bbo;
  fp::BboSnapshot quote_bbo;
  fp::TriggerQuoteDecision trigger_quote;
  std::int64_t local_freshness_ns{0};
  std::int64_t exchange_freshness_ns{0};
  std::int64_t submit_ns{0};
};

[[nodiscard]] std::string EntryKindText(fp::EntryKind kind);
[[nodiscard]] std::string LowerEnumName(std::string_view name);

struct RuntimeContext {
  fp::CsvWriters* writers{nullptr};
  absl::flat_hash_map<std::uint64_t, OrderRecord>* orders{nullptr};
  fp::ProbeNode* current_node{nullptr};

  void OnOrderResponse(const core::OrderResponseEvent& event) {
    auto found = orders->find(event.local_order_id);
    if (found != orders->end() && event.exchange_order_id != 0) {
      found->second.order.exchange_order_id = event.exchange_order_id;
    }
    const std::string lifecycle_kind =
        found == orders->end() ? ""
                               : EntryKindText(found->second.lifecycle_kind);
    const std::string order_role =
        found == orders->end()
            ? ""
            : (found->second.close_order ? "close" : "entry");
    const std::string response_kind =
        LowerEnumName(magic_enum::enum_name(event.kind));
    writers->WriteOrderEvent(fp::OrderEventCsvRow{
        .run_id = run_id,
        .node_id = current_node_id,
        .lifecycle_kind = lifecycle_kind,
        .order_role = order_role,
        .local_order_id = event.local_order_id,
        .group_id = event.group_id,
        .route_id = event.route_id,
        .event_kind = "gateway_response",
        .response_kind = response_kind,
        .exchange_order_id = event.exchange_order_id,
        .exchange_ns = event.exchange_ns,
        .local_ns = event.local_receive_ns,
    });
    fmt::print(
        "fill_probe_order_event run_id={} node_id={} lifecycle_kind={} "
        "order_role={} local_order_id={} group_id={} route_id={} "
        "event_kind=gateway_response response_kind={} exchange_order_id={} "
        "exchange_ns={} local_ns={}\n",
        run_id, current_node_id, lifecycle_kind, order_role,
        event.local_order_id, event.group_id, event.route_id, response_kind,
        event.exchange_order_id, event.exchange_ns, event.local_receive_ns);
    if (found == orders->end() || current_node == nullptr) {
      return;
    }
    if (event.kind == core::OrderResponseKind::kRejected ||
        event.kind == core::OrderResponseKind::kUnknownResult) {
      if (found->second.close_order) {
        current_node->OnCloseTerminal(
            found->second.lifecycle_kind,
            event.kind == core::OrderResponseKind::kRejected
                ? fp::CloseResult::kRejected
                : fp::CloseResult::kUnknown,
            event.local_receive_ns);
      } else {
        current_node->OnEntryTerminal(
            event.local_order_id,
            event.kind == core::OrderResponseKind::kRejected
                ? fp::EntryResult::kRejected
                : fp::EntryResult::kUnknown,
            0.0, 0.0, event.local_receive_ns);
      }
    }
  }

  std::string run_id;
  std::uint64_t current_node_id{0};
};

[[nodiscard]] std::int64_t SystemNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::uint64_t PositiveRunId() {
  const std::int64_t now = SystemNowNs();
  return now > 0 ? static_cast<std::uint64_t>(now) : 1ULL;
}

[[nodiscard]] std::string EntryKindText(fp::EntryKind kind) {
  return kind == fp::EntryKind::kGtc ? "gtc" : "ioc";
}

[[nodiscard]] std::string NodeSideText(fp::NodeSide side) {
  return side == fp::NodeSide::kBuy ? "buy" : "sell";
}

[[nodiscard]] std::string TriggerModeText(fp::TriggerMode mode) {
  return mode == fp::TriggerMode::kBinanceTriggerGateQuote
             ? "binance_trigger_gate_quote"
             : "gate_direct";
}

[[nodiscard]] aquila::OrderSide ToOrderSide(fp::NodeSide side) {
  return side == fp::NodeSide::kBuy ? aquila::OrderSide::kBuy
                                    : aquila::OrderSide::kSell;
}

[[nodiscard]] aquila::OrderSide Opposite(aquila::OrderSide side) {
  return side == aquila::OrderSide::kBuy ? aquila::OrderSide::kSell
                                         : aquila::OrderSide::kBuy;
}

[[nodiscard]] std::string LowerEnumName(std::string_view name) {
  if (!name.empty() && name.front() == 'k') {
    name.remove_prefix(1);
  }
  std::string out;
  out.reserve(name.size());
  for (const char ch : name) {
    if (ch >= 'A' && ch <= 'Z') {
      if (!out.empty()) {
        out.push_back('_');
      }
      out.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

[[nodiscard]] aquila::Result<LoadedContext> LoadContext(
    const std::filesystem::path& config_path) {
  aquila::Result<LoadedContext> result;
  auto config_result = fp::LoadConfig(config_path);
  if (!config_result.ok) {
    result.error = std::move(config_result.error);
    return result;
  }
  auto catalog_result = aquila::config::LoadInstrumentCatalogFromCsv(
      config_result.value.instrument_catalog_file);
  if (!catalog_result.ok) {
    result.error = std::move(catalog_result.error);
    return result;
  }

  LoadedContext context;
  context.config = std::move(config_result.value);
  context.catalog = std::move(catalog_result.value);
  context.instrument = context.catalog.Find(aquila::Exchange::kGate,
                                            context.config.probe.symbol);
  if (context.instrument == nullptr) {
    result.error =
        "instrument_catalog missing Gate symbol " + context.config.probe.symbol;
    return result;
  }
  result.value = std::move(context);
  result.ok = true;
  return result;
}

void DrainBboReader(md::BookTickerShmReader& reader, fp::BboCache* cache,
                    std::uint64_t max_events = kMarketDataDrainCap) {
  for (std::uint64_t i = 0; i < max_events; ++i) {
    aquila::BookTicker ticker{};
    if (!reader.TryReadOne(&ticker)) {
      return;
    }
    cache->OnBookTicker(ticker);
  }
}

[[nodiscard]] bool WaitForBbo(md::BookTickerShmReader& reader,
                              fp::BboCache* cache,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline &&
         !stop_requested.load(std::memory_order_relaxed)) {
    DrainBboReader(reader, cache);
    if (cache->latest().has_value()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

[[nodiscard]] bool FreshEnough(const fp::FillProbeConfig& config,
                               const fp::BboSnapshot& bbo,
                               std::int64_t decision_ns,
                               std::int64_t* local_freshness_ns,
                               std::int64_t* exchange_freshness_ns) {
  *local_freshness_ns = decision_ns - bbo.local_ns;
  *exchange_freshness_ns = decision_ns - bbo.exchange_ns;
  return *local_freshness_ns >= 0 &&
         *local_freshness_ns <= config.probe.max_local_freshness_ns &&
         *exchange_freshness_ns >= 0 &&
         *exchange_freshness_ns <= config.probe.max_exchange_freshness_ns;
}

[[nodiscard]] std::int64_t TimeoutMsToNs(std::uint64_t timeout_ms) {
  constexpr std::uint64_t kNsPerMs = 1'000'000;
  if (timeout_ms >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
          kNsPerMs) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>(timeout_ms * kNsPerMs);
}

[[nodiscard]] md::BookTickerShmConfig ToMarketShmConfig(
    const fp::ExchangeMarketDataConfig& config) {
  return md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = config.shm_name,
      .channel_name = config.channel_name,
      .create = false,
      .remove_existing = false,
  };
}

[[nodiscard]] core::OrderGatewayClientConfig ToOrderGatewayClientConfig(
    const fp::FillProbeConfig& config) {
  return core::OrderGatewayClientConfig{
      .shm_name = config.order_gateway.shm_name,
      .route_count = config.order_gateway.route_count,
      .command_queue_capacity = config.order_gateway.command_queue_capacity,
      .event_queue_capacity = config.order_gateway.event_queue_capacity,
      .startup_ready_timeout_s = config.order_gateway.startup_ready_timeout_s,
  };
}

[[nodiscard]] aquila::OrderFeedbackShmConfig ToFeedbackShmConfig(
    const fp::FillProbeConfig& config) {
  return aquila::OrderFeedbackShmConfig{
      .shm_name = config.feedback.shm_name,
      .channel_name = config.feedback.channel_name,
      .create = false,
      .remove_existing = false,
  };
}

[[nodiscard]] std::uint64_t NextLocalOrderId(std::uint8_t strategy_id,
                                             std::uint64_t* next_order_id) {
  return aquila::LocalOrderIdCodec::Encode(strategy_id, (*next_order_id)++);
}

[[nodiscard]] core::StrategyOrder MakeOrder(
    const fp::FillProbeConfig& config, std::uint64_t local_order_id,
    std::uint64_t group_id, aquila::OrderSide side, aquila::TimeInForce tif,
    double quantity, double price,
    const aquila::config::InstrumentInfo& instrument, bool reduce_only,
    std::uint16_t route_id) {
  core::OrderPlaceRequest request{
      .local_order_id = local_order_id,
      .group_id = group_id,
      .price = price,
      .quantity = quantity,
      .symbol_id = config.probe.symbol_id,
      .gateway_route_id = route_id,
      .exchange = aquila::Exchange::kGate,
      .side = side,
      .order_type = aquila::OrderType::kLimit,
      .time_in_force = tif,
      .price_decimal_places =
          static_cast<std::uint8_t>(instrument.price_decimal_places),
      .quantity_decimal_places =
          static_cast<std::uint8_t>(*instrument.quantity_decimal_places),
      .reduce_only = reduce_only,
  };
  core::SetOrderSymbol(&request, config.probe.exchange_symbol);
  return core::StrategyOrder{.place_request = request};
}

void SetOrderTextStorage(OrderRecord* record, std::string quantity_text,
                         std::string price_text) {
  record->quantity_text = std::move(quantity_text);
  record->price_text = std::move(price_text);
}

void WriteSendEvent(fp::CsvWriters& writers, std::string_view run_id,
                    std::uint64_t node_id, const OrderRecord& record,
                    std::string_view event_kind,
                    core::OrderGatewaySendStatus status,
                    std::int64_t local_ns) {
  const std::string lifecycle_kind = EntryKindText(record.lifecycle_kind);
  const std::string order_role = record.close_order ? "close" : "entry";
  const std::string response_kind =
      LowerEnumName(magic_enum::enum_name(status));
  writers.WriteOrderEvent(fp::OrderEventCsvRow{
      .run_id = std::string(run_id),
      .node_id = node_id,
      .lifecycle_kind = std::string(lifecycle_kind),
      .order_role = std::string(order_role),
      .local_order_id = record.order.place_request.local_order_id,
      .group_id = record.order.place_request.group_id,
      .route_id = record.order.place_request.gateway_route_id,
      .event_kind = std::string(event_kind),
      .response_kind = std::string(response_kind),
      .exchange_order_id = record.order.exchange_order_id,
      .local_ns = local_ns,
      .price = record.price_text,
      .quantity = record.order.place_request.quantity,
  });
  fmt::print(
      "fill_probe_order_submitted run_id={} node_id={} lifecycle_kind={} "
      "order_role={} local_order_id={} group_id={} route_id={} "
      "event_kind={} "
      "response_kind={} local_ns={} price={} quantity={:.12g}\n",
      run_id, node_id, lifecycle_kind, order_role,
      record.order.place_request.local_order_id,
      record.order.place_request.group_id,
      record.order.place_request.gateway_route_id, event_kind, response_kind,
      local_ns, record.price_text, record.order.place_request.quantity);
}

void WriteFeedbackEvent(fp::CsvWriters& writers, std::string_view run_id,
                        std::uint64_t node_id, const OrderRecord* record,
                        const aquila::OrderFeedbackEvent& event) {
  const std::string lifecycle_kind =
      record == nullptr ? ""
                        : std::string(EntryKindText(record->lifecycle_kind));
  const std::string order_role =
      record == nullptr ? "" : (record->close_order ? "close" : "entry");
  const std::string feedback_kind =
      std::string(LowerEnumName(magic_enum::enum_name(event.kind)));
  const std::string finish_reason =
      std::string(LowerEnumName(magic_enum::enum_name(event.finish_reason)));
  const std::string reject_reason =
      std::string(LowerEnumName(magic_enum::enum_name(event.reject_reason)));
  const std::uint64_t group_id =
      record == nullptr ? 0 : record->order.place_request.group_id;
  const std::uint16_t route_id =
      record == nullptr ? core::kAutoGatewayRoute
                        : record->order.place_request.gateway_route_id;
  writers.WriteOrderEvent(fp::OrderEventCsvRow{
      .run_id = std::string(run_id),
      .node_id = node_id,
      .lifecycle_kind = lifecycle_kind,
      .order_role = order_role,
      .local_order_id = event.local_order_id,
      .group_id = group_id,
      .route_id = route_id,
      .event_kind = "feedback",
      .feedback_kind = feedback_kind,
      .exchange_order_id = event.exchange_order_id,
      .exchange_ns = event.exchange_update_ns,
      .local_ns = event.local_receive_ns,
      .price = record == nullptr ? "" : record->price_text,
      .quantity =
          record == nullptr ? 0.0 : record->order.place_request.quantity,
      .cumulative_filled_quantity = event.cumulative_filled_quantity,
      .left_quantity = event.left_quantity,
      .finish_reason = finish_reason,
      .reject_reason = reject_reason,
  });
  fmt::print(
      "fill_probe_order_event run_id={} node_id={} lifecycle_kind={} "
      "order_role={} local_order_id={} group_id={} route_id={} "
      "feedback_kind={} finish_reason={} reject_reason={} "
      "cumulative_filled_quantity={:.12g} left_quantity={:.12g} "
      "exchange_order_id={} exchange_ns={} local_ns={}\n",
      run_id, node_id, lifecycle_kind, order_role, event.local_order_id,
      group_id, route_id, feedback_kind, finish_reason, reject_reason,
      event.cumulative_filled_quantity, event.left_quantity,
      event.exchange_order_id, event.exchange_update_ns,
      event.local_receive_ns);
}

[[nodiscard]] fp::EntryResult EntryResultFromFeedback(
    const aquila::OrderFeedbackEvent& event) {
  switch (event.kind) {
    case aquila::OrderFeedbackKind::kFilled:
      return fp::EntryResult::kFilled;
    case aquila::OrderFeedbackKind::kCancelled:
      return event.cumulative_filled_quantity > 0.0
                 ? fp::EntryResult::kPartialFilled
                 : fp::EntryResult::kCancelled;
    case aquila::OrderFeedbackKind::kRejected:
      return fp::EntryResult::kRejected;
    case aquila::OrderFeedbackKind::kPartialFilled:
    case aquila::OrderFeedbackKind::kAccepted:
    case aquila::OrderFeedbackKind::kContinuityLost:
      return fp::EntryResult::kPending;
  }
  return fp::EntryResult::kUnknown;
}

[[nodiscard]] fp::CloseResult CloseResultFromFeedback(
    const aquila::OrderFeedbackEvent& event) {
  switch (event.kind) {
    case aquila::OrderFeedbackKind::kFilled:
      return fp::CloseResult::kFilled;
    case aquila::OrderFeedbackKind::kCancelled:
      return event.cumulative_filled_quantity > 0.0
                 ? fp::CloseResult::kPartialFilled
                 : fp::CloseResult::kCancelled;
    case aquila::OrderFeedbackKind::kRejected:
      return fp::CloseResult::kRejected;
    case aquila::OrderFeedbackKind::kPartialFilled:
    case aquila::OrderFeedbackKind::kAccepted:
    case aquila::OrderFeedbackKind::kContinuityLost:
      return fp::CloseResult::kPending;
  }
  return fp::CloseResult::kUnknown;
}

void ApplyFeedback(fp::ProbeNode& node,
                   absl::flat_hash_map<std::uint64_t, OrderRecord>& orders,
                   fp::CsvWriters& writers, std::string_view run_id,
                   const aquila::OrderFeedbackEvent& event) {
  auto found = orders.find(event.local_order_id);
  OrderRecord* record = found == orders.end() ? nullptr : &found->second;
  if (record != nullptr && event.exchange_order_id != 0) {
    record->order.exchange_order_id = event.exchange_order_id;
  }
  WriteFeedbackEvent(writers, run_id, node.node_id(), record, event);
  if (record == nullptr || event.kind == aquila::OrderFeedbackKind::kAccepted ||
      event.kind == aquila::OrderFeedbackKind::kContinuityLost) {
    return;
  }

  if (!record->close_order) {
    const fp::EntryResult result = EntryResultFromFeedback(event);
    if (result != fp::EntryResult::kPending) {
      node.OnEntryTerminal(event.local_order_id, result,
                           event.cumulative_filled_quantity, event.fill_price,
                           event.local_receive_ns);
    }
    return;
  }

  const fp::CloseResult result = CloseResultFromFeedback(event);
  if (result == fp::CloseResult::kPending) {
    return;
  }
  if (event.cumulative_filled_quantity > 0.0) {
    node.OnCloseFill(record->lifecycle_kind, event.cumulative_filled_quantity,
                     event.fill_price, event.local_receive_ns);
  } else {
    node.OnCloseTerminal(record->lifecycle_kind, result,
                         event.local_receive_ns);
  }
}

[[nodiscard]] bool SubmitCloseIfNeeded(
    const fp::FillProbeConfig& config,
    const aquila::config::InstrumentInfo& instrument,
    const std::optional<fp::BboSnapshot>& close_bbo,
    core::OrderGatewayClient& gateway, fp::ProbeNode& node, fp::EntryKind kind,
    std::uint64_t group_id, fp::OrderSizing sizing, fp::CsvWriters& writers,
    absl::flat_hash_map<std::uint64_t, OrderRecord>& orders,
    std::uint64_t* next_order_id, std::string_view run_id) {
  const fp::LifecycleState& lifecycle =
      kind == fp::EntryKind::kGtc ? node.gtc() : node.ioc();
  if (!node.CloseRetryAllowed(kind, config.probe.max_close_retries)) {
    return false;
  }

  if (!close_bbo.has_value()) {
    return false;
  }
  const aquila::OrderSide entry_side = ToOrderSide(node.side());
  const aquila::OrderSide close_side = Opposite(entry_side);
  const fp::PriceText close_price =
      fp::ClosePrice(close_side, *close_bbo, config.probe.close_slippage_bps);
  const std::uint64_t local_order_id =
      NextLocalOrderId(config.probe.strategy_id, next_order_id);
  const std::uint16_t route_id = lifecycle.entry_route_id;
  core::StrategyOrder order =
      MakeOrder(config, local_order_id, group_id, close_side,
                aquila::TimeInForce::kImmediateOrCancel, sizing.quantity,
                close_price.price, instrument, /*reduce_only=*/true, route_id);
  OrderRecord record{
      .order = order,
      .lifecycle_kind = kind,
      .close_order = true,
  };
  SetOrderTextStorage(&record, sizing.quantity_text, close_price.price_text);
  node.MarkCloseSubmitted(kind, local_order_id, route_id, SystemNowNs());
  const core::OrderGatewaySendResult send =
      gateway.PlaceOrder(record.order.place_request);
  WriteSendEvent(writers, run_id, node.node_id(), record, "close_submitted",
                 send.status, send.send_local_ns);
  if (send.status != core::OrderGatewaySendStatus::kOk) {
    node.OnCloseTerminal(local_order_id, fp::CloseResult::kRejected,
                         SystemNowNs());
    return false;
  }
  orders.emplace(local_order_id, std::move(record));
  return true;
}

void WriteLifecycleRow(fp::CsvWriters& writers, std::string_view run_id,
                       const fp::ProbeNode& node,
                       const fp::LifecycleState& lifecycle,
                       const OrderRecord* entry_record) {
  writers.WriteLifecycle(fp::LifecycleCsvRow{
      .run_id = std::string(run_id),
      .node_id = node.node_id(),
      .entry_kind = EntryKindText(lifecycle.kind),
      .entry_route_id = lifecycle.entry_route_id,
      .entry_local_order_id = lifecycle.entry_local_order_id,
      .entry_side = NodeSideText(node.side()),
      .entry_tif = lifecycle.kind == fp::EntryKind::kGtc ? "gtc" : "ioc",
      .entry_price = entry_record == nullptr ? "" : entry_record->price_text,
      .entry_quantity =
          entry_record == nullptr ? "" : entry_record->quantity_text,
      .entry_submit_ns = lifecycle.entry_submit_ns,
      .entry_finish_ns = lifecycle.entry_finish_ns,
      .entry_result =
          LowerEnumName(magic_enum::enum_name(lifecycle.entry_result)),
      .entry_filled_qty = lifecycle.entry_filled_qty,
      .entry_avg_fill_price = lifecycle.entry_avg_fill_price,
      .close_route_id = lifecycle.close_route_id,
      .close_attempts = lifecycle.close_attempts,
      .close_filled_qty = lifecycle.close_filled_qty,
      .close_avg_fill_price = lifecycle.close_avg_fill_price,
      .close_attribution = node.status() == fp::NodeStatus::kCompletedClosed
                               ? "closed_by_net_flat"
                               : "none",
  });
}

void WriteNodeRows(
    fp::CsvWriters& writers, std::string_view run_id, const fp::ProbeNode& node,
    const NodeCsvContext& context, const fp::OrderSizing& sizing,
    const absl::flat_hash_map<std::uint64_t, OrderRecord>& orders,
    std::string_view skip_reason, std::string_view unresolved_reason) {
  const bool cross_exchange =
      context.trigger_mode == fp::TriggerMode::kBinanceTriggerGateQuote;
  writers.WriteNode(fp::NodeCsvRow{
      .run_id = std::string(run_id),
      .node_id = node.node_id(),
      .side = NodeSideText(node.side()),
      .trigger_mode = TriggerModeText(context.trigger_mode),
      .binance_bbo_id = cross_exchange ? context.trigger_bbo.id : 0,
      .binance_exchange_ns =
          cross_exchange ? context.trigger_bbo.exchange_ns : 0,
      .binance_local_ns = cross_exchange ? context.trigger_bbo.local_ns : 0,
      .gate_bbo_id = context.quote_bbo.id,
      .gate_exchange_ns = context.quote_bbo.exchange_ns,
      .gate_local_ns = context.quote_bbo.local_ns,
      .bbo_id = context.quote_bbo.id,
      .bbo_exchange_ns = context.quote_bbo.exchange_ns,
      .bbo_local_ns = context.quote_bbo.local_ns,
      .decision_ns = node.decision_ns(),
      .submit_ns = context.submit_ns,
      .finish_ns = node.finish_ns(),
      .local_freshness_ns = context.local_freshness_ns,
      .exchange_freshness_ns = context.exchange_freshness_ns,
      .binance_freshness_ns =
          cross_exchange ? context.trigger_quote.binance_freshness_ns : 0,
      .gate_freshness_ns = cross_exchange
                               ? context.trigger_quote.gate_freshness_ns
                               : context.local_freshness_ns,
      .gate_exchange_delta_ns =
          cross_exchange ? context.trigger_quote.gate_exchange_delta_ns : 0,
      .gate_local_delta_ns =
          cross_exchange ? context.trigger_quote.gate_local_delta_ns : 0,
      .trigger_to_send_ns =
          context.submit_ns == 0 ? 0 : context.submit_ns - node.decision_ns(),
      .bid_price = context.quote_bbo.bid_price,
      .bid_volume = context.quote_bbo.bid_volume,
      .ask_price = context.quote_bbo.ask_price,
      .ask_volume = context.quote_bbo.ask_volume,
      .entry_quantity = sizing.quantity,
      .entry_notional_usdt = sizing.notional_usdt,
      .status = LowerEnumName(magic_enum::enum_name(node.status())),
      .skip_reason = std::string(skip_reason),
      .unresolved_reason = std::string(unresolved_reason),
  });
  const auto gtc_record = orders.find(node.gtc().entry_local_order_id);
  const auto ioc_record = orders.find(node.ioc().entry_local_order_id);
  WriteLifecycleRow(writers, run_id, node, node.gtc(),
                    gtc_record == orders.end() ? nullptr : &gtc_record->second);
  WriteLifecycleRow(writers, run_id, node, node.ioc(),
                    ioc_record == orders.end() ? nullptr : &ioc_record->second);
  const std::string status =
      LowerEnumName(magic_enum::enum_name(node.status()));
  if (node.status() == fp::NodeStatus::kUnresolved ||
      !unresolved_reason.empty()) {
    fmt::print(stderr,
               "fill_probe_node_unresolved run_id={} node_id={} side={} "
               "status={} reason={} finish_ns={} net_position={:.12g}\n",
               run_id, node.node_id(), NodeSideText(node.side()), status,
               unresolved_reason, node.finish_ns(), node.net_position());
    return;
  }
  fmt::print(
      "fill_probe_node_done run_id={} node_id={} side={} status={} "
      "skip_reason={} finish_ns={} net_position={:.12g}\n",
      run_id, node.node_id(), NodeSideText(node.side()), status, skip_reason,
      node.finish_ns(), node.net_position());
}

void WriteSkippedNode(fp::CsvWriters& writers, std::string_view run_id,
                      std::uint64_t node_id, fp::NodeSide side,
                      fp::TriggerMode trigger_mode,
                      const std::optional<fp::BboSnapshot>& trigger_bbo,
                      const std::optional<fp::BboSnapshot>& quote_bbo,
                      std::int64_t decision_ns,
                      fp::TriggerQuoteDecision trigger_quote,
                      std::string_view skip_reason) {
  const bool cross_exchange =
      trigger_mode == fp::TriggerMode::kBinanceTriggerGateQuote;
  std::int64_t local_freshness_ns = 0;
  std::int64_t exchange_freshness_ns = 0;
  if (quote_bbo.has_value()) {
    local_freshness_ns = decision_ns - quote_bbo->local_ns;
    exchange_freshness_ns = decision_ns - quote_bbo->exchange_ns;
  }
  if (cross_exchange && trigger_bbo.has_value() &&
      trigger_quote.binance_freshness_ns == 0) {
    trigger_quote.binance_freshness_ns = decision_ns - trigger_bbo->local_ns;
  }
  if (cross_exchange && quote_bbo.has_value() &&
      trigger_quote.gate_freshness_ns == 0) {
    trigger_quote.gate_freshness_ns = local_freshness_ns;
    trigger_quote.gate_exchange_delta_ns =
        quote_bbo->exchange_ns - trigger_bbo.value_or(*quote_bbo).exchange_ns;
    trigger_quote.gate_local_delta_ns =
        quote_bbo->local_ns - trigger_bbo.value_or(*quote_bbo).local_ns;
  }
  writers.WriteNode(fp::NodeCsvRow{
      .run_id = std::string(run_id),
      .node_id = node_id,
      .side = NodeSideText(side),
      .trigger_mode = TriggerModeText(trigger_mode),
      .binance_bbo_id =
          cross_exchange && trigger_bbo.has_value() ? trigger_bbo->id : 0,
      .binance_exchange_ns = cross_exchange && trigger_bbo.has_value()
                                 ? trigger_bbo->exchange_ns
                                 : 0,
      .binance_local_ns =
          cross_exchange && trigger_bbo.has_value() ? trigger_bbo->local_ns : 0,
      .gate_bbo_id = quote_bbo.has_value() ? quote_bbo->id : 0,
      .gate_exchange_ns = quote_bbo.has_value() ? quote_bbo->exchange_ns : 0,
      .gate_local_ns = quote_bbo.has_value() ? quote_bbo->local_ns : 0,
      .bbo_id = quote_bbo.has_value() ? quote_bbo->id : 0,
      .bbo_exchange_ns = quote_bbo.has_value() ? quote_bbo->exchange_ns : 0,
      .bbo_local_ns = quote_bbo.has_value() ? quote_bbo->local_ns : 0,
      .decision_ns = decision_ns,
      .local_freshness_ns = local_freshness_ns,
      .exchange_freshness_ns = exchange_freshness_ns,
      .binance_freshness_ns =
          cross_exchange ? trigger_quote.binance_freshness_ns : 0,
      .gate_freshness_ns =
          cross_exchange ? trigger_quote.gate_freshness_ns : local_freshness_ns,
      .gate_exchange_delta_ns =
          cross_exchange ? trigger_quote.gate_exchange_delta_ns : 0,
      .gate_local_delta_ns =
          cross_exchange ? trigger_quote.gate_local_delta_ns : 0,
      .bid_price = quote_bbo.has_value() ? quote_bbo->bid_price : 0.0,
      .bid_volume = quote_bbo.has_value() ? quote_bbo->bid_volume : 0.0,
      .ask_price = quote_bbo.has_value() ? quote_bbo->ask_price : 0.0,
      .ask_volume = quote_bbo.has_value() ? quote_bbo->ask_volume : 0.0,
      .status = "skipped",
      .skip_reason = std::string(skip_reason),
  });
  fmt::print(
      "fill_probe_node_done run_id={} node_id={} side={} status=skipped "
      "skip_reason={} decision_ns={}\n",
      run_id, node_id, NodeSideText(side), skip_reason, decision_ns);
}

[[nodiscard]] int ValidateOnly(const LoadedContext& context) {
  if (context.instrument == nullptr) {
    fmt::print(stderr, "instrument missing\n");
    return 1;
  }
  fmt::print("fill_probe_config_ok name={} symbol={} symbol_id={} catalog={}\n",
             context.config.probe.name, context.config.probe.symbol,
             context.config.probe.symbol_id,
             context.config.instrument_catalog_file.string());
  return 0;
}

[[nodiscard]] int RunProbe(const LoadedContext& context, bool preflight_only) {
  md::BookTickerShmReader gate_reader(
      ToMarketShmConfig(context.config.market_data.gate));
  std::optional<md::BookTickerShmReader> binance_reader;
  if (context.config.probe.trigger_mode ==
      fp::TriggerMode::kBinanceTriggerGateQuote) {
    binance_reader.emplace(
        ToMarketShmConfig(context.config.market_data.binance));
  }

  fp::BboCache gate_cache(context.config.probe.symbol_id,
                          context.instrument->price_tick);
  fp::BboCache binance_cache(context.config.probe.symbol_id,
                             context.instrument->price_tick);
  if (!WaitForBbo(gate_reader, &gate_cache, std::chrono::seconds(5))) {
    fmt::print(stderr, "market_data_bbo_timeout exchange=gate symbol_id={}\n",
               context.config.probe.symbol_id);
    return 1;
  }
  if (binance_reader.has_value() &&
      !WaitForBbo(*binance_reader, &binance_cache, std::chrono::seconds(5))) {
    fmt::print(stderr,
               "market_data_bbo_timeout exchange=binance symbol_id={}\n",
               context.config.probe.symbol_id);
    return 1;
  }
  const fp::BboSnapshot preflight_quote = *gate_cache.latest();

  const fp::OrderSizingResult sizing_result =
      fp::BuildOrderSizing(*context.instrument, preflight_quote.ask_price);
  if (!sizing_result.ok) {
    fmt::print(stderr, "order_sizing_failed error={}\n", sizing_result.error);
    return 1;
  }
  if (sizing_result.value.notional_usdt >
      context.config.probe.max_entry_notional_usdt) {
    fmt::print(stderr,
               "entry_notional_too_large notional_usdt={:.12g} limit={:.12g}\n",
               sizing_result.value.notional_usdt,
               context.config.probe.max_entry_notional_usdt);
    return 1;
  }

  auto gateway_result = core::OrderGatewayClient::Open(
      ToOrderGatewayClientConfig(context.config));
  if (!gateway_result.ok) {
    fmt::print(stderr, "order_gateway_shm_open_failed error={}\n",
               gateway_result.error);
    return 1;
  }
  core::OrderGatewayClient gateway = std::move(gateway_result.value);
  if (!gateway.Start()) {
    fmt::print(stderr, "order_gateway_routes_not_ready timeout_s={}\n",
               context.config.order_gateway.startup_ready_timeout_s);
    return 1;
  }

  auto feedback_manager_result = aquila::OrderFeedbackShmManager::Open(
      ToFeedbackShmConfig(context.config));
  if (!feedback_manager_result.ok) {
    fmt::print(stderr, "feedback_shm_open_failed error={}\n",
               feedback_manager_result.error);
    return 1;
  }
  aquila::OrderFeedbackShmManager feedback_manager =
      std::move(feedback_manager_result.value);
  const std::uint64_t run_id_numeric = PositiveRunId();
  auto feedback_reader_result = aquila::OrderFeedbackShmReader::Claim(
      feedback_manager.channel(), context.config.probe.strategy_id,
      run_id_numeric, context.config.feedback.force_claim);
  if (!feedback_reader_result.ok) {
    fmt::print(stderr, "feedback_lane_claim_failed error={}\n",
               feedback_reader_result.error);
    return 1;
  }
  aquila::OrderFeedbackShmReader feedback_reader =
      std::move(feedback_reader_result.value);
  const std::string run_id =
      fmt::format("{}_{}", context.config.probe.name, run_id_numeric);

  fmt::print(
      "fill_probe_start run_id={} name={} symbol={} symbol_id={} "
      "trigger_mode={} max_nodes={} duration_ms={} preflight_only={}\n",
      run_id, context.config.probe.name, context.config.probe.symbol,
      context.config.probe.symbol_id,
      TriggerModeText(context.config.probe.trigger_mode),
      context.config.probe.max_nodes, context.config.probe.duration_ms,
      preflight_only);
  fmt::print(
      "fill_probe_preflight_ok run_id={} symbol={} bbo_id={} bid={:.12g} "
      "ask={:.12g} quantity={} notional_usdt={:.12g}\n",
      run_id, context.config.probe.symbol, preflight_quote.id,
      preflight_quote.bid_price, preflight_quote.ask_price,
      sizing_result.value.quantity_text, sizing_result.value.notional_usdt);
  if (preflight_only) {
    return 0;
  }

  fp::CsvWriters writers(context.config.output.run_dir);
  auto csv_result = writers.Open();
  if (!csv_result.ok) {
    fmt::print(stderr, "csv_open_failed error={}\n", csv_result.error);
    return 1;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  stop_requested.store(false, std::memory_order_relaxed);

  absl::flat_hash_map<std::uint64_t, OrderRecord> orders;
  RuntimeContext runtime{
      .writers = &writers, .orders = &orders, .run_id = run_id};
  std::uint64_t next_order_id = 1;
  fp::SubmittedNodeBudget node_budget(context.config.probe.max_nodes);
  const auto run_start = std::chrono::steady_clock::now();
  const std::int64_t unresolved_timeout_ns =
      TimeoutMsToNs(context.config.probe.unresolved_timeout_ms);
  std::uint64_t next_skipped_node_id = context.config.probe.max_nodes + 1;
  std::uint64_t last_gate_trigger_id = gate_cache.latest()->id;
  std::uint64_t last_binance_trigger_id =
      binance_cache.latest().has_value() ? binance_cache.latest()->id : 0;

  while (node_budget.CanSubmitNode()) {
    if (stop_requested.load(std::memory_order_relaxed)) {
      break;
    }
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - run_start)
            .count();
    if (elapsed_ms >=
        static_cast<std::int64_t>(context.config.probe.duration_ms)) {
      break;
    }

    DrainBboReader(gate_reader, &gate_cache);
    if (binance_reader.has_value()) {
      DrainBboReader(*binance_reader, &binance_cache);
    }

    const std::int64_t decision_ns = SystemNowNs();
    std::optional<fp::BboSnapshot> trigger_bbo;
    std::optional<fp::BboSnapshot> quote_bbo;
    fp::TriggerQuoteDecision trigger_quote;
    std::int64_t local_freshness_ns = 0;
    std::int64_t exchange_freshness_ns = 0;

    if (context.config.probe.trigger_mode == fp::TriggerMode::kGateDirect) {
      if (!gate_cache.latest().has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (gate_cache.latest()->id == last_gate_trigger_id) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      trigger_bbo = *gate_cache.latest();
      quote_bbo = *gate_cache.latest();
      last_gate_trigger_id = trigger_bbo->id;
      if (!FreshEnough(context.config, *quote_bbo, decision_ns,
                       &local_freshness_ns, &exchange_freshness_ns)) {
        const std::uint64_t skipped_node_id = next_skipped_node_id++;
        const fp::NodeSide skipped_side = (skipped_node_id % 2 == 1)
                                              ? fp::NodeSide::kBuy
                                              : fp::NodeSide::kSell;
        WriteSkippedNode(writers, run_id, skipped_node_id, skipped_side,
                         context.config.probe.trigger_mode, trigger_bbo,
                         quote_bbo, decision_ns, trigger_quote,
                         "stale_gate_direct_quote");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
    } else {
      if (!binance_cache.latest().has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      if (binance_cache.latest()->id == last_binance_trigger_id) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        continue;
      }
      trigger_bbo = *binance_cache.latest();
      last_binance_trigger_id = trigger_bbo->id;
      if (!gate_cache.latest().has_value()) {
        const std::uint64_t skipped_node_id = next_skipped_node_id++;
        const fp::NodeSide skipped_side = (skipped_node_id % 2 == 1)
                                              ? fp::NodeSide::kBuy
                                              : fp::NodeSide::kSell;
        WriteSkippedNode(writers, run_id, skipped_node_id, skipped_side,
                         context.config.probe.trigger_mode, trigger_bbo,
                         std::nullopt, decision_ns, trigger_quote,
                         "missing_gate_quote");
        continue;
      }
      quote_bbo = *gate_cache.latest();
      trigger_quote = fp::EvaluateTriggerQuote(
          *trigger_bbo, *quote_bbo, decision_ns,
          fp::FreshnessLimits{
              .max_binance_freshness_ns =
                  context.config.probe.max_binance_freshness_ns,
              .max_gate_freshness_ns =
                  context.config.probe.max_gate_freshness_ns,
          });
      local_freshness_ns = trigger_quote.gate_freshness_ns;
      exchange_freshness_ns = decision_ns - quote_bbo->exchange_ns;
      if (!trigger_quote.accepted) {
        const std::uint64_t skipped_node_id = next_skipped_node_id++;
        const fp::NodeSide skipped_side = (skipped_node_id % 2 == 1)
                                              ? fp::NodeSide::kBuy
                                              : fp::NodeSide::kSell;
        WriteSkippedNode(writers, run_id, skipped_node_id, skipped_side,
                         context.config.probe.trigger_mode, trigger_bbo,
                         quote_bbo, decision_ns, trigger_quote,
                         trigger_quote.skip_reason);
        continue;
      }
    }

    const fp::OrderSizingResult entry_sizing_result =
        fp::BuildOrderSizing(*context.instrument, quote_bbo->ask_price);
    if (!entry_sizing_result.ok) {
      fmt::print(stderr, "order_sizing_failed error={}\n",
                 entry_sizing_result.error);
      return 1;
    }
    if (entry_sizing_result.value.notional_usdt >
        context.config.probe.max_entry_notional_usdt) {
      fmt::print(
          stderr,
          "entry_notional_too_large notional_usdt={:.12g} limit={:.12g}\n",
          entry_sizing_result.value.notional_usdt,
          context.config.probe.max_entry_notional_usdt);
      return 1;
    }

    const std::uint64_t node_id = node_budget.ReserveSubmittedNode();
    const fp::NodeSide node_side =
        (node_id % 2 == 1) ? fp::NodeSide::kBuy : fp::NodeSide::kSell;
    fp::ProbeNode node = fp::ProbeNode::Start(node_id, node_side, decision_ns);
    fmt::print(
        "fill_probe_node_start run_id={} node_id={} side={} trigger_mode={} "
        "trigger_bbo_id={} gate_bbo_id={} bid={:.12g} ask={:.12g} "
        "decision_ns={}\n",
        run_id, node_id, NodeSideText(node_side),
        TriggerModeText(context.config.probe.trigger_mode), trigger_bbo->id,
        quote_bbo->id, quote_bbo->bid_price, quote_bbo->ask_price,
        node.decision_ns());
    runtime.current_node_id = node_id;
    runtime.current_node = &node;

    const aquila::OrderSide entry_side = ToOrderSide(node_side);
    const fp::PriceText entry_price = fp::EntryPrice(entry_side, *quote_bbo);
    const std::uint64_t group_id = node_id;
    std::int64_t first_submit_ns = 0;
    for (const fp::EntryKind kind :
         {fp::EntryKind::kGtc, fp::EntryKind::kIoc}) {
      const std::uint16_t route_id =
          kind == fp::EntryKind::kGtc
              ? context.config.order_gateway.gtc_route_id
              : context.config.order_gateway.ioc_route_id;
      const aquila::TimeInForce tif =
          kind == fp::EntryKind::kGtc ? aquila::TimeInForce::kGoodTillCancel
                                      : aquila::TimeInForce::kImmediateOrCancel;
      const std::uint64_t local_order_id =
          NextLocalOrderId(context.config.probe.strategy_id, &next_order_id);
      core::StrategyOrder order =
          MakeOrder(context.config, local_order_id, group_id, entry_side, tif,
                    entry_sizing_result.value.quantity, entry_price.price,
                    *context.instrument, /*reduce_only=*/false, route_id);
      OrderRecord record{.order = order, .lifecycle_kind = kind};
      SetOrderTextStorage(&record, entry_sizing_result.value.quantity_text,
                          entry_price.price_text);
      const core::OrderGatewaySendResult send =
          gateway.PlaceOrder(record.order.place_request);
      if (first_submit_ns == 0) {
        first_submit_ns =
            send.send_local_ns == 0 ? SystemNowNs() : send.send_local_ns;
      }
      WriteSendEvent(writers, run_id, node_id, record, "entry_submitted",
                     send.status, send.send_local_ns);
      if (send.status == core::OrderGatewaySendStatus::kOk) {
        orders.emplace(local_order_id, std::move(record));
        node.MarkEntrySubmitted(kind, local_order_id, route_id,
                                first_submit_ns);
      } else {
        node.MarkEntrySubmitted(kind, local_order_id, route_id, SystemNowNs());
        node.OnEntryTerminal(local_order_id, fp::EntryResult::kRejected, 0.0,
                             0.0, SystemNowNs());
      }
    }

    NodeCsvContext node_csv_context{
        .trigger_mode = context.config.probe.trigger_mode,
        .trigger_bbo = *trigger_bbo,
        .quote_bbo = *quote_bbo,
        .trigger_quote = trigger_quote,
        .local_freshness_ns = local_freshness_ns,
        .exchange_freshness_ns = exchange_freshness_ns,
    };

    while (!node.Done() && !stop_requested.load(std::memory_order_relaxed)) {
      DrainBboReader(gate_reader, &gate_cache);
      if (binance_reader.has_value()) {
        DrainBboReader(*binance_reader, &binance_cache);
      }
      (void)gateway.PollOrderResponses(runtime);
      (void)feedback_reader.Poll(
          64, [&](const aquila::OrderFeedbackEvent& event) {
            ApplyFeedback(node, orders, writers, run_id, event);
          });

      if (node.GtcCancelDue(SystemNowNs())) {
        const auto found = orders.find(node.gtc().entry_local_order_id);
        if (found != orders.end()) {
          const core::OrderGatewaySendResult cancel =
              gateway.CancelOrder(core::OrderCancelRequest{
                  .local_order_id =
                      found->second.order.place_request.local_order_id,
              });
          WriteSendEvent(writers, run_id, node_id, found->second,
                         "gtc_cancel_submitted", cancel.status,
                         cancel.send_local_ns);
          if (cancel.status == core::OrderGatewaySendStatus::kOk) {
            node.MarkGtcCancelSubmitted(SystemNowNs());
          }
        }
      }

      (void)SubmitCloseIfNeeded(
          context.config, *context.instrument, gate_cache.latest(), gateway,
          node, fp::EntryKind::kGtc, group_id, entry_sizing_result.value,
          writers, orders, &next_order_id, run_id);
      (void)SubmitCloseIfNeeded(
          context.config, *context.instrument, gate_cache.latest(), gateway,
          node, fp::EntryKind::kIoc, group_id, entry_sizing_result.value,
          writers, orders, &next_order_id, run_id);

      if (node.UnresolvedDue(SystemNowNs(), unresolved_timeout_ns)) {
        node.MarkUnresolved(SystemNowNs());
        node_csv_context.submit_ns = first_submit_ns;
        WriteNodeRows(writers, run_id, node, node_csv_context,
                      entry_sizing_result.value, orders, "", "node_unresolved");
        return 10;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    node_csv_context.submit_ns = first_submit_ns;
    WriteNodeRows(writers, run_id, node, node_csv_context,
                  entry_sizing_result.value, orders, "", "");
    runtime.current_node = nullptr;
    std::this_thread::sleep_for(
        std::chrono::milliseconds(context.config.probe.node_pause_ms));
  }

  fmt::print("fill_probe_stop run_id={}\n", run_id);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  CLI::App app{"Gate BTC fillability probe"};
  app.add_option("--config", options.config_path, "fill probe TOML path");
  app.add_flag("--validate-config", options.validate_config,
               "Parse config and instrument catalog only");
  app.add_flag("--preflight-only", options.preflight_only,
               "Run SHM and sizing preflight without submitting orders");
  CLI11_PARSE(app, argc, argv);

  if (options.config_path.empty()) {
    fmt::print(stderr, "--config is required\n");
    return 2;
  }

  auto context_result = LoadContext(options.config_path);
  if (!context_result.ok) {
    fmt::print(stderr, "config_error={}\n", context_result.error);
    return 1;
  }
  if (options.validate_config) {
    return ValidateOnly(context_result.value);
  }

  try {
    return RunProbe(context_result.value, options.preflight_only);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "market_data_shm_open_failed error={}\n", exc.what());
    return 1;
  }
}
