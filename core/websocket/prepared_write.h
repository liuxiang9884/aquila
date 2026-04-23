#ifndef AQUILA_CORE_WEBSOCKET_PREPARED_WRITE_H_
#define AQUILA_CORE_WEBSOCKET_PREPARED_WRITE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/websocket/types.h"

namespace aquila::websocket {

struct PreparedWrite {
  std::uint32_t slot_index{0};
  std::uint32_t encoded_size{0};
  std::uint32_t write_offset{0};
  PayloadKind kind{PayloadKind::kBinary};
  std::span<std::byte> storage{};
};

class PreparedWriteArena {
 public:
  PreparedWriteArena(size_t slot_count, size_t bytes_per_slot)
      : slot_count_(slot_count),
        bytes_per_slot_(bytes_per_slot),
        slots_(slot_count_ == 0 ? nullptr
                                : std::make_unique<PreparedWrite[]>(slot_count_)),
        storage_(slot_count_ == 0 || bytes_per_slot_ == 0
                     ? nullptr
                     : std::make_unique<std::byte[]>(slot_count_ *
                                                     bytes_per_slot_)),
        free_list_(slot_count_ == 0 ? nullptr
                                    : std::make_unique<std::uint32_t[]>(
                                          slot_count_)),
        in_use_(slot_count_ == 0 ? nullptr
                                 : std::make_unique<bool[]>(slot_count_)),
        free_count_(slot_count_) {
    for (size_t i = 0; i < slot_count_; ++i) {
      slots_[i].slot_index = static_cast<std::uint32_t>(i);
      slots_[i].storage = std::span<std::byte>(
          storage_.get() == nullptr ? nullptr : storage_.get() + i * bytes_per_slot_,
          bytes_per_slot_);
      free_list_[i] = static_cast<std::uint32_t>(slot_count_ - 1 - i);
      in_use_[i] = false;
    }
  }

  PreparedWrite* TryAcquire() noexcept {
    if (free_count_ == 0) {
      return nullptr;
    }

    const std::uint32_t slot_index = free_list_[--free_count_];
    PreparedWrite& write = slots_[slot_index];
    write.encoded_size = 0;
    write.write_offset = 0;
    write.kind = PayloadKind::kBinary;
    in_use_[slot_index] = true;
    return &write;
  }

  void Release(PreparedWrite* write) noexcept {
    if (write == nullptr || slots_ == nullptr) {
      return;
    }
    if (write < slots_.get() || write >= slots_.get() + slot_count_) {
      return;
    }

    const size_t slot_index = static_cast<size_t>(write - slots_.get());
    if (!in_use_[slot_index]) {
      return;
    }

    write->encoded_size = 0;
    write->write_offset = 0;
    write->kind = PayloadKind::kBinary;
    in_use_[slot_index] = false;
    free_list_[free_count_++] = static_cast<std::uint32_t>(slot_index);
  }

 private:
  size_t slot_count_{0};
  size_t bytes_per_slot_{0};
  std::unique_ptr<PreparedWrite[]> slots_;
  std::unique_ptr<std::byte[]> storage_;
  std::unique_ptr<std::uint32_t[]> free_list_;
  std::unique_ptr<bool[]> in_use_;
  size_t free_count_{0};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_PREPARED_WRITE_H_
