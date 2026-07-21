#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/common/result.h"
#include "core/config/instrument_catalog.h"
#include "core/market_data/data_shm.h"
#include "core/trading/order_feedback_shm.h"
#include "core/trading/order_gateway_client.h"
#include "core/trading/order_id.h"
#include "tools/bitget/gateway_smoke/config.h"
#include "tools/bitget/gateway_smoke/evidence_writer.h"
#include "tools/bitget/gateway_smoke/order_math.h"
#include "tools/bitget/gateway_smoke/state_machine.h"

namespace {

namespace smoke = aquila::tools::bitget::gateway_smoke;
namespace core = aquila::core;
namespace market_data = aquila::market_data;

std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int) {
  stop_requested.store(true, std::memory_order_relaxed);
}

struct LoadedContext {
  smoke::GatewaySmokeConfig config;
  aquila::config::InstrumentCatalog catalog;
};

struct OrderRecord {
  std::string role;
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{0};
  std::uint64_t group_id{0};
  smoke::WireOrder wire;
  bool present{false};
};

[[nodiscard]] std::int64_t SystemNowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::uint64_t PositiveRunId() noexcept {
  const std::int64_t now_ns = SystemNowNs();
  return now_ns > 0 ? static_cast<std::uint64_t>(now_ns) : 1;
}

[[nodiscard]] std::string LowerEnumName(std::string_view name) {
  if (!name.empty() && name.front() == 'k') {
    name.remove_prefix(1);
  }
  std::string output;
  output.reserve(name.size());
  for (const char ch : name) {
    if (ch >= 'A' && ch <= 'Z') {
      if (!output.empty()) {
        output.push_back('_');
      }
      output.push_back(static_cast<char>(ch - 'A' + 'a'));
    } else {
      output.push_back(ch);
    }
  }
  return output;
}

template <typename Enum>
[[nodiscard]] std::string EnumText(Enum value) {
  return LowerEnumName(magic_enum::enum_name(value));
}

[[nodiscard]] aquila::OrderSide Opposite(aquila::OrderSide side) noexcept {
  return side == aquila::OrderSide::kBuy ? aquila::OrderSide::kSell
                                         : aquila::OrderSide::kBuy;
}

[[nodiscard]] aquila::Result<LoadedContext> LoadContext(
    const std::filesystem::path& config_path) {
  aquila::Result<LoadedContext> result;
  auto config_result = smoke::LoadConfig(config_path);
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
  const aquila::config::InstrumentInfo* instrument = catalog_result.value.Find(
      aquila::Exchange::kBitget, config_result.value.symbol);
  if (instrument == nullptr) {
    result.error = "instrument catalog missing Bitget symbol " +
                   config_result.value.symbol;
    return result;
  }
  auto contract_result =
      smoke::ValidateInstrumentContract(config_result.value, *instrument);
  if (!contract_result.ok) {
    result.error = std::move(contract_result.error);
    return result;
  }
  result.value = LoadedContext{
      .config = std::move(config_result.value),
      .catalog = std::move(catalog_result.value),
  };
  result.ok = true;
  return result;
}

[[nodiscard]] market_data::BookTickerShmConfig ToMarketDataConfig(
    const smoke::GatewaySmokeConfig& config) {
  return market_data::BookTickerShmConfig{
      .enabled = true,
      .shm_name = config.market_data.shm_name,
      .channel_name = config.market_data.channel_name,
      .create = false,
      .remove_existing = false,
  };
}

[[nodiscard]] core::OrderGatewayClientConfig ToGatewayClientConfig(
    const smoke::GatewaySmokeConfig& config) {
  return core::OrderGatewayClientConfig{
      .shm_name = config.order_gateway.shm_name,
      .route_count = config.order_gateway.route_count,
      .command_queue_capacity = config.order_gateway.command_queue_capacity,
      .event_queue_capacity = config.order_gateway.event_queue_capacity,
      .startup_ready_timeout_s = config.order_gateway.startup_ready_timeout_s,
  };
}

[[nodiscard]] aquila::OrderFeedbackShmConfig ToFeedbackConfig(
    const smoke::GatewaySmokeConfig& config) {
  return aquila::OrderFeedbackShmConfig{
      .shm_name = config.feedback.shm_name,
      .channel_name = config.feedback.channel_name,
      .create = false,
      .remove_existing = false,
  };
}

