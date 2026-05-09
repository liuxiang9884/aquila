#ifndef AQUILA_CORE_BASE_RING_QUEUE_H_
#define AQUILA_CORE_BASE_RING_QUEUE_H_

#include <bit>
#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

namespace aquila {

template <typename T>
class RingQueue {
 public:
  RingQueue() = default;

  explicit RingQueue(std::size_t capacity) {
    Init(capacity);
  }

  void Init(std::size_t capacity) {
    capacity_ = NormalizeCapacity(capacity);
    mask_ = capacity_ == 0 ? 0 : capacity_ - 1;
    head_ = 0;
    size_ = 0;
    storage_.clear();
    storage_.resize(capacity_);
  }

  void Clear() noexcept {
    head_ = 0;
    size_ = 0;
  }

  void PushBack(const T& value) {
    EnsureWritableSlot();
    storage_[TailIndex()] = value;
    ++size_;
  }

  void PushBack(T&& value) {
    EnsureWritableSlot();
    storage_[TailIndex()] = std::move(value);
    ++size_;
  }

  [[nodiscard]] T PopFront() {
    assert(!empty());
    T value = std::move(storage_[head_]);
    head_ = (head_ + 1) & mask_;
    --size_;
    if (size_ == 0) {
      head_ = 0;
    }
    return value;
  }

  [[nodiscard]] T& operator[](std::size_t offset) noexcept {
    assert(offset < size_);
    return storage_[(head_ + offset) & mask_];
  }

  [[nodiscard]] const T& operator[](std::size_t offset) const noexcept {
    assert(offset < size_);
    return storage_[(head_ + offset) & mask_];
  }

  [[nodiscard]] T& Front() noexcept {
    assert(!empty());
    return storage_[head_];
  }

  [[nodiscard]] const T& Front() const noexcept {
    assert(!empty());
    return storage_[head_];
  }

  [[nodiscard]] bool empty() const noexcept {
    return size_ == 0;
  }

  [[nodiscard]] bool full() const noexcept {
    return capacity_ != 0 && size_ == capacity_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return capacity_;
  }

 private:
  [[nodiscard]] static std::size_t NormalizeCapacity(std::size_t capacity) {
    if (capacity == 0 || std::has_single_bit(capacity)) {
      return capacity;
    }
    return std::bit_ceil(capacity);
  }

  void EnsureWritableSlot() {
    if (size_ != capacity_) {
      return;
    }
    Grow();
  }

  void Grow() {
    const std::size_t new_capacity = capacity_ == 0 ? 1 : capacity_ * 2;
    std::vector<T> next;
    next.resize(new_capacity);
    for (std::size_t i = 0; i < size_; ++i) {
      next[i] = std::move((*this)[i]);
    }
    storage_ = std::move(next);
    capacity_ = new_capacity;
    mask_ = capacity_ - 1;
    head_ = 0;
  }

  [[nodiscard]] std::size_t TailIndex() const noexcept {
    return (head_ + size_) & mask_;
  }

  std::vector<T> storage_;
  std::size_t capacity_{0};
  std::size_t mask_{0};
  std::size_t head_{0};
  std::size_t size_{0};
};

}  // namespace aquila

#endif  // AQUILA_CORE_BASE_RING_QUEUE_H_
