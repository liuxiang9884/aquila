#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>
#include <toml++/toml.hpp>

#include "core/config/order_gateway_config.h"
#include "core/trading/order_gateway_shm.h"
#include "exchange/bitget/trading/order_gateway_worker.h"
#include "exchange/bitget/trading/order_session.h"
#include "exchange/bitget/trading/order_session_config.h"
#include "nova/utils/log.h"

#if defined(__linux__)
#include <pthread.h>

#include <sched.h>
#endif

namespace {

namespace aq_bitget = aquila::bitget;
namespace aq_config = aquila::config;
namespace aq_core = aquila::core;

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int) {
  signal_stop_requested.store(true, std::memory_order_relaxed);
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

struct PreparedRoute {
  aq_config::OrderGatewayRouteConfig route_config;
  aq_bitget::OrderSessionConfig order_session_config;
  aq_bitget::LoginCredentials credentials;
};

[[nodiscard]] bool BuildCredentials(const aq_bitget::OrderSessionConfig& config,
                                    aq_bitget::LoginCredentials* credentials,
                                    std::string* error) {
  const char* api_key = EnvValue(config.credentials.api_key_env);
  if (api_key == nullptr) {
    *error =
        fmt::format("missing API key env {}", config.credentials.api_key_env);
    return false;
  }
  const char* api_secret = EnvValue(config.credentials.api_secret_env);
  if (api_secret == nullptr) {
    *error = fmt::format("missing API secret env {}",
                         config.credentials.api_secret_env);
    return false;
  }
  const char* api_passphrase = EnvValue(config.credentials.api_passphrase_env);
  if (api_passphrase == nullptr) {
    *error = fmt::format("missing API passphrase env {}",
                         config.credentials.api_passphrase_env);
    return false;
  }
  credentials->api_key = api_key;
  credentials->api_secret = api_secret;
  credentials->passphrase = api_passphrase;
  return true;
}

[[nodiscard]] bool LoadPreparedRoutes(
    const aq_config::OrderGatewayConfig& gateway_config,
    bool require_credentials, std::vector<PreparedRoute>* routes,
    std::string* error) {
  routes->clear();
  routes->reserve(gateway_config.routes.size());
  for (const aq_config::OrderGatewayRouteConfig& route_config :
       gateway_config.routes) {
    aq_bitget::OrderSessionConfigResult order_session_result =
        aq_bitget::LoadOrderSessionConfigFile(
            route_config.order_session_config_path);
    if (!order_session_result.ok) {
      *error = order_session_result.error;
      return false;
    }
    if (require_credentials &&
        !order_session_result.value.client_oid_run_namespace.IsConfigured()) {
      *error = fmt::format(
          "route '{}' uses reserved client_oid_run_namespace; run prepare "
          "must generate a run-scoped order session config",
          route_config.name);
      return false;
    }
    if (!routes->empty() &&
        routes->front().order_session_config.client_oid_run_namespace !=
            order_session_result.value.client_oid_run_namespace) {
      *error =
          "all Bitget order gateway routes must use the same "
          "client_oid_run_namespace";
      return false;
    }
    aq_bitget::LoginCredentials credentials;
    if (require_credentials &&
        !BuildCredentials(order_session_result.value, &credentials, error)) {
      return false;
    }
    if (route_config.worker_cpu_id >= 0) {
      order_session_result.value.connection.runtime_policy.io_cpu_id =
          route_config.worker_cpu_id;
    }
    routes->push_back(PreparedRoute{
        .route_config = route_config,
        .order_session_config = std::move(order_session_result.value),
        .credentials = std::move(credentials),
    });
  }
  return true;
}

void BindCurrentThreadToCpu(int cpu_id) noexcept {
  if (cpu_id < 0) {
    return;
  }
#if defined(__linux__)
  cpu_set_t cpuset;
  if (cpu_id >= CPU_SETSIZE) {
    if (::nova::kLogManager.logger() != nullptr) {
      NOVA_WARNING("bitget_order_gateway_bind_cpu_invalid cpu_id={}", cpu_id);
    }
    return;
  }
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_id, &cpuset);
  const int rc =
      pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  if (rc != 0 && ::nova::kLogManager.logger() != nullptr) {
    NOVA_WARNING("bitget_order_gateway_bind_cpu_failed cpu_id={} error={}",
                 cpu_id, std::strerror(rc));
  }
#else
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_WARNING("bitget_order_gateway_bind_cpu_unsupported cpu_id={}", cpu_id);
  }
#endif
}

