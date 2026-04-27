#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

namespace {

struct MixedContext {
  std::uint64_t messages{0};
  size_t bytes{0};
};

DeliveryResult RecordMixedRead(void* context,
                               const MessageView& view) noexcept {
  auto* state = static_cast<MixedContext*>(context);
  ++state->messages;
  state->bytes += view.payload.size();
  return DeliveryResult::kAccepted;
}

class MixedReadWriteSocket {
 public:
  void ResetRead(std::span<const std::byte> frame) noexcept {
    read_frame_ = frame;
    read_delivered_ = false;
  }

  ssize_t ReadSome(std::span<std::byte> buffer) noexcept {
    if (read_delivered_) {
      errno = EAGAIN;
      return -1;
    }
    read_delivered_ = true;
    const size_t bytes = std::min(buffer.size(), read_frame_.size());
    std::copy_n(read_frame_.begin(), bytes, buffer.begin());
    return static_cast<ssize_t>(bytes);
  }

  size_t PendingReadableBytes() const noexcept { return 0; }

  ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
    ++write_calls_;
    bytes_written_ += buffer.size();
    return static_cast<ssize_t>(buffer.size());
  }

  std::uint64_t write_calls() const noexcept { return write_calls_; }
  size_t bytes_written() const noexcept { return bytes_written_; }

 private:
  std::span<const std::byte> read_frame_{};
  bool read_delivered_{true};
  std::uint64_t write_calls_{0};
  size_t bytes_written_{0};
};

bool FillBusinessQueue(CriticalSession<MixedReadWriteSocket>& session,
                       std::span<const std::byte> payload,
                       size_t slots) {
  for (size_t i = 0; i < slots; ++i) {
    PreparedWrite* write = session.TryAcquirePreparedWrite();
    if (write == nullptr) {
      return false;
    }
    std::copy(payload.begin(), payload.end(), write->storage.begin());
    write->encoded_size = static_cast<std::uint32_t>(payload.size());
    write->kind = PayloadKind::kBinary;
    if (session.CommitPreparedWrite(write) != SendStatus::kOk) {
      session.CancelPreparedWrite(write);
      return false;
    }
  }
  return true;
}

void DrainPendingWrites(CriticalSession<MixedReadWriteSocket>& session) {
  while (session.WantsWrite() && !session.ShouldReconnect()) {
    session.DriveWrite();
  }
}

void BenchmarkSessionMixedWriteBeforeRead(benchmark::State& state,
                                          size_t queued_writes,
                                          std::uint32_t write_budget) {
  ConnectionConfig config{};
  config.enable_tls = false;
  config.prepared_write_slots = std::max<size_t>(queued_writes, 1);
  config.prepared_write_bytes = 128;
  config.frame_buffer_bytes = 4096;
  config.max_business_writes_per_drive = write_budget;
  config.heartbeat_interval_ms = std::numeric_limits<std::uint32_t>::max();
  config.heartbeat_timeout_ms = std::numeric_limits<std::uint32_t>::max();

  MixedReadWriteSocket socket;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  MixedContext context{};
  MessageConsumer consumer{&context, &RecordMixedRead};
  CriticalSession<MixedReadWriteSocket> session(config, socket, arena,
                                                metrics);
  session.SetConsumer(consumer);

  const auto frame = BuildServerTextFrame("market-data");
  const auto payload = BuildWritePayload();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(4096);
  std::uint64_t measured_write_calls = 0;

  for (auto _ : state) {
    state.PauseTiming();
    DrainPendingWrites(session);
    socket.ResetRead(frame);
    const std::uint64_t previous_messages = context.messages;
    if (!FillBusinessQueue(session, payload, queued_writes)) {
      state.SkipWithError("fill business queue failed");
      return;
    }
    state.ResumeTiming();

    const std::uint64_t write_calls_before = socket.write_calls();
    const std::uint64_t start_ns = NowNs();
    session.DriveWrite();
    session.DriveRead();
    const std::uint64_t elapsed_ns = NowNs() - start_ns;
    measured_write_calls += socket.write_calls() - write_calls_before;
    if (session.ShouldReconnect()) {
      state.SkipWithError("session requested reconnect");
      return;
    }
    if (context.messages != previous_messages + 1U) {
      state.SkipWithError("read message was not delivered");
      return;
    }
    state.SetIterationTime(static_cast<double>(elapsed_ns) / 1'000'000'000.0);
    samples_ns.push_back(elapsed_ns);
  }

  SetLatencyCounters(state, std::move(samples_ns), "rx_messages",
                     metrics.rx_messages);
  state.counters["queued_writes"] =
      static_cast<double>(queued_writes);
  state.counters["write_budget"] =
      static_cast<double>(write_budget);
  state.counters["measured_write_calls"] =
      static_cast<double>(measured_write_calls);
  state.SetLabel(BuildBenchmarkLabel(false, "mixed-fake-socket",
                                     FormatAffinity(),
                                     FormatSchedulingPolicy()));
}

BENCHMARK_CAPTURE(BenchmarkSessionMixedWriteBeforeRead, writes_0_unbounded, 0U,
                  0U)
    ->Name("session_mixed_write_before_read_writes_0")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkSessionMixedWriteBeforeRead, writes_1_unbounded, 1U,
                  0U)
    ->Name("session_mixed_write_before_read_writes_1_unbounded")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkSessionMixedWriteBeforeRead, writes_8_unbounded, 8U,
                  0U)
    ->Name("session_mixed_write_before_read_writes_8_unbounded")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkSessionMixedWriteBeforeRead, writes_64_unbounded,
                  64U, 0U)
    ->Name("session_mixed_write_before_read_writes_64_unbounded")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkSessionMixedWriteBeforeRead, writes_8_budget_1, 8U,
                  1U)
    ->Name("session_mixed_write_before_read_writes_8_budget_1")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

BENCHMARK_CAPTURE(BenchmarkSessionMixedWriteBeforeRead, writes_64_budget_1,
                  64U, 1U)
    ->Name("session_mixed_write_before_read_writes_64_budget_1")
    ->Iterations(4096)
    ->UseManualTime()
    ->Unit(benchmark::kNanosecond);

}  // namespace
