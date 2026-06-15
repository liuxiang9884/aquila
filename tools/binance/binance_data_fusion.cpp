#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
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

#include "core/config/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_thread.h"
#include "core/market_data/data_shm.h"
#include "exchange/binance/market_data/data_session.h"
#include "exchange/binance/market_data/data_session_config.h"
#include "nova/utils/log.h"
#include "tools/binance/binance_data_fusion_config.h"

namespace {

namespace aq_binance = aquila::binance;
namespace aq_md = aquila::market_data;
namespace aq_tool = aquila::tools::binance;

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int) {
  signal_stop_requested.store(true, std::memory_order_relaxed);
}

struct PreparedBinanceSource {
  aq_tool::BinanceDataFusionSourceConfig launch_source;
  aq_binance::DataSessionConfig data_session_config;
};

[[nodiscard]] const aq_md::BookTickerFusionSourceConfig* FindFusionSource(
    const aq_md::BookTickerFusionConfig& fusion_config,
    std::int32_t source_id) {
  for (const aq_md::BookTickerFusionSourceConfig& source :
       fusion_config.sources) {
    if (source.source_id == source_id) {
      return &source;
    }
  }
  return nullptr;
}

[[nodiscard]] bool ValidateFusionAlignment(
    const aq_tool::BinanceDataFusionConfig& launch_config,
    const aq_md::BookTickerFusionConfig& fusion_config, std::string* error) {
  for (const aq_tool::BinanceDataFusionSourceConfig& launch_source :
       launch_config.sources) {
    const aq_md::BookTickerFusionSourceConfig* fusion_source =
        FindFusionSource(fusion_config, launch_source.source_id);
    if (fusion_source == nullptr) {
      *error =
          fmt::format("missing fusion source_id={}", launch_source.source_id);
      return false;
    }
    if (fusion_source->shm_name != launch_source.book_ticker_shm_name) {
      *error = fmt::format("source_id={} shm mismatch fusion={} launch={}",
                           launch_source.source_id, fusion_source->shm_name,
                           launch_source.book_ticker_shm_name);
      return false;
    }
    if (fusion_source->channel_name != launch_source.book_ticker_channel_name) {
      *error = fmt::format("source_id={} channel mismatch fusion={} launch={}",
                           launch_source.source_id, fusion_source->channel_name,
                           launch_source.book_ticker_channel_name);
      return false;
    }
  }
  return true;
}

void ApplySourceOverride(const aq_tool::BinanceDataFusionSourceConfig& source,
                         aq_binance::DataSessionConfig* data_session_config) {
  data_session_config->name = source.data_session_name;
  data_session_config->book_ticker_shm.enabled = true;
  data_session_config->book_ticker_shm.shm_name = source.book_ticker_shm_name;
  data_session_config->book_ticker_shm.channel_name =
      source.book_ticker_channel_name;
  data_session_config->book_ticker_shm.create = true;
  data_session_config->book_ticker_shm.remove_existing =
      source.remove_existing_source_shm;
  if (source.bind_cpu_id >= 0) {
    data_session_config->connection.runtime_policy.io_cpu_id =
        source.bind_cpu_id;
  }
  data_session_config->diagnostics.latency_outlier.source_id = source.source_id;
}

[[nodiscard]] bool LoadPreparedSources(
    const aq_tool::BinanceDataFusionConfig& launch_config,
    std::vector<PreparedBinanceSource>* sources, std::string* error) {
  sources->clear();
  sources->reserve(launch_config.sources.size());
  for (const aq_tool::BinanceDataFusionSourceConfig& launch_source :
       launch_config.sources) {
    aq_binance::DataSessionConfigResult data_session_result =
        aq_binance::LoadDataSessionConfigFile(
            launch_source.data_session_config);
    if (!data_session_result.ok) {
      *error = data_session_result.error;
      return false;
    }
    ApplySourceOverride(launch_source, &data_session_result.value);
    sources->push_back(PreparedBinanceSource{
        .launch_source = launch_source,
        .data_session_config = std::move(data_session_result.value),
    });
  }
  return true;
}

[[nodiscard]] bool ValidateHomogeneousTransport(
    const std::vector<PreparedBinanceSource>& sources, std::string* error) {
  if (sources.empty()) {
    *error = "launch sources must not be empty";
    return false;
  }
  const bool enable_tls =
      sources.front().data_session_config.connection.enable_tls;
  for (const PreparedBinanceSource& source : sources) {
    if (source.data_session_config.connection.enable_tls != enable_tls) {
      *error = "all Binance data fusion sources must use the same TLS setting";
      return false;
    }
  }
  return true;
}

void PrintDryRun(const aq_tool::BinanceDataFusionConfig& launch_config,
                 const aq_md::BookTickerFusionConfig& fusion_config,
                 const std::vector<PreparedBinanceSource>& sources) {
  fmt::print(
      "result=ok connect=false launch={} source_count={} fusion={} "
      "output_shm={} metadata_output={}\n",
      launch_config.name, sources.size(), fusion_config.name,
      fusion_config.output.shm_name,
      fusion_config.output.metadata_bin.string());
  for (const PreparedBinanceSource& source : sources) {
    const auto& connection = source.data_session_config.connection;
    fmt::print(
        "source_id={} name={} data_session_config={} shm={} channel={} "
        "tls={} bind_cpu_id={}\n",
        source.launch_source.source_id, source.data_session_config.name,
        source.launch_source.data_session_config.string(),
        source.data_session_config.book_ticker_shm.shm_name,
        source.data_session_config.book_ticker_shm.channel_name,
        connection.enable_tls ? "true" : "false",
        connection.runtime_policy.io_cpu_id);
  }
}

