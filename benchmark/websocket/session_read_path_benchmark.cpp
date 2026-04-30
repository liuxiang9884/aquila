#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

struct ReadContext {
  std::uint64_t messages{0};
  size_t bytes{0};
};

DeliveryResult RecordRead(void* context, const MessageView& view) noexcept {
  auto* state = static_cast<ReadContext*>(context);
  ++state->messages;
  state->bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

class PendingChunkSocket {
 public:
  void Reset(std::span<const std::span<const std::byte>> chunks) noexcept {
    chunk_count_ = std::min(chunks.size(), chunks_.size());
    for (size_t i = 0; i < chunk_count_; ++i) {
      chunks_[i] = chunks[i];
    }
    cursor_ = 0;
  }

  ssize_t ReadSome(std::span<std::byte> buffer) noexcept {
    if (cursor_ >= chunk_count_) {
      errno = EAGAIN;
      return -1;
    }
    const auto chunk = chunks_[cursor_++];
    const size_t bytes = std::min(buffer.size(), chunk.size());
    std::copy_n(chunk.begin(), bytes, buffer.begin());
    return static_cast<ssize_t>(bytes);
  }

  size_t PendingReadableBytes() const noexcept {
    return cursor_ < chunk_count_ ? chunks_[cursor_].size() : 0;
  }

 private:
  std::array<std::span<const std::byte>, 8> chunks_{};
  size_t chunk_count_{0};
  size_t cursor_{0};
};

void BenchmarkSessionReadPath(benchmark::State& state) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();

  SocketPair pair;
  if (!CreateSocketPair(pair)) {
    state.SkipWithError("socketpair create failed");
    return;
  }

  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  ReadContext read_context{};
  MessageCallback consumer{&read_context, &RecordRead};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  session.SetMessageCallback(consumer);

  const auto frame = BuildServerTextFrame("market-data");
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    state.PauseTiming();
    const std::uint64_t previous_messages = read_context.messages;
    if (!WriteAllFd(pair.peer_fd, frame)) {
      state.SkipWithError("peer write failed");
      return;
    }
    state.ResumeTiming();

    const std::uint64_t start_ns = NowNs();
    while (read_context.messages == previous_messages) {
      session.DriveRead();
      if (session.ShouldReconnect()) {
        state.SkipWithError("session requested reconnect");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
  }

  SetLatencyCounters(state, std::move(samples_ns), "rx_messages",
                     metrics.rx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "local-socketpair",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK(BenchmarkSessionReadPath)
    ->Name("session_read_path")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

void BenchmarkSessionReadBurst(benchmark::State& state,
                               std::uint32_t max_reads_per_drive) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = 8;
  config.prepared_write_bytes = 128;
  config.read_buffer_bytes = 4096;
  config.frame_buffer_bytes = 4096;
  config.max_reads_per_drive = max_reads_per_drive;
  config.read_until_would_block = false;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();

  PendingChunkSocket socket;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  ReadContext read_context{};
  MessageCallback consumer{&read_context, &RecordRead};
  CriticalSession<PendingChunkSocket> session(config, socket, arena, metrics);
  session.SetMessageCallback(consumer);

  const auto first = BuildServerTextFrame("a");
  const auto second = BuildServerTextFrame("b");
  const auto third = BuildServerTextFrame("c");
  const auto fourth = BuildServerTextFrame("d");
  const std::array<std::span<const std::byte>, 4> chunks{
      std::span<const std::byte>(first),
      std::span<const std::byte>(second),
      std::span<const std::byte>(third),
      std::span<const std::byte>(fourth),
  };

  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);

  for (auto _ : state) {
    state.PauseTiming();
    const std::uint64_t target_messages = read_context.messages + chunks.size();
    socket.Reset(chunks);
    state.ResumeTiming();

    const std::uint64_t start_ns = NowNs();
    while (read_context.messages < target_messages) {
      session.DriveRead();
      if (session.ShouldReconnect()) {
        state.SkipWithError("session requested reconnect");
        return;
      }
    }
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
  }

  SetLatencyCounters(state, std::move(samples_ns), "rx_messages",
                     metrics.rx_messages);
  state.SetLabel(BuildBenchmarkLabel(false, "pending-chunk-socket",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK_CAPTURE(BenchmarkSessionReadBurst, single_read, 1U)
    ->Name("session_read_path_burst_single_read")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkSessionReadBurst, bounded_pump, 4U)
    ->Name("session_read_path_burst_bounded_pump")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
