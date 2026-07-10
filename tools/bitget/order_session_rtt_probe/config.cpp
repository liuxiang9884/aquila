#include "tools/bitget/order_session_rtt_probe/config.h"

#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "core/trading/order_feedback_shm.h"

namespace aquila::tools::bitget_order_session_rtt_probe {
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

[[nodiscard]] bool ReadTable(toml::node_view<const toml::node> node,
                             std::string_view name, std::string* error) {
  if (Missing(node) || node.as_table() != nullptr) {
    return true;
  }
  *error = fmt::format("{} must be table", name);
  return false;
}

[[nodiscard]] bool ReadString(toml::node_view<const toml::node> node,
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

[[nodiscard]] bool ReadPath(toml::node_view<const toml::node> node,
                            const std::filesystem::path& fallback,
                            std::string_view name,
                            std::filesystem::path* output, std::string* error) {
  std::string value;
  if (!ReadString(node, fallback.string(), name, &value, error)) {
    return false;
  }
  *output = value;
  return true;
}

[[nodiscard]] bool ReadBool(toml::node_view<const toml::node> node,
                            bool fallback, std::string_view name, bool* output,
                            std::string* error) {
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

template <typename UInt>
[[nodiscard]] bool ReadPositiveUnsigned(toml::node_view<const toml::node> node,
                                        UInt fallback, std::string_view name,
                                        UInt* output, std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (!value || *value <= 0 ||
      static_cast<std::uint64_t>(*value) >
          static_cast<std::uint64_t>(std::numeric_limits<UInt>::max())) {
    *error = fmt::format("{} must be positive unsigned integer", name);
    return false;
  }
  *output = static_cast<UInt>(*value);
  return true;
}

template <typename UInt>
[[nodiscard]] bool ReadUnsigned(toml::node_view<const toml::node> node,
                                UInt fallback, std::string_view name,
                                UInt* output, std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (!value || *value < 0 ||
      static_cast<std::uint64_t>(*value) >
          static_cast<std::uint64_t>(std::numeric_limits<UInt>::max())) {
    *error = fmt::format("{} must be unsigned integer", name);
    return false;
  }
  *output = static_cast<UInt>(*value);
  return true;
}

[[nodiscard]] bool ReadCpu(toml::node_view<const toml::node> node,
                           std::int32_t fallback, std::string_view name,
                           std::int32_t* output, std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<std::int64_t> value = node.value<std::int64_t>();
  if (!value || *value < -1 ||
      *value > std::numeric_limits<std::int32_t>::max()) {
    *error = fmt::format("{} must be -1 or int32", name);
    return false;
  }
  *output = static_cast<std::int32_t>(*value);
  return true;
}

[[nodiscard]] bool ReadFraction(toml::node_view<const toml::node> node,
                                double fallback, std::string_view name,
                                double* output, std::string* error) {
  if (Missing(node)) {
    *output = fallback;
    return true;
  }
  const std::optional<double> value = node.value<double>();
  if (!value || !std::isfinite(*value) || *value <= 0.0 || *value > 1.0) {
    *error = fmt::format("{} must be finite and in (0, 1]", name);
    return false;
  }
  *output = *value;
  return true;
}

[[nodiscard]] bool Validate(const ProbeConfig& config, std::string* error) {
  if (config.name.empty()) {
    *error = "probe.name must be non-empty";
    return false;
  }
  if (config.inputs.order_session_config.empty() ||
      config.inputs.data_reader_config.empty() ||
      config.inputs.connections_file.empty()) {
    *error = "probe.inputs paths must be non-empty";
    return false;
  }
  if (config.order.order_mode != "ioc") {
    *error = "probe.order.order_mode must be ioc";
    return false;
  }
  if (config.feedback.strategy_id >= kMaxOrderFeedbackStrategies) {
    *error = "probe.feedback.strategy_id must be less than 8";
    return false;
  }
  if (config.feedback.shm_config.empty()) {
    *error = "probe.feedback.shm_config must be non-empty";
    return false;
  }
  if (config.output.root_dir.empty()) {
    *error = "probe.output.root_dir must be non-empty";
    return false;
  }
  return true;
}

}  // namespace

ProbeConfigResult ParseProbeConfig(const toml::table& root) {
  ProbeConfig config;
  std::string error;
  const auto probe = root["probe"];
  if (probe.as_table() == nullptr) {
    return Failure("probe must be table");
  }
  if (!ReadString(probe["name"], config.name, "probe.name", &config.name,
                  &error) ||
      !ReadString(probe["run_id"], config.run_id, "probe.run_id",
                  &config.run_id, &error)) {
    return Failure(std::move(error));
  }

  const auto inputs = probe["inputs"];
  if (!ReadTable(inputs, "probe.inputs", &error) ||
      !ReadPath(inputs["order_session_config"],
                config.inputs.order_session_config,
                "probe.inputs.order_session_config",
                &config.inputs.order_session_config, &error) ||
      !ReadPath(inputs["data_reader_config"], config.inputs.data_reader_config,
                "probe.inputs.data_reader_config",
                &config.inputs.data_reader_config, &error) ||
      !ReadPath(inputs["connections_file"], config.inputs.connections_file,
                "probe.inputs.connections_file",
                &config.inputs.connections_file, &error)) {
    return Failure(std::move(error));
  }

  const auto sessions = probe["sessions"];
  if (!ReadTable(sessions, "probe.sessions", &error) ||
      !ReadPositiveUnsigned(sessions["wait_login_timeout_ms"],
                            config.sessions.wait_login_timeout_ms,
                            "probe.sessions.wait_login_timeout_ms",
                            &config.sessions.wait_login_timeout_ms, &error) ||
      !ReadPositiveUnsigned(sessions["request_timeout_ms"],
                            config.sessions.request_timeout_ms,
                            "probe.sessions.request_timeout_ms",
                            &config.sessions.request_timeout_ms, &error)) {
    return Failure(std::move(error));
  }

  const auto sampling = probe["sampling"];
  if (!ReadTable(sampling, "probe.sampling", &error) ||
      !ReadPositiveUnsigned(sampling["samples_per_session"],
                            config.sampling.samples_per_session,
                            "probe.sampling.samples_per_session",
                            &config.sampling.samples_per_session, &error) ||
      !ReadUnsigned(sampling["cycle_cooldown_us"],
                    config.sampling.cycle_cooldown_us,
                    "probe.sampling.cycle_cooldown_us",
                    &config.sampling.cycle_cooldown_us, &error) ||
      !ReadUnsigned(sampling["order_session_interval_us"],
                    config.sampling.order_session_interval_us,
                    "probe.sampling.order_session_interval_us",
                    &config.sampling.order_session_interval_us, &error) ||
      !ReadPositiveUnsigned(sampling["max_events_per_drain"],
                            config.sampling.max_events_per_drain,
                            "probe.sampling.max_events_per_drain",
                            &config.sampling.max_events_per_drain, &error) ||
      !ReadPositiveUnsigned(sampling["feedback_queue_capacity"],
                            config.sampling.feedback_queue_capacity,
                            "probe.sampling.feedback_queue_capacity",
                            &config.sampling.feedback_queue_capacity, &error) ||
      !ReadCpu(sampling["coordinator_cpu"], config.sampling.coordinator_cpu,
               "probe.sampling.coordinator_cpu",
               &config.sampling.coordinator_cpu, &error)) {
    return Failure(std::move(error));
  }

  const auto order = probe["order"];
  if (!ReadTable(order, "probe.order", &error) ||
      !ReadString(order["order_mode"], config.order.order_mode,
                  "probe.order.order_mode", &config.order.order_mode, &error) ||
      !ReadFraction(order["passive_price_limit_fraction"],
                    config.order.passive_price_limit_fraction,
                    "probe.order.passive_price_limit_fraction",
                    &config.order.passive_price_limit_fraction, &error) ||
      !ReadPositiveUnsigned(order["bbo_freshness_ns"],
                            config.order.bbo_freshness_ns,
                            "probe.order.bbo_freshness_ns",
                            &config.order.bbo_freshness_ns, &error)) {
    return Failure(std::move(error));
  }

  const auto feedback = probe["feedback"];
  std::uint32_t strategy_id = config.feedback.strategy_id;
  if (!ReadTable(feedback, "probe.feedback", &error) ||
      !ReadPath(feedback["shm_config"], config.feedback.shm_config,
                "probe.feedback.shm_config", &config.feedback.shm_config,
                &error) ||
      !ReadUnsigned(feedback["strategy_id"], strategy_id,
                    "probe.feedback.strategy_id", &strategy_id, &error) ||
      !ReadBool(feedback["force_claim"], config.feedback.force_claim,
                "probe.feedback.force_claim", &config.feedback.force_claim,
                &error) ||
      !ReadPositiveUnsigned(
          feedback["poll_budget"], config.feedback.poll_budget,
          "probe.feedback.poll_budget", &config.feedback.poll_budget, &error) ||
      !ReadPositiveUnsigned(feedback["terminal_timeout_ms"],
                            config.feedback.terminal_timeout_ms,
                            "probe.feedback.terminal_timeout_ms",
                            &config.feedback.terminal_timeout_ms, &error)) {
    return Failure(std::move(error));
  }
  if (strategy_id > std::numeric_limits<std::uint8_t>::max()) {
    return Failure("probe.feedback.strategy_id must fit uint8");
  }
  config.feedback.strategy_id = static_cast<std::uint8_t>(strategy_id);

  const auto output = probe["output"];
  if (!ReadTable(output, "probe.output", &error) ||
      !ReadPath(output["root_dir"], config.output.root_dir,
                "probe.output.root_dir", &config.output.root_dir, &error)) {
    return Failure(std::move(error));
  }
  if (!Validate(config, &error)) {
    return Failure(std::move(error));
  }
  return Success(std::move(config));
}

ProbeConfigResult LoadProbeConfigFile(const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseProbeConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(
        fmt::format("failed to load Bitget RTT probe config '{}': {}",
                    path.string(), exc.what()));
  }
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe
