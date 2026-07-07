#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <toml++/toml.hpp>

#include "core/config/book_ticker_fusion_config.h"
#include "core/config/trade_fusion_config.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/thread.h"
#include "exchange/gate/market_data/data_session.h"
#include "exchange/gate/market_data/data_session_config.h"
#include "nova/utils/log.h"
#include "tools/gate/gate_data_fusion_config.h"
#include "tools/market_data/data_fusion_tool_support.h"

namespace {

namespace aq_gate = aquila::gate;
namespace aq_md = aquila::market_data;
namespace aq_tool = aquila::tools::gate;
namespace aq_tool_md = aquila::tools::market_data;

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int) {
  signal_stop_requested.store(true, std::memory_order_relaxed);
}

struct PreparedGateSource {
  aq_tool::GateDataFusionSourceConfig launch_source;
  aq_gate::DataSessionConfig data_session_config;
};

[[nodiscard]] bool LoadPreparedSources(
    const aq_tool::GateDataFusionConfig& launch_config,
    std::vector<PreparedGateSource>* sources, std::string* error) {
  sources->clear();
  sources->reserve(launch_config.sources.size());
  for (const aq_tool::GateDataFusionSourceConfig& launch_source :
       launch_config.sources) {
    aq_gate::DataSessionConfigResult data_session_result =
        aq_gate::LoadDataSessionConfigFile(launch_source.data_session_config);
    if (!data_session_result.ok) {
      *error = data_session_result.error;
      return false;
    }
    aq_tool_md::ApplyFusionSourceOverrides(launch_config.feeds, launch_source,
                                           &data_session_result.value);
    sources->push_back(PreparedGateSource{
        .launch_source = launch_source,
        .data_session_config = std::move(data_session_result.value),
    });
  }
  return true;
}

template <typename WebSocketPolicy>
class GateDataFusionSourceWorker {
 public:
  explicit GateDataFusionSourceWorker(aq_gate::DataSessionConfig config)
      : publisher_(config.data_shm), session_(std::move(config), publisher_) {}

  ~GateDataFusionSourceWorker() {
    Stop();
    Join();
  }

  void Start() {
    finished_.store(false, std::memory_order_release);
    thread_ = std::thread([this] {
      (void)session_.Start();
      finished_.store(true, std::memory_order_release);
    });
  }

  void Stop() noexcept {
    session_.Stop();
  }

  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
    publisher_.FlushPublishedCount();
  }

  [[nodiscard]] std::uint64_t published_count() const noexcept {
    return publisher_.published_book_tickers();
  }

  [[nodiscard]] std::uint64_t published_trades() const noexcept {
    return publisher_.published_trades();
  }

  [[nodiscard]] bool finished() const noexcept {
    return finished_.load(std::memory_order_acquire);
  }

 private:
  using Session = aq_gate::DataSession<aq_md::DataShmPublisher, WebSocketPolicy,
                                       aq_gate::DataSessionDiagnosticsPolicy>;

  aq_md::DataShmPublisher publisher_;
  Session session_;
  std::thread thread_;
  std::atomic<bool> finished_{false};
};

