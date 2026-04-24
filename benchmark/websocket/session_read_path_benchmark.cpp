#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/critical_session.h"
#include "core/websocket/metrics.h"
#include "core/websocket/prepared_write.h"
#include "core/websocket/types.h"

#include <cstdint>
#include <utility>
#include <vector>

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

}  // namespace

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
  ReadContext read_context{};
  MessageConsumer consumer{&read_context, &RecordRead};
  CriticalSession<LocalFdSocket> session(config, pair.client, arena, metrics);
  session.SetConsumer(consumer);

  const auto frame = BuildServerTextFrame("market-data");
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(kSamples);

  for (size_t sample_index = 0; sample_index < kSamples; ++sample_index) {
    const std::uint64_t previous_messages = read_context.messages;
    if (!WriteAllFd(pair.peer_fd, frame)) {
      return 1;
    }

    const std::uint64_t start_ns = NowNs();
    while (read_context.messages == previous_messages) {
      session.DriveRead();
      if (session.ShouldReconnect()) {
        return 1;
      }
    }
    const std::uint64_t stop_ns = NowNs();
    samples_ns.push_back(stop_ns - start_ns);
  }

  PrintReport("session_read_path", std::move(samples_ns), false,
              "local-socketpair", "rx_messages", metrics.rx_messages);
  return 0;
}
