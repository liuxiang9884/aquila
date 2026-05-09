#ifndef AQUILA_CORE_BASE_MONOTONIC_DEQUE_H_
#define AQUILA_CORE_BASE_MONOTONIC_DEQUE_H_

#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace aquila {

template <typename T, typename Compare = std::less<T>>
class MonotonicDeque {
 public:
  explicit MonotonicDeque(Compare compare = Compare{})
      : compare_(std::move(compare)) {}

  void Reserve(std::size_t capacity) {
    storage_.reserve(capacity);
  }

  void Clear() noexcept {
    storage_.clear();
    front_index_ = 0;
  }

  void Push(const T& value) {
    while (!empty() && compare_(value, Back())) {
      storage_.pop_back();
    }
    CompactIfFullWithFrontSlack();
    storage_.push_back(value);
  }

  void Push(T&& value) {
    while (!empty() && compare_(value, Back())) {
      storage_.pop_back();
    }
    CompactIfFullWithFrontSlack();
    storage_.push_back(std::move(value));
  }

  void PopFront() noexcept {
    assert(!empty());
    ++front_index_;
    if (front_index_ == storage_.size()) {
      Clear();
    }
  }

  [[nodiscard]] const T& Front() const noexcept {
    assert(!empty());
    return storage_[front_index_];
  }

  [[nodiscard]] const T& Back() const noexcept {
    assert(!empty());
    return storage_.back();
  }

  [[nodiscard]] bool empty() const noexcept {
    return front_index_ == storage_.size();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return storage_.size() - front_index_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return storage_.capacity();
  }

 private:
  void CompactIfFullWithFrontSlack() {
    if (front_index_ == 0 || storage_.size() != storage_.capacity()) {
      return;
    }
    storage_.erase(
        storage_.begin(),
        storage_.begin() + static_cast<std::ptrdiff_t>(front_index_));
    front_index_ = 0;
  }

  std::vector<T> storage_;
  std::size_t front_index_{0};
  Compare compare_;
};

}  // namespace aquila

#endif  // AQUILA_CORE_BASE_MONOTONIC_DEQUE_H_
