#include "exchange/gate/trading/order_feedback_session.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/trading/order_feedback_shm.h"
#include "exchange/gate/trading/order_feedback_session_config.h"
#include "nova/utils/log.h"

namespace {

namespace gate = aquila::gate;

struct CliOptions {
  std::filesystem::path config_path{
      "config/order_feedback/gate_order_feedback_session.toml"};
  double duration_sec{30.0};
  bool connect{false};
};

struct LoggingGuard {
  explicit LoggingGuard(const toml::table& toml) {
    nova::LogConfig log_config;
    log_config.FromToml(toml["log"]);
    nova::InitializeLogging(log_config);
  }

  ~LoggingGuard() {
    nova::StopLogging();
  }
};

void PrintBool(std::string_view name, bool value) {
  fmt::print("{}={}", name, value ? "true" : "false");
}

const char* EnvValue(const std::string& name) {
  if (name.empty()) {
    return nullptr;
  }
  const char* value = std::getenv(name.c_str());
  if (value == nullptr || value[0] == '\0') {
    return nullptr;
  }
  return value;
}

aquila::OrderFeedbackShmConfig ToShmConfig(
    const aquila::config::OrderFeedbackShmRuntimeConfig& config) {
  return aquila::OrderFeedbackShmConfig{
      .shm_name = config.shm_name,
      .channel_name = config.channel_name,
      .create = config.create,
      .remove_existing = config.remove_existing,
  };
}

void PrintConfig(const gate::OrderFeedbackSessionConfig& config,
                 const CliOptions& options) {
  fmt::print("config name={} duration_sec={:.3f} connect={}\n", config.name,
             options.duration_sec, options.connect ? "true" : "false");
  fmt::print("websocket host={} service={} target={} tls={} bind_cpu_id={}\n",
             config.connection.host, config.connection.service,
             config.connection.target,
             config.connection.enable_tls ? "true" : "false",
             config.connection.runtime_policy.io_cpu_id);
  fmt::print("credentials api_key_env={} api_secret_env={}\n",
             config.credentials.api_key_env, config.credentials.api_secret_env);
  fmt::print(
      "shm shm_name={} channel_name={} max_strategy_count={} "
      "queue_capacity={} ",
      config.shm.shm_name, config.shm.channel_name,
      config.shm.max_strategy_count, config.shm.queue_capacity);
  PrintBool("create", config.shm.create);
  fmt::print(" ");
  PrintBool("remove_existing", config.shm.remove_existing);
  fmt::print("\n");
  NOVA_INFO(
      "config name={} duration_sec={:.3f} connect={} host={} service={} "
      "target={} tls={} bind_cpu_id={} shm_name={} channel_name={} "
      "queue_capacity={} create={} remove_existing={}",
      config.name, options.duration_sec, options.connect ? "true" : "false",
      config.connection.host, config.connection.service,
      config.connection.target, config.connection.enable_tls ? "true" : "false",
      config.connection.runtime_policy.io_cpu_id, config.shm.shm_name,
      config.shm.channel_name, config.shm.queue_capacity,
      config.shm.create ? "true" : "false",
      config.shm.remove_existing ? "true" : "false");
}

template <typename Publisher>
void PrintStats(const gate::OrderFeedbackSessionStats& session_stats,
                const gate::OrderFeedbackParserStats& parser_stats,
                const Publisher& publisher) {
  fmt::print(
      "stats text_messages={} binary_messages={} login_sent={} "
      "login_accepted={} login_rejected={} subscribe_sent={} "
      "subscribe_acks={} control_errors={} parse_errors={} "
      "events_published={} publish_failures={} global_gaps_published={} "
      "parser_messages_seen={} parser_messages_parsed={} "
      "parser_orders_seen={} parser_events_emitted={} parser_dropped_events={} "
      "parser_need_more={} parser_unsupported_template={} "
      "parser_unexpected_block_length={} parser_unexpected_event={} "
      "parser_unexpected_channel={} parser_malformed_payload={} "
      "shm_published={} shm_invalid_routes={}\n",
      session_stats.text_messages, session_stats.binary_messages,
      session_stats.login_sent, session_stats.login_accepted,
      session_stats.login_rejected, session_stats.subscribe_sent,
      session_stats.subscribe_acks, session_stats.control_errors,
      session_stats.parse_errors, session_stats.events_published,
      session_stats.publish_failures, session_stats.global_gaps_published,
      parser_stats.messages_seen, parser_stats.messages_parsed,
      parser_stats.orders_seen, parser_stats.events_emitted,
      parser_stats.dropped_events, parser_stats.need_more_count,
      parser_stats.unsupported_template_count,
      parser_stats.unexpected_block_length_count,
      parser_stats.unexpected_event_count,
      parser_stats.unexpected_channel_count,
      parser_stats.malformed_payload_count, publisher.published_count(),
      publisher.invalid_route_count());
  NOVA_INFO(
      "summary text_messages={} binary_messages={} login_sent={} "
      "login_accepted={} login_rejected={} subscribe_sent={} "
      "subscribe_acks={} control_errors={} parse_errors={} "
      "events_published={} publish_failures={} global_gaps_published={} "
      "parser_messages_seen={} parser_messages_parsed={} "
      "parser_orders_seen={} parser_events_emitted={} parser_dropped_events={} "
      "parser_need_more={} parser_unsupported_template={} "
      "parser_unexpected_block_length={} parser_unexpected_event={} "
      "parser_unexpected_channel={} parser_malformed_payload={} "
      "shm_published={} shm_invalid_routes={}",
      session_stats.text_messages, session_stats.binary_messages,
      session_stats.login_sent, session_stats.login_accepted,
      session_stats.login_rejected, session_stats.subscribe_sent,
      session_stats.subscribe_acks, session_stats.control_errors,
      session_stats.parse_errors, session_stats.events_published,
      session_stats.publish_failures, session_stats.global_gaps_published,
      parser_stats.messages_seen, parser_stats.messages_parsed,
      parser_stats.orders_seen, parser_stats.events_emitted,
      parser_stats.dropped_events, parser_stats.need_more_count,
      parser_stats.unsupported_template_count,
      parser_stats.unexpected_block_length_count,
      parser_stats.unexpected_event_count,
      parser_stats.unexpected_channel_count,
      parser_stats.malformed_payload_count, publisher.published_count(),
      publisher.invalid_route_count());
}

struct LoggingOrderFeedbackPublisher {
  explicit LoggingOrderFeedbackPublisher(
      aquila::OrderFeedbackShmChannel& channel)
      : publisher(channel) {}

