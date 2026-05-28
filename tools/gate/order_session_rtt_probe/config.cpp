#include "tools/gate/order_session_rtt_probe/config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace aquila::tools::gate_order_session_rtt_probe {
namespace {

[[nodiscard]] ProbeConfigResult Failure(std::string error) {
  ProbeConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] ProbeConfigResult Success(ProbeConfig config) {
  ProbeConfigResult result;
  result.ok = true;
  result.value = std::move(config);
  return result;
}

[[nodiscard]] std::string StringOr(toml::node_view<const toml::node> node,
                                   const std::string& fallback) {
  const std::optional<std::string> value = node.value<std::string>();
  return value.value_or(fallback);
}

[[nodiscard]] bool BoolOr(toml::node_view<const toml::node> node,
                          bool fallback) {
  const std::optional<bool> value = node.value<bool>();
  return value.value_or(fallback);
}

[[nodiscard]] bool ReadPositiveUInt32(toml::node_view<const toml::node> node,
                                      std::uint32_t fallback,
                                      std::string_view name,
                                      std::uint32_t* output,
                                      std::string* error) {
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (!value) {
    *output = fallback;
    return true;
  }
  if (*value <= 0 || *value > static_cast<std::int64_t>(
                                  std::numeric_limits<std::uint32_t>::max())) {
    *error = fmt::format("{} must be positive uint32", name);
    return false;
  }
  *output = static_cast<std::uint32_t>(*value);
  return true;
}

[[nodiscard]] bool ReadNonNegativeSize(toml::node_view<const toml::node> node,
                                       std::size_t fallback,
                                       std::string_view name,
                                       std::size_t* output,
                                       std::string* error) {
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (!value) {
    *output = fallback;
    return true;
  }
  if (*value < 0) {
    *error = fmt::format("{} must be non-negative", name);
    return false;
  }
  *output = static_cast<std::size_t>(*value);
  return true;
}

[[nodiscard]] bool ReadPositiveSize(toml::node_view<const toml::node> node,
                                    std::size_t fallback, std::string_view name,
                                    std::size_t* output, std::string* error) {
  if (!ReadNonNegativeSize(node, fallback, name, output, error)) {
    return false;
  }
  if (*output == 0) {
    *error = fmt::format("{} must be positive", name);
    return false;
  }
  return true;
}

[[nodiscard]] std::int32_t Int32Or(toml::node_view<const toml::node> node,
                                   std::int32_t fallback) {
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  return value ? static_cast<std::int32_t>(*value) : fallback;
}

[[nodiscard]] double DoubleOr(toml::node_view<const toml::node> node,
                              double fallback) {
  const std::optional<double> value = node.value<double>();
  return value.value_or(fallback);
}

[[nodiscard]] std::filesystem::path PathOr(
    toml::node_view<const toml::node> node,
    const std::filesystem::path& fallback) {
  const std::optional<std::string> value = node.value<std::string>();
  return value ? std::filesystem::path{*value} : fallback;
}

[[nodiscard]] std::vector<std::int32_t> Int32ArrayOrEmpty(
    toml::node_view<const toml::node> node) {
  std::vector<std::int32_t> values;
  const toml::array* array = node.as_array();
  if (array == nullptr) {
    return values;
  }
  values.reserve(array->size());
  for (const toml::node& item : *array) {
    const std::optional<std::int64_t> value = item.value<std::int64_t>();
    if (value) {
      values.push_back(static_cast<std::int32_t>(*value));
    }
  }
  return values;
}

}  // namespace

