#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/trading/order_feedback_shm.h"
#include "exchange/bitget/trading/order_feedback_session.h"
#include "exchange/bitget/trading/order_feedback_session_config.h"
#include "nova/utils/log.h"
#include "tools/bitget/private_session_probe_outcome.h"

namespace {

namespace aq = aquila;
namespace aq_bitget = aquila::bitget;
namespace ws = aquila::websocket;

aq::OrderFeedbackShmConfig ToShmConfig(
    const aq::config::OrderFeedbackShmRuntimeConfig& config) {
  return {
      .shm_name = config.shm_name,
      .channel_name = config.channel_name,
      .create = config.create,
      .remove_existing = config.remove_existing,
  };
}

void PrintConfig(const aq_bitget::OrderFeedbackSessionConfig& config,
                 bool connect, std::uint32_t duration_seconds) {
  NOVA_INFO(
      "bitget_order_feedback_session_config name={} host={} port={} "
      "target={} tls={} category={} position_mode={} margin_mode={} "
      "api_key_env={} api_secret_env={} api_passphrase_env={} shm_name={} "
      "channel_name={} max_strategy_count={} queue_capacity={} create={} "
      "remove_existing={} connect={} duration_s={}",
      config.name, config.connection.host, config.connection.port,
      config.connection.target, config.connection.enable_tls ? "true" : "false",
      config.category, config.position_mode, config.margin_mode,
      config.credentials.api_key_env, config.credentials.api_secret_env,
      config.credentials.api_passphrase_env, config.shm.shm_name,
      config.shm.channel_name, config.shm.max_strategy_count,
      config.shm.queue_capacity, config.shm.create ? "true" : "false",
      config.shm.remove_existing ? "true" : "false", connect ? "true" : "false",
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

bool LoadCredentials(const aq_bitget::OrderFeedbackSessionConfig& config,
                     aq_bitget::LoginCredentials* credentials) {
  return ReadCredential(config.credentials.api_key_env,
                        &credentials->api_key) &&
         ReadCredential(config.credentials.api_secret_env,
                        &credentials->api_secret) &&
         ReadCredential(config.credentials.api_passphrase_env,
                        &credentials->passphrase);
}

template <typename WebSocketPolicy>
int RunLoginSubscribeOnly(aq_bitget::OrderFeedbackSessionConfig config,
                          aq_bitget::LoginCredentials credentials,
                          std::uint32_t duration_seconds) {
  auto manager_result =
      aq::OrderFeedbackShmManager::OpenOrCreate(ToShmConfig(config.shm));
  if (!manager_result.ok) {
    NOVA_ERROR("bitget_order_feedback_session shm_error={}",
               manager_result.error);
    return 1;
  }

  aq::OrderFeedbackShmPublisher publisher(manager_result.value.channel());
  using Session = aq_bitget::OrderFeedbackSession<
      aq::OrderFeedbackShmPublisher, WebSocketPolicy,
      aq_bitget::OrderFeedbackSessionDiagnostics>;
  Session session(std::move(config.connection), std::move(credentials),
                  publisher);

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
  const aq_bitget::OrderFeedbackSessionStats& stats = session.stats();
  const aq_bitget::OrderFeedbackParserStats& parser_stats =
      session.parser_stats();
  const aq_bitget::PrivateSessionProbeOutcome outcome{
      .started_ok = started_ok,
      .completed_requested_duration = completed_requested_duration,
      .reached_ready = stats.login_accepted != 0 && stats.subscribe_acks != 0,
  };
  const bool ok = aq_bitget::PrivateSessionProbeSucceeded(outcome);
  NOVA_INFO(
      "bitget_order_feedback_session_summary result={} "
      "completed_requested_duration={} ever_active={} ever_ready={} phase={} "
      "error={} text_messages={} login_sent={} login_accepted={} "
      "login_rejected={} subscribe_sent={} subscribe_acks={} "
      "subscribe_errors={} pings_sent={} pongs_received={} "
      "heartbeat_timeouts={} order_envelopes={} orders_seen={} "
      "events_published={} foreign_orders_ignored={} "
      "unroutable_orders_ignored={} validation_errors={} "
      "decode_continuity_lost_events={} "
      "disconnect_continuity_lost_events={} publish_failures={} "
      "shm_published={} shm_invalid_routes={} rx_messages={} tx_messages={} "
      "reconnects={}",
      ok ? "ok" : "failed", completed_requested_duration ? "true" : "false",
      session.ever_active() ? "true" : "false",
      outcome.reached_ready ? "true" : "false",
      magic_enum::enum_name(session.phase()),
      magic_enum::enum_name(session.last_error()), stats.text_messages,
      stats.login_sent, stats.login_accepted, stats.login_rejected,
      stats.subscribe_sent, stats.subscribe_acks, stats.subscribe_errors,
      stats.pings_sent, stats.pongs_received, stats.heartbeat_timeouts,
      parser_stats.order_envelopes, parser_stats.orders_seen,
      stats.events_published, parser_stats.foreign_orders_ignored,
      parser_stats.unroutable_orders_ignored, parser_stats.validation_errors,
      stats.decode_continuity_lost_events,
      stats.disconnect_continuity_lost_events, stats.publish_failures,
      publisher.published_count(), publisher.invalid_route_count(),
      metrics.rx_messages, metrics.tx_messages, metrics.reconnects);
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/order_feedback/bitget_order_feedback_session.toml"};
  bool connect{false};
  std::uint32_t duration_seconds{40};

  CLI::App app{"Bitget login/subscribe-only order feedback session probe"};
  app.add_option("--config", config_path, "order feedback session TOML path");
  app.add_flag("--connect", connect,
               "connect, authenticate and subscribe without sending orders");
  app.add_option("--duration-s", duration_seconds, "connection duration")
      ->check(CLI::PositiveNumber);
  CLI11_PARSE(app, argc, argv);

  const toml::parse_result toml = toml::parse_file(config_path.string());
  nova::LoggingGuard logging_guard{toml};
  aq_bitget::OrderFeedbackSessionConfigResult config_result =
      aq_bitget::ParseOrderFeedbackSessionConfig(toml, config_path);
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
    return RunLoginSubscribeOnly<
        aq_bitget::OrderFeedbackSessionDefaultTlsWebSocketPolicy>(
        std::move(config_result.value), std::move(credentials),
        duration_seconds);
  }
  return RunLoginSubscribeOnly<
      aq_bitget::OrderFeedbackSessionDefaultPlainWebSocketPolicy>(
      std::move(config_result.value), std::move(credentials), duration_seconds);
}
