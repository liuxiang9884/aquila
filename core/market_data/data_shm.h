#ifndef AQUILA_CORE_MARKET_DATA_DATA_SHM_H_
#define AQUILA_CORE_MARKET_DATA_DATA_SHM_H_

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/market_data/data_shm_config.h"
#include "core/market_data/types.h"
#include "nova/concurrency/sp_broadcast_queue.h"
#include "nova/interprocess/shm_allocator.h"

namespace aquila::market_data {

inline constexpr std::uint32_t kBookTickerShmMagic = 0x41514C42U;
inline constexpr std::uint32_t kBookTickerShmVersion = 1;
inline constexpr std::size_t kBookTickerShmAllocatorInstances = 64;

using BookTickerQueue =
    nova::static_impl::SPBroadcastQueue<aquila::BookTicker,
                                        kBookTickerShmCapacity>;

struct BookTickerShmHeader {
  std::uint32_t magic{kBookTickerShmMagic};
  std::uint32_t version{kBookTickerShmVersion};
  std::uint32_t abi_size{sizeof(aquila::BookTicker)};
  std::uint32_t capacity{kBookTickerShmCapacity};
  std::uint64_t producer_pid{0};
  std::uint64_t created_ns{0};
  std::atomic<std::uint64_t> published_count{0};
  std::atomic<std::uint64_t> heartbeat_ns{0};
};

struct BookTickerShmChannel {
  BookTickerShmHeader header{};
  BookTickerQueue queue{};
};

static_assert(kBookTickerShmCapacity == 65536);
static_assert((kBookTickerShmCapacity & (kBookTickerShmCapacity - 1)) == 0);
static_assert(std::is_standard_layout_v<BookTickerShmChannel>);
static_assert(std::is_trivially_copyable_v<BookTickerShmChannel>);

namespace detail {

[[nodiscard]] inline std::uint64_t NowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
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

[[nodiscard]] inline std::string ValidateChannelName(
    std::string_view channel_name) {
  if (channel_name.empty()) {
    throw std::invalid_argument("book_ticker_shm.channel_name is required");
  }
  if (channel_name.size() >= nova::kShmNameSize) {
    throw std::invalid_argument("book_ticker_shm.channel_name is too long");
  }
  return std::string(channel_name);
}

[[nodiscard]] inline std::string PrepareShmName(
    const BookTickerShmConfig& config) {
  if (config.shm_name.empty()) {
    throw std::invalid_argument("book_ticker_shm.shm_name is required");
  }
  if (!config.create && config.remove_existing) {
    throw std::invalid_argument(
        "book_ticker_shm.remove_existing requires create=true");
  }

  std::string shm_name = NormalizeShmName(config.shm_name);
  if (config.remove_existing) {
    ::shm_unlink(shm_name.c_str());
  }
  return shm_name;
}

inline void ValidateChannelHeader(const BookTickerShmChannel& channel) {
  if (channel.header.magic != kBookTickerShmMagic) {
    throw std::runtime_error("book_ticker_shm.magic mismatch");
  }
  if (channel.header.version != kBookTickerShmVersion) {
    throw std::runtime_error("book_ticker_shm.version mismatch");
  }
  if (channel.header.abi_size != sizeof(aquila::BookTicker)) {
    throw std::runtime_error("book_ticker_shm.abi_size mismatch");
  }
  if (channel.header.capacity != kBookTickerShmCapacity) {
    throw std::runtime_error("book_ticker_shm.capacity mismatch");
  }
}

}  // namespace detail

class BookTickerShmManager {
 public:
  explicit BookTickerShmManager(const BookTickerShmConfig& config)
      : shm_name_(detail::PrepareShmName(config)),
        channel_name_(detail::ValidateChannelName(config.channel_name)),
        allocator_(shm_name_.c_str(), StorageSize(), config.create) {
    if (config.create) {
      const bool existed = allocator_.IsConstructed(channel_name_);
      channel_ = allocator_.Construct<BookTickerShmChannel>(channel_name_);
      if (!existed) {
        channel_->header.producer_pid = static_cast<std::uint64_t>(::getpid());
        channel_->header.created_ns = detail::NowNs();
      }
    } else {
      channel_ = allocator_.Find<BookTickerShmChannel>(channel_name_);
      if (channel_ == nullptr) {
        throw std::runtime_error("book_ticker_shm.channel_name not found");
      }
    }
    detail::ValidateChannelHeader(*channel_);
  }

  [[nodiscard]] BookTickerShmChannel& channel() noexcept {
    return *channel_;
  }

  [[nodiscard]] const BookTickerShmChannel& channel() const noexcept {
    return *channel_;
  }

  [[nodiscard]] static std::size_t StorageSize() noexcept {
    return sizeof(BookTickerShmChannel);
  }

 private:
  std::string shm_name_;
  std::string channel_name_;
  nova::ShmAllocator<kBookTickerShmAllocatorInstances> allocator_;
  BookTickerShmChannel* channel_{nullptr};
};

class DataShmPublisher {
 public:
  explicit DataShmPublisher(const BookTickerShmConfig& config)
      : manager_(config),
        published_count_(manager_.channel().header.published_count.load(
            std::memory_order_relaxed)) {}

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    BookTickerShmChannel& channel = manager_.channel();
    channel.queue.Push(book_ticker);
    ++published_count_;
    channel.header.published_count.store(published_count_,
                                         std::memory_order_relaxed);
  }

  void UpdateHeartbeatNs(std::uint64_t heartbeat_ns) noexcept {
    manager_.channel().header.heartbeat_ns.store(heartbeat_ns,
                                                 std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t published_count() const noexcept {
    return published_count_;
  }

 private:
  BookTickerShmManager manager_;
  std::uint64_t published_count_{0};
};

class BookTickerShmReader {
 public:
  explicit BookTickerShmReader(const BookTickerShmConfig& config)
      : manager_(config) {
    SeekLatest();
  }

  void SeekLatest() noexcept {
    read_pos_ = manager_.channel().queue.Current();
  }

  void SeekEarliestVisible() noexcept {
    const auto current = manager_.channel().queue.Current();
    const auto capacity = manager_.channel().queue.capacity();
    read_pos_ = current > capacity ? current - capacity : 0;
  }

  [[nodiscard]] bool TryReadOne(aquila::BookTicker* out) noexcept {
    const auto current = manager_.channel().queue.Current();
    if (read_pos_ == current) {
      return false;
    }

    const auto capacity = manager_.channel().queue.capacity();
    const auto unread_count = current - read_pos_;
    if (unread_count > capacity) {
      read_pos_ = current - capacity;
      ++overrun_count_;
    }

    *out = manager_.channel().queue.Value(read_pos_);
    ++read_pos_;
    return true;
  }

  [[nodiscard]] std::uint64_t read_pos() const noexcept {
    return read_pos_;
  }

  [[nodiscard]] std::uint64_t overrun_count() const noexcept {
    return overrun_count_;
  }

 private:
  BookTickerShmManager manager_;
  std::uint64_t read_pos_{0};
  std::uint64_t overrun_count_{0};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_SHM_H_
