#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

using namespace aquila::websocket;
using namespace aquila::websocket::benchmarking;

int main() {
  constexpr size_t kSamples = 4096;
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
    return 1;
  }

  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  const auto payload = BuildWritePayload();
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kSamples);
  std::array<std::byte, 64> peer_drain{};

  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    PreparedWrite* write = session.TryAcquirePreparedWrite();
    if (write == nullptr) {
      return 1;
    }
    std::copy(payload.begin(), payload.end(), write->storage.begin());
    write->encoded_size = static_cast<std::uint32_t>(payload.size());
    write->kind = PayloadKind::kBinary;

    const std::uint64_t start_ns = NowNs();
    if (session.CommitPreparedWrite(write) != SendStatus::kOk) {
      return 1;
    }
    while (session.WantsWrite()) {
      session.DriveWrite();
      if (session.ShouldReconnect()) {
        return 1;
      }
    }
    const std::uint64_t stop_ns = NowNs();
    samples_ns.push_back(stop_ns - start_ns);

    if (!ReadExactFd(pair.peer_fd, peer_drain)) {
      return 1;
    }
  }

  PrintReport("session_write_path", std::move(samples_ns), false,
              "local-socketpair", "tx_messages", metrics.tx_messages);
  return 0;
}
