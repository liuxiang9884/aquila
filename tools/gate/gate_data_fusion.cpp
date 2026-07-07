#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <toml++/toml.hpp>

#include "core/config/book_ticker_fusion_config.h"
#include "core/config/trade_fusion_config.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_thread.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_thread.h"
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
    if (launch_config.feed == aq_tool_md::DataFusionFeed::kBookTicker) {
      aq_tool_md::ApplyFusionSourceOverride<
          aq_tool_md::BookTickerDataFusionFeedTraits>(
          launch_source, &data_session_result.value);
    } else {
      aq_tool_md::ApplyFusionSourceOverride<
          aq_tool_md::TradeDataFusionFeedTraits>(launch_source,
                                                 &data_session_result.value);
    }
    sources->push_back(PreparedGateSource{
        .launch_source = launch_source,
        .data_session_config = std::move(data_session_result.value),
    });
  }
  return true;
}

template <typename WebSocketPolicy>
class GateSourceWorker {
 public:
  explicit GateSourceWorker(aq_gate::DataSessionConfig config)
      : publisher_(config.book_ticker_shm),
        session_(std::move(config), publisher_) {}

  ~GateSourceWorker() {
    Stop();
    Join();
  }

  void Start() {
    thread_ = std::thread([this] { (void)session_.Start(); });
  }

  void Stop() noexcept {
    session_.Stop();
    publisher_.FlushPublishedCount();
  }

  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
    publisher_.FlushPublishedCount();
  }

  [[nodiscard]] std::uint64_t published_count() const noexcept {
    return publisher_.published_count();
  }

 private:
  using Session = aq_gate::DataSession<aq_md::DataShmPublisher, WebSocketPolicy,
                                       aq_gate::DataSessionDiagnosticsPolicy>;

  aq_md::DataShmPublisher publisher_;
  Session session_;
  std::thread thread_;
};

template <typename WebSocketPolicy>
class GateTradeSourceWorker {
 public:
  explicit GateTradeSourceWorker(aq_gate::DataSessionConfig config)
      : publisher_(config.trade_shm), session_(std::move(config), publisher_) {}

  ~GateTradeSourceWorker() {
    Stop();
    Join();
  }

  void Start() {
    thread_ = std::thread([this] { (void)session_.Start(); });
  }

  void Stop() noexcept {
    session_.Stop();
    publisher_.FlushPublishedCount();
  }

  void Join() {
    if (thread_.joinable()) {
      thread_.join();
    }
    publisher_.FlushPublishedCount();
  }

  [[nodiscard]] std::uint64_t published_count() const noexcept {
    return publisher_.published_trades();
  }

 private:
  using Session = aq_gate::DataSession<aq_md::DataShmPublisher, WebSocketPolicy,
                                       aq_gate::DataSessionDiagnosticsPolicy>;

  aq_md::DataShmPublisher publisher_;
  Session session_;
  std::thread thread_;
};

