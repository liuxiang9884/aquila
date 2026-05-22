#ifndef AQUILA_MONITOR_MODEL_MONITOR_SPSC_QUEUE_H_
#define AQUILA_MONITOR_MODEL_MONITOR_SPSC_QUEUE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace aquila::monitor {

template <typename T, std::size_t Capacity>
class MonitorSpscQueue {
  static_assert(Capacity > 1);
  static_assert((Capacity & (Capacity - 1)) == 0);

 public:
  bool TryPush(const T& value) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    if (tail - head == Capacity) {
      dropped_push_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    buffer_[tail & kIndexMask] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool TryPop(T* out) noexcept {
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }

    *out = buffer_[head & kIndexMask];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t dropped_push_count() const noexcept {
    return dropped_push_count_.load(std::memory_order_relaxed);
  }

 private:
  static constexpr std::uint64_t kIndexMask = Capacity - 1;

  std::array<T, Capacity> buffer_{};
  alignas(64) std::atomic<std::uint64_t> head_{0};
  alignas(64) std::atomic<std::uint64_t> tail_{0};
  alignas(64) std::atomic<std::uint64_t> dropped_push_count_{0};
};

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_MODEL_MONITOR_SPSC_QUEUE_H_