  bool Publish(const aquila::OrderFeedbackEvent& event) noexcept {
    const bool ok = publisher.Publish(event);
    const std::int64_t latency_ns =
        event.exchange_update_ns == 0
            ? 0
            : event.local_receive_ns - event.exchange_update_ns;
    NOVA_INFO(
        "feedback_event publish_ok={} kind={} local_order_id={} "
        "exchange_order_id={} exchange_update_ns={} local_receive_ns={} "
        "latency_ns={} cumulative_filled_quantity={} left_quantity={} "
        "cancelled_quantity={} fill_price={:.12g} role={} finish_reason={} "
        "reject_reason={} gap_scope={} gap_reason={} gap_sequence={}",
        ok ? "true" : "false", magic_enum::enum_name(event.kind),
        event.local_order_id, event.exchange_order_id, event.exchange_update_ns,
        event.local_receive_ns, latency_ns, event.cumulative_filled_quantity,
        event.left_quantity, event.cancelled_quantity, event.fill_price,
        magic_enum::enum_name(event.role),
        magic_enum::enum_name(event.finish_reason),
        magic_enum::enum_name(event.reject_reason),
        magic_enum::enum_name(event.gap_scope),
        magic_enum::enum_name(event.gap_reason), event.gap_sequence);
    if (!ok) {
      NOVA_WARNING("feedback_event_publish_failed local_order_id={} kind={}",
                   event.local_order_id, magic_enum::enum_name(event.kind));
    }
    return ok;
  }

  bool PublishGlobalGap(aquila::OrderFeedbackGapReason reason,
                        std::int64_t local_receive_ns) noexcept {
    const bool ok = publisher.PublishGlobalGap(reason, local_receive_ns);
    NOVA_WARNING(
        "feedback_global_gap publish_ok={} reason={} local_receive_ns={}",
        ok ? "true" : "false", magic_enum::enum_name(reason), local_receive_ns);
    return ok;
  }

  [[nodiscard]] std::uint64_t published_count() const noexcept {
    return publisher.published_count();
  }

  [[nodiscard]] std::uint64_t invalid_route_count() const noexcept {
    return publisher.invalid_route_count();
  }