ProbeConfigResult ParseProbeConfig(const toml::table& root) {
  ProbeConfig config;
  const toml::node_view<const toml::node> probe = root["probe"];
  const toml::node_view<const toml::node> inputs = root["probe"]["inputs"];
  const toml::node_view<const toml::node> sessions = root["probe"]["sessions"];
  const toml::node_view<const toml::node> sampling = root["probe"]["sampling"];
  const toml::node_view<const toml::node> order = root["probe"]["order"];
  const toml::node_view<const toml::node> feedback = root["probe"]["feedback"];
  const toml::node_view<const toml::node> safety = root["probe"]["safety"];
  const toml::node_view<const toml::node> output = root["probe"]["output"];

  config.name = StringOr(probe["name"], config.name);
  config.execute = BoolOr(probe["execute"], config.execute);
  config.run_id = StringOr(probe["run_id"], config.run_id);

  config.inputs.order_session_config = PathOr(
      inputs["order_session_config"], config.inputs.order_session_config);
  config.inputs.data_reader_config =
      PathOr(inputs["data_reader_config"], config.inputs.data_reader_config);
  config.inputs.candidate_ip_file =
      PathOr(inputs["candidate_ip_file"], config.inputs.candidate_ip_file);

  std::string error;
  if (!ReadPositiveSize(sessions["active_session_count"],
                        config.sessions.active_session_count,
                        "probe.sessions.active_session_count",
                        &config.sessions.active_session_count, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadNonNegativeSize(sessions["max_candidates"],
                           config.sessions.max_candidates,
                           "probe.sessions.max_candidates",
                           &config.sessions.max_candidates, &error)) {
    return Failure(std::move(error));
  }
  config.sessions.enable_tcp_info =
      BoolOr(sessions["enable_tcp_info"], config.sessions.enable_tcp_info);
  if (!ReadPositiveUInt32(sessions["wait_login_timeout_ms"],
                          config.sessions.wait_login_timeout_ms,
                          "probe.sessions.wait_login_timeout_ms",
                          &config.sessions.wait_login_timeout_ms, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(sessions["request_timeout_ms"],
                          config.sessions.request_timeout_ms,
                          "probe.sessions.request_timeout_ms",
                          &config.sessions.request_timeout_ms, &error)) {
    return Failure(std::move(error));
  }
  config.sessions.worker_cpu_ids =
      Int32ArrayOrEmpty(sessions["worker_cpu_ids"]);

  if (!ReadPositiveUInt32(sampling["samples_per_ip"],
                          config.sampling.samples_per_ip,
                          "probe.sampling.samples_per_ip",
                          &config.sampling.samples_per_ip, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(sampling["cycle_cooldown_ms"],
                          config.sampling.cycle_cooldown_ms,
                          "probe.sampling.cycle_cooldown_ms",
                          &config.sampling.cycle_cooldown_ms, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(sampling["max_events_per_drain"],
                          config.sampling.max_events_per_drain,
                          "probe.sampling.max_events_per_drain",
                          &config.sampling.max_events_per_drain, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(sampling["cycles_per_connection_generation"],
                          config.sampling.cycles_per_connection_generation,
                          "probe.sampling.cycles_per_connection_generation",
                          &config.sampling.cycles_per_connection_generation,
                          &error)) {
    return Failure(std::move(error));
  }
  config.sampling.idle_policy =
      StringOr(sampling["idle_policy"], config.sampling.idle_policy);
  config.sampling.coordinator_cpu =
      Int32Or(sampling["coordinator_cpu"], config.sampling.coordinator_cpu);

  config.order.side = StringOr(order["side"], config.order.side);
  config.order.passive_price_limit_fraction =
      DoubleOr(order["passive_price_limit_fraction"],
               config.order.passive_price_limit_fraction);
  config.order.quantity_mode =
      StringOr(order["quantity_mode"], config.order.quantity_mode);
  config.order.reduce_only_close =
      BoolOr(order["reduce_only_close"], config.order.reduce_only_close);
  if (config.order.side != "buy") {
    return Failure("probe.order.side must be buy in V1a");
  }
  if (config.order.quantity_mode != "min_quantity") {
    return Failure("probe.order.quantity_mode must be min_quantity in V1a");
  }

  config.feedback.enabled =
      BoolOr(feedback["enabled"], config.feedback.enabled);
  config.feedback.shm_config =
      PathOr(feedback["shm_config"], config.feedback.shm_config);
  if (!ReadPositiveUInt32(feedback["strategy_id"], config.feedback.strategy_id,
                          "probe.feedback.strategy_id",
                          &config.feedback.strategy_id, &error)) {
    return Failure(std::move(error));
  }
  config.feedback.force_claim =
      BoolOr(feedback["force_claim"], config.feedback.force_claim);
  if (!ReadPositiveUInt32(feedback["poll_budget"], config.feedback.poll_budget,
                          "probe.feedback.poll_budget",
                          &config.feedback.poll_budget, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(feedback["terminal_timeout_ms"],
                          config.feedback.terminal_timeout_ms,
                          "probe.feedback.terminal_timeout_ms",
                          &config.feedback.terminal_timeout_ms, &error)) {
    return Failure(std::move(error));
  }

  config.safety.preflight_rest_check = BoolOr(
      safety["preflight_rest_check"], config.safety.preflight_rest_check);
  config.safety.run_end_rest_check =
      BoolOr(safety["run_end_rest_check"], config.safety.run_end_rest_check);
  if (!ReadPositiveUInt32(safety["rest_timeout_ms"],
                          config.safety.rest_timeout_ms,
                          "probe.safety.rest_timeout_ms",
                          &config.safety.rest_timeout_ms, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(safety["rest_poll_interval_ms"],
                          config.safety.rest_poll_interval_ms,
                          "probe.safety.rest_poll_interval_ms",
                          &config.safety.rest_poll_interval_ms, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(safety["rest_poll_timeout_ms"],
                          config.safety.rest_poll_timeout_ms,
                          "probe.safety.rest_poll_timeout_ms",
                          &config.safety.rest_poll_timeout_ms, &error)) {
    return Failure(std::move(error));
  }
  config.safety.stop_on_continuity_lost = BoolOr(
      safety["stop_on_continuity_lost"], config.safety.stop_on_continuity_lost);
  config.safety.confirm_dedicated_account =
      BoolOr(safety["confirm_dedicated_account"],
             config.safety.confirm_dedicated_account);

  config.output.root_dir = PathOr(output["root_dir"], config.output.root_dir);

  if (config.inputs.candidate_ip_file.empty()) {
    return Failure("probe.inputs.candidate_ip_file is required");
  }
  return Success(std::move(config));
}

ProbeConfigResult LoadProbeConfigFile(const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseProbeConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(fmt::format("failed to load probe config '{}': {}",
                               path.string(), exc.what()));
  }
}

}  // namespace aquila::tools::gate_order_session_rtt_probe
