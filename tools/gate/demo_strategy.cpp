#include "tools/gate/demo_strategy.h"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/config/strategy_config.h"
#include "core/strategy/strategy_runtime.h"
#include "exchange/gate/trading/order_session_config.h"
#include "nova/utils/log.h"
#include "tools/gate/strategy_runtime_adapter.h"

namespace {

namespace demo = aquila::tools::gate_demo_strategy;
namespace gate = aquila::gate;
namespace gate_runtime = aquila::tools::gate_strategy_runtime;
namespace strategy = aquila::strategy;

struct CliOptions {
  std::filesystem::path config_path{"config/strategies/demo_strategy.toml"};
  std::string api_key_env;
  std::string api_secret_env;
  bool execute{false};
};

struct LoadedConfig {
  aquila::config::StrategyConfig strategy;
  aquila::config::DataReaderConfig data_reader;
  gate::OrderSessionConfig order_session;
  demo::DemoStrategyConfig demo_strategy;
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

std::string_view ModeText(aquila::config::StrategyMode mode) noexcept {
  switch (mode) {
    case aquila::config::StrategyMode::kDryRun:
      return "dry_run";
    case aquila::config::StrategyMode::kLive:
      return "live";
  }
  return "unknown";
}

bool ValidateLoadedConfig(const LoadedConfig& loaded) {
  if (loaded.strategy.name != "demo") {
    fmt::print(stderr, "[FAIL] strategy.name must be demo for this tool\n");
    NOVA_ERROR("strategy.name must be demo for this tool actual={}",
               loaded.strategy.name);
    return false;
  }
  const aquila::config::InstrumentInfo* gate_instrument =
      loaded.data_reader.instrument_catalog.Find(aquila::Exchange::kGate,
                                                 loaded.demo_strategy.contract);
  if (gate_instrument == nullptr ||
      gate_instrument->symbol_id != loaded.demo_strategy.symbol_id) {
    fmt::print(stderr,
               "[FAIL] demo contract={} does not match Gate symbol_id={}\n",
               loaded.demo_strategy.contract, loaded.demo_strategy.symbol_id);
    NOVA_ERROR("demo contract/symbol_id mismatch contract={} symbol_id={}",
               loaded.demo_strategy.contract, loaded.demo_strategy.symbol_id);
    return false;
  }
  const std::uint64_t worst_case_orders =
      static_cast<std::uint64_t>(loaded.demo_strategy.rounds) * 2U;
  if (loaded.strategy.order_capacity < worst_case_orders) {
    fmt::print(stderr,
               "[FAIL] strategy.order_capacity={} is smaller than worst-case "
               "demo orders={} for rounds={}\n",
               loaded.strategy.order_capacity, worst_case_orders,
               loaded.demo_strategy.rounds);
    NOVA_ERROR(
        "strategy.order_capacity too small capacity={} worst_case_orders={} "
        "rounds={}",
        loaded.strategy.order_capacity, worst_case_orders,
        loaded.demo_strategy.rounds);
    return false;
  }
  return true;
}

void PrintSummary(const LoadedConfig& loaded, bool execute) {
  fmt::print(
      "demo_strategy execute={} name={} mode={} strategy_id={} "
      "order_capacity={} contract={} symbol_id={} wait_seconds={} rounds={} "
      "data_reader={} sources={} order_session={} host={} tls={} "
      "feedback_enabled={} feedback_shm={} feedback_channel={} "
      "feedback_poll_budget={}\n",
      execute ? "true" : "false", loaded.strategy.name,
      ModeText(loaded.strategy.mode), loaded.strategy.strategy_id,
      loaded.strategy.order_capacity, loaded.demo_strategy.contract,
      loaded.demo_strategy.symbol_id, loaded.demo_strategy.wait_seconds,
      loaded.demo_strategy.rounds, loaded.data_reader.name,
      loaded.data_reader.sources.size(), loaded.order_session.name,
      loaded.order_session.connection.host,
      loaded.order_session.connection.enable_tls ? "true" : "false",
      loaded.strategy.feedback.enabled ? "true" : "false",
      loaded.strategy.feedback.shm_name, loaded.strategy.feedback.channel_name,
      loaded.strategy.feedback.poll_budget);
  NOVA_INFO(
      "demo_strategy execute={} name={} mode={} strategy_id={} "
      "order_capacity={} contract={} symbol_id={} wait_seconds={} rounds={} "
      "data_reader={} sources={} order_session={} host={} tls={} "
      "feedback_enabled={} feedback_shm={} feedback_channel={} "
      "feedback_poll_budget={}",
      execute ? "true" : "false", loaded.strategy.name,
      ModeText(loaded.strategy.mode), loaded.strategy.strategy_id,
      loaded.strategy.order_capacity, loaded.demo_strategy.contract,
      loaded.demo_strategy.symbol_id, loaded.demo_strategy.wait_seconds,
      loaded.demo_strategy.rounds, loaded.data_reader.name,
      loaded.data_reader.sources.size(), loaded.order_session.name,
      loaded.order_session.connection.host,
      loaded.order_session.connection.enable_tls ? "true" : "false",
      loaded.strategy.feedback.enabled ? "true" : "false",
      loaded.strategy.feedback.shm_name, loaded.strategy.feedback.channel_name,
      loaded.strategy.feedback.poll_budget);
}

bool LoadConfig(const CliOptions& options, LoadedConfig* loaded) {
  auto strategy_result =
      aquila::config::LoadStrategyConfigFile(options.config_path);
  if (!strategy_result.ok) {
    fmt::print(stderr, "[FAIL] strategy_config_error={}\n",
               strategy_result.error);
    NOVA_ERROR("strategy_config_error={}", strategy_result.error);
    return false;
  }
  loaded->strategy = std::move(strategy_result.value);

  auto data_reader_result = aquila::config::LoadDataReaderConfigFile(
      loaded->strategy.data_reader.config_path);
  if (!data_reader_result.ok) {
    fmt::print(stderr, "[FAIL] data_reader_config_error={}\n",
               data_reader_result.error);
    NOVA_ERROR("data_reader_config_error={}", data_reader_result.error);
    return false;
  }
  loaded->data_reader = std::move(data_reader_result.value);

  auto order_session_result = gate::LoadOrderSessionConfigFile(
      loaded->strategy.order_session.config_path);
  if (!order_session_result.ok) {
    fmt::print(stderr, "[FAIL] order_session_config_error={}\n",
               order_session_result.error);
    NOVA_ERROR("order_session_config_error={}", order_session_result.error);
    return false;
  }
  loaded->order_session = std::move(order_session_result.value);

  const toml::table demo_toml =
      toml::parse_file(loaded->strategy.user_config_path.string());
  auto demo_result = demo::ParseDemoStrategyConfig(demo_toml);
  if (!demo_result.ok) {
    fmt::print(stderr, "[FAIL] demo_config_error={}\n", demo_result.error);
    NOVA_ERROR("demo_config_error={}", demo_result.error);
    return false;
  }
  loaded->demo_strategy = std::move(demo_result.value);

  return ValidateLoadedConfig(*loaded);
}

gate::LoginCredentials LoadCredentials(const CliOptions& options,
                                       const gate::OrderSessionConfig& config,
                                       bool* ok) {
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
    *ok = false;
    return {};
  }
  const char* api_secret = EnvValue(api_secret_env);
  if (api_secret == nullptr) {
    fmt::print(stderr, "[FAIL] missing env var {}\n", api_secret_env);
    NOVA_ERROR("missing env var {}", api_secret_env);
    *ok = false;
    return {};
  }
  *ok = true;
  return gate::LoginCredentials{.api_key = api_key, .api_secret = api_secret};
}

template <typename WebSocketPolicy>
int RunRuntime(LoadedConfig loaded, gate::LoginCredentials credentials) {
  using OrderSession =
      gate_runtime::GateOrderSessionAdapter<WebSocketPolicy,
                                            gate::OrderSessionDiagnostics>;
  using Runtime = strategy::StrategyRuntime<demo::DemoStrategy, OrderSession>;

  gate::OrderSessionConfig order_session_config =
      std::move(loaded.order_session);
  auto runtime_result = Runtime::Create(
      std::move(loaded.strategy), std::move(loaded.data_reader),
      [order_session_config = std::move(order_session_config),
       credentials = std::move(credentials)]() mutable {
        return OrderSession(std::move(order_session_config),
                            std::move(credentials));
      },
      std::move(loaded.demo_strategy));
  if (!runtime_result.ok) {
    fmt::print(stderr, "[FAIL] runtime_create_error={}\n",
               runtime_result.error);
    NOVA_ERROR("runtime_create_error={}", runtime_result.error);
    return 1;
  }
  return runtime_result.value->Run();
}

int Run(const CliOptions& options) {
  LoadedConfig loaded;
  if (!LoadConfig(options, &loaded)) {
    return 1;
  }
  PrintSummary(loaded, options.execute);
  if (!options.execute) {
    fmt::print("dry_run=true no websocket connection opened no shm opened\n");
    NOVA_INFO("dry_run=true no websocket connection opened no shm opened");
    return 0;
  }

  bool credentials_ok = false;
  gate::LoginCredentials credentials =
      LoadCredentials(options, loaded.order_session, &credentials_ok);
  if (!credentials_ok) {
    return 2;
  }
  if (loaded.order_session.connection.enable_tls) {
    return RunRuntime<gate::OrderSessionDefaultTlsWebSocketPolicy>(
        std::move(loaded), std::move(credentials));
  }
  return RunRuntime<gate::OrderSessionDefaultPlainWebSocketPolicy>(
      std::move(loaded), std::move(credentials));
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;

  CLI::App app{
      "Run the Gate demo strategy through StrategyRuntime. Defaults to "
      "dry-run"};
  app.add_option("--config", options.config_path, "Strategy runtime TOML path");
  app.add_option("--api-key", options.api_key_env,
                 "Override API key environment variable name from config");
  app.add_option("--api-secret", options.api_secret_env,
                 "Override API secret environment variable name from config");
  app.add_flag("--execute", options.execute,
               "Actually run live WebSocket/order/feedback chain");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::table toml = toml::parse_file(options.config_path.string());
    LoggingGuard logging_guard{toml};
    return Run(options);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "[FAIL] config_error={}\n", exc.what());
    return 1;
  }
}