template <typename WebSocketPolicy>
int RunConnected(const aq_tool::GateDataFusionConfig& launch_config,
                 aq_md::BookTickerFusionConfig fusion_config,
                 std::vector<PreparedGateSource> sources,
                 std::uint64_t max_runtime_ms) {
  std::vector<std::unique_ptr<GateSourceWorker<WebSocketPolicy>>> workers;
  workers.reserve(sources.size());
  for (PreparedGateSource& source : sources) {
    workers.push_back(std::make_unique<GateSourceWorker<WebSocketPolicy>>(
        std::move(source.data_session_config)));
  }

  aq_md::BookTickerFusionThread fusion_thread(std::move(fusion_config));
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  signal_stop_requested.store(false, std::memory_order_relaxed);

  fusion_thread.Start();
  for (std::unique_ptr<GateSourceWorker<WebSocketPolicy>>& worker : workers) {
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
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  for (std::unique_ptr<GateSourceWorker<WebSocketPolicy>>& worker : workers) {
    worker->Stop();
  }
  fusion_thread.Stop();
  for (std::unique_ptr<GateSourceWorker<WebSocketPolicy>>& worker : workers) {
    worker->Join();
  }
  const aq_md::BookTickerFusionThreadStats fusion_stats = fusion_thread.Join();

  std::uint64_t source_published_count{0};
  for (const std::unique_ptr<GateSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    source_published_count += worker->published_count();
  }

  aq_tool_md::LogDataFusionRunSummary<
      aq_tool_md::BookTickerDataFusionFeedTraits>(
      launch_config.name, workers.size(), source_published_count, fusion_stats);
  return fusion_stats.ok ? 0 : 1;
}

template <typename WebSocketPolicy>
int RunConnectedTrade(const aq_tool::GateDataFusionConfig& launch_config,
                      aq_md::TradeFusionConfig fusion_config,
                      std::vector<PreparedGateSource> sources,
                      std::uint64_t max_runtime_ms) {
  std::vector<std::unique_ptr<GateTradeSourceWorker<WebSocketPolicy>>> workers;
  workers.reserve(sources.size());
  for (PreparedGateSource& source : sources) {
    workers.push_back(std::make_unique<GateTradeSourceWorker<WebSocketPolicy>>(
        std::move(source.data_session_config)));
  }

  aq_md::TradeFusionThread fusion_thread(std::move(fusion_config));
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  signal_stop_requested.store(false, std::memory_order_relaxed);

  fusion_thread.Start();
  for (std::unique_ptr<GateTradeSourceWorker<WebSocketPolicy>>& worker :
       workers) {
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
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  for (std::unique_ptr<GateTradeSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Stop();
  }
  fusion_thread.Stop();
  for (std::unique_ptr<GateTradeSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Join();
  }
  const aq_md::TradeFusionThreadStats fusion_stats = fusion_thread.Join();

  std::uint64_t source_published_count{0};
  for (const std::unique_ptr<GateTradeSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    source_published_count += worker->published_count();
  }

  aq_tool_md::LogDataFusionRunSummary<aq_tool_md::TradeDataFusionFeedTraits>(
      launch_config.name, workers.size(), source_published_count, fusion_stats);
  return fusion_stats.ok ? 0 : 1;
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
    nova::LoggingGuard logging_guard{launch_toml};
    const aq_tool::GateDataFusionConfigResult launch_result =
        aq_tool::ParseGateDataFusionConfig(launch_toml);
    if (!launch_result.ok) {
      NOVA_ERROR("config_error={}", launch_result.error);
      return 1;
    }
    const aq_tool::GateDataFusionConfig& launch_config = launch_result.value;

    if (launch_config.feed == aq_tool_md::DataFusionFeed::kTrade) {
      const aquila::config::TradeFusionConfigResult fusion_result =
          aquila::config::LoadTradeFusionConfigFile(
              launch_config.fusion_config);
      if (!fusion_result.ok) {
        NOVA_ERROR("fusion_config_error={}", fusion_result.error);
        return 1;
      }
      aq_md::TradeFusionConfig fusion_config = fusion_result.value;

      std::string error;
      if (!aq_tool_md::ValidateFusionAlignment<
              aq_tool_md::TradeDataFusionFeedTraits>(launch_config,
                                                     fusion_config, &error)) {
        NOVA_ERROR("fusion_alignment_error={}", error);
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

      if (!connect) {
        aq_tool_md::LogDataFusionDryRun<aq_tool_md::TradeDataFusionFeedTraits>(
            launch_config, fusion_config, sources);
        return 0;
      }

      if (sources.front().data_session_config.connection.enable_tls) {
        return RunConnectedTrade<aq_gate::DefaultTlsWebSocketPolicy>(
            launch_config, std::move(fusion_config), std::move(sources),
            max_runtime_ms);
      }
      return RunConnectedTrade<aq_gate::DefaultPlainWebSocketPolicy>(
          launch_config, std::move(fusion_config), std::move(sources),
          max_runtime_ms);
    }

    const aquila::config::BookTickerFusionConfigResult fusion_result =
        aquila::config::LoadBookTickerFusionConfigFile(
            launch_config.fusion_config);
    if (!fusion_result.ok) {
      NOVA_ERROR("fusion_config_error={}", fusion_result.error);
      return 1;
    }
    aq_md::BookTickerFusionConfig fusion_config = fusion_result.value;

    std::string error;
    if (!aq_tool_md::ValidateFusionAlignment<
            aq_tool_md::BookTickerDataFusionFeedTraits>(
            launch_config, fusion_config, &error)) {
      NOVA_ERROR("fusion_alignment_error={}", error);
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

    if (!connect) {
      aq_tool_md::LogDataFusionDryRun<
          aq_tool_md::BookTickerDataFusionFeedTraits>(launch_config,
                                                      fusion_config, sources);
      return 0;
    }

    if (sources.front().data_session_config.connection.enable_tls) {
      return RunConnected<aq_gate::DefaultTlsWebSocketPolicy>(
          launch_config, std::move(fusion_config), std::move(sources),
          max_runtime_ms);
    }
    return RunConnected<aq_gate::DefaultPlainWebSocketPolicy>(
        launch_config, std::move(fusion_config), std::move(sources),
        max_runtime_ms);
  } catch (const std::exception& exc) {
    nova::LogConfig fallback_log_config = aq_tool_md::MakeConsoleOnlyLogConfig(
        "gate_data_fusion_startup_console");
    nova::InitializeLogging(fallback_log_config);
    NOVA_ERROR("gate_data_fusion_error={}", exc.what());
    nova::StopLogging();
    return 1;
  }
}
