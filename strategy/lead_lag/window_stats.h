#ifndef AQUILA_STRATEGY_LEAD_LAG_WINDOW_STATS_H_
#define AQUILA_STRATEGY_LEAD_LAG_WINDOW_STATS_H_

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "core/base/ring_queue.h"

namespace aquila::strategy::leadlag {

struct TimedValue {
  std::int64_t local_ns{0};
  double value{0.0};
};

class MeanWindow {
 public:
  void Init(std::uint64_t window_ns, std::size_t capacity) {
    window_ns_ = window_ns;
    samples_.Init(capacity);
    sum_ = 0.0;
  }

  void Clear() noexcept {
    samples_.Clear();
    sum_ = 0.0;
  }

  void Update(std::int64_t local_ns, double value) {
    EvictExpired(local_ns);
    samples_.PushBack(TimedValue{
        .local_ns = local_ns,
        .value = value,
    });
    sum_ += value;
  }

  [[nodiscard]] double mean() const noexcept {
    if (samples_.empty()) {
      return 0.0;
    }
    return sum_ / static_cast<double>(samples_.size());
  }

  [[nodiscard]] bool empty() const noexcept {
    return samples_.empty();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return samples_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return samples_.capacity();
  }

 private:
  void EvictExpired(std::int64_t now_ns) {
    while (!samples_.empty()) {
      const TimedValue& oldest = samples_.Front();
      if (now_ns < oldest.local_ns ||
          static_cast<std::uint64_t>(now_ns - oldest.local_ns) <= window_ns_) {
        break;
      }
      const TimedValue popped = samples_.PopFront();
      sum_ -= popped.value;
    }
  }

  RingQueue<TimedValue> samples_;
  std::uint64_t window_ns_{0};
  double sum_{0.0};
};

class MeanStdWindow {
 public:
  void Init(std::uint64_t window_ns, std::size_t capacity) {
    window_ns_ = window_ns;
    samples_.Init(capacity);
    sum_ = 0.0;
    sum_sq_ = 0.0;
  }

  void Clear() noexcept {
    samples_.Clear();
    sum_ = 0.0;
    sum_sq_ = 0.0;
  }

  void Update(std::int64_t local_ns, double value) {
    EvictExpired(local_ns);
    samples_.PushBack(TimedValue{
        .local_ns = local_ns,
        .value = value,
    });
    sum_ += value;
    sum_sq_ += value * value;
  }

  [[nodiscard]] double mean() const noexcept {
    if (samples_.empty()) {
      return 0.0;
    }
    return sum_ / static_cast<double>(samples_.size());
  }

  [[nodiscard]] double stddev() const noexcept {
    if (samples_.empty()) {
      return 0.0;
    }
    const double count = static_cast<double>(samples_.size());
    const double current_mean = sum_ / count;
    const double variance = (sum_sq_ / count) - current_mean * current_mean;
    return variance > 0.0 ? std::sqrt(variance) : 0.0;
  }

  [[nodiscard]] bool empty() const noexcept {
    return samples_.empty();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return samples_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return samples_.capacity();
  }

 private:
  void EvictExpired(std::int64_t now_ns) {
    while (!samples_.empty()) {
      const TimedValue& oldest = samples_.Front();
      if (now_ns < oldest.local_ns ||
          static_cast<std::uint64_t>(now_ns - oldest.local_ns) <= window_ns_) {
        break;
      }
      const TimedValue popped = samples_.PopFront();
      sum_ -= popped.value;
      sum_sq_ -= popped.value * popped.value;
    }
  }

  RingQueue<TimedValue> samples_;
  std::uint64_t window_ns_{0};
  double sum_{0.0};
  double sum_sq_{0.0};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_WINDOW_STATS_H_
