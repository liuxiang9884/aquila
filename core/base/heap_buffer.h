#ifndef AQUILA_CORE_BASE_HEAP_BUFFER_H_
#define AQUILA_CORE_BASE_HEAP_BUFFER_H_

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace aquila {

template <typename T, typename Compare = std::less<T>>
class HeapBuffer {
 public:
  explicit HeapBuffer(Compare compare = Compare{})
      : compare_(std::move(compare)) {}

  void Reserve(std::size_t capacity) {
    values_.reserve(capacity);
  }

  void Clear() noexcept {
    values_.clear();
  }

  void Push(const T& value) {
    values_.push_back(value);
    std::push_heap(values_.begin(), values_.end(), compare_);
  }

  void Push(T&& value) {
    values_.push_back(std::move(value));
    std::push_heap(values_.begin(), values_.end(), compare_);
  }

  [[nodiscard]] T PopTop() {
    assert(!empty());
    std::pop_heap(values_.begin(), values_.end(), compare_);
    T value = std::move(values_.back());
    values_.pop_back();
    return value;
  }

  [[nodiscard]] const T& Top() const noexcept {
    assert(!empty());
    return values_.front();
  }

  [[nodiscard]] bool empty() const noexcept {
    return values_.empty();
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return values_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return values_.capacity();
  }

 private:
  std::vector<T> values_;
  Compare compare_;
};

}  // namespace aquila

#endif  // AQUILA_CORE_BASE_HEAP_BUFFER_H_
