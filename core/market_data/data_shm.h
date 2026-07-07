#ifndef AQUILA_CORE_MARKET_DATA_DATA_SHM_H_
#define AQUILA_CORE_MARKET_DATA_DATA_SHM_H_

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/market_data/data_shm_config.h"
#include "core/market_data/types.h"
#include "nova/concurrency/sp_broadcast_queue.h"
#include "nova/interprocess/shm_allocator.h"

namespace aquila::market_data {

inline constexpr std::uint32_t kBookTickerShmMagic = 0x41514C42U;
inline constexpr std::uint32_t kBookTickerShmVersion = 1;
inline constexpr std::uint32_t kTradeShmMagic = 0x41514C54U;
inline constexpr std::uint32_t kTradeShmVersion = 1;
inline constexpr std::size_t kBookTickerShmAllocatorInstances = 64;

using BookTickerQueue =
    nova::static_impl::SPBroadcastQueue<aquila::BookTicker,
                                        kBookTickerShmCapacity>;
using TradeQueue =
    nova::static_impl::SPBroadcastQueue<aquila::Trade, kTradeShmCapacity>;

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

struct TradeShmHeader {
  std::uint32_t magic{kTradeShmMagic};
  std::uint32_t version{kTradeShmVersion};
  std::uint32_t abi_size{sizeof(aquila::Trade)};
  std::uint32_t capacity{kTradeShmCapacity};
  std::uint64_t producer_pid{0};
  std::uint64_t created_ns{0};
  std::atomic<std::uint64_t> published_count{0};
  std::atomic<std::uint64_t> heartbeat_ns{0};
};

struct TradeShmChannel {
  TradeShmHeader header{};
  TradeQueue queue{};
};

static_assert(kBookTickerShmCapacity == 65536);
static_assert((kBookTickerShmCapacity & (kBookTickerShmCapacity - 1)) == 0);
static_assert(std::is_standard_layout_v<BookTickerShmChannel>);
static_assert(std::is_trivially_copyable_v<BookTickerShmChannel>);
static_assert(kTradeShmCapacity == 65536);
static_assert((kTradeShmCapacity & (kTradeShmCapacity - 1)) == 0);
static_assert(std::is_standard_layout_v<TradeShmChannel>);
static_assert(std::is_trivially_copyable_v<TradeShmChannel>);

namespace detail {

[[nodiscard]] constexpr std::size_t AlignUp(std::size_t value,
                                            std::size_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

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
    throw std::invalid_argument("data_shm_sink.channel_name is required");
  }
  if (channel_name.size() >= nova::kShmNameSize) {
    throw std::invalid_argument("data_shm_sink.channel_name is too long");
  }
  return std::string(channel_name);
}

[[nodiscard]] inline std::string PrepareShmName(std::string_view name,
                                                bool create,
                                                bool remove_existing) {
  if (name.empty()) {
    throw std::invalid_argument("data_shm_sink.shm_name is required");
  }
  if (!create && remove_existing) {
    throw std::invalid_argument(
        "data_shm_sink.remove_existing requires create=true");
  }

  std::string shm_name = NormalizeShmName(name);
  if (remove_existing) {
    ::shm_unlink(shm_name.c_str());
  }
  return shm_name;
}

[[nodiscard]] inline std::string PrepareShmName(
    const BookTickerShmConfig& config) {
  return PrepareShmName(config.shm_name, config.create, config.remove_existing);
}

[[nodiscard]] inline std::string PrepareShmName(const TradeShmConfig& config) {
  return PrepareShmName(config.shm_name, config.create, config.remove_existing);
}

[[nodiscard]] inline std::string PrepareShmName(const DataShmConfig& config) {
  if (config.book_ticker_enabled && config.trade_enabled &&
      !config.book_ticker_channel_name.empty() &&
      config.book_ticker_channel_name == config.trade_channel_name) {
    throw std::invalid_argument(
        "data_shm_sink.channel_name book_ticker and trade channels must be "
        "distinct");
  }
  return PrepareShmName(config.shm_name, config.create, config.remove_existing);
}

inline void ValidateChannelHeader(const BookTickerShmChannel& channel) {
  if (channel.header.magic != kBookTickerShmMagic) {
    throw std::runtime_error("data_shm_sink.magic mismatch");
  }
  if (channel.header.version != kBookTickerShmVersion) {
    throw std::runtime_error("data_shm_sink.version mismatch");
  }
  if (channel.header.abi_size != sizeof(aquila::BookTicker)) {
    throw std::runtime_error("data_shm_sink.abi_size mismatch");
  }
  if (channel.header.capacity != kBookTickerShmCapacity) {
    throw std::runtime_error("data_shm_sink.capacity mismatch");
  }
}

inline void ValidateChannelHeader(const TradeShmChannel& channel) {
  if (channel.header.magic != kTradeShmMagic) {
    throw std::runtime_error("data_shm_sink.magic mismatch");
  }
  if (channel.header.version != kTradeShmVersion) {
    throw std::runtime_error("data_shm_sink.version mismatch");
  }
  if (channel.header.abi_size != sizeof(aquila::Trade)) {
    throw std::runtime_error("data_shm_sink.abi_size mismatch");
  }
  if (channel.header.capacity != kTradeShmCapacity) {
    throw std::runtime_error("data_shm_sink.capacity mismatch");
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
        throw std::runtime_error("data_shm_sink.channel_name not found");
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

class TradeShmManager {
 public:
  explicit TradeShmManager(const TradeShmConfig& config)
      : shm_name_(detail::PrepareShmName(config)),
        channel_name_(detail::ValidateChannelName(config.channel_name)),
        allocator_(shm_name_.c_str(), StorageSize(), config.create) {
    if (config.create) {
      const bool existed = allocator_.IsConstructed(channel_name_);
      channel_ = allocator_.Construct<TradeShmChannel>(channel_name_);
      if (!existed) {
        channel_->header.producer_pid = static_cast<std::uint64_t>(::getpid());
        channel_->header.created_ns = detail::NowNs();
      }
    } else {
      channel_ = allocator_.Find<TradeShmChannel>(channel_name_);
      if (channel_ == nullptr) {
        throw std::runtime_error("data_shm_sink.channel_name not found");
      }
    }
    detail::ValidateChannelHeader(*channel_);
  }

  [[nodiscard]] TradeShmChannel& channel() noexcept {
    return *channel_;
  }

  [[nodiscard]] const TradeShmChannel& channel() const noexcept {
    return *channel_;
  }

  [[nodiscard]] static std::size_t StorageSize() noexcept {
    return sizeof(TradeShmChannel);
  }

 private:
  std::string shm_name_;
  std::string channel_name_;
  nova::ShmAllocator<kBookTickerShmAllocatorInstances> allocator_;
  TradeShmChannel* channel_{nullptr};
};

class DataShmManager {
 public:
  explicit DataShmManager(const DataShmConfig& config)
      : shm_name_(detail::PrepareShmName(config)),
        allocator_(shm_name_.c_str(), StorageSize(), config.create) {
    if (!config.book_ticker_enabled && !config.trade_enabled) {
      throw std::invalid_argument(
          "data_shm_sink requires at least one enabled channel");
    }
    if (config.book_ticker_enabled) {
      book_ticker_channel_name_ =
          detail::ValidateChannelName(config.book_ticker_channel_name);
    }
    if (config.trade_enabled) {
      trade_channel_name_ =
          detail::ValidateChannelName(config.trade_channel_name);
    }
    if (config.create) {
      if (config.book_ticker_enabled) {
        const bool book_ticker_existed =
            allocator_.IsConstructed(book_ticker_channel_name_);
        book_ticker_channel_ = allocator_.Construct<BookTickerShmChannel>(
            book_ticker_channel_name_);
        if (!book_ticker_existed) {
          book_ticker_channel_->header.producer_pid =
              static_cast<std::uint64_t>(::getpid());
          book_ticker_channel_->header.created_ns = detail::NowNs();
        }
      }

      if (config.trade_enabled) {
        const bool trade_existed =
            allocator_.IsConstructed(trade_channel_name_);
        trade_channel_ =
            allocator_.Construct<TradeShmChannel>(trade_channel_name_);
        if (!trade_existed) {
          trade_channel_->header.producer_pid =
              static_cast<std::uint64_t>(::getpid());
          trade_channel_->header.created_ns = detail::NowNs();
        }
      }
    } else {
      if (config.book_ticker_enabled) {
        book_ticker_channel_ =
            allocator_.Find<BookTickerShmChannel>(book_ticker_channel_name_);
        if (book_ticker_channel_ == nullptr) {
          throw std::runtime_error(
              "data_shm_sink.book_ticker_channel_name not found");
        }
      }
      if (config.trade_enabled) {
        trade_channel_ = allocator_.Find<TradeShmChannel>(trade_channel_name_);
        if (trade_channel_ == nullptr) {
          throw std::runtime_error(
              "data_shm_sink.trade_channel_name not found");
        }
      }
    }
    if (book_ticker_channel_ != nullptr) {
      detail::ValidateChannelHeader(*book_ticker_channel_);
    }
    if (trade_channel_ != nullptr) {
      detail::ValidateChannelHeader(*trade_channel_);
    }
  }

  [[nodiscard]] BookTickerShmChannel& book_ticker_channel() noexcept {
    return *book_ticker_channel_;
  }

  [[nodiscard]] TradeShmChannel& trade_channel() noexcept {
    return *trade_channel_;
  }

  [[nodiscard]] const BookTickerShmChannel& book_ticker_channel()
      const noexcept {
    return *book_ticker_channel_;
  }

  [[nodiscard]] const TradeShmChannel& trade_channel() const noexcept {
    return *trade_channel_;
  }

  [[nodiscard]] BookTickerShmChannel* book_ticker_channel_ptr() noexcept {
    return book_ticker_channel_;
  }

  [[nodiscard]] TradeShmChannel* trade_channel_ptr() noexcept {
    return trade_channel_;
  }

  [[nodiscard]] static std::size_t StorageSize() noexcept {
    const std::size_t trade_offset =
        detail::AlignUp(sizeof(BookTickerShmChannel), alignof(TradeShmChannel));
    return trade_offset + sizeof(TradeShmChannel);
  }

 private:
  std::string shm_name_;
  std::string book_ticker_channel_name_;
  std::string trade_channel_name_;
  nova::ShmAllocator<kBookTickerShmAllocatorInstances> allocator_;
  BookTickerShmChannel* book_ticker_channel_{nullptr};
  TradeShmChannel* trade_channel_{nullptr};
};

class DataShmPublisher {
 public:
  explicit DataShmPublisher(const BookTickerShmConfig& config)
      : book_ticker_manager_(std::make_unique<BookTickerShmManager>(config)),
        book_ticker_channel_(&book_ticker_manager_->channel()),
        published_book_tickers_(book_ticker_channel_->queue.Current()) {}

  explicit DataShmPublisher(const TradeShmConfig& config)
      : trade_manager_(std::make_unique<TradeShmManager>(config)),
        trade_channel_(&trade_manager_->channel()),
        published_trades_(trade_channel_->queue.Current()) {}

  explicit DataShmPublisher(const DataShmConfig& config)
      : data_manager_(std::make_unique<DataShmManager>(config)),
        book_ticker_channel_(data_manager_->book_ticker_channel_ptr()),
        trade_channel_(data_manager_->trade_channel_ptr()),
        published_book_tickers_(book_ticker_channel_ == nullptr
                                    ? 0
                                    : book_ticker_channel_->queue.Current()),
        published_trades_(
            trade_channel_ == nullptr ? 0 : trade_channel_->queue.Current()) {}

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    if (book_ticker_channel_ == nullptr) [[unlikely]] {
      assert(false && "book ticker channel is not available");
      return;
    }
    book_ticker_channel_->queue.Push(book_ticker);
    ++published_book_tickers_;
  }

  template <typename Writer>
  void EmplaceBookTickerWith(Writer&& writer) noexcept(
      noexcept(std::declval<BookTickerQueue&>().EmplaceWith(
          std::forward<Writer>(writer)))) {
    if (book_ticker_channel_ == nullptr) [[unlikely]] {
      assert(false && "book ticker channel is not available");
      return;
    }
    book_ticker_channel_->queue.EmplaceWith(std::forward<Writer>(writer));
    ++published_book_tickers_;
  }

  void OnTrade(const aquila::Trade& trade) noexcept {
    if (trade_channel_ == nullptr) [[unlikely]] {
      assert(false && "trade channel is not available");
      return;
    }
    trade_channel_->queue.Push(trade);
    ++published_trades_;
  }

  template <typename Writer>
  void EmplaceTradeWith(Writer&& writer) noexcept(noexcept(
      std::declval<TradeQueue&>().EmplaceWith(std::forward<Writer>(writer)))) {
    if (trade_channel_ == nullptr) [[unlikely]] {
      assert(false && "trade channel is not available");
      return;
    }
    trade_channel_->queue.EmplaceWith(std::forward<Writer>(writer));
    ++published_trades_;
  }

  void FlushPublishedCount() noexcept {
    if (book_ticker_channel_ != nullptr) {
      book_ticker_channel_->header.published_count.store(
          published_book_tickers_, std::memory_order_relaxed);
    }
    if (trade_channel_ != nullptr) {
      trade_channel_->header.published_count.store(published_trades_,
                                                   std::memory_order_relaxed);
    }
  }

  void UpdateHeartbeatNs(std::uint64_t heartbeat_ns) noexcept {
    FlushPublishedCount();
    if (book_ticker_channel_ != nullptr) {
      book_ticker_channel_->header.heartbeat_ns.store(
          heartbeat_ns, std::memory_order_relaxed);
    }
    if (trade_channel_ != nullptr) {
      trade_channel_->header.heartbeat_ns.store(heartbeat_ns,
                                                std::memory_order_relaxed);
    }
  }

  [[nodiscard]] std::uint64_t published_book_tickers() const noexcept {
    return published_book_tickers_;
  }

  [[nodiscard]] std::uint64_t published_trades() const noexcept {
    return published_trades_;
  }

  [[nodiscard]] std::uint64_t published_count() const noexcept {
    return published_book_tickers();
  }

  [[nodiscard]] bool has_book_ticker_channel() const noexcept {
    return book_ticker_channel_ != nullptr;
  }

  [[nodiscard]] bool has_trade_channel() const noexcept {
    return trade_channel_ != nullptr;
  }

 private:
  std::unique_ptr<BookTickerShmManager> book_ticker_manager_;
  std::unique_ptr<TradeShmManager> trade_manager_;
  std::unique_ptr<DataShmManager> data_manager_;
  BookTickerShmChannel* book_ticker_channel_{nullptr};
  TradeShmChannel* trade_channel_{nullptr};
  std::uint64_t published_book_tickers_{0};
  std::uint64_t published_trades_{0};
};

class BookTickerShmReader {
 public:
  explicit BookTickerShmReader(const BookTickerShmConfig& config)
      : manager_(config), queue_(&manager_.channel().queue) {
    SeekLatest();
  }

  void SeekLatest() noexcept {
    read_pos_ = queue_->Current();
  }

  void SeekEarliestVisible() noexcept {
    const auto current = queue_->Current();
    read_pos_ = current > kCapacity ? current - kCapacity : 0;
  }

  [[nodiscard]] bool TryReadOne(aquila::BookTicker* out) noexcept {
    const auto current = queue_->Current();
    if (PrepareReadableWindow(current) == 0) {
      return false;
    }

    *out = queue_->Value(read_pos_);
    ++read_pos_;
    return true;
  }

  [[nodiscard]] bool TryReadLatest(aquila::BookTicker* out,
                                   std::uint64_t* skipped_count) noexcept {
    const auto current = queue_->Current();
    const auto visible_unread_count = PrepareReadableWindow(current);
    if (visible_unread_count == 0) {
      if (skipped_count != nullptr) {
        *skipped_count = 0;
      }
      return false;
    }

    const auto latest_pos = current - 1;
    *out = queue_->Value(latest_pos);
    read_pos_ = current;

    if (skipped_count != nullptr) {
      *skipped_count = visible_unread_count - 1;
    }
    return true;
  }

  [[nodiscard]] std::uint64_t read_pos() const noexcept {
    return read_pos_;
  }

  [[nodiscard]] std::uint64_t overrun_count() const noexcept {
    return overrun_count_;
  }

 private:
  [[nodiscard]] std::uint64_t PrepareReadableWindow(
      std::uint64_t current) noexcept {
    if (read_pos_ == current) {
      return 0;
    }

    const auto unread_count = current - read_pos_;
    // Keep the exact-capacity boundary readable: first-version BookTicker SHM
    // prioritizes not dropping an already published BBO. Stronger in-flight
    // overwrite detection at this boundary requires per-slot sequence metadata.
    if (unread_count > kCapacity) {
      read_pos_ = current - kCapacity;
      ++overrun_count_;
    }
    return current - read_pos_;
  }

  static constexpr std::uint64_t kCapacity = kBookTickerShmCapacity;

  BookTickerShmManager manager_;
  BookTickerQueue* queue_{nullptr};
  std::uint64_t read_pos_{0};
  std::uint64_t overrun_count_{0};
};

class TradeShmReader {
 public:
  explicit TradeShmReader(const TradeShmConfig& config)
      : manager_(config), queue_(&manager_.channel().queue) {
    SeekLatest();
  }

  void SeekLatest() noexcept {
    read_pos_ = queue_->Current();
  }

  void SeekEarliestVisible() noexcept {
    const auto current = queue_->Current();
    read_pos_ = current > kCapacity ? current - kCapacity : 0;
  }

  [[nodiscard]] bool TryReadOne(aquila::Trade* out) noexcept {
    const auto current = queue_->Current();
    if (PrepareReadableWindow(current) == 0) {
      return false;
    }

    *out = queue_->Value(read_pos_);
    ++read_pos_;
    return true;
  }

  [[nodiscard]] bool TryReadLatest(aquila::Trade* out,
                                   std::uint64_t* skipped_count) noexcept {
    const auto current = queue_->Current();
    const auto visible_unread_count = PrepareReadableWindow(current);
    if (visible_unread_count == 0) {
      if (skipped_count != nullptr) {
        *skipped_count = 0;
      }
      return false;
    }

    const auto latest_pos = current - 1;
    *out = queue_->Value(latest_pos);
    read_pos_ = current;

    if (skipped_count != nullptr) {
      *skipped_count = visible_unread_count - 1;
    }
    return true;
  }

  [[nodiscard]] std::uint64_t read_pos() const noexcept {
    return read_pos_;
  }

  [[nodiscard]] std::uint64_t overrun_count() const noexcept {
    return overrun_count_;
  }

 private:
  [[nodiscard]] std::uint64_t PrepareReadableWindow(
      std::uint64_t current) noexcept {
    if (read_pos_ == current) {
      return 0;
    }

    const auto unread_count = current - read_pos_;
    if (unread_count > kCapacity) {
      read_pos_ = current - kCapacity;
      ++overrun_count_;
    }
    return current - read_pos_;
  }

  static constexpr std::uint64_t kCapacity = kTradeShmCapacity;

  TradeShmManager manager_;
  TradeQueue* queue_{nullptr};
  std::uint64_t read_pos_{0};
  std::uint64_t overrun_count_{0};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_SHM_H_
