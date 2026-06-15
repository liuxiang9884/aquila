#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include "core/market_data/data_shm.h"
#include "core/market_data/types.h"
#include "evaluation/market_data/book_ticker_replay_benchmark_support.h"
#include "nova/concurrency/spsc_queue.h"

namespace {

namespace md = aquila::market_data;
namespace eval = aquila::market_data::evaluation;

using Clock = std::chrono::steady_clock;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

struct BenchmarkOptions {
  std::filesystem::path input_path;
  std::string mode{"both"};
  double speed{16.0};
  std::uint64_t max_records{0};
  std::uint64_t spsc_capacity{md::kBookTickerShmCapacity};
  int producer_cpu{-1};
  int consumer_cpu{-1};
};

struct TransportResult {
  std::string_view transport;
  std::uint64_t input_records{0};
  std::uint64_t published_records{0};
  std::uint64_t consumed_records{0};
  std::uint64_t overrun_count{0};
  std::int64_t elapsed_ns{0};
  eval::LatencySummary latency;
};

[[nodiscard]] std::int64_t NowNs() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#else
  std::this_thread::yield();
#endif
}

void PinCurrentThread(int cpu) {
  if (cpu < 0) {
    return;
  }

  cpu_set_t cpu_set;
  CPU_ZERO(&cpu_set);
  CPU_SET(cpu, &cpu_set);
  const int rc =
      ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set), &cpu_set);
  if (rc != 0) {
    throw std::runtime_error(
        fmt::format("failed to pin thread to cpu {}: rc={}", cpu, rc));
  }
}

void BusyWaitUntil(Clock::time_point deadline) noexcept {
  while (Clock::now() < deadline) {
    CpuRelax();
  }
}

[[nodiscard]] std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_book_ticker_queue_replay_{}_{}", ::getpid(),
                     suffix);
}

[[nodiscard]] md::BookTickerShmConfig MakeCreateConfig(
    std::string_view suffix) {
  return md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .channel_name = "book_ticker_channel",
      .create = true,
      .remove_existing = true,
  };
}

