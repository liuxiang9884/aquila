#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CONFIG_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "tools/gate/order_session_rtt_probe/order_mode.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct ProbeInputConfig {
  std::filesystem::path order_session_config{
      "config/order_sessions/gate_order_session.toml"};
  std::filesystem::path data_reader_config{
      "config/data_readers/strategy_data_reader_requested_20260521.toml"};
  std::filesystem::path candidate_ip_file;
};

struct ProbeSessionEndpointOverride {
  std::size_t index{0};
  std::optional<std::string> host;
  std::optional<std::string> connect_ip;
  std::optional<std::string> port;
  std::optional<bool> enable_tls;
};

struct ProbeSessionConfig {
  std::size_t active_session_count{1};
  std::size_t max_candidates{1};
  bool enable_tcp_info{true};
  std::uint32_t wait_login_timeout_ms{10000};
  std::uint32_t request_timeout_ms{5000};
  std::vector<std::int32_t> worker_cpu_ids;
  std::vector<ProbeSessionEndpointOverride> endpoint_overrides;
};

struct ProbeSamplingConfig {
  std::uint32_t samples_per_ip{1};
  std::uint32_t cycle_cooldown_ms{500};
  std::uint32_t order_session_interval_ms{0};
  std::uint32_t max_events_per_drain{128};
  std::uint32_t cycles_per_connection_generation{1};
  std::string idle_policy{"spin"};
  std::int32_t coordinator_cpu{-1};
};

struct ProbeOrderConfig {
  ProbeOrderMode order_mode{ProbeOrderMode::kIocAndGtc};
  std::string side{"buy"};
  double passive_price_limit_fraction{0.5};
  std::string quantity_mode{"min_quantity"};
  bool reduce_only_close{true};
};

struct ProbeFeedbackConfig {
  bool enabled{true};
  std::filesystem::path shm_config{
      "config/order_feedback/gate_order_feedback_shm.toml"};
  std::uint32_t strategy_id{7};
  bool force_claim{false};
  std::uint32_t poll_budget{64};
  std::uint32_t terminal_timeout_ms{5000};
};

struct ProbeSafetyConfig {
  bool preflight_rest_check{true};
  bool run_end_rest_check{true};
  std::uint32_t rest_timeout_ms{8000};
  std::uint32_t rest_poll_interval_ms{500};
  std::uint32_t rest_poll_timeout_ms{10000};
  bool stop_on_continuity_lost{true};
  bool confirm_dedicated_account{true};
};

struct ProbeOutputConfig {
  std::filesystem::path root_dir{
      "/home/liuxiang/tmp/gate_order_session_rtt_probe"};
};

struct ProbeConfig {
  std::string name{"gate_order_session_rtt_probe"};
  bool execute{false};
  std::string run_id;
  ProbeInputConfig inputs;
  ProbeSessionConfig sessions;
  ProbeSamplingConfig sampling;
  ProbeOrderConfig order;
  ProbeFeedbackConfig feedback;
  ProbeSafetyConfig safety;
  ProbeOutputConfig output;
};

using ProbeConfigResult = Result<ProbeConfig>;

[[nodiscard]] ProbeConfigResult ParseProbeConfig(const toml::table& root);

[[nodiscard]] ProbeConfigResult LoadProbeConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CONFIG_H_