template <typename WebSocketPolicy>
class BinanceSourceWorker {
 public:
  explicit BinanceSourceWorker(aq_binance::DataSessionConfig config)
      : publisher_(config.book_ticker_shm),
        session_(std::move(config), publisher_) {}

  void Start() {
    thread_ = std::thread([this] { start_result_ = session_.Start(); });
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
  using Session =
      aq_binance::DataSession<aq_md::DataShmPublisher, WebSocketPolicy,
                              aq_binance::DataSessionDiagnosticsPolicy>;

  aq_md::DataShmPublisher publisher_;
  Session session_;
  std::thread thread_;
  bool start_result_{false};
};

template <typename WebSocketPolicy>
int RunConnected(const aq_tool::BinanceDataFusionConfig& launch_config,
                 aq_md::BookTickerFusionConfig fusion_config,
                 std::vector<PreparedBinanceSource> sources,
                 std::uint64_t max_runtime_ms) {
  std::vector<std::unique_ptr<BinanceSourceWorker<WebSocketPolicy>>> workers;
  workers.reserve(sources.size());
  for (PreparedBinanceSource& source : sources) {
    workers.push_back(std::make_unique<BinanceSourceWorker<WebSocketPolicy>>(
        std::move(source.data_session_config)));
  }

  aq_md::BookTickerFusionThread fusion_thread(std::move(fusion_config));
  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  signal_stop_requested.store(false, std::memory_order_relaxed);

  for (std::unique_ptr<BinanceSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Start();
  }
  fusion_thread.Start();

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

  for (std::unique_ptr<BinanceSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Stop();
  }
  fusion_thread.Stop();
  for (std::unique_ptr<BinanceSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    worker->Join();
  }
  const aq_md::BookTickerFusionThreadStats fusion_stats = fusion_thread.Join();

  std::uint64_t source_published_count{0};
  for (const std::unique_ptr<BinanceSourceWorker<WebSocketPolicy>>& worker :
       workers) {
    source_published_count += worker->published_count();
  }

  fmt::print(
      "result={} launch={} source_count={} source_published_count={} "
      "fusion_total_read_count={} fusion_total_published_count={} "
      "fusion_metadata_write_errors={} fusion_flush_ok={} error={}\n",
      fusion_stats.ok ? "ok" : "failed", launch_config.name, workers.size(),
      source_published_count, fusion_stats.total_read_count,
      fusion_stats.total_published_count,
      fusion_stats.total_metadata_write_errors,
      fusion_stats.flush_ok ? "true" : "false", fusion_stats.error);
  return fusion_stats.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/market_data_fusion/"
      "binance_data_fusion_book_ticker_4sources.toml"};
  bool connect{false};
  std::uint64_t max_runtime_ms{0};

  CLI::App app{"Binance data fusion"};
  app.add_option("--config", config_path, "data fusion TOML path");
  app.add_flag("--connect", connect, "connect data sessions");
  app.add_option("--max-runtime-ms", max_runtime_ms, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  try {
    const toml::parse_result launch_toml =
        toml::parse_file(config_path.string());
    nova::LoggingGuard logging_guard{launch_toml};

    const aq_tool::BinanceDataFusionConfigResult launch_result =
        aq_tool::ParseBinanceDataFusionConfig(launch_toml);
    if (!launch_result.ok) {
      fmt::print(stderr, "config_error={}\n", launch_result.error);
      return 1;
    }
    const aq_tool::BinanceDataFusionConfig& launch_config = launch_result.value;

    const aquila::config::BookTickerFusionConfigResult fusion_result =
        aquila::config::LoadBookTickerFusionConfigFile(
            launch_config.fusion_config);
    if (!fusion_result.ok) {
      fmt::print(stderr, "fusion_config_error={}\n", fusion_result.error);
      return 1;
    }
    aq_md::BookTickerFusionConfig fusion_config = fusion_result.value;

    std::string error;
    if (!ValidateFusionAlignment(launch_config, fusion_config, &error)) {
      fmt::print(stderr, "fusion_alignment_error={}\n", error);
      return 1;
    }

    std::vector<PreparedBinanceSource> sources;
    if (!LoadPreparedSources(launch_config, &sources, &error)) {
      fmt::print(stderr, "data_session_config_error={}\n", error);
      return 1;
    }
    if (!ValidateHomogeneousTransport(sources, &error)) {
      fmt::print(stderr, "transport_error={}\n", error);
      return 1;
    }

    if (!connect) {
      PrintDryRun(launch_config, fusion_config, sources);
      return 0;
    }

    if (sources.front().data_session_config.connection.enable_tls) {
      return RunConnected<aq_binance::DefaultTlsWebSocketPolicy>(
          launch_config, std::move(fusion_config), std::move(sources),
          max_runtime_ms);
    }
    return RunConnected<aq_binance::DefaultPlainWebSocketPolicy>(
        launch_config, std::move(fusion_config), std::move(sources),
        max_runtime_ms);
  } catch (const std::exception& exc) {
    fmt::print(stderr, "binance_data_fusion_error={}\n", exc.what());
    return 1;
  }
}
