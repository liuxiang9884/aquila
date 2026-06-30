#ifndef AQUILA_CORE_TRADING_ORDER_GATEWAY_SHM_H_
#define AQUILA_CORE_TRADING_ORDER_GATEWAY_SHM_H_

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/common/result.h"
#include "core/trading/order_gateway_shm_types.h"
#include <fcntl.h>

namespace aquila::core {

struct OrderGatewayShmConfig {
  std::string shm_name;
  bool create{true};
  bool remove_existing{false};
  std::uint16_t route_count{0};
  std::uint32_t command_queue_capacity{0};
  std::uint32_t event_queue_capacity{0};
  std::uint32_t startup_ready_timeout_s{30};
};

struct alignas(64) OrderGatewayQueueHeader {
  alignas(64) std::atomic<std::uint64_t> head{0};
  alignas(64) std::atomic<std::uint64_t> tail{0};
  std::uint32_t capacity{0};
  std::uint32_t slot_size{0};
  std::uint64_t reserved[6]{};
};

static_assert(std::is_standard_layout_v<OrderGatewayQueueHeader>);

namespace order_gateway_shm_detail {

inline constexpr std::size_t kCacheLineBytes = 64;

template <typename T>
[[nodiscard]] inline Result<T> Failure(std::string error) {
  Result<T> result;
  result.error = std::move(error);
  return result;
}

template <typename T>
[[nodiscard]] inline Result<T> Success(T value) {
  Result<T> result;
  result.value = std::move(value);
  result.ok = true;
  return result;
}

[[nodiscard]] inline std::string ErrnoMessage(std::string_view prefix) {
  const int saved_errno = errno;
  std::string message(prefix);
  message.append(": ");
  message.append(std::strerror(saved_errno));
  return message;
}

[[nodiscard]] inline std::size_t AlignUp(std::size_t value,
                                         std::size_t alignment) noexcept {
  return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] inline std::string NormalizeShmName(std::string_view shm_name) {
  if (shm_name.empty()) {
    return {};
  }
  if (shm_name.front() == '/') {
    return std::string(shm_name);
  }
  std::string normalized{"/"};
  normalized.append(shm_name);
  return normalized;
}

[[nodiscard]] inline const char* ValidateCreateConfig(
    const OrderGatewayShmConfig& config) noexcept {
  if (config.shm_name.empty()) {
    return "order_gateway_shm.shm_name is required";
  }
  if (!config.create && config.remove_existing) {
    return "order_gateway_shm.remove_existing requires create=true";
  }
  if (config.route_count == 0) {
    return "order_gateway_shm.route_count must be positive";
  }
  if (config.route_count > kMaxOrderGatewayRoutes) {
    return "order_gateway_shm.route_count exceeds max routes";
  }
  if (config.command_queue_capacity == 0) {
    return "order_gateway_shm.command_queue_capacity must be positive";
  }
  if (config.event_queue_capacity == 0) {
    return "order_gateway_shm.event_queue_capacity must be positive";
  }
  if (config.startup_ready_timeout_s == 0) {
    return "order_gateway_shm.startup_ready_timeout_s must be positive";
  }
  return nullptr;
}

[[nodiscard]] inline const char* ValidateOpenConfig(
    const OrderGatewayShmConfig& config) noexcept {
  if (config.shm_name.empty()) {
    return "order_gateway_shm.shm_name is required";
  }
  if (config.remove_existing) {
    return "order_gateway_shm.remove_existing requires create=true";
  }
  return nullptr;
}

[[nodiscard]] inline bool HasExpectedLayout(
    const OrderGatewayShmConfig& config) noexcept {
  return config.route_count != 0 || config.command_queue_capacity != 0 ||
         config.event_queue_capacity != 0;
}

[[nodiscard]] inline std::size_t QueueStorageBytes(
    std::uint32_t capacity, std::uint32_t slot_size) noexcept {
  const std::size_t header_bytes =
      AlignUp(sizeof(OrderGatewayQueueHeader), kCacheLineBytes);
  const std::size_t slot_bytes =
      static_cast<std::size_t>(capacity) * static_cast<std::size_t>(slot_size);
  return AlignUp(header_bytes + slot_bytes, kCacheLineBytes);
}

[[nodiscard]] inline std::size_t StorageSize(
    std::uint16_t route_count, std::uint32_t command_queue_capacity,
    std::uint32_t event_queue_capacity) noexcept {
  std::size_t size = AlignUp(sizeof(OrderGatewayShmHeader), kCacheLineBytes);
  for (std::uint16_t route = 0; route < route_count; ++route) {
    size = AlignUp(size, kCacheLineBytes);
    size +=
        QueueStorageBytes(command_queue_capacity, sizeof(OrderGatewayCommand));
    size = AlignUp(size, kCacheLineBytes);
    size += QueueStorageBytes(event_queue_capacity, sizeof(OrderGatewayEvent));
  }
  return AlignUp(size, kCacheLineBytes);
}

inline void InitializeQueue(void* queue_memory, std::uint32_t capacity,
                            std::uint32_t slot_size) noexcept {
  auto* header = new (queue_memory) OrderGatewayQueueHeader{};
  header->head.store(0, std::memory_order_relaxed);
  header->tail.store(0, std::memory_order_relaxed);
  header->capacity = capacity;
  header->slot_size = slot_size;
}

inline void InitializeLayout(void* mapping, std::size_t mapping_size,
                             const OrderGatewayShmConfig& config) noexcept {
  std::memset(mapping, 0, mapping_size);

  auto* header = new (mapping) OrderGatewayShmHeader{};
  header->route_count = config.route_count;
  header->command_queue_capacity = config.command_queue_capacity;
  header->event_queue_capacity = config.event_queue_capacity;
  header->startup_ready_timeout_s = config.startup_ready_timeout_s;

  auto* base = static_cast<std::byte*>(mapping);
  std::size_t offset = AlignUp(sizeof(OrderGatewayShmHeader), kCacheLineBytes);
  for (std::uint16_t route = 0; route < config.route_count; ++route) {
    offset = AlignUp(offset, kCacheLineBytes);
    const std::size_t command_bytes = QueueStorageBytes(
        config.command_queue_capacity, sizeof(OrderGatewayCommand));
    header->command_queue_descriptors[route] = OrderGatewayQueueDescriptor{
        .offset = static_cast<std::uint64_t>(offset),
        .bytes = static_cast<std::uint64_t>(command_bytes),
        .capacity = config.command_queue_capacity,
        .slot_size = sizeof(OrderGatewayCommand),
    };
    InitializeQueue(base + offset, config.command_queue_capacity,
                    sizeof(OrderGatewayCommand));
    offset += command_bytes;

    offset = AlignUp(offset, kCacheLineBytes);
    const std::size_t event_bytes = QueueStorageBytes(
        config.event_queue_capacity, sizeof(OrderGatewayEvent));
    header->event_queue_descriptors[route] = OrderGatewayQueueDescriptor{
        .offset = static_cast<std::uint64_t>(offset),
        .bytes = static_cast<std::uint64_t>(event_bytes),
        .capacity = config.event_queue_capacity,
        .slot_size = sizeof(OrderGatewayEvent),
    };
    InitializeQueue(base + offset, config.event_queue_capacity,
                    sizeof(OrderGatewayEvent));
    offset += event_bytes;
  }
}

[[nodiscard]] inline bool DescriptorInRange(
    const OrderGatewayQueueDescriptor& descriptor,
    std::size_t mapping_size) noexcept {
  if (descriptor.offset > mapping_size || descriptor.bytes > mapping_size) {
    return false;
  }
  return descriptor.offset <= mapping_size - descriptor.bytes;
}

[[nodiscard]] inline const char* ValidateHeader(
    const OrderGatewayShmHeader& header, std::size_t mapping_size,
    const OrderGatewayShmConfig* expected_config) noexcept {
  if (header.magic != kOrderGatewayShmMagic) {
    return "order_gateway_shm.magic mismatch";
  }
  if (header.version != kOrderGatewayShmVersion) {
    return "order_gateway_shm.version mismatch";
  }
  if (header.header_size != sizeof(OrderGatewayShmHeader)) {
    return "order_gateway_shm.header_size mismatch";
  }
  if (header.route_count == 0 || header.route_count > kMaxOrderGatewayRoutes) {
    return "order_gateway_shm.route_count mismatch";
  }
  if (header.command_queue_capacity == 0 || header.event_queue_capacity == 0) {
    return "order_gateway_shm.queue_capacity mismatch";
  }
  if (header.startup_ready_timeout_s == 0) {
    return "order_gateway_shm.startup_ready_timeout_s mismatch";
  }

  if (expected_config != nullptr) {
    if (expected_config->route_count != 0 &&
        expected_config->route_count != header.route_count) {
      return "order_gateway_shm.expected route_count mismatch";
    }
    if (expected_config->command_queue_capacity != 0 &&
        expected_config->command_queue_capacity !=
            header.command_queue_capacity) {
      return "order_gateway_shm.expected command_queue_capacity mismatch";
    }
    if (expected_config->event_queue_capacity != 0 &&
        expected_config->event_queue_capacity != header.event_queue_capacity) {
      return "order_gateway_shm.expected event_queue_capacity mismatch";
    }
    if (expected_config->startup_ready_timeout_s != 0 &&
        expected_config->startup_ready_timeout_s !=
            header.startup_ready_timeout_s) {
      return "order_gateway_shm.expected startup_ready_timeout_s mismatch";
    }
  }

  std::size_t expected_offset =
      AlignUp(sizeof(OrderGatewayShmHeader), kCacheLineBytes);
  for (std::uint16_t route = 0; route < header.route_count; ++route) {
    expected_offset = AlignUp(expected_offset, kCacheLineBytes);
    const std::size_t expected_command_bytes = QueueStorageBytes(
        header.command_queue_capacity, sizeof(OrderGatewayCommand));
    const OrderGatewayQueueDescriptor& command_descriptor =
        header.command_queue_descriptors[route];
    if (command_descriptor.capacity != header.command_queue_capacity ||
        command_descriptor.slot_size != sizeof(OrderGatewayCommand) ||
        command_descriptor.offset != expected_offset ||
        command_descriptor.bytes != expected_command_bytes ||
        !DescriptorInRange(command_descriptor, mapping_size)) {
      return "order_gateway_shm.command_queue descriptor mismatch";
    }
    expected_offset += expected_command_bytes;

    expected_offset = AlignUp(expected_offset, kCacheLineBytes);
    const std::size_t expected_event_bytes = QueueStorageBytes(
        header.event_queue_capacity, sizeof(OrderGatewayEvent));
    const OrderGatewayQueueDescriptor& event_descriptor =
        header.event_queue_descriptors[route];
    if (event_descriptor.capacity != header.event_queue_capacity ||
        event_descriptor.slot_size != sizeof(OrderGatewayEvent) ||
        event_descriptor.offset != expected_offset ||
        event_descriptor.bytes != expected_event_bytes ||
        !DescriptorInRange(event_descriptor, mapping_size)) {
      return "order_gateway_shm.event_queue descriptor mismatch";
    }
    expected_offset += expected_event_bytes;
  }
  if (AlignUp(expected_offset, kCacheLineBytes) != mapping_size) {
    return "order_gateway_shm.mapping_size mismatch";
  }
  return nullptr;
}

}  // namespace order_gateway_shm_detail

template <typename T>
class OrderGatewaySpscQueueView {
 public:
  OrderGatewaySpscQueueView() noexcept = default;

