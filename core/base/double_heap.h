#ifndef AQUILA_CORE_BASE_DOUBLE_HEAP_H_
#define AQUILA_CORE_BASE_DOUBLE_HEAP_H_

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <functional>

#include "core/base/heap_buffer.h"

namespace aquila {

template <typename T>
class DoubleHeap {
 public:
  DoubleHeap() = default;

  DoubleHeap(double quantile, std::size_t capacity) {
    Init(quantile, capacity);
  }

  void Init(double quantile, std::size_t capacity) {
    assert(quantile >= 0.0 && quantile <= 1.0);
    quantile_ = quantile;
    count_ = 0;
    lower_.Clear();
    upper_.Clear();
    lower_.Reserve(capacity);
    upper_.Reserve(capacity);
  }

  void Reset() noexcept {
    count_ = 0;
    lower_.Clear();
    upper_.Clear();
  }

  void Add(const T& value) {
    if (lower_.empty() || value <= lower_.Top()) {
      lower_.Push(value);
    } else {
      upper_.Push(value);
    }
    ++count_;
    Rebalance();
  }

  void Add(T&& value) {
    if (lower_.empty() || value <= lower_.Top()) {
      lower_.Push(std::move(value));
    } else {
      upper_.Push(std::move(value));
    }
    ++count_;
    Rebalance();
  }

  [[nodiscard]] bool HasValue() const noexcept {
    return count_ != 0;
  }

  [[nodiscard]] T Value() const {
    if (!HasValue()) {
      return T{};
    }
    return lower_.Top();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return count_;
  }

  [[nodiscard]] std::size_t lower_capacity() const noexcept {
    return lower_.capacity();
  }

  [[nodiscard]] std::size_t upper_capacity() const noexcept {
    return upper_.capacity();
  }

  [[nodiscard]] std::size_t lower_size() const noexcept {
    return lower_.size();
  }

  [[nodiscard]] std::size_t upper_size() const noexcept {
    return upper_.size();
  }

 private:
  [[nodiscard]] std::size_t TargetLowerSize() const noexcept {
    if (count_ == 0) {
      return 0;
    }
    std::size_t target =
        static_cast<std::size_t>(std::ceil(quantile_ * count_));
    target = std::max<std::size_t>(1, target);
    return std::min(target, count_);
  }

  void Rebalance() {
    const std::size_t target = TargetLowerSize();
    while (lower_.size() > target) {
      upper_.Push(lower_.PopTop());
    }
    while (lower_.size() < target) {
      assert(!upper_.empty());
      lower_.Push(upper_.PopTop());
    }
  }

  double quantile_{0.5};
  std::size_t count_{0};
  HeapBuffer<T, std::less<T>> lower_;
  HeapBuffer<T, std::greater<T>> upper_;
};

}  // namespace aquila

#endif  // AQUILA_CORE_BASE_DOUBLE_HEAP_H_
