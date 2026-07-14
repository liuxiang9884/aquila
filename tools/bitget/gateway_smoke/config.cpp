#include "tools/bitget/gateway_smoke/config.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

namespace aquila::tools::bitget::gateway_smoke {
namespace {

template <typename T>
[[nodiscard]] Result<T> Failure(std::string error) {
  Result<T> result;
  result.error = std::move(error);
  return result;
}

template <typename T>
[[nodiscard]] Result<T> Success(T value) {
  Result<T> result;
  result.value = std::move(value);
  result.ok = true;
  return result;
}

[[nodiscard]] bool AssignString(toml::node_view<const toml::node> node,
                                std::string_view name, std::string* output,
                                std::string* error) {
  const auto value = node.value<std::string>();
  if (!value.has_value() || value->empty()) {
    *error = std::string{name} + " is required";
    return false;
  }
  *output = *value;
  return true;
}

template <typename T>
[[nodiscard]] bool AssignUnsigned(toml::node_view<const toml::node> node,
                                  std::string_view name, T* output,
                                  std::string* error) {
  const auto value = node.value<std::int64_t>();
  if (!value.has_value() || *value < 0 ||
      static_cast<std::uint64_t>(*value) >
          static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    *error = std::string{name} + " is required and must be in range";
    return false;
  }
  *output = static_cast<T>(*value);
  return true;
}

[[nodiscard]] bool AssignSigned(toml::node_view<const toml::node> node,
                                std::string_view name, std::int32_t* output,
                                std::string* error) {
  const auto value = node.value<std::int64_t>();
  if (!value.has_value() ||
      *value <
          static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) ||
      *value >
          static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
    *error = std::string{name} + " is required and must be in range";
    return false;
  }
  *output = static_cast<std::int32_t>(*value);
  return true;
}

[[nodiscard]] bool AssignDouble(toml::node_view<const toml::node> node,
                                std::string_view name, double* output,
                                std::string* error) {
  const auto value = node.value<double>();
  if (!value.has_value()) {
    *error = std::string{name} + " is required";
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] bool AssignSide(toml::node_view<const toml::node> node,
                              OrderSide* output, std::string* error) {
  const auto value = node.value<std::string>();
  if (value == "buy") {
    *output = OrderSide::kBuy;
    return true;
  }
  if (value == "sell") {
    *output = OrderSide::kSell;
    return true;
  }
  *error = "gateway_smoke.side must be buy or sell";
  return false;
}

[[nodiscard]] GatewaySmokeConfigResult Validate(GatewaySmokeConfig config) {
  if (config.strategy_id >= 8) {
    return Failure<GatewaySmokeConfig>(
        "gateway_smoke.strategy_id must be less than 8");
  }
  if (!std::isfinite(config.quantity) || config.quantity <= 0.0) {
    return Failure<GatewaySmokeConfig>(
        "gateway_smoke.quantity must be positive");
  }
  if (!std::isfinite(config.passive_price_limit_fraction) ||
      config.passive_price_limit_fraction <= 0.0 ||
      config.passive_price_limit_fraction > 1.0) {
    return Failure<GatewaySmokeConfig>(
        "gateway_smoke.passive_price_limit_fraction must be in (0, 1]");
  }
  if (config.close_slippage_bps == 0 || config.close_slippage_bps > 1'000) {
    return Failure<GatewaySmokeConfig>(
        "gateway_smoke.close_slippage_bps must be in [1, 1000]");
  }
  if (config.bbo_freshness_ns == 0 || config.ack_timeout_ms == 0 ||
      config.terminal_timeout_ms == 0) {
    return Failure<GatewaySmokeConfig>(
        "gateway_smoke freshness and timeouts must be positive");
  }
  if (config.order_gateway.route_count != 1) {
    return Failure<GatewaySmokeConfig>("order_gateway.route_count must be 1");
  }
  if (config.route_id != 0) {
    return Failure<GatewaySmokeConfig>("gateway_smoke.route_id must be 0");
  }
  if (config.feedback.poll_budget == 0) {
    return Failure<GatewaySmokeConfig>("feedback.poll_budget must be positive");
  }
  return Success(std::move(config));
}

[[nodiscard]] GatewaySmokeConfigResult ParseConfig(const toml::table& root) {
  GatewaySmokeConfig config;
  std::string error;
  const auto smoke = root["gateway_smoke"];
  if (!AssignString(smoke["name"], "gateway_smoke.name", &config.name,
                    &error) ||
      !AssignString(smoke["run_id"], "gateway_smoke.run_id", &config.run_id,
                    &error) ||
      !AssignString(smoke["symbol"], "gateway_smoke.symbol", &config.symbol,
                    &error) ||
      !AssignString(smoke["exchange_symbol"], "gateway_smoke.exchange_symbol",
                    &config.exchange_symbol, &error) ||
      !AssignSigned(smoke["symbol_id"], "gateway_smoke.symbol_id",
                    &config.symbol_id, &error) ||
      !AssignUnsigned(smoke["strategy_id"], "gateway_smoke.strategy_id",
                      &config.strategy_id, &error) ||
      !AssignSide(smoke["side"], &config.side, &error) ||
      !AssignDouble(smoke["quantity"], "gateway_smoke.quantity",
                    &config.quantity, &error) ||
      !AssignDouble(smoke["passive_price_limit_fraction"],
                    "gateway_smoke.passive_price_limit_fraction",
                    &config.passive_price_limit_fraction, &error) ||
      !AssignUnsigned(smoke["close_slippage_bps"],
                      "gateway_smoke.close_slippage_bps",
                      &config.close_slippage_bps, &error) ||
      !AssignUnsigned(smoke["bbo_freshness_ns"],
                      "gateway_smoke.bbo_freshness_ns",
                      &config.bbo_freshness_ns, &error) ||
      !AssignUnsigned(smoke["ack_timeout_ms"], "gateway_smoke.ack_timeout_ms",
                      &config.ack_timeout_ms, &error) ||
      !AssignUnsigned(smoke["terminal_timeout_ms"],
                      "gateway_smoke.terminal_timeout_ms",
                      &config.terminal_timeout_ms, &error) ||
      !AssignUnsigned(smoke["route_id"], "gateway_smoke.route_id",
                      &config.route_id, &error)) {
    return Failure<GatewaySmokeConfig>(std::move(error));
  }

  std::string catalog_file;
  if (!AssignString(root["instrument_catalog"]["file"],
                    "instrument_catalog.file", &catalog_file, &error)) {
    return Failure<GatewaySmokeConfig>(std::move(error));
  }
  config.instrument_catalog_file = catalog_file;

  const auto market_data = root["market_data"];
  if (!AssignString(market_data["shm_name"], "market_data.shm_name",
                    &config.market_data.shm_name, &error)) {
    return Failure<GatewaySmokeConfig>(std::move(error));
  }
  config.market_data.channel_name =
      market_data["channel_name"].value_or(config.market_data.channel_name);

  const auto gateway = root["order_gateway"];
  if (!AssignString(gateway["shm_name"], "order_gateway.shm_name",
                    &config.order_gateway.shm_name, &error) ||
      !AssignUnsigned(gateway["route_count"], "order_gateway.route_count",
                      &config.order_gateway.route_count, &error) ||
      !AssignUnsigned(gateway["command_queue_capacity"],
                      "order_gateway.command_queue_capacity",
                      &config.order_gateway.command_queue_capacity, &error) ||
      !AssignUnsigned(gateway["event_queue_capacity"],
                      "order_gateway.event_queue_capacity",
                      &config.order_gateway.event_queue_capacity, &error) ||
      !AssignUnsigned(gateway["startup_ready_timeout_s"],
                      "order_gateway.startup_ready_timeout_s",
                      &config.order_gateway.startup_ready_timeout_s, &error)) {
    return Failure<GatewaySmokeConfig>(std::move(error));
  }

  const auto feedback = root["feedback"];
  if (!AssignString(feedback["shm_name"], "feedback.shm_name",
                    &config.feedback.shm_name, &error) ||
      !AssignUnsigned(feedback["poll_budget"], "feedback.poll_budget",
                      &config.feedback.poll_budget, &error)) {
    return Failure<GatewaySmokeConfig>(std::move(error));
  }
  config.feedback.channel_name =
      feedback["channel_name"].value_or(config.feedback.channel_name);
  config.feedback.force_claim = feedback["force_claim"].value_or(false);

  std::string run_dir;
  if (!AssignString(root["output"]["run_dir"], "output.run_dir", &run_dir,
                    &error)) {
    return Failure<GatewaySmokeConfig>(std::move(error));
  }
  config.run_dir = run_dir;
  return Validate(std::move(config));
}

}  // namespace

GatewaySmokeConfigResult LoadConfig(const std::filesystem::path& path) {
  try {
    return ParseConfig(toml::parse_file(path.string()));
  } catch (const std::exception& exc) {
    return Failure<GatewaySmokeConfig>(
        std::string{"failed to load gateway smoke config: "} + exc.what());
  }
}

Result<bool> ValidateInstrumentContract(
    const GatewaySmokeConfig& config,
    const config::InstrumentInfo& instrument) {
  if (instrument.exchange != Exchange::kBitget ||
      instrument.symbol != config.symbol ||
      instrument.exchange_symbol != config.exchange_symbol ||
      instrument.symbol_id != config.symbol_id) {
    return Failure<bool>("instrument identity does not match gateway smoke");
  }
  if (!std::isfinite(instrument.min_quantity) ||
      instrument.min_quantity <= 0.0 ||
      std::fabs(config.quantity - instrument.min_quantity) > 1e-12) {
    return Failure<bool>(
        "gateway_smoke.quantity must equal instrument min_quantity");
  }
  if (!instrument.quantity_step.has_value() ||
      !instrument.quantity_decimal_places.has_value() ||
      *instrument.quantity_step <= 0.0 ||
      *instrument.quantity_decimal_places < 0 || instrument.price_tick <= 0.0 ||
      instrument.price_decimal_places < 0 ||
      !instrument.price_limit_up.has_value() ||
      !instrument.price_limit_down.has_value()) {
    return Failure<bool>("instrument trading metadata is incomplete");
  }
  return Success(true);
}

}  // namespace aquila::tools::bitget::gateway_smoke