[[nodiscard]] bool SaneBbo(const aquila::BookTicker& ticker,
                           const smoke::GatewaySmokeConfig& config) noexcept {
  return ticker.exchange == aquila::Exchange::kBitget &&
         ticker.symbol_id == config.symbol_id &&
         std::isfinite(ticker.bid_price) && ticker.bid_price > 0.0 &&
         std::isfinite(ticker.ask_price) && ticker.ask_price > 0.0 &&
         ticker.bid_price <= ticker.ask_price;
}

[[nodiscard]] bool FreshBbo(const aquila::BookTicker& ticker,
                            const smoke::GatewaySmokeConfig& config,
                            std::int64_t now_ns) noexcept {
  if (!SaneBbo(ticker, config) || now_ns < ticker.local_ns) {
    return false;
  }
  return static_cast<std::uint64_t>(now_ns - ticker.local_ns) <=
         config.bbo_freshness_ns;
}

void DrainBbo(market_data::BookTickerShmReader& reader,
              const smoke::GatewaySmokeConfig& config,
              std::optional<aquila::BookTicker>* latest) noexcept {
  for (std::uint64_t count = 0; count < market_data::kBookTickerShmCapacity;
       ++count) {
    aquila::BookTicker ticker{};
    if (!reader.TryReadOne(&ticker)) {
      return;
    }
    if (SaneBbo(ticker, config)) {
      *latest = ticker;
    }
  }
}