[[nodiscard]] bool RoutesUseSameTls(const std::vector<PreparedRoute>& routes,
                                    std::string* error) {
  if (routes.empty()) {
    *error = "order gateway requires at least one route";
    return false;
  }
  const bool tls = routes.front().order_session_config.connection.enable_tls;
  for (std::size_t i = 1; i < routes.size(); ++i) {
    if (routes[i].order_session_config.connection.enable_tls != tls) {
      *error = "mixed TLS/plain order gateway routes are not supported";
      return false;
    }
  }
  return true;
}

[[nodiscard]] aq_core::OrderGatewayShmConfig ToShmConfig(
    const aq_config::OrderGatewayConfig& config, bool remove_existing) {
  return aq_core::OrderGatewayShmConfig{
      .shm_name = config.shm_name,
      .create = true,
      .remove_existing = remove_existing,
      .route_count = config.route_count,
      .command_queue_capacity = config.command_queue_capacity,
      .event_queue_capacity = config.event_queue_capacity,
      .startup_ready_timeout_s = config.startup_ready_timeout_s,
  };
}

void LogDryRun(const aq_config::OrderGatewayConfig& gateway_config,
               const std::vector<PreparedRoute>& routes) {
  NOVA_INFO(
      "bitget_order_gateway_dry_run name={} shm_name={} route_count={} "
      "command_queue_capacity={} event_queue_capacity={} "
      "startup_ready_timeout_s={}",
      gateway_config.name, gateway_config.shm_name, gateway_config.route_count,
      gateway_config.command_queue_capacity,
      gateway_config.event_queue_capacity,
      gateway_config.startup_ready_timeout_s);
  for (std::size_t i = 0; i < routes.size(); ++i) {
    const PreparedRoute& route = routes[i];
    NOVA_INFO(
        "bitget_order_gateway_route route_id={} name={} worker_cpu_id={} "
        "order_session_cpu_id={} order_session_config={} host={} "
        "connect_ip={} tls={} client_oid_run_namespace={}",
        i, route.route_config.name, route.route_config.worker_cpu_id,
        route.order_session_config.connection.runtime_policy.io_cpu_id,
        route.route_config.order_session_config_path.string(),
        route.order_session_config.connection.host,
        route.order_session_config.connection.connect_ip,
        route.order_session_config.connection.enable_tls ? "true" : "false",
        route.order_session_config.client_oid_run_namespace.View());
  }
}

template <typename WebSocketPolicy>
class BitgetOrderGatewayRouteWorker {
 public:
  using Session =
      aq_bitget::OrderSession<aq_bitget::OrderGatewaySessionEventHandler,
                              WebSocketPolicy,
                              aq_bitget::NoopOrderSessionDiagnostics>;
  using CommandWorker = aq_bitget::OrderGatewayCommandWorker<Session>;

  BitgetOrderGatewayRouteWorker(std::uint16_t route_id, int worker_cpu_id,
                                aq_core::OrderGatewayCommandQueue command_queue,
                                aq_core::OrderGatewayEventQueue event_queue,
                                aq_core::OrderGatewayShmHeader* shm_header,
                                aq_bitget::OrderSessionConfig config,
                                aq_bitget::LoginCredentials credentials)
      : publisher_(route_id, event_queue, shm_header),
        handler_(publisher_),
        session_(std::move(config.connection), std::move(credentials),
                 config.client_oid_run_namespace, handler_,
                 config.request_map_capacity, config.order_id_cache_capacity),
        command_worker_(route_id, command_queue, session_, publisher_),
        worker_cpu_id_(worker_cpu_id) {
    session_.SetRuntimeHook(this, &RuntimeHook);
  }

  BitgetOrderGatewayRouteWorker(const BitgetOrderGatewayRouteWorker&) = delete;
  BitgetOrderGatewayRouteWorker& operator=(
      const BitgetOrderGatewayRouteWorker&) = delete;

  void Start() {
    thread_ = std::thread([this] {
      BindCurrentThreadToCpu(worker_cpu_id_);
      start_result_ = session_.Start();
      (void)publisher_.PublishStopped();
      done_.store(true, std::memory_order_release);
    });
  }

  void Stop() noexcept {
    session_.Stop();
  }

  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] bool start_result() const noexcept {
    return start_result_;
  }

  [[nodiscard]] bool done() const noexcept {
    return done_.load(std::memory_order_acquire);
  }

 private:
  static void RuntimeHook(void* raw) noexcept {
    auto* self = static_cast<BitgetOrderGatewayRouteWorker*>(raw);
    const std::uint64_t drained = self->command_worker_.Drain(64);
    (void)drained;
    if (self->command_worker_.stopped() ||
        self->publisher_.event_queue_failed()) {
      self->session_.Stop();
    }
  }

  aq_bitget::OrderGatewayWorkerPublisher publisher_;
  aq_bitget::OrderGatewaySessionEventHandler handler_;
  Session session_;
  CommandWorker command_worker_;
  std::thread thread_;
  int worker_cpu_id_{-1};
  std::atomic<bool> done_{false};
  bool start_result_{false};
};