template <typename WebSocketPolicy>
int RunConnected(const aq_tool::GateDataFusionConfig& launch_config,
                 std::optional<aq_md::BookTickerFusionConfig> book_config,
                 std::optional<aq_md::TradeFusionConfig> trade_config,
                 std::vector<PreparedGateSource> sources,
                 std::uint64_t max_runtime_ms) {
  std::vector<std::unique_ptr<GateDataFusionSourceWorker<WebSocketPolicy>>>
      workers;
  workers.reserve(sources.size());
  for (PreparedGateSource& source : sources) {
    workers.push_back(
        std::make_unique<GateDataFusionSourceWorker<WebSocketPolicy>>(
            std::move(source.data_session_config)));
  }

  std::unique_ptr<aq_md::BookTickerFusionThread> book_thread;
  if (book_config.has_value()) {
    book_thread = std::make_unique<aq_md::BookTickerFusionThread>(
        std::move(*book_config));
  }
  std::unique_ptr<aq_md::TradeFusionThread> trade_thread;
  if (trade_config.has_value()) {
    trade_thread =
        std::make_unique<aq_md::TradeFusionThread>(std::move(*trade_config));
  }

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  signal_stop_requested.store(false, std::memory_order_relaxed);

  if (book_thread != nullptr) {
    book_thread->Start();
  }
  if (trade_thread != nullptr) {
    trade_thread->Start();
  }
  for (std::unique_ptr<GateDataFusionSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Start();
  }

  bool unexpected_stop{false};
  const auto start = std::chrono::steady_clock::now();
  while (!signal_stop_requested.load(std::memory_order_relaxed)) {
    if ((book_thread != nullptr && book_thread->finished()) ||
        (trade_thread != nullptr && trade_thread->finished())) {
      unexpected_stop = true;
      break;
    }
    for (const std::unique_ptr<GateDataFusionSourceWorker<WebSocketPolicy>>&
             worker : workers) {
      if (worker->finished()) {
        unexpected_stop = true;
        break;
      }
    }
    if (unexpected_stop) {
      break;
    }
    if (max_runtime_ms != 0) {
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start)
              .count();
      if (elapsed_ms >= static_cast<std::int64_t>(max_runtime_ms)) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  for (std::unique_ptr<GateDataFusionSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Stop();
  }
  for (std::unique_ptr<GateDataFusionSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Join();
  }
  if (book_thread != nullptr) {
    book_thread->Stop();
  }
  if (trade_thread != nullptr) {
    trade_thread->Stop();
  }

  bool ok = !unexpected_stop;
  if (book_thread != nullptr) {
    const aq_md::BookTickerFusionThreadStats stats = book_thread->Join();
    std::uint64_t source_published_count{0};
    for (const std::unique_ptr<GateDataFusionSourceWorker<WebSocketPolicy>>&
             worker : workers) {
      source_published_count += worker->published_count();
    }
    aq_tool_md::LogDataFusionRunSummary<
        aq_tool_md::BookTickerDataFusionFeedTraits>(
        launch_config.name, workers.size(), source_published_count, stats);
    ok = ok && stats.ok;
  }
  if (trade_thread != nullptr) {
    const aq_md::TradeFusionThreadStats stats = trade_thread->Join();
    std::uint64_t source_published_count{0};
    for (const std::unique_ptr<GateDataFusionSourceWorker<WebSocketPolicy>>&
             worker : workers) {
      source_published_count += worker->published_trades();
    }
    aq_tool_md::LogDataFusionRunSummary<aq_tool_md::TradeDataFusionFeedTraits>(
        launch_config.name, workers.size(), source_published_count, stats);
    ok = ok && stats.ok;
  }
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/market_data_fusion/gate_data_fusion_book_ticker_4sources.toml"};
  bool connect{false};
  std::uint64_t max_runtime_ms{0};

  CLI::App app{"Gate data fusion"};
  app.add_option("--config", config_path, "data fusion TOML path");
  app.add_flag("--connect", connect, "connect data sessions");
  app.add_option("--max-runtime-ms", max_runtime_ms, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result launch_toml =
        toml::parse_file(config_path.string());
    const aq_tool::GateDataFusionConfigResult launch_result =
        aq_tool::ParseGateDataFusionConfig(launch_toml);
    if (!launch_result.ok) {
      aq_tool_md::LogDataFusionStartupError(
          "gate_data_fusion_config_console", "config_error",
          launch_result.error);
      return 1;
    }
    const aq_tool::GateDataFusionConfig& launch_config = launch_result.value;

    std::string startup_error;
    if (!aq_tool_md::ValidateDataFusionLogBackendCpuBinding(launch_config,
                                                            &startup_error)) {
      aq_tool_md::LogDataFusionStartupError(
          "gate_data_fusion_config_console", "cpu_binding_error",
          startup_error);
      return 1;
    }

    const nova::LogConfig log_config = aq_tool_md::MakeDataFusionLogConfig(
        launch_toml, launch_config.backend_cpu_affinity);
    aq_tool_md::ScopedNovaLogging logging_guard{log_config};

    std::optional<aq_md::BookTickerFusionConfig> book_config;
    if (aq_tool_md::HasFusionFeed(launch_config.feeds,
                                  aq_tool_md::DataFusionFeed::kBookTicker)) {
      const aquila::config::BookTickerFusionConfigResult fusion_result =
          aquila::config::LoadBookTickerFusionConfigFile(
              launch_config.book_ticker_fusion_config);
      if (!fusion_result.ok) {
        NOVA_ERROR("fusion_config_error={}", fusion_result.error);
        return 1;
      }
      book_config = fusion_result.value;

      std::string error;
      if (!aq_tool_md::ValidateFusionAlignment<
              aq_tool_md::BookTickerDataFusionFeedTraits>(
              launch_config, *book_config, &error)) {
        NOVA_ERROR("fusion_alignment_error={}", error);
        return 1;
      }
    }

    std::optional<aq_md::TradeFusionConfig> trade_config;
    if (aq_tool_md::HasFusionFeed(launch_config.feeds,
                                  aq_tool_md::DataFusionFeed::kTrade)) {
      const aquila::config::TradeFusionConfigResult fusion_result =
          aquila::config::LoadTradeFusionConfigFile(
              launch_config.trade_fusion_config);
      if (!fusion_result.ok) {
        NOVA_ERROR("fusion_config_error={}", fusion_result.error);
        return 1;
      }
      trade_config = fusion_result.value;

      std::string error;
      if (!aq_tool_md::ValidateFusionAlignment<
              aq_tool_md::TradeDataFusionFeedTraits>(launch_config,
                                                     *trade_config, &error)) {
        NOVA_ERROR("fusion_alignment_error={}", error);
        return 1;
      }
    }

    std::string error;
    if (!aq_tool_md::ValidateDataFusionShmNames(
            launch_config, book_config ? &*book_config : nullptr,
            trade_config ? &*trade_config : nullptr, &error)) {
      NOVA_ERROR("fusion_output_error={}", error);
      return 1;
    }

    std::vector<PreparedGateSource> sources;
    if (!LoadPreparedSources(launch_config, &sources, &error)) {
      NOVA_ERROR("data_session_config_error={}", error);
      return 1;
    }
    if (!aq_tool_md::SourcesUseSameTls(sources, "Gate", &error)) {
      NOVA_ERROR("transport_error={}", error);
      return 1;
    }
    if (!aq_tool_md::ValidatePreparedDataFusionCpuBindings(
            launch_config, sources, book_config ? &*book_config : nullptr,
            trade_config ? &*trade_config : nullptr, &error)) {
      NOVA_ERROR("cpu_binding_error={}", error);
      return 1;
    }

    if (!connect) {
      if (book_config.has_value()) {
        aq_tool_md::LogDataFusionDryRun<
            aq_tool_md::BookTickerDataFusionFeedTraits>(launch_config,
                                                        *book_config, sources);
      }
      if (trade_config.has_value()) {
        aq_tool_md::LogDataFusionDryRun<aq_tool_md::TradeDataFusionFeedTraits>(
            launch_config, *trade_config, sources);
      }
      return 0;
    }

    if (sources.front().data_session_config.connection.enable_tls) {
      return RunConnected<aq_gate::DefaultTlsWebSocketPolicy>(
          launch_config, std::move(book_config), std::move(trade_config),
          std::move(sources), max_runtime_ms);
    }
    return RunConnected<aq_gate::DefaultPlainWebSocketPolicy>(
        launch_config, std::move(book_config), std::move(trade_config),
        std::move(sources), max_runtime_ms);
  } catch (const std::exception& exc) {
    nova::LogConfig fallback_log_config = aq_tool_md::MakeConsoleOnlyLogConfig(
        "gate_data_fusion_startup_console");
    nova::InitializeLogging(fallback_log_config);
    NOVA_ERROR("gate_data_fusion_error={}", exc.what());
    nova::StopLogging();
    return 1;
  }
}