[[nodiscard]] bool WaitForFreshBbo(market_data::BookTickerShmReader& reader,
                                   const smoke::GatewaySmokeConfig& config,
                                   std::optional<aquila::BookTicker>* latest,
                                   std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline &&
         !stop_requested.load(std::memory_order_relaxed)) {
    DrainBbo(reader, config, latest);
    if (latest->has_value() && FreshBbo(**latest, config, SystemNowNs())) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

[[nodiscard]] core::OrderPlaceRequest MakePlaceRequest(
    const smoke::GatewaySmokeConfig& config,
    const aquila::config::InstrumentInfo& instrument,
    const OrderRecord& record) {
  core::OrderPlaceRequest request{
      .local_order_id = record.local_order_id,
      .parent_id = record.parent_id,
      .group_id = record.group_id,
      .price = record.wire.price,
      .quantity = record.wire.quantity,
      .symbol_id = config.symbol_id,
      .gateway_route_id = config.route_id,
      .exchange = aquila::Exchange::kBitget,
      .side = record.wire.side,
      .order_type = record.wire.order_type,
      .time_in_force = record.wire.time_in_force,
      .price_decimal_places =
          static_cast<std::uint8_t>(instrument.price_decimal_places),
      .quantity_decimal_places =
          static_cast<std::uint8_t>(*instrument.quantity_decimal_places),
      .reduce_only = record.wire.reduce_only,
  };
  core::SetOrderSymbol(&request, config.exchange_symbol);
  return request;
}

[[nodiscard]] OrderRecord* FindOrder(OrderRecord* entry, OrderRecord* close,
                                     std::uint64_t local_order_id) noexcept {
  if (entry->present && entry->local_order_id == local_order_id) {
    return entry;
  }
  if (close->present && close->local_order_id == local_order_id) {
    return close;
  }
  return nullptr;
}

struct GatewayRuntime {
  smoke::SmokeStateMachine* state{nullptr};
  smoke::EvidenceWriter* writer{nullptr};
  const smoke::GatewaySmokeConfig* config{nullptr};
  OrderRecord* entry{nullptr};
  OrderRecord* close{nullptr};

  void OnOrderResponse(const core::OrderResponseEvent& event) noexcept {
    OrderRecord* record = FindOrder(entry, close, event.local_order_id);
    writer->WriteEvent(smoke::EvidenceEventRow{
        .run_id = config->run_id,
        .event_source = "gateway",
        .event_kind = "gateway_response",
        .order_role = record == nullptr ? "unknown" : record->role,
        .local_order_id = event.local_order_id,
        .parent_id = event.parent_id,
        .group_id = event.group_id,
        .route_id = event.route_id,
        .response_kind = EnumText(event.kind),
        .exchange_order_id = event.exchange_order_id,
        .exchange_ns = event.exchange_ns,
        .local_ns = event.local_receive_ns,
        .price = record == nullptr ? "" : record->wire.price_text,
        .quantity = record == nullptr ? 0.0 : record->wire.quantity,
    });
    state->OnGatewayResponse(event);
  }
};

void ApplyFeedback(const aquila::OrderFeedbackEvent& event,
                   smoke::SmokeStateMachine* state,
                   smoke::EvidenceWriter* writer,
                   const smoke::GatewaySmokeConfig& config, OrderRecord* entry,
                   OrderRecord* close) {
  OrderRecord* record = FindOrder(entry, close, event.local_order_id);
  const bool terminal = event.kind == aquila::OrderFeedbackKind::kFilled ||
                        event.kind == aquila::OrderFeedbackKind::kCancelled ||
                        event.kind == aquila::OrderFeedbackKind::kRejected;
  writer->WriteEvent(smoke::EvidenceEventRow{
      .run_id = config.run_id,
      .event_source = "feedback",
      .event_kind = terminal ? "feedback_terminal" : "feedback_update",
      .order_role = record == nullptr ? "unknown" : record->role,
      .local_order_id = event.local_order_id,
      .parent_id = record == nullptr ? 0 : record->parent_id,
      .group_id = record == nullptr ? 0 : record->group_id,
      .route_id = config.route_id,
      .feedback_kind = EnumText(event.kind),
      .exchange_order_id = event.exchange_order_id,
      .exchange_ns = event.exchange_update_ns,
      .local_ns = event.local_receive_ns,
      .price = record == nullptr ? "" : record->wire.price_text,
      .quantity = record == nullptr ? 0.0 : record->wire.quantity,
      .cumulative_filled_quantity = event.cumulative_filled_quantity,
      .left_quantity = event.left_quantity,
      .finish_reason = EnumText(event.finish_reason),
      .reject_reason = EnumText(event.reject_reason),
  });
  state->OnFeedback(event);
}

void WriteSendEvent(smoke::EvidenceWriter* writer,
                    const smoke::GatewaySmokeConfig& config,
                    const OrderRecord& record,
                    const core::OrderGatewaySendResult& send) {
  writer->WriteEvent(smoke::EvidenceEventRow{
      .run_id = config.run_id,
      .event_source = "runner",
      .event_kind = "order_submitted",
      .order_role = record.role,
      .local_order_id = record.local_order_id,
      .parent_id = record.parent_id,
      .group_id = record.group_id,
      .route_id = config.route_id,
      .response_kind = EnumText(send.status),
      .local_ns = send.send_local_ns,
      .price = record.wire.price_text,
      .quantity = record.wire.quantity,
  });
}

[[nodiscard]] smoke::SmokeSummary MakeSummary(
    const smoke::GatewaySmokeConfig& config,
    const smoke::SmokeStateMachine& state, std::string failure_override) {
  std::string final_result = "pending";
  if (state.failed() || !failure_override.empty()) {
    final_result = "failed";
  } else if (state.result() == smoke::SmokeResult::kNoFill) {
    final_result = "no_fill";
  } else if (state.result() == smoke::SmokeResult::kClosed) {
    final_result = "closed";
  }
  if (failure_override.empty()) {
    failure_override = EnumText(state.failure());
  }
  return smoke::SmokeSummary{
      .run_id = config.run_id,
      .final_result = std::move(final_result),
      .failure_reason = std::move(failure_override),
      .entry_local_order_id = state.entry().local_order_id,
      .entry_acked = state.entry().acked,
      .entry_terminal = state.entry().terminal,
      .entry_filled_quantity = state.entry().cumulative_filled_quantity,
      .close_required = state.entry().cumulative_filled_quantity > 1e-12,
      .close_local_order_id = state.close().local_order_id,
      .close_acked = state.close().acked,
      .close_terminal = state.close().terminal,
      .close_filled_quantity = state.close().cumulative_filled_quantity,
  };
}

[[nodiscard]] int Finish(smoke::EvidenceWriter* writer,
                         const smoke::GatewaySmokeConfig& config,
                         const smoke::SmokeStateMachine& state,
                         std::string failure_override = {}) {
  const smoke::SmokeSummary summary =
      MakeSummary(config, state, std::move(failure_override));
  const auto result = writer->WriteSummary(summary);
  fmt::print(
      "bitget_gateway_smoke_summary run_id={} result={} failure={} "
      "entry_local_order_id={} entry_acked={} entry_terminal={} "
      "entry_filled_quantity={:.12g} close_local_order_id={} "
      "close_acked={} close_terminal={} close_filled_quantity={:.12g}\n",
      summary.run_id, summary.final_result, summary.failure_reason,
      summary.entry_local_order_id, summary.entry_acked, summary.entry_terminal,
      summary.entry_filled_quantity, summary.close_local_order_id,
      summary.close_acked, summary.close_terminal,
      summary.close_filled_quantity);
  return result.ok && summary.final_result != "failed" ? 0 : 1;
}

[[nodiscard]] int RunAttached(const LoadedContext& context, bool execute) {
  const smoke::GatewaySmokeConfig& config = context.config;
  const aquila::config::InstrumentInfo* instrument =
      context.catalog.Find(aquila::Exchange::kBitget, config.symbol);
  if (instrument == nullptr) {
    fmt::print(stderr, "instrument_missing symbol={}\n", config.symbol);
    return 1;
  }

  market_data::BookTickerShmReader bbo_reader(ToMarketDataConfig(config));
  std::optional<aquila::BookTicker> latest_bbo;
  if (!WaitForFreshBbo(bbo_reader, config, &latest_bbo,
                       std::chrono::seconds(5))) {
    fmt::print(stderr, "market_data_bbo_timeout symbol={}\n", config.symbol);
    return 1;
  }
  const smoke::WireOrderResult entry_wire_result = smoke::BuildEntryOrder(
      *latest_bbo, *instrument, config.side, config.quantity,
      config.passive_price_limit_fraction);
  if (!entry_wire_result.ok) {
    fmt::print(stderr, "entry_order_build_failed error={}\n",
               entry_wire_result.error);
    return 1;
  }

  auto gateway_result =
      core::OrderGatewayClient::Open(ToGatewayClientConfig(config));
  if (!gateway_result.ok) {
    fmt::print(stderr, "order_gateway_shm_open_failed error={}\n",
               gateway_result.error);
    return 1;
  }
  core::OrderGatewayClient gateway = std::move(gateway_result.value);
  if (!gateway.Start()) {
    fmt::print(stderr, "order_gateway_routes_not_ready timeout_s={}\n",
               config.order_gateway.startup_ready_timeout_s);
    return 1;
  }

  auto feedback_manager_result =
      aquila::OrderFeedbackShmManager::Open(ToFeedbackConfig(config));
  if (!feedback_manager_result.ok) {
    fmt::print(stderr, "feedback_shm_open_failed error={}\n",
               feedback_manager_result.error);
    return 1;
  }
  aquila::OrderFeedbackShmManager feedback_manager =
      std::move(feedback_manager_result.value);
  const std::uint64_t numeric_run_id = PositiveRunId();
  auto feedback_reader_result = aquila::OrderFeedbackShmReader::Claim(
      feedback_manager.channel(), config.strategy_id, numeric_run_id,
      config.feedback.force_claim);
  if (!feedback_reader_result.ok) {
    fmt::print(stderr, "feedback_lane_claim_failed error={}\n",
               feedback_reader_result.error);
    return 1;
  }
  aquila::OrderFeedbackShmReader feedback_reader =
      std::move(feedback_reader_result.value);

  fmt::print(
      "bitget_gateway_smoke_preflight_ok run_id={} symbol={} quantity={} "
      "side={} bbo_id={} bid={:.12g} ask={:.12g} entry_price={} "
      "execute={}\n",
      config.run_id, config.symbol, entry_wire_result.value.quantity_text,
      EnumText(config.side), latest_bbo->id, latest_bbo->bid_price,
      latest_bbo->ask_price, entry_wire_result.value.price_text, execute);
  if (!execute) {
    return 0;
  }

  smoke::EvidenceWriter writer(config.run_dir);
  const auto writer_result = writer.Open();
  if (!writer_result.ok) {
    fmt::print(stderr, "evidence_open_failed error={}\n", writer_result.error);
    return 1;
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  stop_requested.store(false, std::memory_order_relaxed);

  smoke::SmokeStateMachine state;
  OrderRecord entry{
      .role = "entry",
      .local_order_id =
          aquila::LocalOrderIdCodec::Encode(config.strategy_id, 1),
      .parent_id = numeric_run_id,
      .group_id = numeric_run_id,
      .wire = entry_wire_result.value,
      .present = true,
  };
  OrderRecord close;
  GatewayRuntime runtime{
      .state = &state,
      .writer = &writer,
      .config = &config,
      .entry = &entry,
      .close = &close,
  };

  const core::OrderPlaceRequest entry_order =
      MakePlaceRequest(config, *instrument, entry);
  state.MarkEntrySubmitted(entry.local_order_id, SystemNowNs(),
                           entry.wire.quantity);
  const core::OrderGatewaySendResult entry_send =
      gateway.PlaceOrder(entry_order);
  WriteSendEvent(&writer, config, entry, entry_send);
  if (entry_send.status != core::OrderGatewaySendStatus::kOk) {
    return Finish(&writer, config, state, "entry_send_failed");
  }

  const std::int64_t ack_timeout_ns =
      static_cast<std::int64_t>(config.ack_timeout_ms) * 1'000'000;
  const std::int64_t terminal_timeout_ns =
      static_cast<std::int64_t>(config.terminal_timeout_ms) * 1'000'000;
  while (!state.done() && !state.failed()) {
    if (stop_requested.load(std::memory_order_relaxed)) {
      return Finish(&writer, config, state, "signal_requested");
    }
    DrainBbo(bbo_reader, config, &latest_bbo);
    (void)gateway.PollOrderResponses(runtime);
    (void)feedback_reader.Poll(config.feedback.poll_budget,
                               [&](const aquila::OrderFeedbackEvent& event) {
                                 ApplyFeedback(event, &state, &writer, config,
                                               &entry, &close);
                               });
    state.CheckTimeout(SystemNowNs(), ack_timeout_ns, terminal_timeout_ns);

    if (state.close_required()) {
      if (!WaitForFreshBbo(bbo_reader, config, &latest_bbo,
                           std::chrono::seconds(5))) {
        return Finish(&writer, config, state, "close_bbo_timeout");
      }
      const smoke::WireOrderResult close_wire_result = smoke::BuildCloseOrder(
          *latest_bbo, *instrument, Opposite(config.side),
          state.entry_filled_quantity(), config.close_slippage_bps);
      if (!close_wire_result.ok) {
        return Finish(&writer, config, state,
                      "close_order_build_failed:" + close_wire_result.error);
      }
      close = OrderRecord{
          .role = "close",
          .local_order_id =
              aquila::LocalOrderIdCodec::Encode(config.strategy_id, 2),
          .parent_id = numeric_run_id,
          .group_id = numeric_run_id,
          .wire = close_wire_result.value,
          .present = true,
      };
      state.MarkCloseSubmitted(close.local_order_id, SystemNowNs(),
                               close.wire.quantity);
      if (state.failed()) {
        return Finish(&writer, config, state);
      }
      const core::OrderGatewaySendResult close_send =
          gateway.PlaceOrder(MakePlaceRequest(config, *instrument, close));
      WriteSendEvent(&writer, config, close, close_send);
      if (close_send.status != core::OrderGatewaySendStatus::kOk) {
        return Finish(&writer, config, state, "close_send_failed");
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  return Finish(&writer, config, state);
}

[[nodiscard]] int ValidateOnly(const LoadedContext& context) {
  fmt::print(
      "bitget_gateway_smoke_config_ok name={} run_id={} symbol={} "
      "exchange_symbol={} symbol_id={} strategy_id={} side={} quantity={:.12g} "
      "route_count={} route_id={}\n",
      context.config.name, context.config.run_id, context.config.symbol,
      context.config.exchange_symbol, context.config.symbol_id,
      context.config.strategy_id, EnumText(context.config.side),
      context.config.quantity, context.config.order_gateway.route_count,
      context.config.route_id);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml"};
  bool validate_only{false};
  bool preflight_only{false};
  bool execute{false};

  CLI::App app{"Bitget gateway one-shot smoke"};
  app.add_option("--config", config_path, "gateway smoke TOML path");
  CLI::Option* validate_flag =
      app.add_flag("--validate-only", validate_only, "validate without SHM");
  CLI::Option* preflight_flag = app.add_flag(
      "--preflight-only", preflight_only, "attach SHM without sending orders");
  CLI::Option* execute_flag =
      app.add_flag("--execute", execute, "send one guarded IOC smoke order");
  validate_flag->excludes(preflight_flag)->excludes(execute_flag);
  preflight_flag->excludes(validate_flag)->excludes(execute_flag);
  execute_flag->excludes(validate_flag)->excludes(preflight_flag);
  CLI11_PARSE(app, argc, argv);

  if (static_cast<int>(validate_only) + static_cast<int>(preflight_only) +
          static_cast<int>(execute) !=
      1) {
    fmt::print(stderr, "exactly one run mode is required\n");
    return 1;
  }

  try {
    auto context_result = LoadContext(config_path);
    if (!context_result.ok) {
      fmt::print(stderr, "config_error={}\n", context_result.error);
      return 1;
    }
    if (validate_only) {
      return ValidateOnly(context_result.value);
    }
    return RunAttached(context_result.value, execute);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "bitget_gateway_smoke_exception={}\n", exc.what());
    return 1;
  }
}
