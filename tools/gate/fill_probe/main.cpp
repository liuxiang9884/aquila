#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
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
#include "tools/gate/fill_probe/config.h"
#include "tools/gate/fill_probe/csv_writer.h"
#include "tools/gate/fill_probe/order_math.h"
#include "tools/gate/fill_probe/state_machine.h"

namespace {

namespace fp = aquila::tools::gate::fill_probe;
namespace md = aquila::market_data;
namespace core = aquila::core;

std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

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
  std::string symbol_text;
  std::string quantity_text;
  std::string price_text;
};

[[nodiscard]] std::string EntryKindText(fp::EntryKind kind);

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
    writers->WriteOrderEvent(fp::OrderEventCsvRow{
        .run_id = run_id,
        .node_id = current_node_id,
        .lifecycle_kind = lifecycle_kind,
        .order_role = order_role,
        .local_order_id = event.local_order_id,
        .parent_id = event.parent_id,
        .route_id = event.route_id,
        .event_kind = "gateway_response",
        .response_kind = std::string(magic_enum::enum_name(event.kind)),
        .exchange_order_id = event.exchange_order_id,
        .exchange_ns = event.exchange_ns,
        .local_ns = event.local_receive_ns,
    });
    if (found == orders->end() || current_node == nullptr) {
      return;
    }
    if (event.kind == core::OrderResponseKind::kRejected ||
        event.kind == core::OrderResponseKind::kUnknownResult) {
      if (found->second.close_order) {
        current_node->OnCloseTerminal(
            event.local_order_id,
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

[[nodiscard]] fp::BboSnapshot ToSnapshot(const aquila::BookTicker& ticker,
                                         double price_tick) {
  return fp::BboSnapshot{
      .id = static_cast<std::uint64_t>(ticker.id),
      .symbol_id = ticker.symbol_id,
      .exchange_ns = ticker.exchange_ns,
      .local_ns = ticker.local_ns,
      .bid_price = ticker.bid_price,
      .bid_volume = ticker.bid_volume,
      .ask_price = ticker.ask_price,
      .ask_volume = ticker.ask_volume,
      .price_tick = price_tick,
  };
}

[[nodiscard]] bool SaneBbo(const aquila::BookTicker& ticker) {
  return std::isfinite(ticker.bid_price) && ticker.bid_price > 0.0 &&
         std::isfinite(ticker.ask_price) && ticker.ask_price > 0.0 &&
         ticker.bid_price <= ticker.ask_price;
}

[[nodiscard]] bool ReadBboWithin(md::BookTickerShmReader& reader,
                                 std::int32_t symbol_id, double price_tick,
                                 std::chrono::milliseconds timeout,
                                 fp::BboSnapshot* out) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline &&
         !stop_requested.load(std::memory_order_relaxed)) {
    aquila::BookTicker ticker{};
    std::uint64_t skipped = 0;
    if (reader.TryReadLatest(&ticker, &skipped) &&
        ticker.symbol_id == symbol_id && SaneBbo(ticker)) {
      *out = ToSnapshot(ticker, price_tick);
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

[[nodiscard]] md::BookTickerShmConfig ToMarketShmConfig(
    const fp::FillProbeConfig& config) {
  return md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = config.market_data.shm_name,
      .channel_name = config.market_data.channel_name,
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
    std::uint64_t parent_id, aquila::OrderSide side, aquila::TimeInForce tif,
    double quantity, std::string_view quantity_text,
    std::string_view price_text, bool reduce_only, std::uint16_t route_id) {
  return core::StrategyOrder{
      .local_order_id = local_order_id,
      .parent_id = parent_id,
      .exchange = aquila::Exchange::kGate,
      .symbol_id = config.probe.symbol_id,
      .symbol = config.probe.exchange_symbol,
      .side = side,
      .type = aquila::OrderType::kLimit,
      .time_in_force = tif,
      .quantity = quantity,
      .quantity_text = quantity_text,
      .price_text = price_text,
      .reduce_only = reduce_only,
      .gateway_route_id = route_id,
  };
}

void SetOrderTextStorage(OrderRecord* record, std::string symbol,
                         std::string quantity_text, std::string price_text) {
  record->symbol_text = std::move(symbol);
  record->quantity_text = std::move(quantity_text);
  record->price_text = std::move(price_text);
  record->order.symbol = record->symbol_text;
  record->order.quantity_text = record->quantity_text;
  record->order.price_text = record->price_text;
}

[[nodiscard]] core::StrategyOrder OrderForGateway(const OrderRecord& record) {
  core::StrategyOrder order = record.order;
  order.symbol = record.symbol_text;
  order.quantity_text = record.quantity_text;
  order.price_text = record.price_text;
  return order;
}

void WriteSendEvent(fp::CsvWriters& writers, std::string_view run_id,
                    std::uint64_t node_id, const OrderRecord& record,
                    std::string_view event_kind,
                    core::OrderGatewaySendStatus status,
                    std::int64_t local_ns) {
  writers.WriteOrderEvent(fp::OrderEventCsvRow{
      .run_id = std::string(run_id),
      .node_id = node_id,
      .lifecycle_kind = EntryKindText(record.lifecycle_kind),
      .order_role = record.close_order ? "close" : "entry",
      .local_order_id = record.order.local_order_id,
      .parent_id = record.order.parent_id,
      .route_id = record.order.gateway_route_id,
      .event_kind = std::string(event_kind),
      .response_kind = LowerEnumName(magic_enum::enum_name(status)),
      .exchange_order_id = record.order.exchange_order_id,
      .local_ns = local_ns,
      .price = record.price_text,
      .quantity = record.order.quantity,
  });
}

void WriteFeedbackEvent(fp::CsvWriters& writers, std::string_view run_id,
                        std::uint64_t node_id, const OrderRecord* record,
                        const aquila::OrderFeedbackEvent& event) {
  writers.WriteOrderEvent(fp::OrderEventCsvRow{
      .run_id = std::string(run_id),
      .node_id = node_id,
      .lifecycle_kind =
          record == nullptr ? "" : EntryKindText(record->lifecycle_kind),
      .order_role =
          record == nullptr ? "" : (record->close_order ? "close" : "entry"),
      .local_order_id = event.local_order_id,
      .parent_id = record == nullptr ? 0 : record->order.parent_id,
      .route_id = record == nullptr ? core::kAutoGatewayRoute
                                    : record->order.gateway_route_id,
      .event_kind = "feedback",
      .feedback_kind = LowerEnumName(magic_enum::enum_name(event.kind)),
      .exchange_order_id = event.exchange_order_id,
      .exchange_ns = event.exchange_update_ns,
      .local_ns = event.local_receive_ns,
      .price = record == nullptr ? "" : record->price_text,
      .quantity = record == nullptr ? 0.0 : record->order.quantity,
      .cumulative_filled_quantity = event.cumulative_filled_quantity,
      .left_quantity = event.left_quantity,
      .finish_reason =
          LowerEnumName(magic_enum::enum_name(event.finish_reason)),
      .reject_reason =
          LowerEnumName(magic_enum::enum_name(event.reject_reason)),
  });
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
    node.OnCloseFill(event.local_order_id, event.cumulative_filled_quantity,
                     event.fill_price, event.local_receive_ns);
  } else {
    node.OnCloseTerminal(event.local_order_id, result, event.local_receive_ns);
  }
}

[[nodiscard]] bool SubmitCloseIfNeeded(
    const fp::FillProbeConfig& config, md::BookTickerShmReader& market_reader,
    core::OrderGatewayClient& gateway, fp::ProbeNode& node, fp::EntryKind kind,
    std::uint64_t parent_id, double price_tick, fp::OrderSizing sizing,
    fp::CsvWriters& writers,
    absl::flat_hash_map<std::uint64_t, OrderRecord>& orders,
    std::uint64_t* next_order_id, std::string_view run_id) {
  const fp::LifecycleState& lifecycle =
      kind == fp::EntryKind::kGtc ? node.gtc() : node.ioc();
  if (!node.CloseRetryAllowed(kind, config.probe.max_close_retries)) {
    return false;
  }

  fp::BboSnapshot bbo;
  if (!ReadBboWithin(market_reader, config.probe.symbol_id, price_tick,
                     std::chrono::milliseconds(1), &bbo)) {
    return false;
  }
  const aquila::OrderSide entry_side = ToOrderSide(node.side());
  const aquila::OrderSide close_side = Opposite(entry_side);
  const fp::PriceText close_price =
      fp::ClosePrice(close_side, bbo, config.probe.close_slippage_bps);
  const std::uint64_t local_order_id =
      NextLocalOrderId(config.probe.strategy_id, next_order_id);
  const std::uint16_t route_id = lifecycle.entry_route_id;
  core::StrategyOrder order =
      MakeOrder(config, local_order_id, parent_id, close_side,
                aquila::TimeInForce::kImmediateOrCancel, sizing.quantity,
                sizing.quantity_text, close_price.price_text,
                /*reduce_only=*/true, route_id);
  OrderRecord record{
      .order = order,
      .lifecycle_kind = kind,
      .close_order = true,
  };
  SetOrderTextStorage(&record, config.probe.exchange_symbol,
                      sizing.quantity_text, close_price.price_text);
  node.MarkCloseSubmitted(kind, local_order_id, route_id, SystemNowNs());
  const core::OrderGatewaySendResult send =
      gateway.PlaceOrder(OrderForGateway(record));
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
                       const fp::LifecycleState& lifecycle) {
  writers.WriteLifecycle(fp::LifecycleCsvRow{
      .run_id = std::string(run_id),
      .node_id = node.node_id(),
      .entry_kind = EntryKindText(lifecycle.kind),
      .entry_route_id = lifecycle.entry_route_id,
      .entry_local_order_id = lifecycle.entry_local_order_id,
      .entry_side = NodeSideText(node.side()),
      .entry_tif = lifecycle.kind == fp::EntryKind::kGtc ? "gtc" : "ioc",
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

void WriteNodeRows(fp::CsvWriters& writers, std::string_view run_id,
                   const fp::ProbeNode& node, const fp::BboSnapshot& bbo,
                   const fp::OrderSizing& sizing, std::int64_t submit_ns,
                   std::int64_t local_freshness_ns,
                   std::int64_t exchange_freshness_ns,
                   std::string_view skip_reason,
                   std::string_view unresolved_reason) {
  writers.WriteNode(fp::NodeCsvRow{
      .run_id = std::string(run_id),
      .node_id = node.node_id(),
      .side = NodeSideText(node.side()),
      .bbo_id = bbo.id,
      .bbo_exchange_ns = bbo.exchange_ns,
      .bbo_local_ns = bbo.local_ns,
      .decision_ns = node.decision_ns(),
      .submit_ns = submit_ns,
      .finish_ns = node.finish_ns(),
      .local_freshness_ns = local_freshness_ns,
      .exchange_freshness_ns = exchange_freshness_ns,
      .bid_price = bbo.bid_price,
      .bid_volume = bbo.bid_volume,
      .ask_price = bbo.ask_price,
      .ask_volume = bbo.ask_volume,
      .entry_quantity = sizing.quantity,
      .entry_notional_usdt = sizing.notional_usdt,
      .status = LowerEnumName(magic_enum::enum_name(node.status())),
      .skip_reason = std::string(skip_reason),
      .unresolved_reason = std::string(unresolved_reason),
  });
  WriteLifecycleRow(writers, run_id, node, node.gtc());
  WriteLifecycleRow(writers, run_id, node, node.ioc());
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
  md::BookTickerShmReader market_reader(ToMarketShmConfig(context.config));
  fp::BboSnapshot bbo;
  if (!ReadBboWithin(market_reader, context.config.probe.symbol_id,
                     context.instrument->price_tick, std::chrono::seconds(5),
                     &bbo)) {
    fmt::print(stderr, "market_data_bbo_timeout symbol_id={}\n",
               context.config.probe.symbol_id);
    return 1;
  }

  const fp::OrderSizingResult sizing_result =
      fp::BuildOrderSizing(*context.instrument, bbo.ask_price);
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

  fmt::print(
      "fill_probe_preflight_ok symbol={} bbo_id={} bid={:.12g} ask={:.12g} "
      "quantity={} notional_usdt={:.12g}\n",
      context.config.probe.symbol, bbo.id, bbo.bid_price, bbo.ask_price,
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

  const std::string run_id =
      fmt::format("{}_{}", context.config.probe.name, run_id_numeric);
  absl::flat_hash_map<std::uint64_t, OrderRecord> orders;
  RuntimeContext runtime{
      .writers = &writers, .orders = &orders, .run_id = run_id};
  std::uint64_t next_order_id = 1;
  const auto run_start = std::chrono::steady_clock::now();

  for (std::uint64_t node_id = 1; node_id <= context.config.probe.max_nodes;
       ++node_id) {
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

    if (!ReadBboWithin(market_reader, context.config.probe.symbol_id,
                       context.instrument->price_tick,
                       std::chrono::milliseconds(5), &bbo)) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(context.config.probe.node_pause_ms));
      continue;
    }

    const fp::NodeSide node_side =
        (node_id % 2 == 1) ? fp::NodeSide::kBuy : fp::NodeSide::kSell;
    fp::ProbeNode node =
        fp::ProbeNode::Start(node_id, node_side, SystemNowNs());
    runtime.current_node_id = node_id;
    runtime.current_node = &node;
    std::int64_t local_freshness_ns = 0;
    std::int64_t exchange_freshness_ns = 0;
    if (!FreshEnough(context.config, bbo, node.decision_ns(),
                     &local_freshness_ns, &exchange_freshness_ns)) {
      WriteNodeRows(writers, run_id, node, bbo, sizing_result.value, 0,
                    local_freshness_ns, exchange_freshness_ns, "freshness_gate",
                    "");
      std::this_thread::sleep_for(
          std::chrono::milliseconds(context.config.probe.node_pause_ms));
      continue;
    }

    const aquila::OrderSide entry_side = ToOrderSide(node_side);
    const fp::PriceText entry_price = fp::EntryPrice(entry_side, bbo);
    const std::uint64_t parent_id = node_id;
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
      core::StrategyOrder order = MakeOrder(
          context.config, local_order_id, parent_id, entry_side, tif,
          sizing_result.value.quantity, sizing_result.value.quantity_text,
          entry_price.price_text, /*reduce_only=*/false, route_id);
      OrderRecord record{.order = order, .lifecycle_kind = kind};
      SetOrderTextStorage(&record, context.config.probe.exchange_symbol,
                          sizing_result.value.quantity_text,
                          entry_price.price_text);
      const core::OrderGatewaySendResult send =
          gateway.PlaceOrder(OrderForGateway(record));
      if (first_submit_ns == 0) {
        first_submit_ns =
            send.send_local_ns == 0 ? SystemNowNs() : send.send_local_ns;
      }
      if (send.status == core::OrderGatewaySendStatus::kOk) {
        orders.emplace(local_order_id, record);
        node.MarkEntrySubmitted(kind, local_order_id, route_id,
                                first_submit_ns);
      } else {
        node.MarkEntrySubmitted(kind, local_order_id, route_id, SystemNowNs());
        node.OnEntryTerminal(local_order_id, fp::EntryResult::kRejected, 0.0,
                             0.0, SystemNowNs());
      }
      WriteSendEvent(writers, run_id, node_id, record, "entry_submitted",
                     send.status, send.send_local_ns);
    }

    while (!node.Done() && !stop_requested.load(std::memory_order_relaxed)) {
      (void)gateway.PollOrderResponses(runtime);
      (void)feedback_reader.Poll(
          64, [&](const aquila::OrderFeedbackEvent& event) {
            ApplyFeedback(node, orders, writers, run_id, event);
          });

      if (node.GtcCancelDue(SystemNowNs())) {
        const auto found = orders.find(node.gtc().entry_local_order_id);
        if (found != orders.end()) {
          const core::OrderGatewaySendResult cancel =
              gateway.CancelOrder(OrderForGateway(found->second));
          node.MarkGtcCancelSubmitted(SystemNowNs());
          WriteSendEvent(writers, run_id, node_id, found->second,
                         "gtc_cancel_submitted", cancel.status,
                         cancel.send_local_ns);
        }
      }

      (void)SubmitCloseIfNeeded(
          context.config, market_reader, gateway, node, fp::EntryKind::kGtc,
          parent_id, context.instrument->price_tick, sizing_result.value,
          writers, orders, &next_order_id, run_id);
      (void)SubmitCloseIfNeeded(
          context.config, market_reader, gateway, node, fp::EntryKind::kIoc,
          parent_id, context.instrument->price_tick, sizing_result.value,
          writers, orders, &next_order_id, run_id);

      if (node.UnresolvedDue(SystemNowNs())) {
        node.MarkUnresolved(SystemNowNs());
        WriteNodeRows(writers, run_id, node, bbo, sizing_result.value,
                      first_submit_ns, local_freshness_ns,
                      exchange_freshness_ns, "", "node_unresolved");
        fmt::print(stderr, "node_unresolved node_id={}\n", node.node_id());
        return 10;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    WriteNodeRows(writers, run_id, node, bbo, sizing_result.value,
                  first_submit_ns, local_freshness_ns, exchange_freshness_ns,
                  "", "");
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
