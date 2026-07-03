#include "tools/gate/fill_probe/config.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <toml++/toml.hpp>

namespace aquila::tools::gate::fill_probe {
namespace {

[[nodiscard]] FillProbeConfigResult Failure(std::string error) {
  FillProbeConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] FillProbeConfigResult Success(FillProbeConfig config) {
  FillProbeConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

template <typename T>
[[nodiscard]] bool AssignInteger(toml::node_view<const toml::node> node,
                                 std::string_view name, T* out,
                                 std::string* error) {
  const auto value = node.value<std::int64_t>();
  if (!value.has_value()) {
    *error = std::string{name} + " is required";
    return false;
  }
  if (*value < 0 ||
      static_cast<std::uint64_t>(*value) >
          static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    *error = std::string{name} + " is out of range";
    return false;
  }
  *out = static_cast<T>(*value);
  return true;
}

[[nodiscard]] bool AssignSignedInteger(toml::node_view<const toml::node> node,
                                       std::string_view name, std::int64_t* out,
                                       std::string* error) {
  const auto value = node.value<std::int64_t>();
  if (!value.has_value()) {
    *error = std::string{name} + " is required";
    return false;
  }
  *out = *value;
  return true;
}

[[nodiscard]] bool AssignDouble(toml::node_view<const toml::node> node,
                                std::string_view name, double* out,
                                std::string* error) {
  const auto value = node.value<double>();
  if (!value.has_value()) {
    *error = std::string{name} + " is required";
    return false;
  }
  *out = *value;
  return true;
}

[[nodiscard]] bool AssignString(toml::node_view<const toml::node> node,
                                std::string_view name, std::string* out,
                                std::string* error) {
  const auto value = node.value<std::string>();
  if (!value.has_value() || value->empty()) {
    *error = std::string{name} + " is required";
    return false;
  }
  *out = *value;
  return true;
}

void AssignBool(toml::node_view<const toml::node> node, bool fallback,
                bool* out) {
  *out = node.value<bool>().value_or(fallback);
}

[[nodiscard]] bool ValidateConfig(const FillProbeConfig& config,
                                  std::string* error) {
  if (config.probe.symbol.empty()) {
    *error = "fill_probe.symbol is required";
    return false;
  }
  if (config.probe.exchange_symbol.empty()) {
    *error = "fill_probe.exchange_symbol is required";
    return false;
  }
  if (config.market_data.shm_name.empty()) {
    *error = "market_data.shm_name is required";
    return false;
  }
  if (config.order_gateway.shm_name.empty()) {
    *error = "order_gateway.shm_name is required";
    return false;
  }
  if (config.feedback.shm_name.empty()) {
    *error = "feedback.shm_name is required";
    return false;
  }
  if (config.output.run_dir.empty()) {
    *error = "output.run_dir is required";
    return false;
  }
  if (config.order_gateway.route_count != 2) {
    *error = "order_gateway.route_count must be 2";
    return false;
  }
  if (config.order_gateway.gtc_route_id != 0) {
    *error = "order_gateway.gtc_route_id must be 0";
    return false;
  }
  if (config.order_gateway.ioc_route_id != 1) {
    *error = "order_gateway.ioc_route_id must be 1";
    return false;
  }
  if (config.order_gateway.gtc_route_id == config.order_gateway.ioc_route_id) {
    *error = "order_gateway.gtc_route_id and ioc_route_id must differ";
    return false;
  }
  if (config.probe.max_nodes == 0) {
    *error = "fill_probe.max_nodes must be positive";
    return false;
  }
  if (config.probe.duration_ms == 0) {
    *error = "fill_probe.duration_ms must be positive";
    return false;
  }
  if (config.probe.gtc_cancel_after_ms != 1000) {
    *error = "fill_probe.gtc_cancel_after_ms must be 1000";
    return false;
  }
  if (config.probe.node_pause_ms != 1000) {
    *error = "fill_probe.node_pause_ms must be 1000";
    return false;
  }
  if (config.probe.max_close_retries != 3) {
    *error = "fill_probe.max_close_retries must be 3";
    return false;
  }
  if (config.probe.close_slippage_bps != 100) {
    *error = "fill_probe.close_slippage_bps must be 100";
    return false;
  }
  if (config.probe.max_entry_notional_usdt <= 0.0 ||
      config.probe.max_entry_notional_usdt > 10.0) {
    *error = "fill_probe.max_entry_notional_usdt must be > 0 and <= 10";
    return false;
  }
  if (config.probe.max_local_freshness_ns <= 0) {
    *error = "fill_probe.max_local_freshness_ns must be positive";
    return false;
  }
  if (config.probe.max_exchange_freshness_ns <= 0) {
    *error = "fill_probe.max_exchange_freshness_ns must be positive";
    return false;
  }
  return true;
}

[[nodiscard]] FillProbeConfigResult ParseConfig(const toml::table& root) {
  FillProbeConfig config;
  std::string error;

  const toml::node_view<const toml::node> probe = root["fill_probe"];
  if (!AssignString(probe["name"], "fill_probe.name", &config.probe.name,
                    &error) ||
      !AssignString(probe["symbol"], "fill_probe.symbol", &config.probe.symbol,
                    &error) ||
      !AssignString(probe["exchange_symbol"], "fill_probe.exchange_symbol",
                    &config.probe.exchange_symbol, &error) ||
      !AssignInteger(probe["symbol_id"], "fill_probe.symbol_id",
                     &config.probe.symbol_id, &error) ||
      !AssignInteger(probe["strategy_id"], "fill_probe.strategy_id",
                     &config.probe.strategy_id, &error) ||
      !AssignInteger(probe["max_nodes"], "fill_probe.max_nodes",
                     &config.probe.max_nodes, &error) ||
      !AssignInteger(probe["duration_ms"], "fill_probe.duration_ms",
                     &config.probe.duration_ms, &error) ||
      !AssignInteger(probe["node_pause_ms"], "fill_probe.node_pause_ms",
                     &config.probe.node_pause_ms, &error) ||
      !AssignInteger(probe["gtc_cancel_after_ms"],
                     "fill_probe.gtc_cancel_after_ms",
                     &config.probe.gtc_cancel_after_ms, &error) ||
      !AssignInteger(probe["unresolved_timeout_ms"],
                     "fill_probe.unresolved_timeout_ms",
                     &config.probe.unresolved_timeout_ms, &error) ||
      !AssignDouble(probe["max_entry_notional_usdt"],
                    "fill_probe.max_entry_notional_usdt",
                    &config.probe.max_entry_notional_usdt, &error) ||
      !AssignInteger(probe["max_close_retries"], "fill_probe.max_close_retries",
                     &config.probe.max_close_retries, &error) ||
      !AssignInteger(probe["close_slippage_bps"],
                     "fill_probe.close_slippage_bps",
                     &config.probe.close_slippage_bps, &error) ||
      !AssignSignedInteger(probe["max_local_freshness_ns"],
                           "fill_probe.max_local_freshness_ns",
                           &config.probe.max_local_freshness_ns, &error) ||
      !AssignSignedInteger(probe["max_exchange_freshness_ns"],
                           "fill_probe.max_exchange_freshness_ns",
                           &config.probe.max_exchange_freshness_ns, &error)) {
    return Failure(std::move(error));
  }

  std::string catalog_file;
  if (!AssignString(root["instrument_catalog"]["file"],
                    "instrument_catalog.file", &catalog_file, &error)) {
    return Failure(std::move(error));
  }
  config.instrument_catalog_file = catalog_file;

  const toml::node_view<const toml::node> market_data = root["market_data"];
  if (!AssignString(market_data["shm_name"], "market_data.shm_name",
                    &config.market_data.shm_name, &error)) {
    return Failure(std::move(error));
  }
  config.market_data.channel_name =
      market_data["channel_name"].value_or(config.market_data.channel_name);

  const toml::node_view<const toml::node> order_gateway = root["order_gateway"];
  if (!AssignString(order_gateway["shm_name"], "order_gateway.shm_name",
                    &config.order_gateway.shm_name, &error) ||
      !AssignInteger(order_gateway["route_count"], "order_gateway.route_count",
                     &config.order_gateway.route_count, &error) ||
      !AssignInteger(order_gateway["command_queue_capacity"],
                     "order_gateway.command_queue_capacity",
                     &config.order_gateway.command_queue_capacity, &error) ||
      !AssignInteger(order_gateway["event_queue_capacity"],
                     "order_gateway.event_queue_capacity",
                     &config.order_gateway.event_queue_capacity, &error) ||
      !AssignInteger(order_gateway["startup_ready_timeout_s"],
                     "order_gateway.startup_ready_timeout_s",
                     &config.order_gateway.startup_ready_timeout_s, &error) ||
      !AssignInteger(order_gateway["gtc_route_id"],
                     "order_gateway.gtc_route_id",
                     &config.order_gateway.gtc_route_id, &error) ||
      !AssignInteger(order_gateway["ioc_route_id"],
                     "order_gateway.ioc_route_id",
                     &config.order_gateway.ioc_route_id, &error)) {
    return Failure(std::move(error));
  }

  const toml::node_view<const toml::node> feedback = root["feedback"];
  if (!AssignString(feedback["shm_name"], "feedback.shm_name",
                    &config.feedback.shm_name, &error)) {
    return Failure(std::move(error));
  }
  config.feedback.channel_name =
      feedback["channel_name"].value_or(config.feedback.channel_name);
  AssignBool(feedback["force_claim"], config.feedback.force_claim,
             &config.feedback.force_claim);

  std::string run_dir;
  if (!AssignString(root["output"]["run_dir"], "output.run_dir", &run_dir,
                    &error)) {
    return Failure(std::move(error));
  }
  config.output.run_dir = run_dir;

  if (!ValidateConfig(config, &error)) {
    return Failure(std::move(error));
  }
  return Success(std::move(config));
}

}  // namespace

FillProbeConfigResult LoadConfig(const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load fill probe config: "} +
                   exc.what());
  }
}

}  // namespace aquila::tools::gate::fill_probe