template <typename WebSocketPolicy>
int RunConnected(const aq_config::OrderGatewayConfig& gateway_config,
                 std::vector<PreparedRoute> routes, bool remove_existing_shm,
                 std::uint64_t max_runtime_ms) {
  aq_core::OrderGatewayShmConfig shm_config =
      ToShmConfig(gateway_config, remove_existing_shm);
  aquila::Result<aq_core::OrderGatewayShmManager> shm_result =
      aq_core::OrderGatewayShmManager::Create(shm_config);
  if (!shm_result.ok) {
    NOVA_ERROR("order_gateway_shm_error={}", shm_result.error);
    return 1;
  }
  aq_core::OrderGatewayShmManager& shm = shm_result.value;

  std::vector<std::unique_ptr<BitgetOrderGatewayRouteWorker<WebSocketPolicy>>>
      workers;
  workers.reserve(routes.size());
  for (std::size_t i = 0; i < routes.size(); ++i) {
    workers.push_back(
        std::make_unique<BitgetOrderGatewayRouteWorker<WebSocketPolicy>>(
            static_cast<std::uint16_t>(i), routes[i].route_config.worker_cpu_id,
            shm.CommandQueue(i), shm.EventQueue(i), &shm.header(),
            std::move(routes[i].order_session_config),
            std::move(routes[i].credentials)));
  }

  signal_stop_requested.store(false, std::memory_order_relaxed);
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  for (auto& worker : workers) {
    worker->Start();
  }

  const auto start = std::chrono::steady_clock::now();
  while (!signal_stop_requested.load(std::memory_order_relaxed)) {
    if (max_runtime_ms != 0) {
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed_ms >= static_cast<std::int64_t>(max_runtime_ms)) {
        break;
      }
    }
    bool all_workers_done = !workers.empty();
    for (const auto& worker : workers) {
      if (!worker->done()) {
        all_workers_done = false;
        break;
      }
    }
    if (all_workers_done) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  for (auto& worker : workers) {
    worker->Stop();
  }
  for (auto& worker : workers) {
    worker->Join();
  }
  for (const auto& worker : workers) {
    if (!worker->start_result()) {
      return 1;
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/order_gateways/bitget_order_gateway.toml"};
  bool connect{false};
  bool validate_only{false};
  bool remove_existing_shm{false};
  std::uint64_t max_runtime_ms{0};

  CLI::App app{"Bitget order gateway"};
  app.add_option("--config", config_path, "order gateway TOML path");
  app.add_flag("--connect", connect, "connect order sessions");
  app.add_flag("--validate-only", validate_only,
               "Validate config and route session configs without connecting");
  app.add_flag("--remove-existing-shm", remove_existing_shm,
               "unlink existing order gateway SHM before create");
  app.add_option("--max-runtime-ms", max_runtime_ms, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result launch_toml =
        toml::parse_file(config_path.string());
    nova::LoggingGuard logging_guard{launch_toml};
    const aq_config::OrderGatewayConfigResult gateway_result =
        aq_config::ParseOrderGatewayConfig(launch_toml, config_path);
    if (!gateway_result.ok) {
      NOVA_ERROR("order_gateway_config_error={}", gateway_result.error);
      return 1;
    }

    std::string error;
    std::vector<PreparedRoute> routes;
    if (!LoadPreparedRoutes(gateway_result.value, connect, &routes, &error)) {
      NOVA_ERROR("order_gateway_route_error={}", error);
      return 1;
    }
    if (!RoutesUseSameTls(routes, &error)) {
      NOVA_ERROR("order_gateway_transport_error={}", error);
      return 1;
    }

    if (validate_only) {
      connect = false;
    }
    if (!connect) {
      LogDryRun(gateway_result.value, routes);
      return 0;
    }

    if (routes.front().order_session_config.connection.enable_tls) {
      return RunConnected<aq_bitget::OrderSessionDefaultTlsWebSocketPolicy>(
          gateway_result.value, std::move(routes), remove_existing_shm,
          max_runtime_ms);
    }
    return RunConnected<aq_bitget::OrderSessionDefaultPlainWebSocketPolicy>(
        gateway_result.value, std::move(routes), remove_existing_shm,
        max_runtime_ms);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "bitget_order_gateway failed: {}\n", exc.what());
    return 1;
  }
}
