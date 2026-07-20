#include "tools/gate/order_session_failure_probe.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>
#include <toml++/toml.hpp>

#include "core/common/types.h"
#include "core/trading/order_latency.h"
#include "core/trading/order_types.h"
#include "exchange/gate/trading/decimal_size_header.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/order_session_config.h"
#include "nova/utils/log.h"

namespace {

namespace gate = aquila::gate;
namespace probe = aquila::tools::gate_order_session_failure_probe;

constexpr double kMaxSubmitSize = 5.0;

struct CliOptions {
  std::filesystem::path config_path{
      "config/order_sessions/gate_order_session.toml"};
  std::string api_key_env;
  std::string api_secret_env;
  std::string probe_mode{"cancel-rejected"};
  std::string contract{"BTC_USDT"};
  std::string side{"buy"};
  std::string price{"0.01"};
  std::string tif{"ioc"};
  double size{0.0};
  std::uint64_t local_order_id{1};
  std::uint64_t cancel_exchange_order_id{9000000000000000000ULL};
  double wait_seconds{20.0};
  bool reduce_only{false};
  bool keep_open{false};
  bool execute{false};
};

std::string ToLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string ToUpper(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
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

aquila::OrderSide ParseSide(std::string_view side) noexcept {
  return side == "buy" ? aquila::OrderSide::kBuy : aquila::OrderSide::kSell;
}

aquila::TimeInForce ParseTimeInForce(std::string_view tif) noexcept {
  return tif == "gtc" ? aquila::TimeInForce::kGoodTillCancel
                      : aquila::TimeInForce::kImmediateOrCancel;
}

bool ValidateOptions(const CliOptions& options, probe::ProbeMode mode) {
  if (options.wait_seconds <= 0.0) {
    fmt::print(stderr, "[FAIL] wait-seconds must be positive\n");
    NOVA_ERROR("wait-seconds must be positive");
    return false;
  }
  if (options.local_order_id == 0) {
    fmt::print(stderr, "[FAIL] local-order-id must be positive\n");
    NOVA_ERROR("local-order-id must be positive");
    return false;
  }
  if (mode == probe::ProbeMode::kSubmitRejected) {
    if (options.size < 0 || options.size > kMaxSubmitSize) {
      fmt::print(stderr, "[FAIL] size must be in [0, {}]\n", kMaxSubmitSize);
      NOVA_ERROR("size must be in [0, {}]", kMaxSubmitSize);
      return false;
    }
    if (options.price.empty()) {
      fmt::print(stderr, "[FAIL] price must not be empty\n");
      NOVA_ERROR("price must not be empty");
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint8_t DecimalPlaces(std::string_view text) noexcept {
  const std::size_t dot = text.find('.');
  return dot == std::string_view::npos
             ? 0
             : static_cast<std::uint8_t>(text.size() - dot - 1);
}

aquila::core::OrderPlaceRequest BuildProbeOrder(const CliOptions& options) {
  const std::string quantity_text = fmt::format("{}", options.size);
  aquila::core::OrderPlaceRequest request{
      .local_order_id = options.local_order_id,
      .price = std::stod(options.price),
      .quantity = options.size,
      .exchange = aquila::Exchange::kGate,
      .side = ParseSide(options.side),
      .order_type = aquila::OrderType::kLimit,
      .time_in_force = ParseTimeInForce(options.tif),
      .price_decimal_places = DecimalPlaces(options.price),
      .quantity_decimal_places = DecimalPlaces(quantity_text),
      .reduce_only = options.reduce_only,
  };
  aquila::core::SetOrderSymbol(&request, options.contract);
  return request;
}

struct ToolResponseHandler {
  void* context{nullptr};
  void (*on_login_ready)(void*) noexcept {nullptr};
  void (*on_order_response)(void*,
                            const gate::OrderResponse&) noexcept {nullptr};

  void OnOrderSessionLoginReady() noexcept {
    if (on_login_ready != nullptr) {
      on_login_ready(context);
    }
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    if (on_order_response != nullptr) {
      on_order_response(context, response);
    }
  }
};

template <typename SessionT>
struct RunContext {
  SessionT* session{nullptr};
  probe::ProbeMode mode{probe::ProbeMode::kCancelRejected};
  aquila::core::OrderPlaceRequest order{};
  std::uint64_t cancel_exchange_order_id{0};
  bool keep_open{false};
  bool submitted{false};
  bool cancel_submitted{false};
  bool safety_cancel_submitted{false};
  int exit_code{1};
  std::vector<gate::OrderResponse> responses;
  std::atomic<bool> done{false};
  std::atomic<bool> session_returned{false};
  bool start_result{false};
  std::mutex mutex;

  static void LoginReadyCallback(void* raw) noexcept {
    static_cast<RunContext*>(raw)->OnLoginReady();
  }

  static void OrderResponseCallback(
      void* raw, const gate::OrderResponse& response) noexcept {
    static_cast<RunContext*>(raw)->OnOrderResponse(response);
  }

  void OnLoginReady() noexcept {
    if (mode == probe::ProbeMode::kCancelRejected) {
      SubmitCancel(order.local_order_id, cancel_exchange_order_id,
                   /*safety_cancel=*/false);
      return;
    }
    SubmitPlace();
  }

  void SubmitPlace() noexcept {
    bool finish = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (submitted) {
        return;
      }
      const gate::OrderSendResult sent = session->PlaceOrder(order);
      fmt::print(
          "place status={} local_order_id={} contract={} size={} price={} "
          "tif={} reduce_only={} request_sequence={}\n",
          magic_enum::enum_name(sent.status), order.local_order_id,
          order.SymbolView(), order.quantity, order.price,
          magic_enum::enum_name(order.time_in_force),
          order.reduce_only ? "true" : "false", sent.request_sequence);
      NOVA_INFO(
          "place status={} local_order_id={} contract={} size={} price={} "
          "tif={} reduce_only={} request_sequence={}",
          magic_enum::enum_name(sent.status), order.local_order_id,
          order.SymbolView(), order.quantity, order.price,
          magic_enum::enum_name(order.time_in_force),
          order.reduce_only ? "true" : "false", sent.request_sequence);
      if (sent.status != gate::OrderSendStatus::kOk) {
        exit_code = 1;
        finish = true;
      } else {
        submitted = true;
      }
    }
    if (finish) {
      Finish();
    }
  }

  void SubmitCancel(std::uint64_t local_order_id,
                    std::uint64_t exchange_order_id,
                    bool safety_cancel) noexcept {
    bool finish = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (cancel_submitted || safety_cancel_submitted) {
        return;
      }
      session->CacheExchangeOrderId(local_order_id, exchange_order_id);
      const gate::OrderSendResult sent = session->CancelOrder(
          aquila::core::OrderCancelRequest{.local_order_id = local_order_id});
      fmt::print(
          "cancel status={} local_order_id={} exchange_order_id={} "
          "safety_cancel={} request_sequence={}\n",
          magic_enum::enum_name(sent.status), local_order_id, exchange_order_id,
          safety_cancel ? "true" : "false", sent.request_sequence);
      NOVA_INFO(
          "cancel status={} local_order_id={} exchange_order_id={} "
          "safety_cancel={} request_sequence={}",
          magic_enum::enum_name(sent.status), local_order_id, exchange_order_id,
          safety_cancel ? "true" : "false", sent.request_sequence);
      if (sent.status != gate::OrderSendStatus::kOk) {
        exit_code = 1;
        finish = true;
      } else if (safety_cancel) {
        safety_cancel_submitted = true;
      } else {
        cancel_submitted = true;
      }
    }
    if (finish) {
      Finish();
    }
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    bool submit_safety_cancel = false;
    bool finish = false;
    int next_exit_code = 1;
    {
      std::lock_guard<std::mutex> lock(mutex);
      responses.push_back(response);
      const probe::ProbeResponseDecision decision =
          probe::ResolveProbeResponseDecision(probe::ProbeResponseInput{
              .mode = mode,
              .kind = response.kind,
              .keep_open = keep_open,
              .safety_cancel_submitted = safety_cancel_submitted,
          });
      submit_safety_cancel = decision.submit_safety_cancel;
      finish = decision.finish;
      next_exit_code = decision.exit_code;
      if (finish) {
        exit_code = next_exit_code;
      }
    }

    const std::int64_t exchange_to_local_ns = aquila::core::LatencyDeltaNs(
        response.local_receive_ns, response.exchange_ns);
    fmt::print(
        "response kind={} local_order_id={} exchange_order_id={} "
        "request_sequence={} http_status={} error_label_hash={} "
        "local_receive_ns={} exchange_ns={} exchange_to_local_ns={}\n",
        magic_enum::enum_name(response.kind), response.local_order_id,
        response.exchange_order_id, response.request_sequence,
        response.http_status, response.error_label_hash,
        response.local_receive_ns, response.exchange_ns, exchange_to_local_ns);
    NOVA_INFO(
        "response kind={} local_order_id={} exchange_order_id={} "
        "request_sequence={} http_status={} error_label_hash={} "
        "local_receive_ns={} exchange_ns={} exchange_to_local_ns={}",
        magic_enum::enum_name(response.kind), response.local_order_id,
        response.exchange_order_id, response.request_sequence,
        response.http_status, response.error_label_hash,
        response.local_receive_ns, response.exchange_ns, exchange_to_local_ns);

    if (submit_safety_cancel) {
      SubmitCancel(response.local_order_id, response.exchange_order_id,
                   /*safety_cancel=*/true);
      return;
    }
    if (finish) {
      Finish();
    }
  }

  void Finish() noexcept {
    done.store(true, std::memory_order_release);
    if (session != nullptr) {
      session->Stop();
    }
  }
};

template <typename WebSocketPolicy>
int RunLive(gate::OrderSessionConfig config, gate::LoginCredentials credentials,
            const CliOptions& options, probe::ProbeMode mode) {
  ToolResponseHandler handler;
  using Session = gate::OrderSession<ToolResponseHandler, WebSocketPolicy,
                                     gate::OrderSessionDiagnostics>;

  const std::string contract = ToUpper(options.contract);
  CliOptions normalized = options;
  normalized.contract = contract;

  Session session(std::move(config.connection), std::move(credentials), handler,
                  config.request_map_capacity);
  RunContext<Session> context;
  context.session = &session;
  context.mode = mode;
  context.order = BuildProbeOrder(normalized);
  context.cancel_exchange_order_id = normalized.cancel_exchange_order_id;
  context.keep_open = options.keep_open;
  context.responses.reserve(4);
  handler.context = &context;
  handler.on_login_ready = &RunContext<Session>::LoginReadyCallback;
  handler.on_order_response = &RunContext<Session>::OrderResponseCallback;

  std::thread session_thread([&context, &session]() {
    context.start_result = session.Start();
    context.session_returned.store(true, std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(options.wait_seconds);
  while (!context.done.load(std::memory_order_acquire) &&
         !context.session_returned.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  bool timed_out = false;
  if (!context.done.load(std::memory_order_acquire) &&
      !context.session_returned.load(std::memory_order_acquire)) {
    timed_out = true;
    session.Stop();
  }
  if (session_thread.joinable()) {
    session_thread.join();
  }

  const gate::OrderSessionStats& stats = session.stats();
  fmt::print(
      "summary probe={} start_result={} timed_out={} submitted={} "
      "cancel_submitted={} safety_cancel_submitted={} responses={} "
      "login_sent={} login_accepted={} login_rejected={} place_sent={} "
      "cancel_sent={} local_send_failures={}\n",
      probe::ProbeModeName(mode), context.start_result ? "true" : "false",
      timed_out ? "true" : "false", context.submitted ? "true" : "false",
      context.cancel_submitted ? "true" : "false",
      context.safety_cancel_submitted ? "true" : "false",
      context.responses.size(), stats.login_sent, stats.login_accepted,
      stats.login_rejected, stats.place_sent, stats.cancel_sent,
      stats.local_send_failures);
  NOVA_INFO(
      "summary probe={} start_result={} timed_out={} submitted={} "
      "cancel_submitted={} safety_cancel_submitted={} responses={} "
      "login_sent={} login_accepted={} login_rejected={} place_sent={} "
      "cancel_sent={} local_send_failures={}",
      probe::ProbeModeName(mode), context.start_result ? "true" : "false",
      timed_out ? "true" : "false", context.submitted ? "true" : "false",
      context.cancel_submitted ? "true" : "false",
      context.safety_cancel_submitted ? "true" : "false",
      context.responses.size(), stats.login_sent, stats.login_accepted,
      stats.login_rejected, stats.place_sent, stats.cancel_sent,
      stats.local_send_failures);
  if (timed_out) {
    return 1;
  }
  return context.exit_code;
}

int Run(const CliOptions& options, const toml::table& toml) {
  probe::ProbeMode mode{};
  if (!probe::ParseProbeMode(ToLower(options.probe_mode), &mode)) {
    fmt::print(stderr,
               "[FAIL] probe must be submit-rejected or cancel-rejected\n");
    NOVA_ERROR("invalid probe={}", options.probe_mode);
    return 2;
  }
  if (!ValidateOptions(options, mode)) {
    return 2;
  }

  fmt::print(
      "order_session_failure_probe execute={} probe={} contract={} side={} "
      "size={} price={} tif={} reduce_only={} keep_open={} "
      "local_order_id={} cancel_exchange_order_id={}\n",
      options.execute ? "true" : "false", probe::ProbeModeName(mode),
      ToUpper(options.contract), options.side, options.size, options.price,
      options.tif, options.reduce_only ? "true" : "false",
      options.keep_open ? "true" : "false", options.local_order_id,
      options.cancel_exchange_order_id);
  NOVA_INFO(
      "order_session_failure_probe execute={} probe={} contract={} side={} "
      "size={} price={} tif={} reduce_only={} keep_open={} "
      "local_order_id={} cancel_exchange_order_id={}",
      options.execute ? "true" : "false", probe::ProbeModeName(mode),
      ToUpper(options.contract), options.side, options.size, options.price,
      options.tif, options.reduce_only ? "true" : "false",
      options.keep_open ? "true" : "false", options.local_order_id,
      options.cancel_exchange_order_id);
  if (!options.execute) {
    fmt::print("dry_run=true no websocket connection opened\n");
    NOVA_INFO("dry_run=true no websocket connection opened");
    return 0;
  }

  gate::OrderSessionConfigResult config_result =
      gate::ParseOrderSessionConfig(toml, options.config_path);
  if (!config_result.ok) {
    fmt::print(stderr, "[FAIL] config_error={}\n", config_result.error);
    NOVA_ERROR("config_error={}", config_result.error);
    return 1;
  }

  gate::OrderSessionConfig config = std::move(config_result.value);
  const std::string api_key_env = options.api_key_env.empty()
                                      ? config.credentials.api_key_env
                                      : options.api_key_env;
  const std::string api_secret_env = options.api_secret_env.empty()
                                         ? config.credentials.api_secret_env
                                         : options.api_secret_env;
  const char* api_key = EnvValue(api_key_env);
  if (api_key == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n", api_key_env);
    NOVA_ERROR("missing env var {}", api_key_env);
    return 2;
  }
  const char* api_secret = EnvValue(api_secret_env);
  if (api_secret == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n", api_secret_env);
    NOVA_ERROR("missing env var {}", api_secret_env);
    return 2;
  }

  gate::LoginCredentials credentials{.api_key = api_key,
                                     .api_secret = api_secret};
  fmt::print(
      "config name={} host={} target={} tls={} request_map_capacity={} "
      "size_decimal_header={}\n",
      config.name, config.connection.host, config.connection.target,
      config.connection.enable_tls ? "true" : "false",
      config.request_map_capacity,
      gate::HasGateSizeDecimalHeader(config.connection) ? "true" : "false");
  NOVA_INFO(
      "config name={} host={} target={} tls={} request_map_capacity={} "
      "size_decimal_header={}",
      config.name, config.connection.host, config.connection.target,
      config.connection.enable_tls ? "true" : "false",
      config.request_map_capacity,
      gate::HasGateSizeDecimalHeader(config.connection) ? "true" : "false");
  if (config.connection.enable_tls) {
    return RunLive<gate::OrderSessionDefaultTlsWebSocketPolicy>(
        std::move(config), std::move(credentials), options, mode);
  }
  return RunLive<gate::OrderSessionDefaultPlainWebSocketPolicy>(
      std::move(config), std::move(credentials), options, mode);
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{
      "Probe Gate futures OrderSession failure responses through WebSocket"};
  app.add_option("--config", options.config_path,
                 "Gate order session TOML path");
  app.add_option("--api-key", options.api_key_env,
                 "Override API key environment variable name from config");
  app.add_option("--api-secret", options.api_secret_env,
                 "Override API secret environment variable name from config");
  app.add_option("--probe", options.probe_mode, "Failure probe mode")
      ->check(CLI::IsMember({"submit-rejected", "cancel-rejected"}));
  app.add_option("--contract", options.contract,
                 "Gate futures contract for submit probe");
  app.add_option("--side", options.side, "Submit probe side")
      ->check(CLI::IsMember({"buy", "sell"}));
  app.add_option("--size", options.size,
                 "Submit probe non-negative contract size. Hard limit: <= 5");
  app.add_option("--price", options.price, "Submit probe limit price");
  app.add_option("--tif", options.tif, "Submit probe time in force")
      ->check(CLI::IsMember({"gtc", "ioc"}));
  app.add_option("--local-order-id", options.local_order_id,
                 "Local order id for the probe request");
  app.add_option("--cancel-exchange-order-id", options.cancel_exchange_order_id,
                 "Exchange order id used by cancel-rejected probe");
  app.add_option("--wait-seconds", options.wait_seconds,
                 "Maximum seconds to wait for final response");
  app.add_flag("--reduce-only", options.reduce_only,
               "Set reduce_only=true for submit probe");
  app.add_flag("--keep-open", options.keep_open,
               "Do not submit safety cancel after unexpected accepted submit");
  app.add_flag("--execute", options.execute,
               "Actually submit through WebSocket. Omitted means dry-run");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result toml =
        toml::parse_file(options.config_path.string());
    nova::LoggingGuard logging_guard{toml};
    return Run(options, toml);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "[FAIL] config_error={}\n", exc.what());
    return 1;
  }
}
