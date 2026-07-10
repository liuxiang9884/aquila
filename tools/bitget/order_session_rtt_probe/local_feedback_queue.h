#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_LOCAL_FEEDBACK_QUEUE_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_LOCAL_FEEDBACK_QUEUE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/trading/order_feedback_event.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

class LocalFeedbackQueue {
 public:
  explicit LocalFeedbackQueue(std::size_t capacity) : events_(capacity) {}

  LocalFeedbackQueue(const LocalFeedbackQueue&) = delete;
  LocalFeedbackQueue& operator=(const LocalFeedbackQueue&) = delete;

  [[nodiscard]] bool TryPush(const OrderFeedbackEvent& event) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (size_ == events_.size()) {
      dropped_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    events_[write_index_] = event;
    write_index_ = (write_index_ + 1) % events_.size();
    ++size_;
    return true;
  }

  template <typename Handler>
  std::size_t Poll(std::size_t max_events, Handler&& handler) noexcept {
    std::size_t consumed = 0;
    while (consumed < max_events) {
      OrderFeedbackEvent event{};
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) {
          break;
        }
        event = events_[read_index_];
        read_index_ = (read_index_ + 1) % events_.size();
        --size_;
      }
      handler(event);
      ++consumed;
    }
    return consumed;
  }

  [[nodiscard]] std::uint64_t dropped_count() const noexcept {
    return dropped_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return events_.size();
  }

 private:
  std::vector<OrderFeedbackEvent> events_;
  mutable std::mutex mutex_;
  std::size_t read_index_{0};
  std::size_t write_index_{0};
  std::size_t size_{0};
  std::atomic<std::uint64_t> dropped_count_{0};
};

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_LOCAL_FEEDBACK_QUEUE_H_
