#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_CONFIG_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

struct ProbeInputConfig {
  std::filesystem::path order_session_config{
      "config/order_sessions/bitget_order_session.toml"};
  std::filesystem::path data_reader_config{
      "config/data_readers/bitget_order_session_rtt_probe.toml"};
  std::filesystem::path connections_file{
      "config/order_session_rtt_probe/"
      "bitget_order_session_rtt_connections.csv"};
};

struct ProbeSessionConfig {
  std::uint32_t wait_login_timeout_ms{10000};
  std::uint32_t request_timeout_ms{5000};
};

struct ProbeSamplingConfig {
  std::uint32_t samples_per_session{1};
  std::uint64_t cycle_cooldown_us{500000};
  std::uint64_t order_session_interval_us{500000};
  std::uint32_t max_events_per_drain{128};
  std::uint32_t feedback_queue_capacity{256};
  std::int32_t coordinator_cpu{-1};
};

struct ProbeOrderConfig {
  std::string order_mode{"ioc"};
  std::string symbol{"BTC_USDT"};
  double passive_price_limit_fraction{0.5};
  std::uint64_t bbo_freshness_ns{1'000'000'000ULL};
};

struct ProbeFeedbackConfig {
  std::filesystem::path shm_config{
      "config/order_feedback/bitget_order_feedback_session.toml"};
  std::uint8_t strategy_id{7};
  bool force_claim{false};
  std::uint32_t poll_budget{64};
  std::uint32_t terminal_timeout_ms{5000};
};

struct ProbeOutputConfig {
  std::filesystem::path root_dir{
      "/home/liuxiang/tmp/bitget_order_session_rtt_probe"};
};

struct ProbeConfig {
  std::string name{"bitget_order_session_rtt_probe"};
  std::string run_id;
  ProbeInputConfig inputs;
  ProbeSessionConfig sessions;
  ProbeSamplingConfig sampling;
  ProbeOrderConfig order;
  ProbeFeedbackConfig feedback;
  ProbeOutputConfig output;
};

using ProbeConfigResult = Result<ProbeConfig>;

[[nodiscard]] ProbeConfigResult ParseProbeConfig(const toml::table& root);

[[nodiscard]] ProbeConfigResult LoadProbeConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_CONFIG_H_
