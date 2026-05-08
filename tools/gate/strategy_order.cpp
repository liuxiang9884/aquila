#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/common/types.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/order_session_config.h"
#include "strategy/order_types.h"
#include "strategy/strategy.h"

namespace {

namespace gate = aquila::gate;
namespace strategy = aquila::strategy;

constexpr std::int64_t kMaxOrderSize = 5;

struct CliOptions {
  std::filesystem::path config_path{
      "config/order_sessions/gate_order_session.toml"};
  std::string api_key_env;
  std::string api_secret_env;
  std::string contract{"BTC_USDT"};
  std::string side{"buy"};
  std::string order_type{"limit"};
  std::string price{"1"};
  std::string tif{"gtc"};
  std::int64_t size{1};
  std::int32_t symbol_id{0};
  std::size_t order_capacity{8};
  double wait_seconds{15.0};
  bool reduce_only{false};
  bool execute{false};
  bool keep_open{false};
};

std::string ToUpper(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return value;
}

std::string ToLower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
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

aquila::OrderSide ParseSide(std::string_view side) {
  if (side == "buy") {
    return aquila::OrderSide::kBuy;
  }
  return aquila::OrderSide::kSell;
}

aquila::TimeInForce ParseTimeInForce(std::string_view tif) {
  if (tif == "ioc") {
    return aquila::TimeInForce::kImmediateOrCancel;
  }
  return aquila::TimeInForce::kGoodTillCancel;
}

strategy::OrderResponseKind ToStrategyKind(gate::OrderResponseKind kind) {
  switch (kind) {
    case gate::OrderResponseKind::kAck:
      return strategy::OrderResponseKind::kAck;
    case gate::OrderResponseKind::kAccepted:
      return strategy::OrderResponseKind::kAccepted;
    case gate::OrderResponseKind::kRejected:
      return strategy::OrderResponseKind::kRejected;
    case gate::OrderResponseKind::kCancelAccepted:
      return strategy::OrderResponseKind::kCancelAccepted;
    case gate::OrderResponseKind::kCancelRejected:
      return strategy::OrderResponseKind::kCancelRejected;
  }
  return strategy::OrderResponseKind::kRejected;
}

strategy::OrderResponseEvent ToStrategyEvent(
    const gate::OrderResponse& response) {
  return strategy::OrderResponseEvent{
      .kind = ToStrategyKind(response.kind),
      .local_order_id = response.local_order_id,
      .exchange_order_id = response.exchange_order_id,
      .error_label_hash = response.error_label_hash,
  };
}

struct PreparedOrder {
  std::string contract;
  std::string price_text;
  aquila::OrderSide side{aquila::OrderSide::kBuy};
  aquila::TimeInForce time_in_force{aquila::TimeInForce::kGoodTillCancel};
  std::int64_t size{1};
  std::int32_t symbol_id{0};
  bool reduce_only{false};
};

bool PrepareOrder(const CliOptions& options, PreparedOrder* output) {
  const std::string order_type = ToLower(options.order_type);
  const std::string tif = ToLower(options.tif);
  const std::string side = ToLower(options.side);
  if (options.size <= 0 || options.size > kMaxOrderSize) {
    fmt::print(stderr, "[FAIL] size must be in [1, {}]\n", kMaxOrderSize);
    return false;
  }
  if (options.order_capacity == 0) {
    fmt::print(stderr, "[FAIL] order-capacity must be positive\n");
    return false;
  }
  if (options.wait_seconds <= 0.0) {
    fmt::print(stderr, "[FAIL] wait-seconds must be positive\n");
    return false;
  }
  if (order_type != "limit" && order_type != "market") {
    fmt::print(stderr, "[FAIL] order-type must be limit or market\n");
    return false;
  }
  if (side != "buy" && side != "sell") {
    fmt::print(stderr, "[FAIL] side must be buy or sell\n");
    return false;
  }
  if (tif != "gtc" && tif != "ioc") {
    fmt::print(stderr, "[FAIL] tif must be gtc or ioc\n");
    return false;
  }

  output->contract = ToUpper(options.contract);
  output->side = ParseSide(side);
  output->size = options.size;
  output->symbol_id = options.symbol_id;
  output->reduce_only = options.reduce_only;
  if (order_type == "market") {
    output->price_text = "0";
    output->time_in_force = aquila::TimeInForce::kImmediateOrCancel;
    return true;
  }

  output->price_text = options.price;
  output->time_in_force = ParseTimeInForce(tif);
  if (output->price_text.empty()) {
    fmt::print(stderr, "[FAIL] price must not be empty\n");
    return false;
  }
  if (output->price_text == "0" &&
      output->time_in_force != aquila::TimeInForce::kImmediateOrCancel) {
    fmt::print(stderr, "[FAIL] price=0 requires tif=ioc\n");
    return false;
  }
  return true;
}

strategy::OrderCreateRequest BuildCreateRequest(const PreparedOrder& order) {
  return strategy::OrderCreateRequest{
      .exchange = aquila::Exchange::kGate,
      .symbol_id = order.symbol_id,
      .symbol = order.contract,
      .side = order.side,
      .time_in_force = order.time_in_force,
      .quantity = order.size,
      .price_text = order.price_text,
      .reduce_only = order.reduce_only,
  };
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
  strategy::Strategy<SessionT>* strategy_instance{nullptr};
  strategy::OrderCreateRequest request{};
  bool keep_open{false};
  bool submitted{false};
  bool cancel_submitted{false};
  std::int64_t local_order_id{0};
  int exit_code{1};
  std::vector<gate::OrderResponse> responses;
  std::atomic<bool> done{false};
  std::atomic<bool> session_returned{false};
  bool start_result{false};

  static void LoginReadyCallback(void* raw) noexcept {
    static_cast<RunContext*>(raw)->OnLoginReady();
  }

  static void OrderResponseCallback(
      void* raw, const gate::OrderResponse& response) noexcept {
    static_cast<RunContext*>(raw)->OnOrderResponse(response);
  }

  void OnLoginReady() noexcept {
    if (submitted) {
      return;
    }
    const strategy::OrderPlaceResult placed =
        strategy_instance->PlaceLimitOrder(request);
    fmt::print("place status={} local_order_id={}\n",
               magic_enum::enum_name(placed.status), placed.local_order_id);
    if (placed.status != strategy::OrderPlaceStatus::kOk) {
      exit_code = 1;
      Finish();
      return;
    }
    submitted = true;
    local_order_id = placed.local_order_id;
    fmt::print("place submitted local_order_id={} gate_text=t-{}\n",
               local_order_id, local_order_id);
  }

  void OnOrderResponse(const gate::OrderResponse& response) noexcept {
    responses.push_back(response);
    strategy_instance->OnOrderResponse(ToStrategyEvent(response));
    fmt::print(
        "response kind={} local_order_id={} exchange_order_id={} "
        "request_sequence={} http_status={} error_label_hash={}\n",
        magic_enum::enum_name(response.kind), response.local_order_id,
        response.exchange_order_id, response.request_sequence,
        response.http_status, response.error_label_hash);

    switch (response.kind) {
      case gate::OrderResponseKind::kAck:
        return;
      case gate::OrderResponseKind::kAccepted:
        if (!keep_open && !cancel_submitted) {
          SubmitCancel(response.local_order_id);
          return;
        }
        exit_code = 0;
        Finish();
        return;
      case gate::OrderResponseKind::kRejected:
        exit_code = 1;
        Finish();
        return;
      case gate::OrderResponseKind::kCancelAccepted:
        exit_code = 0;
        Finish();
        return;
      case gate::OrderResponseKind::kCancelRejected:
        exit_code = 1;
        Finish();
        return;
    }
  }

  void SubmitCancel(std::int64_t order_id) noexcept {
    const strategy::OrderCancelResult cancelled =
        strategy_instance->CancelOrder(order_id);
    fmt::print("cancel status={} local_order_id={}\n",
               magic_enum::enum_name(cancelled.status),
               cancelled.local_order_id);
    if (cancelled.status != strategy::OrderCancelStatus::kOk) {
      exit_code = 1;
      Finish();
      return;
    }
    cancel_submitted = true;
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
            const CliOptions& options, const PreparedOrder& prepared_order) {
  ToolResponseHandler handler;
  using Session = gate::OrderSession<ToolResponseHandler, WebSocketPolicy,
                                     gate::OrderSessionDiagnostics>;

  Session session(std::move(config.connection), std::move(credentials), handler,
                  config.request_map_capacity);
  strategy::Strategy<Session> strategy_instance(session,
                                                options.order_capacity);
  RunContext<Session> context;
  context.session = &session;
  context.strategy_instance = &strategy_instance;
  context.request = BuildCreateRequest(prepared_order);
  context.keep_open = options.keep_open;
  context.responses.reserve(8);
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
      "summary start_result={} timed_out={} submitted={} cancel_submitted={} "
      "responses={} login_sent={} login_accepted={} login_rejected={} "
      "place_sent={} cancel_sent={} local_send_failures={}\n",
      context.start_result ? "true" : "false", timed_out ? "true" : "false",
      context.submitted ? "true" : "false",
      context.cancel_submitted ? "true" : "false", context.responses.size(),
      stats.login_sent, stats.login_accepted, stats.login_rejected,
      stats.place_sent, stats.cancel_sent, stats.local_send_failures);
  if (timed_out) {
    return 1;
  }
  return context.exit_code;
}

int Run(const CliOptions& options) {
  PreparedOrder prepared_order;
  if (!PrepareOrder(options, &prepared_order)) {
    return 2;
  }

  fmt::print(
      "order route=strategy_websocket execute={} contract={} side={} "
      "order_type={} size={} price={} tif={} reduce_only={} keep_open={}\n",
      options.execute ? "true" : "false", prepared_order.contract, options.side,
      options.order_type, prepared_order.size, prepared_order.price_text,
      magic_enum::enum_name(prepared_order.time_in_force),
      prepared_order.reduce_only ? "true" : "false",
      options.keep_open ? "true" : "false");
  if (!options.execute) {
    fmt::print("dry_run=true no websocket connection opened\n");
    return 0;
  }

  gate::OrderSessionConfigResult config_result =
      gate::LoadOrderSessionConfigFile(options.config_path);
  if (!config_result.ok) {
    fmt::print(stderr, "[FAIL] config_error={}\n", config_result.error);
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
    return 2;
  }
  const char* api_secret = EnvValue(api_secret_env);
  if (api_secret == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n", api_secret_env);
    return 2;
  }

  gate::LoginCredentials credentials{.api_key = api_key,
                                     .api_secret = api_secret};
  fmt::print(
      "config name={} host={} target={} tls={} request_map_capacity={}\n",
      config.name, config.connection.host, config.connection.target,
      config.connection.enable_tls ? "true" : "false",
      config.request_map_capacity);
  if (config.connection.enable_tls) {
    return RunLive<gate::OrderSessionDefaultTlsWebSocketPolicy>(
        std::move(config), std::move(credentials), options, prepared_order);
  }
  return RunLive<gate::OrderSessionDefaultPlainWebSocketPolicy>(
      std::move(config), std::move(credentials), options, prepared_order);
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{"Place one Gate futures order through Strategy + WebSocket"};
  app.add_option("--config", options.config_path,
                 "Gate order session TOML path");
  app.add_option("--api-key", options.api_key_env,
                 "Override API key environment variable name from config");
  app.add_option("--api-secret", options.api_secret_env,
                 "Override API secret environment variable name from config");
  app.add_option("--contract", options.contract, "Gate futures contract");
  app.add_option("--side", options.side, "Order side")
      ->check(CLI::IsMember({"buy", "sell"}));
  app.add_option("--order-type", options.order_type, "Order type")
      ->check(CLI::IsMember({"limit", "market"}));
  app.add_option("--size", options.size,
                 "Positive contract size. Hard risk limit: <= 5");
  app.add_option("--price", options.price,
                 "Limit price. Market orders force price=0 and tif=ioc");
  app.add_option("--tif", options.tif, "Time in force")
      ->check(CLI::IsMember({"gtc", "ioc"}));
  app.add_option("--symbol-id", options.symbol_id,
                 "Optional internal symbol id for Strategy order");
  app.add_option("--order-capacity", options.order_capacity,
                 "Strategy order pool max live orders");
  app.add_option("--wait-seconds", options.wait_seconds,
                 "Maximum seconds to wait for login and final response");
  app.add_flag("--reduce-only", options.reduce_only, "Set reduce_only=true");
  app.add_flag("--execute", options.execute,
               "Actually submit through WebSocket. Omitted means dry-run");
  app.add_flag("--keep-open", options.keep_open,
               "Do not auto-cancel after accepted place response");
  CLI11_PARSE(app, argc, argv);

  return Run(options);
}