[[nodiscard]] md::BookTickerShmConfig MakeAttachConfig(
    const md::BookTickerShmConfig& create_config) {
  md::BookTickerShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
[[gnu::noinline]] void PublishToShm(
    md::DataShmPublisher* publisher,
    const aquila::BookTicker& book_ticker) noexcept {
  publisher->OnBookTicker(book_ticker);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

template <typename Publisher>
void ReplayRecords(const std::vector<aquila::BookTicker>& records, double speed,
                   Publisher&& publish) {
  auto next_deadline = Clock::now();
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (i > 0) {
      const std::int64_t delay_ns = eval::ReplayDelayNs(
          records[i - 1].exchange_ns, records[i].exchange_ns, speed);
      next_deadline += std::chrono::nanoseconds(delay_ns);
      BusyWaitUntil(next_deadline);
    }

    aquila::BookTicker book_ticker = records[i];
    book_ticker.local_ns = NowNs();
    publish(book_ticker);
  }
}

[[nodiscard]] TransportResult RunSpsc(
    const std::vector<aquila::BookTicker>& records,
    const BenchmarkOptions& options) {
  nova::SPSCQueue<aquila::BookTicker> queue(options.spsc_capacity);
  std::atomic<bool> consumer_ready{false};
  std::atomic<bool> producer_done{false};
  std::vector<std::int64_t> latencies_ns;
  latencies_ns.reserve(records.size());

  const auto started_at = Clock::now();
  std::thread consumer([&] {
    PinCurrentThread(options.consumer_cpu);
    consumer_ready.store(true, std::memory_order_release);

    aquila::BookTicker book_ticker{};
    while (!producer_done.load(std::memory_order_acquire)) {
      if (queue.TryPop(book_ticker)) {
        latencies_ns.push_back(NowNs() - book_ticker.local_ns);
      } else {
        CpuRelax();
      }
    }
    while (queue.TryPop(book_ticker)) {
      latencies_ns.push_back(NowNs() - book_ticker.local_ns);
    }
  });

  std::thread producer([&] {
    PinCurrentThread(options.producer_cpu);
    while (!consumer_ready.load(std::memory_order_acquire)) {
      CpuRelax();
    }
    ReplayRecords(records, options.speed,
                  [&](const aquila::BookTicker& book_ticker) {
                    queue.Push(book_ticker);
                  });
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();
  const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              Clock::now() - started_at)
                              .count();

  return TransportResult{
      .transport = "spsc",
      .input_records = records.size(),
      .published_records = records.size(),
      .consumed_records = latencies_ns.size(),
      .overrun_count = 0,
      .elapsed_ns = elapsed_ns,
      .latency = eval::SummarizeLatencies(std::move(latencies_ns)),
  };
}

[[nodiscard]] TransportResult RunShm(
    const std::vector<aquila::BookTicker>& records,
    const BenchmarkOptions& options) {
  const md::BookTickerShmConfig create_config = MakeCreateConfig("shm");
  ShmCleanup cleanup(create_config.shm_name);
  md::DataShmPublisher publisher(create_config);

  std::atomic<bool> consumer_ready{false};
  std::atomic<bool> producer_done{false};
  std::vector<std::int64_t> latencies_ns;
  latencies_ns.reserve(records.size());
  std::uint64_t overrun_count = 0;

  const auto started_at = Clock::now();
  std::thread consumer([&] {
    PinCurrentThread(options.consumer_cpu);
    md::BookTickerShmReader reader(MakeAttachConfig(create_config));
    consumer_ready.store(true, std::memory_order_release);

    aquila::BookTicker book_ticker{};
    while (!producer_done.load(std::memory_order_acquire)) {
      if (reader.TryReadOne(&book_ticker)) {
        latencies_ns.push_back(NowNs() - book_ticker.local_ns);
      } else {
        CpuRelax();
      }
    }
    while (reader.TryReadOne(&book_ticker)) {
      latencies_ns.push_back(NowNs() - book_ticker.local_ns);
    }
    overrun_count = reader.overrun_count();
  });

  std::thread producer([&] {
    PinCurrentThread(options.producer_cpu);
    while (!consumer_ready.load(std::memory_order_acquire)) {
      CpuRelax();
    }
    ReplayRecords(records, options.speed,
                  [&](const aquila::BookTicker& book_ticker) {
                    PublishToShm(&publisher, book_ticker);
                  });
    publisher.FlushPublishedCount();
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();
  const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              Clock::now() - started_at)
                              .count();

  return TransportResult{
      .transport = "shm_broadcast",
      .input_records = records.size(),
      .published_records = publisher.published_count(),
      .consumed_records = latencies_ns.size(),
      .overrun_count = overrun_count,
      .elapsed_ns = elapsed_ns,
      .latency = eval::SummarizeLatencies(std::move(latencies_ns)),
  };
}

void PrintResult(const TransportResult& result) {
  fmt::print(
      "transport={} input_records={} published_records={} consumed_records={} "
      "overrun_count={} elapsed_ns={} latency_count={} latency_min_ns={} "
      "latency_p50_ns={} latency_p95_ns={} latency_p99_ns={} "
      "latency_p999_ns={} latency_max_ns={}\n",
      result.transport, result.input_records, result.published_records,
      result.consumed_records, result.overrun_count, result.elapsed_ns,
      result.latency.count, result.latency.min_ns, result.latency.p50_ns,
      result.latency.p95_ns, result.latency.p99_ns, result.latency.p999_ns,
      result.latency.max_ns);
}

void PrintComparison(const TransportResult& spsc, const TransportResult& shm) {
  fmt::print(
      "comparison=shm_minus_spsc p50_delta_ns={} p95_delta_ns={} "
      "p99_delta_ns={} p999_delta_ns={} max_delta_ns={}\n",
      shm.latency.p50_ns - spsc.latency.p50_ns,
      shm.latency.p95_ns - spsc.latency.p95_ns,
      shm.latency.p99_ns - spsc.latency.p99_ns,
      shm.latency.p999_ns - spsc.latency.p999_ns,
      shm.latency.max_ns - spsc.latency.max_ns);
}

}  // namespace

int main(int argc, char** argv) {
  BenchmarkOptions options;

  CLI::App app{"Replay BookTicker dump through SPSC and SHM broadcast queues"};
  app.add_option("--input", options.input_path, "BookTicker binary dump path")
      ->required()
      ->check(CLI::ExistingFile);
  app.add_option("--mode", options.mode, "Transport: both, spsc, shm")
      ->check(CLI::IsMember({"both", "spsc", "shm"}));
  app.add_option("--speed", options.speed, "Replay speed multiplier")
      ->check(CLI::PositiveNumber);
  app.add_option("--max-records", options.max_records,
                 "Maximum records to load, 0 means all");
  app.add_option("--spsc-capacity", options.spsc_capacity,
                 "SPSC queue capacity before power-of-two rounding")
      ->check(CLI::PositiveNumber);
  app.add_option("--producer-cpu", options.producer_cpu,
                 "Optional producer CPU pin, -1 disables pinning");
  app.add_option("--consumer-cpu", options.consumer_cpu,
                 "Optional consumer CPU pin, -1 disables pinning");
  CLI11_PARSE(app, argc, argv);

  if (options.spsc_capacity < 2) {
    fmt::print(stderr, "--spsc-capacity must be at least 2\n");
    return 1;
  }

  const auto load_result =
      eval::LoadBookTickerDump(options.input_path, options.max_records);
  if (!load_result.ok) {
    fmt::print(stderr, "failed to load input: {}\n", load_result.error);
    return 1;
  }
  if (load_result.value.empty()) {
    fmt::print(stderr, "input dump contains no BookTicker records\n");
    return 1;
  }

  try {
    const std::vector<aquila::BookTicker>& records = load_result.value;
    if (options.mode == "spsc") {
      PrintResult(RunSpsc(records, options));
      return 0;
    }
    if (options.mode == "shm") {
      PrintResult(RunShm(records, options));
      return 0;
    }

    const TransportResult spsc = RunSpsc(records, options);
    const TransportResult shm = RunShm(records, options);
    PrintResult(spsc);
    PrintResult(shm);
    PrintComparison(spsc, shm);
  } catch (const std::exception& ex) {
    fmt::print(stderr, "benchmark failed: {}\n", ex.what());
    return 1;
  }

  return 0;
}
