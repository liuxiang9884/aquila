#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <thread>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include "core/websocket/runtime_policy.h"
#include "tools/market_data/book_ticker_fusion_config.h"
#include "tools/market_data/book_ticker_fusion_runner.h"

namespace {

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int signal) {
  (void)signal;
  signal_stop_requested.store(true, std::memory_order_relaxed);
}

bool ApplyAffinity(const aquila::tools::market_data::BookTickerFusionConfig&
                       config) noexcept {
  if (config.bind_cpu_id < 0) {
    return true;
  }
  aquila::websocket::RuntimePolicy policy;
  policy.affinity_mode = aquila::websocket::AffinityMode::kBestEffort;
  policy.io_cpu_id = config.bind_cpu_id;
  policy.lock_memory = false;
  policy.prefault_stack = true;
  policy.active_spin = true;
  return aquila::websocket::ApplyRuntimePolicy(policy);
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/market_data_fusion/gate_book_ticker_fusion_4sources.toml"};
  std::uint64_t max_polls{0};

  CLI::App app{"Gate BookTicker fastest-route fusion"};
  app.add_option("--config", config_path, "fusion TOML path");
  app.add_option("--max-polls", max_polls, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  try {
    auto config_result =
        aquila::tools::market_data::LoadBookTickerFusionConfigFile(
            config_path);
    if (!config_result.ok) {
      fmt::print(stderr, "config_error={}\n", config_result.error);
      return 1;
    }

    const aquila::tools::market_data::BookTickerFusionConfig& config =
        config_result.value;
    if (!ApplyAffinity(config)) {
      fmt::print(stderr, "affinity_warning bind_cpu_id={}\n",
                 config.bind_cpu_id);
    }

    aquila::tools::market_data::BookTickerFusionRunner runner(config);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::uint64_t polls{0};
    while (!signal_stop_requested.load(std::memory_order_relaxed) &&
           (max_polls == 0 || polls < max_polls)) {
      const aquila::tools::market_data::BookTickerFusionPollStats stats =
          runner.PollOnce();
      ++polls;
      if (stats.metadata_write_errors != 0) {
        fmt::print(stderr, "metadata_write_error count={}\n",
                   stats.metadata_write_errors);
        return 1;
      }
      if (stats.read_count == 0) {
        std::this_thread::yield();
      }
    }

    if (!runner.Flush()) {
      fmt::print(stderr, "flush_error metadata_output={}\n",
                 config.output.metadata_bin.string());
      return 1;
    }

    fmt::print(
        "result=ok polls={} total_read_count={} total_published_count={} "
        "metadata_write_errors={} metadata_output={}\n",
        polls, runner.total_read_count(), runner.total_published_count(),
        runner.total_metadata_write_errors(),
        config.output.metadata_bin.string());
    return 0;
  } catch (const std::exception& exc) {
    fmt::print(stderr, "gate_book_ticker_fusion_error={}\n", exc.what());
    return 1;
  }
}
