#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LOCAL_FEEDBACK_QUEUE_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LOCAL_FEEDBACK_QUEUE_H_

#include <cstddef>
#include <deque>
#include <mutex>

#include "core/trading/order_feedback_event.h"

namespace aquila::tools::gate_order_session_rtt_probe {

class LocalFeedbackQueue {
 public:
  LocalFeedbackQueue() = default;
  ~LocalFeedbackQueue() = default;

  LocalFeedbackQueue(const LocalFeedbackQueue&) = delete;
  LocalFeedbackQueue& operator=(const LocalFeedbackQueue&) = delete;

  void Push(const OrderFeedbackEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
  }

  template <typename Handler>
  std::size_t Poll(std::size_t max_events, Handler&& handler) noexcept {
    if (max_events == 0) {
      return 0;
    }

    std::size_t consumed = 0;
    while (consumed < max_events) {
      OrderFeedbackEvent event{};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) {
          break;
        }
        event = events_.front();
        events_.pop_front();
      }
      handler(event);
      ++consumed;
    }
    return consumed;
  }

 private:
  std::mutex mutex_;
  std::deque<OrderFeedbackEvent> events_;
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_LOCAL_FEEDBACK_QUEUE_H_