  OrderGatewaySpscQueueView(std::byte* base,
                            const OrderGatewayQueueDescriptor& descriptor)
      : header_(reinterpret_cast<OrderGatewayQueueHeader*>(base +
                                                           descriptor.offset)),
        slots_(base + descriptor.offset +
               order_gateway_shm_detail::AlignUp(
                   sizeof(OrderGatewayQueueHeader),
                   order_gateway_shm_detail::kCacheLineBytes)),
        capacity_(descriptor.capacity) {}

  [[nodiscard]] bool TryPush(const T& value) noexcept {
    if (header_ == nullptr || capacity_ == 0) {
      return false;
    }
    const std::uint64_t tail = header_->tail.load(std::memory_order_relaxed);
    const std::uint64_t head = header_->head.load(std::memory_order_acquire);
    if (tail - head >= capacity_) {
      return false;
    }
    std::memcpy(Slot(tail), &value, sizeof(T));
    header_->tail.store(tail + 1U, std::memory_order_release);
    return true;
  }

  [[nodiscard]] bool TryPop(T* out) noexcept {
    if (header_ == nullptr || out == nullptr || capacity_ == 0) {
      return false;
    }
    const std::uint64_t head = header_->head.load(std::memory_order_relaxed);
    const std::uint64_t tail = header_->tail.load(std::memory_order_acquire);
    if (head == tail) {
      return false;
    }
    std::memcpy(out, Slot(head), sizeof(T));
    header_->head.store(head + 1U, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::uint32_t capacity() const noexcept {
    return capacity_;
  }

  [[nodiscard]] std::uint64_t size() const noexcept {
    if (header_ == nullptr) {
      return 0;
    }
    const std::uint64_t head = header_->head.load(std::memory_order_acquire);
    const std::uint64_t tail = header_->tail.load(std::memory_order_acquire);
    return tail - head;
  }

 private:
  [[nodiscard]] std::byte* Slot(std::uint64_t sequence) const noexcept {
    return slots_ + (sequence % capacity_) * sizeof(T);
  }

  OrderGatewayQueueHeader* header_{nullptr};
  std::byte* slots_{nullptr};
  std::uint32_t capacity_{0};
};

using OrderGatewayCommandQueue = OrderGatewaySpscQueueView<OrderGatewayCommand>;
using OrderGatewayEventQueue = OrderGatewaySpscQueueView<OrderGatewayEvent>;

static_assert(std::is_trivially_copyable_v<OrderGatewayCommand>);
static_assert(std::is_trivially_copyable_v<OrderGatewayEvent>);

class OrderGatewayShmManager {
 public:
  OrderGatewayShmManager() noexcept = default;

  ~OrderGatewayShmManager() {
    Reset();
  }

  OrderGatewayShmManager(OrderGatewayShmManager&& other) noexcept {
    MoveFrom(std::move(other));
  }

  OrderGatewayShmManager& operator=(OrderGatewayShmManager&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(std::move(other));
    }
    return *this;
  }

  OrderGatewayShmManager(const OrderGatewayShmManager&) = delete;
  OrderGatewayShmManager& operator=(const OrderGatewayShmManager&) = delete;

  [[nodiscard]] static Result<OrderGatewayShmManager> Create(
      const OrderGatewayShmConfig& config) {
    if (const char* error =
            order_gateway_shm_detail::ValidateCreateConfig(config)) {
      return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(error);
    }
    return Build(config, true);
  }

  [[nodiscard]] static Result<OrderGatewayShmManager> Open(
      const OrderGatewayShmConfig& config) {
    if (const char* error =
            order_gateway_shm_detail::ValidateOpenConfig(config)) {
      return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(error);
    }
    return Build(config, false);
  }

  [[nodiscard]] OrderGatewayShmHeader& header() noexcept {
    return *header_;
  }

  [[nodiscard]] const OrderGatewayShmHeader& header() const noexcept {
    return *header_;
  }

  [[nodiscard]] OrderGatewayCommandQueue CommandQueue(
      std::uint16_t route_id) noexcept {
    if (header_ == nullptr || route_id >= header_->route_count) {
      return {};
    }
    return OrderGatewayCommandQueue(
        static_cast<std::byte*>(mapping_),
        header_->command_queue_descriptors[route_id]);
  }

  [[nodiscard]] OrderGatewayEventQueue EventQueue(
      std::uint16_t route_id) noexcept {
    if (header_ == nullptr || route_id >= header_->route_count) {
      return {};
    }
    return OrderGatewayEventQueue(static_cast<std::byte*>(mapping_),
                                  header_->event_queue_descriptors[route_id]);
  }

  [[nodiscard]] std::size_t mapping_size() const noexcept {
    return mapping_size_;
  }

 private:
  static Result<OrderGatewayShmManager> Build(
      const OrderGatewayShmConfig& config, bool create) {
    OrderGatewayShmManager manager;
    manager.shm_name_ =
        order_gateway_shm_detail::NormalizeShmName(config.shm_name);
    if (config.remove_existing) {
      ::shm_unlink(manager.shm_name_.c_str());
    }

    const int flags = create ? (O_RDWR | O_CREAT | O_EXCL) : O_RDWR;
    manager.fd_ = ::shm_open(manager.shm_name_.c_str(), flags, 0600);
    if (manager.fd_ == -1) {
      return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(
          order_gateway_shm_detail::ErrnoMessage("order_gateway_shm.shm_open"));
    }

    if (create) {
      manager.mapping_size_ = order_gateway_shm_detail::StorageSize(
          config.route_count, config.command_queue_capacity,
          config.event_queue_capacity);
      if (::ftruncate(manager.fd_, static_cast<off_t>(manager.mapping_size_)) ==
          -1) {
        const std::string error = order_gateway_shm_detail::ErrnoMessage(
            "order_gateway_shm.ftruncate");
        manager.Reset();
        ::shm_unlink(manager.shm_name_.c_str());
        return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(error);
      }
    } else {
      struct stat stat_buffer {};
      if (::fstat(manager.fd_, &stat_buffer) == -1 ||
          stat_buffer.st_size <= 0) {
        const std::string error =
            order_gateway_shm_detail::ErrnoMessage("order_gateway_shm.fstat");
        manager.Reset();
        return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(error);
      }
      manager.mapping_size_ = static_cast<std::size_t>(stat_buffer.st_size);
    }

    manager.mapping_ =
        ::mmap(nullptr, manager.mapping_size_, PROT_READ | PROT_WRITE,
               MAP_SHARED, manager.fd_, 0);
    if (manager.mapping_ == MAP_FAILED) {
      const std::string error =
          order_gateway_shm_detail::ErrnoMessage("order_gateway_shm.mmap");
      manager.mapping_ = nullptr;
      manager.Reset();
      if (create) {
        ::shm_unlink(manager.shm_name_.c_str());
      }
      return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(error);
    }

    if (manager.mapping_size_ < sizeof(OrderGatewayShmHeader)) {
      manager.Reset();
      if (create) {
        ::shm_unlink(manager.shm_name_.c_str());
      }
      return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(
          "order_gateway_shm.mapping_size is too small");
    }

    manager.header_ =
        reinterpret_cast<OrderGatewayShmHeader*>(manager.mapping_);
    if (create) {
      order_gateway_shm_detail::InitializeLayout(manager.mapping_,
                                                 manager.mapping_size_, config);
    }

    const OrderGatewayShmConfig* expected_config =
        (!create && order_gateway_shm_detail::HasExpectedLayout(config))
            ? &config
            : nullptr;
    if (const char* error = order_gateway_shm_detail::ValidateHeader(
            *manager.header_, manager.mapping_size_, expected_config)) {
      manager.Reset();
      if (create) {
        ::shm_unlink(manager.shm_name_.c_str());
      }
      return order_gateway_shm_detail::Failure<OrderGatewayShmManager>(error);
    }

    return order_gateway_shm_detail::Success<OrderGatewayShmManager>(
        std::move(manager));
  }

  void Reset() noexcept {
    if (mapping_ != nullptr) {
      ::munmap(mapping_, mapping_size_);
      mapping_ = nullptr;
    }
    if (fd_ != -1) {
      ::close(fd_);
      fd_ = -1;
    }
    header_ = nullptr;
    mapping_size_ = 0;
  }

  void MoveFrom(OrderGatewayShmManager&& other) noexcept {
    shm_name_ = std::move(other.shm_name_);
    fd_ = other.fd_;
    mapping_ = other.mapping_;
    mapping_size_ = other.mapping_size_;
    header_ = other.header_;
    other.fd_ = -1;
    other.mapping_ = nullptr;
    other.mapping_size_ = 0;
    other.header_ = nullptr;
  }

  std::string shm_name_;
  int fd_{-1};
  void* mapping_{nullptr};
  std::size_t mapping_size_{0};
  OrderGatewayShmHeader* header_{nullptr};
};

}  // namespace aquila::core

#endif  // AQUILA_CORE_TRADING_ORDER_GATEWAY_SHM_H_
