#include "tools/gate/order_session_rtt_probe/config.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "core/config/order_feedback_shm_config.h"

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

[[nodiscard]] bool Missing(toml::node_view<const toml::node> node) noexcept {
  return node.node() == nullptr;
}

[[nodiscard]] bool ReadTableOrMissing(toml::node_view<const toml::node> node,
                                      std::string_view name,
                                      std::string* error) {
  if (Missing(node) || node.as_table() != nullptr) {
    return true;
  }
  *error = fmt::format("{} must be table", name);
  return false;
}

[[nodiscard]] bool ReadStringOr(toml::node_view<const toml::node> node,
                                const std::string& fallback,
                                std::string_view name, std::string* output,
                                std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<std::string> value = node.value<std::string>();
  if (!value) {
    *error = fmt::format("{} must be string", name);
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] bool ReadBoolOr(toml::node_view<const toml::node> node,
                              bool fallback, std::string_view name,
                              bool* output, std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<bool> value = node.value<bool>();
  if (!value) {
    *error = fmt::format("{} must be bool", name);
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] bool ReadPositiveUInt32(toml::node_view<const toml::node> node,
                                      std::uint32_t fallback,
                                      std::string_view name,
                                      std::uint32_t* output,
                                      std::string* error) {
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  if (!value) {
    *error = fmt::format("{} must be integer", name);
    return false;
  }
  if (*value <= 0 || *value > static_cast<std::int64_t>(
                                  std::numeric_limits<std::uint32_t>::max())) {
    *error = fmt::format("{} must be positive uint32", name);
    return false;
  }
  *output = static_cast<std::uint32_t>(*value);
  return true;
}

[[nodiscard]] bool ReadUInt32InRange(
    toml::node_view<const toml::node> node, std::uint32_t fallback,
    std::string_view name, std::uint32_t min_value, std::uint32_t max_value,
    std::uint32_t* output, std::string* error) {
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  if (!value) {
    *error = fmt::format("{} must be integer", name);
    return false;
  }
  if (*value < static_cast<std::int64_t>(min_value) ||
      *value > static_cast<std::int64_t>(max_value)) {
    *error = fmt::format("{} must be in [{}, {}]", name, min_value, max_value);
    return false;
  }
  *output = static_cast<std::uint32_t>(*value);
  return true;
}

[[nodiscard]] bool ReadInt32InRange(toml::node_view<const toml::node> node,
                                    std::int32_t fallback,
                                    std::string_view name,
                                    std::int32_t min_value,
                                    std::int32_t max_value,
                                    std::int32_t* output, std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (!value) {
    *error = fmt::format("{} must be integer", name);
    return false;
  }
  if (*value < min_value || *value > max_value) {
    *error = fmt::format("{} must be in [{}, {}]", name, min_value, max_value);
    return false;
  }
  *output = static_cast<std::int32_t>(*value);
  return true;
}

[[nodiscard]] bool ReadDoubleOr(toml::node_view<const toml::node> node,
                                double fallback, std::string_view name,
                                double* output, std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  if (const std::optional<double> value = node.value<double>()) {
    *output = *value;
    return true;
  }
  if (const std::optional<std::int64_t> value = node.value<std::int64_t>()) {
    *output = static_cast<double>(*value);
    return true;
  }
  *error = fmt::format("{} must be number", name);
  return false;
}

[[nodiscard]] bool ReadPathOr(toml::node_view<const toml::node> node,
                              const std::filesystem::path& fallback,
                              std::string_view name,
                              std::filesystem::path* output,
                              std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<std::string> value = node.value<std::string>();
  if (!value) {
    *error = fmt::format("{} must be string path", name);
    return false;
  }
  *output = std::filesystem::path{*value};
  return true;
}

[[nodiscard]] bool ReadProbeOrderModeOr(toml::node_view<const toml::node> node,
                                        ProbeOrderMode fallback,
                                        std::string_view name,
                                        ProbeOrderMode* output,
                                        std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<std::string> value = node.value<std::string>();
  if (!value) {
    *error = fmt::format("{} must be string", name);
    return false;
  }
  if (*value == "ioc") {
    *output = ProbeOrderMode::kIoc;
    return true;
  }
  if (*value == "gtc") {
    *output = ProbeOrderMode::kGtc;
    return true;
  }
  if (*value == "ioc+gtc" || *value == "gtc+ioc") {
    *output = ProbeOrderMode::kIocAndGtc;
    return true;
  }
  *error = fmt::format("{} must be one of ioc, gtc, ioc+gtc", name);
  return false;
}

}  // namespace

ProbeConfigResult ParseProbeConfig(const toml::table& root) {
  ProbeConfig config;
  const toml::node_view<const toml::node> probe = root["probe"];
  const toml::node_view<const toml::node> inputs = root["probe"]["inputs"];
  const toml::node_view<const toml::node> sessions = root["probe"]["sessions"];
  const toml::node_view<const toml::node> timestamping =
      root["probe"]["sessions"]["timestamping"];
  const toml::node_view<const toml::node> sampling = root["probe"]["sampling"];
  const toml::node_view<const toml::node> order = root["probe"]["order"];
  const toml::node_view<const toml::node> feedback = root["probe"]["feedback"];
  const toml::node_view<const toml::node> safety = root["probe"]["safety"];
  const toml::node_view<const toml::node> output = root["probe"]["output"];

  std::string error;
  if (!ReadTableOrMissing(probe, "probe", &error) ||
      !ReadTableOrMissing(inputs, "probe.inputs", &error) ||
      !ReadTableOrMissing(sessions, "probe.sessions", &error) ||
      !ReadTableOrMissing(timestamping, "probe.sessions.timestamping",
                          &error) ||
      !ReadTableOrMissing(sampling, "probe.sampling", &error) ||
      !ReadTableOrMissing(order, "probe.order", &error) ||
      !ReadTableOrMissing(feedback, "probe.feedback", &error) ||
      !ReadTableOrMissing(safety, "probe.safety", &error) ||
      !ReadTableOrMissing(output, "probe.output", &error)) {
    return Failure(std::move(error));
  }

  if (!ReadStringOr(probe["name"], config.name, "probe.name", &config.name,
                    &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(probe["execute"], config.execute, "probe.execute",
                  &config.execute, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadStringOr(probe["run_id"], config.run_id, "probe.run_id",
                    &config.run_id, &error)) {
    return Failure(std::move(error));
  }

  if (!ReadPathOr(inputs["order_session_config"],
                  config.inputs.order_session_config,
                  "probe.inputs.order_session_config",
                  &config.inputs.order_session_config, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPathOr(inputs["data_reader_config"],
                  config.inputs.data_reader_config,
                  "probe.inputs.data_reader_config",
                  &config.inputs.data_reader_config, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPathOr(inputs["connections_file"], config.inputs.connections_file,
                  "probe.inputs.connections_file",
                  &config.inputs.connections_file, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(sessions["enable_tcp_info"], config.sessions.enable_tcp_info,
                  "probe.sessions.enable_tcp_info",
                  &config.sessions.enable_tcp_info, &error)) {
    return Failure(std::move(error));
  }
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
  if (!ReadBoolOr(timestamping["enabled"], config.sessions.timestamping.enabled,
                  "probe.sessions.timestamping.enabled",
                  &config.sessions.timestamping.enabled, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(timestamping["tx_sched"],
                  config.sessions.timestamping.tx_sched,
                  "probe.sessions.timestamping.tx_sched",
                  &config.sessions.timestamping.tx_sched, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(timestamping["tx_software"],
                  config.sessions.timestamping.tx_software,
                  "probe.sessions.timestamping.tx_software",
                  &config.sessions.timestamping.tx_software, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(timestamping["tx_ack"], config.sessions.timestamping.tx_ack,
                  "probe.sessions.timestamping.tx_ack",
                  &config.sessions.timestamping.tx_ack, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(timestamping["rx_software"],
                  config.sessions.timestamping.rx_software,
                  "probe.sessions.timestamping.rx_software",
                  &config.sessions.timestamping.rx_software, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(timestamping["hardware"],
                  config.sessions.timestamping.hardware,
                  "probe.sessions.timestamping.hardware",
                  &config.sessions.timestamping.hardware, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadUInt32InRange(
          timestamping["max_errqueue_events_per_drain"],
          config.sessions.timestamping.max_errqueue_events_per_drain,
          "probe.sessions.timestamping.max_errqueue_events_per_drain", 0,
          std::numeric_limits<std::uint32_t>::max(),
          &config.sessions.timestamping.max_errqueue_events_per_drain,
          &error)) {
    return Failure(std::move(error));
  }

  if (!ReadPositiveUInt32(sampling["samples_per_session"],
                          config.sampling.samples_per_session,
                          "probe.sampling.samples_per_session",
                          &config.sampling.samples_per_session, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadPositiveUInt32(sampling["cycle_cooldown_ms"],
                          config.sampling.cycle_cooldown_ms,
                          "probe.sampling.cycle_cooldown_ms",
                          &config.sampling.cycle_cooldown_ms, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadUInt32InRange(sampling["order_session_interval_ms"],
                         config.sampling.order_session_interval_ms,
                         "probe.sampling.order_session_interval_ms", 0,
                         std::numeric_limits<std::uint32_t>::max(),
                         &config.sampling.order_session_interval_ms, &error)) {
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
  if (config.sampling.cycles_per_connection_generation != 1) {
    return Failure(
        "probe.sampling.cycles_per_connection_generation must be 1 in V1a");
  }
  if (!ReadStringOr(sampling["idle_policy"], config.sampling.idle_policy,
                    "probe.sampling.idle_policy", &config.sampling.idle_policy,
                    &error)) {
    return Failure(std::move(error));
  }
  if (!ReadInt32InRange(sampling["coordinator_cpu"],
                        config.sampling.coordinator_cpu,
                        "probe.sampling.coordinator_cpu", -1,
                        std::numeric_limits<std::int32_t>::max(),
                        &config.sampling.coordinator_cpu, &error)) {
    return Failure(std::move(error));
  }

  if (!ReadProbeOrderModeOr(order["order_mode"], config.order.order_mode,
                            "probe.order.order_mode", &config.order.order_mode,
                            &error)) {
    return Failure(std::move(error));
  }
  if (!ReadStringOr(order["side"], config.order.side, "probe.order.side",
                    &config.order.side, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadDoubleOr(order["passive_price_limit_fraction"],
                    config.order.passive_price_limit_fraction,
                    "probe.order.passive_price_limit_fraction",
                    &config.order.passive_price_limit_fraction, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadStringOr(order["quantity_mode"], config.order.quantity_mode,
                    "probe.order.quantity_mode", &config.order.quantity_mode,
                    &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(order["reduce_only_close"], config.order.reduce_only_close,
                  "probe.order.reduce_only_close",
                  &config.order.reduce_only_close, &error)) {
    return Failure(std::move(error));
  }
  if (config.order.side != "buy") {
    return Failure("probe.order.side must be buy in V1a");
  }
  if (!std::isfinite(config.order.passive_price_limit_fraction) ||
      config.order.passive_price_limit_fraction <= 0.0 ||
      config.order.passive_price_limit_fraction > 1.0) {
    return Failure(
        "probe.order.passive_price_limit_fraction must be in (0, 1]");
  }
  if (config.order.quantity_mode != "min_quantity") {
    return Failure("probe.order.quantity_mode must be min_quantity in V1a");
  }
  if (!config.order.reduce_only_close) {
    return Failure("probe.order.reduce_only_close must be true in V1a");
  }

  if (!ReadBoolOr(feedback["enabled"], config.feedback.enabled,
                  "probe.feedback.enabled", &config.feedback.enabled, &error)) {
    return Failure(std::move(error));
  }
  if (!config.feedback.enabled) {
    return Failure("probe.feedback.enabled must be true in V1a");
  }
  if (!ReadPathOr(feedback["shm_config"], config.feedback.shm_config,
                  "probe.feedback.shm_config", &config.feedback.shm_config,
                  &error)) {
    return Failure(std::move(error));
  }
  if (!ReadUInt32InRange(feedback["strategy_id"], config.feedback.strategy_id,
                         "probe.feedback.strategy_id", 0,
                         config::kOrderFeedbackShmMaxStrategyCount - 1,
                         &config.feedback.strategy_id, &error)) {
    return Failure(std::move(error));
  }
  if (!ReadBoolOr(feedback["force_claim"], config.feedback.force_claim,
                  "probe.feedback.force_claim", &config.feedback.force_claim,
                  &error)) {
    return Failure(std::move(error));
  }
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

  if (!ReadBoolOr(safety["preflight_rest_check"],
                  config.safety.preflight_rest_check,
                  "probe.safety.preflight_rest_check",
                  &config.safety.preflight_rest_check, &error)) {
    return Failure(std::move(error));
  }
  if (!config.safety.preflight_rest_check) {
    return Failure("probe.safety.preflight_rest_check must be true in V1a");
  }
  if (!ReadBoolOr(safety["run_end_rest_check"],
                  config.safety.run_end_rest_check,
                  "probe.safety.run_end_rest_check",
                  &config.safety.run_end_rest_check, &error)) {
    return Failure(std::move(error));
  }
  if (!config.safety.run_end_rest_check) {
    return Failure("probe.safety.run_end_rest_check must be true in V1a");
  }
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
  if (!ReadBoolOr(safety["stop_on_continuity_lost"],
                  config.safety.stop_on_continuity_lost,
                  "probe.safety.stop_on_continuity_lost",
                  &config.safety.stop_on_continuity_lost, &error)) {
    return Failure(std::move(error));
  }
  if (!config.safety.stop_on_continuity_lost) {
    return Failure("probe.safety.stop_on_continuity_lost must be true in V1a");
  }
  if (!ReadBoolOr(safety["confirm_dedicated_account"],
                  config.safety.confirm_dedicated_account,
                  "probe.safety.confirm_dedicated_account",
                  &config.safety.confirm_dedicated_account, &error)) {
    return Failure(std::move(error));
  }
  if (!config.safety.confirm_dedicated_account) {
    return Failure(
        "probe.safety.confirm_dedicated_account must be true in V1a");
  }

  if (!ReadPathOr(output["root_dir"], config.output.root_dir,
                  "probe.output.root_dir", &config.output.root_dir, &error)) {
    return Failure(std::move(error));
  }

  if (config.inputs.connections_file.empty()) {
    return Failure("probe.inputs.connections_file is required");
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
