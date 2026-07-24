#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/websocket/types.h"
#include "exchange/bitget/trading/order_session.h"
#include "exchange/bitget/trading/order_session_config.h"
#include "nova/utils/log.h"
#include "tools/bitget/private_session_probe_outcome.h"

namespace {

namespace aq_bitget = aquila::bitget;
namespace ws = aquila::websocket;

struct LoginOnlyResponseHandler {
  bool ready{false};
  bool ever_ready{false};
  std::uint64_t ready_events{0};
  std::uint64_t not_ready_events{0};
  std::uint64_t unexpected_order_responses{0};

  void OnOrderSessionLoginReady() noexcept {
    ready = true;
    ever_ready = true;
    ++ready_events;
    NOVA_INFO("bitget_order_session_login ready=true ready_events={}",
              ready_events);
  }

  void OnOrderSessionLoginNotReady() noexcept {
    ready = false;
    ++not_ready_events;
    NOVA_WARNING("bitget_order_session_login ready=false not_ready_events={}",
                 not_ready_events);
  }

  void OnOrderResponse(const aq_bitget::OrderResponse&) noexcept {
    ++unexpected_order_responses;
    NOVA_ERROR("bitget_order_session_probe unexpected_order_response count={}",
               unexpected_order_responses);
  }
};

void PrintConfig(const aq_bitget::OrderSessionConfig& config, bool connect,
                 std::uint32_t duration_seconds) {
  NOVA_INFO(
      "bitget_order_session_probe_config name={} host={} port={} target={} "
      "tls={} category={} position_mode={} margin_mode={} "
      "client_oid_run_namespace={} "
      "request_map_capacity={} order_id_cache_capacity={} connect={} "
      "duration_s={}",
      config.name, config.connection.host, config.connection.port,
      config.connection.target, config.connection.enable_tls ? "true" : "false",
      config.category, config.position_mode, config.margin_mode,
      config.client_oid_run_namespace.View(), config.request_map_capacity,
      config.order_id_cache_capacity, connect ? "true" : "false",
      duration_seconds);
}

bool ReadCredential(std::string_view env_name, std::string* output) {
  const char* value = std::getenv(std::string{env_name}.c_str());
  if (value == nullptr || value[0] == '\0') {
    NOVA_ERROR("missing credential environment variable name={}", env_name);
    return false;
  }
  output->assign(value);
  return true;
}

bool LoadCredentials(const aq_bitget::OrderSessionConfig& config,
                     aq_bitget::LoginCredentials* credentials) {
  return ReadCredential(config.credentials.api_key_env,
                        &credentials->api_key) &&
         ReadCredential(config.credentials.api_secret_env,
                        &credentials->api_secret) &&
         ReadCredential(config.credentials.api_passphrase_env,
                        &credentials->passphrase);
}

template <typename WebSocketPolicy>
int RunLoginOnly(aq_bitget::OrderSessionConfig config,
                 aq_bitget::LoginCredentials credentials,
                 std::uint32_t duration_seconds) {
  LoginOnlyResponseHandler handler;
  using Session =
      aq_bitget::OrderSession<LoginOnlyResponseHandler, WebSocketPolicy,
                              aq_bitget::OrderSessionDiagnostics>;
  Session session(std::move(config.connection), std::move(credentials),
                  config.client_oid_run_namespace, handler,
                  config.request_map_capacity, config.order_id_cache_capacity);

  std::mutex stop_mutex;
  std::condition_variable stop_condition;
  bool run_finished = false;
  bool completed_requested_duration = false;
  std::thread stopper([&] {
    std::unique_lock lock(stop_mutex);
    if (!stop_condition.wait_for(lock, std::chrono::seconds(duration_seconds),
                                 [&run_finished] { return run_finished; })) {
      completed_requested_duration = true;
      session.Stop();
    }
  });

  const bool started_ok = session.Start();
  {
    std::lock_guard lock(stop_mutex);
    run_finished = true;
  }
  stop_condition.notify_one();
  stopper.join();

  const ws::Metrics metrics = session.SnapshotMetrics();
  const aq_bitget::OrderSessionStats& stats = session.stats();
  const aq_bitget::PrivateSessionProbeOutcome outcome{
      .started_ok = started_ok,
      .completed_requested_duration = completed_requested_duration,
      .reached_ready = handler.ever_ready,
      .response_stream_clean = handler.unexpected_order_responses == 0,
  };
  const bool ok = aq_bitget::PrivateSessionProbeSucceeded(outcome);
  NOVA_INFO(
      "bitget_order_session_summary result={} "
      "completed_requested_duration={} ever_ready={} ready_events={} "
      "not_ready_events={} phase={} error={} login_sent={} "
      "login_accepted={} login_rejected={} pings_sent={} pongs_received={} "
      "heartbeat_timeouts={} unexpected_order_responses={} rx_messages={} "
      "tx_messages={} reconnects={}",
      ok ? "ok" : "failed", completed_requested_duration ? "true" : "false",
      handler.ever_ready ? "true" : "false", handler.ready_events,
      handler.not_ready_events, magic_enum::enum_name(session.phase()),
      magic_enum::enum_name(session.last_error()), stats.login_sent,
      stats.login_accepted, stats.login_rejected, stats.pings_sent,
      stats.pongs_received, stats.heartbeat_timeouts,
      handler.unexpected_order_responses, metrics.rx_messages,
      metrics.tx_messages, metrics.reconnects);
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/order_sessions/bitget_order_session.toml"};
  bool connect{false};
  std::uint32_t duration_seconds{40};

  CLI::App app{"Bitget login-only order session probe"};
  app.add_option("--config", config_path, "order session TOML path");
  app.add_flag("--connect", connect,
               "connect and authenticate without sending orders");
  app.add_option("--duration-s", duration_seconds, "connection duration")
      ->check(CLI::PositiveNumber);
  CLI11_PARSE(app, argc, argv);

  const toml::parse_result toml = toml::parse_file(config_path.string());
  nova::LoggingGuard logging_guard{toml};

  aq_bitget::OrderSessionConfigResult config_result =
      aq_bitget::ParseOrderSessionConfig(toml, config_path);
  if (!config_result.ok) {
    NOVA_ERROR("config_error={}", config_result.error);
    return 1;
  }
  PrintConfig(config_result.value, connect, duration_seconds);
  if (!connect) {
    return 0;
  }

  aq_bitget::LoginCredentials credentials;
  if (!LoadCredentials(config_result.value, &credentials)) {
    return 1;
  }
  if (config_result.value.connection.enable_tls) {
    return RunLoginOnly<aq_bitget::OrderSessionDefaultTlsWebSocketPolicy>(
        std::move(config_result.value), std::move(credentials),
        duration_seconds);
  }
  return RunLoginOnly<aq_bitget::OrderSessionDefaultPlainWebSocketPolicy>(
      std::move(config_result.value), std::move(credentials), duration_seconds);
}
