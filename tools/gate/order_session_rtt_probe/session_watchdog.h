#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_WATCHDOG_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_WATCHDOG_H_

#include <atomic>
#include <chrono>
#include <thread>

namespace aquila::tools::gate_order_session_rtt_probe {

struct SessionWatchdogResult {
  bool start_result{false};
  bool runner_stop_observed{false};
  bool duration_watchdog_fired{false};
};

template <typename SessionT, typename StopRequestedFn>
[[nodiscard]] SessionWatchdogResult RunSessionWithWatchdog(
    SessionT& session, StopRequestedFn stop_requested, double duration_sec,
    double watchdog_grace_sec,
    std::chrono::milliseconds poll_interval = std::chrono::milliseconds(10)) {
  std::atomic<bool> session_returned{false};
  bool start_result = false;
  std::thread session_thread([&] {
    start_result = session.Start();
    session_returned.store(true, std::memory_order_release);
  });

  SessionWatchdogResult result;
  const auto start = std::chrono::steady_clock::now();
  const bool has_deadline = duration_sec > 0.0;
  const auto deadline =
      start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(duration_sec) +
                  std::chrono::duration<double>(
                      watchdog_grace_sec > 0.0 ? watchdog_grace_sec : 0.0));

  while (!session_returned.load(std::memory_order_acquire)) {
    if (stop_requested()) {
      result.runner_stop_observed = true;
      session.Stop();
      break;
    }
    if (has_deadline && std::chrono::steady_clock::now() >= deadline) {
      result.duration_watchdog_fired = true;
      session.Stop();
      break;
    }
    std::this_thread::sleep_for(poll_interval);
  }

  session_thread.join();
  result.start_result = start_result;
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SESSION_WATCHDOG_H_
