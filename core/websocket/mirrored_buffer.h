#ifndef AQUILA_CORE_WEBSOCKET_MIRRORED_BUFFER_H_
#define AQUILA_CORE_WEBSOCKET_MIRRORED_BUFFER_H_

#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/memfd.h>
#endif

namespace aquila::websocket {

class MirroredBuffer {
 public:
  MirroredBuffer() noexcept = default;

  explicit MirroredBuffer(size_t requested_capacity) noexcept {
    Init(requested_capacity);
  }

  MirroredBuffer(const MirroredBuffer&) = delete;
  MirroredBuffer& operator=(const MirroredBuffer&) = delete;

  MirroredBuffer(MirroredBuffer&& other) noexcept { MoveFrom(other); }

  MirroredBuffer& operator=(MirroredBuffer&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }

  ~MirroredBuffer() noexcept { Reset(); }

  bool Init(size_t requested_capacity) noexcept {
    Reset();
    const size_t capacity = NormalizeCapacity(requested_capacity);
    if (capacity == 0 || capacity > MaxMappableCapacity()) {
      return false;
    }

#if defined(__linux__) && defined(SYS_memfd_create)
    void* reservation =
        ::mmap(nullptr, capacity * 2U, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) {
      return false;
    }

    const int fd = static_cast<int>(
        ::syscall(SYS_memfd_create, "aquila-ws-ring", MFD_CLOEXEC));
    if (fd < 0) {
      ::munmap(reservation, capacity * 2U);
      return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(capacity)) != 0) {
      const int saved_errno = errno;
      ::close(fd);
      ::munmap(reservation, capacity * 2U);
      errno = saved_errno;
      return false;
    }

    void* first =
        ::mmap(reservation, capacity, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED, fd, 0);
    if (first != reservation) {
      const int saved_errno = errno;
      ::close(fd);
      ::munmap(reservation, capacity * 2U);
      errno = saved_errno;
      return false;
    }

    auto* second_address = static_cast<std::byte*>(reservation) + capacity;
    void* second =
        ::mmap(second_address, capacity, PROT_READ | PROT_WRITE,
               MAP_SHARED | MAP_FIXED, fd, 0);
    const int saved_errno = errno;
    ::close(fd);
    if (second != second_address) {
      ::munmap(reservation, capacity * 2U);
      errno = saved_errno;
      return false;
    }

    base_ = static_cast<std::byte*>(reservation);
    capacity_ = capacity;
    mapped_bytes_ = capacity * 2U;
    return true;
#else
    (void)capacity;
    errno = ENOSYS;
    return false;
#endif
  }

  void Reset() noexcept {
    if (base_ != nullptr) {
      ::munmap(base_, mapped_bytes_);
      base_ = nullptr;
    }
    capacity_ = 0;
    mapped_bytes_ = 0;
  }

  std::byte* data() noexcept { return base_; }
  const std::byte* data() const noexcept { return base_; }
  size_t capacity() const noexcept { return capacity_; }
  bool valid() const noexcept { return base_ != nullptr && capacity_ != 0; }

  std::span<std::byte> span() noexcept {
    return std::span<std::byte>(base_, capacity_);
  }

 private:
  static size_t NormalizeCapacity(size_t requested_capacity) noexcept {
    const long raw_page_size = ::sysconf(_SC_PAGESIZE);
    const size_t page_size =
        raw_page_size > 0 ? static_cast<size_t>(raw_page_size) : 4096U;
    size_t capacity = requested_capacity < page_size ? page_size
                                                     : requested_capacity;
    if (capacity > (std::numeric_limits<size_t>::max() >> 1U)) {
      return 0;
    }
    if (!std::has_single_bit(capacity)) {
      capacity = std::bit_ceil(capacity);
    }
    if (capacity % page_size != 0) {
      const size_t pages = (capacity + page_size - 1U) / page_size;
      if (pages > std::numeric_limits<size_t>::max() / page_size) {
        return 0;
      }
      capacity = pages * page_size;
      if (!std::has_single_bit(capacity)) {
        capacity = std::bit_ceil(capacity);
      }
    }
    return capacity;
  }

  static constexpr size_t MaxMappableCapacity() noexcept {
    return std::numeric_limits<size_t>::max() / 2U;
  }

  void MoveFrom(MirroredBuffer& other) noexcept {
    base_ = other.base_;
    capacity_ = other.capacity_;
    mapped_bytes_ = other.mapped_bytes_;
    other.base_ = nullptr;
    other.capacity_ = 0;
    other.mapped_bytes_ = 0;
  }

  std::byte* base_{nullptr};
  size_t capacity_{0};
  size_t mapped_bytes_{0};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_MIRRORED_BUFFER_H_