  aquila::OrderFeedbackShmPublisher publisher;
};

template <typename WebSocketPolicy>
int RunLive(gate::OrderFeedbackSessionConfig config,
            gate::LoginCredentials credentials, const CliOptions& options) {
  auto manager_result =
      aquila::OrderFeedbackShmManager::OpenOrCreate(ToShmConfig(config.shm));
  if (!manager_result.ok) {
    fmt::print(stderr, "[FAIL] shm_error={}\n", manager_result.error);
    NOVA_ERROR("shm_error={}", manager_result.error);
    return 1;
  }

  aquila::OrderFeedbackShmManager manager = std::move(manager_result.value);
  LoggingOrderFeedbackPublisher publisher(manager.channel());
  using Session =
      gate::OrderFeedbackSession<LoggingOrderFeedbackPublisher, WebSocketPolicy,
                                 gate::OrderFeedbackSessionDiagnostics>;
  Session session(std::move(config.connection), std::move(credentials),
                  publisher);

  std::atomic<bool> session_returned{false};
  bool start_result = false;
  std::thread session_thread([&]() {
    start_result = session.Start();
    session_returned.store(true, std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(options.duration_sec);
  while (!session_returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  const bool timed_out = !session_returned.load(std::memory_order_acquire);
  if (timed_out) {
    session.Stop();
  }
  if (session_thread.joinable()) {
    session_thread.join();
  }

  fmt::print("summary start_result={} duration_reached={} ready={}\n",
             start_result ? "true" : "false", timed_out ? "true" : "false",
             session.ready() ? "true" : "false");
  NOVA_INFO("session_result start_result={} duration_reached={} ready={}",
            start_result ? "true" : "false", timed_out ? "true" : "false",
            session.ready() ? "true" : "false");
  PrintStats(session.stats(), session.parser_stats(), publisher);
  return start_result ? 0 : 1;
}

int Run(const CliOptions& options, const toml::table& toml) {
  if (options.duration_sec <= 0.0) {
    fmt::print(stderr, "[FAIL] duration-sec must be positive\n");
    NOVA_ERROR("duration-sec must be positive");
    return 2;
  }

  gate::OrderFeedbackSessionConfigResult config_result =
      gate::ParseOrderFeedbackSessionConfig(toml, options.config_path);
  if (!config_result.ok) {
    fmt::print(stderr, "[FAIL] config_error={}\n", config_result.error);
    NOVA_ERROR("config_error={}", config_result.error);
    return 1;
  }

  PrintConfig(config_result.value, options);

  if (!options.connect) {
    fmt::print("dry_run=true no websocket connection opened\n");
    NOVA_INFO("dry_run=true no websocket connection opened");
    return 0;
  }

  const char* api_key = EnvValue(config_result.value.credentials.api_key_env);
  if (api_key == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n",
               config_result.value.credentials.api_key_env);
    NOVA_ERROR("missing env var {}",
               config_result.value.credentials.api_key_env);
    return 2;
  }
  const char* api_secret =
      EnvValue(config_result.value.credentials.api_secret_env);
  if (api_secret == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n",
               config_result.value.credentials.api_secret_env);
    NOVA_ERROR("missing env var {}",
               config_result.value.credentials.api_secret_env);
    return 2;
  }

  gate::LoginCredentials credentials{.api_key = api_key,
                                     .api_secret = api_secret};
  std::fflush(stdout);
  if (config_result.value.connection.enable_tls) {
    return RunLive<gate::OrderFeedbackSessionDefaultTlsWebSocketPolicy>(
        std::move(config_result.value), std::move(credentials), options);
  }
  return RunLive<gate::OrderFeedbackSessionDefaultPlainWebSocketPolicy>(
      std::move(config_result.value), std::move(credentials), options);
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{"Gate order feedback session probe"};
  app.add_option("--config", options.config_path,
                 "Gate order feedback session TOML path");
  app.add_option("--duration-sec", options.duration_sec,
                 "Maximum seconds to run after connecting");
  app.add_flag("--connect", options.connect,
               "Connect and run once OrderFeedbackSession exists");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result toml =
        toml::parse_file(options.config_path.string());
    LoggingGuard logging_guard{toml};
    return Run(options, toml);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "[FAIL] config_error={}\n", exc.what());
    return 1;
  }
}
