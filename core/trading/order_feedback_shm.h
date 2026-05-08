#ifndef AQUILA_CORE_TRADING_ORDER_FEEDBACK_SHM_H_
#define AQUILA_CORE_TRADING_ORDER_FEEDBACK_SHM_H_

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/common/result.h"
#include "core/trading/order_feedback_event.h"
#include "core/trading/order_id.h"
#include "nova/concurrency/spsc_queue.h"
#include "nova/interprocess/shm_allocator.h"

namespace aquila {

inline constexpr std::uint32_t kOrderFeedbackShmMagic = 0x41514C4FU;
inline constexpr std::uint32_t kOrderFeedbackShmVersion = 1;
inline constexpr std::uint32_t kMaxOrderFeedbackStrategies = 8;
inline constexpr std::uint32_t kOrderFeedbackQueueCapacity = 65536;
inline constexpr std::size_t kOrderFeedbackShmAllocatorInstances = 64;

using OrderFeedbackQueue =
    nova::static_impl::SPSCQueue<OrderFeedbackEvent,
                                 kOrderFeedbackQueueCapacity>;

struct OrderFeedbackShmHeader {
  std::uint32_t magic{kOrderFeedbackShmMagic};
  std::uint32_t version{kOrderFeedbackShmVersion};
  std::uint32_t abi_size{sizeof(OrderFeedbackEvent)};
  std::uint32_t max_strategy_count{kMaxOrderFeedbackStrategies};
  std::uint32_t queue_capacity{kOrderFeedbackQueueCapacity};
  std::uint32_t event_size{sizeof(OrderFeedbackEvent)};
  std::uint64_t producer_pid{0};
  std::uint64_t producer_run_id{0};
  std::atomic<std::uint64_t> invalid_route_count{0};
};

struct OrderFeedbackLaneHeader {
  std::uint8_t strategy_id{0};
  std::atomic<std::uint64_t> consumer_pid{0};
  std::atomic<std::uint64_t> consumer_run_id{0};
  std::atomic<std::uint64_t> queue_full_count{0};
  std::atomic<std::uint64_t> dropped_count{0};
};

struct OrderFeedbackLane {
  OrderFeedbackLaneHeader header{};
  OrderFeedbackQueue queue{};
};

struct OrderFeedbackShmChannel {
  OrderFeedbackShmHeader header{};
  OrderFeedbackLane lanes[kMaxOrderFeedbackStrategies]{};
};

struct OrderFeedbackShmConfig {
  std::string shm_name;
  std::string channel_name;
  bool create{true};
  bool remove_existing{false};
};

static_assert(kMaxOrderFeedbackStrategies == 8);
static_assert(kOrderFeedbackQueueCapacity == 65536);
static_assert((kOrderFeedbackQueueCapacity &
               (kOrderFeedbackQueueCapacity - 1)) == 0);
static_assert(std::is_standard_layout_v<OrderFeedbackShmHeader>);
static_assert(std::is_standard_layout_v<OrderFeedbackLaneHeader>);
static_assert(std::is_standard_layout_v<OrderFeedbackLane>);
static_assert(std::is_standard_layout_v<OrderFeedbackShmChannel>);
static_assert(std::is_trivially_copyable_v<OrderFeedbackShmChannel>);

namespace order_feedback_shm_detail {

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

[[nodiscard]] inline Result<std::string> ValidateChannelName(
    std::string_view channel_name) {
  if (channel_name.empty()) {
    return Failure<std::string>("order_feedback_shm.channel_name is required");
  }
  if (channel_name.size() >= nova::kShmNameSize) {
    return Failure<std::string>("order_feedback_shm.channel_name is too long");
  }
  return Success<std::string>(std::string(channel_name));
}

[[nodiscard]] inline Result<std::string> PrepareShmName(
    const OrderFeedbackShmConfig& config) {
  if (config.shm_name.empty()) {
    return Failure<std::string>("order_feedback_shm.shm_name is required");
  }
  if (!config.create && config.remove_existing) {
    return Failure<std::string>(
        "order_feedback_shm.remove_existing requires create=true");
  }

  std::string shm_name = NormalizeShmName(config.shm_name);
  if (config.remove_existing) {
    ::shm_unlink(shm_name.c_str());
  }
  return Success<std::string>(std::move(shm_name));
}

inline void InitializeLaneHeaders(OrderFeedbackShmChannel& channel) noexcept {
  for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
    channel.lanes[i].header.strategy_id = static_cast<std::uint8_t>(i);
  }
}

[[nodiscard]] inline const char* ValidateChannelHeader(
    const OrderFeedbackShmChannel& channel) noexcept {
  if (channel.header.magic != kOrderFeedbackShmMagic) {
    return "order_feedback_shm.magic mismatch";
  }
  if (channel.header.version != kOrderFeedbackShmVersion) {
    return "order_feedback_shm.version mismatch";
  }
  if (channel.header.abi_size != sizeof(OrderFeedbackEvent)) {
    return "order_feedback_shm.abi_size mismatch";
  }
  if (channel.header.event_size != sizeof(OrderFeedbackEvent)) {
    return "order_feedback_shm.event_size mismatch";
  }
  if (channel.header.max_strategy_count != kMaxOrderFeedbackStrategies) {
    return "order_feedback_shm.max_strategy_count mismatch";
  }
  if (channel.header.queue_capacity != kOrderFeedbackQueueCapacity) {
    return "order_feedback_shm.queue_capacity mismatch";
  }
  return nullptr;
}

}  // namespace order_feedback_shm_detail

class OrderFeedbackShmManager {
 public:
  [[nodiscard]] static Result<OrderFeedbackShmManager> OpenOrCreate(
      const OrderFeedbackShmConfig& config) {
    return Build(config);
  }

  [[nodiscard]] static Result<OrderFeedbackShmManager> Create(
      const OrderFeedbackShmConfig& config) {
    OrderFeedbackShmConfig create_config = config;
    create_config.create = true;
    return Build(create_config);
  }

  [[nodiscard]] static Result<OrderFeedbackShmManager> Open(
      const OrderFeedbackShmConfig& config) {
    OrderFeedbackShmConfig open_config = config;
    open_config.create = false;
    return Build(open_config);
  }

  OrderFeedbackShmManager(OrderFeedbackShmManager&&) noexcept = default;
  OrderFeedbackShmManager& operator=(OrderFeedbackShmManager&&) noexcept =
      default;
  OrderFeedbackShmManager(const OrderFeedbackShmManager&) = delete;
  OrderFeedbackShmManager& operator=(const OrderFeedbackShmManager&) = delete;

  [[nodiscard]] OrderFeedbackShmChannel& channel() noexcept {
    return *channel_;
  }

  [[nodiscard]] const OrderFeedbackShmChannel& channel() const noexcept {
    return *channel_;
  }

  [[nodiscard]] static std::size_t StorageSize() noexcept {
    return sizeof(OrderFeedbackShmChannel);
  }

 private:
  template <typename T>
  friend struct Result;

  OrderFeedbackShmManager() noexcept = default;

  [[nodiscard]] static Result<OrderFeedbackShmManager> Build(
      const OrderFeedbackShmConfig& config) {
    try {
      auto shm_name_result = order_feedback_shm_detail::PrepareShmName(config);
      if (!shm_name_result.ok) {
        return order_feedback_shm_detail::Failure<OrderFeedbackShmManager>(
            std::move(shm_name_result.error));
      }

      auto channel_name_result =
          order_feedback_shm_detail::ValidateChannelName(config.channel_name);
      if (!channel_name_result.ok) {
        return order_feedback_shm_detail::Failure<OrderFeedbackShmManager>(
            std::move(channel_name_result.error));
      }

      OrderFeedbackShmManager manager;
      manager.shm_name_ = std::move(shm_name_result.value);
      manager.channel_name_ = std::move(channel_name_result.value);
      manager.allocator_ = std::make_unique<
          nova::ShmAllocator<kOrderFeedbackShmAllocatorInstances>>(
          manager.shm_name_.c_str(), StorageSize(), config.create);

      if (config.create) {
        const bool existed =
            manager.allocator_->IsConstructed(manager.channel_name_);
        manager.channel_ =
            manager.allocator_->Construct<OrderFeedbackShmChannel>(
                manager.channel_name_);
        if (!existed) {
          manager.channel_->header.producer_pid =
              static_cast<std::uint64_t>(::getpid());
          order_feedback_shm_detail::InitializeLaneHeaders(*manager.channel_);
        }
      } else {
        manager.channel_ = manager.allocator_->Find<OrderFeedbackShmChannel>(
            manager.channel_name_);
        if (manager.channel_ == nullptr) {
          return order_feedback_shm_detail::Failure<OrderFeedbackShmManager>(
              "order_feedback_shm.channel_name not found");
        }
      }

      if (const char* error = order_feedback_shm_detail::ValidateChannelHeader(
              *manager.channel_);
          error != nullptr) {
        return order_feedback_shm_detail::Failure<OrderFeedbackShmManager>(
            error);
      }

      return order_feedback_shm_detail::Success<OrderFeedbackShmManager>(
          std::move(manager));
    } catch (const std::exception& exc) {
      std::string error{"order_feedback_shm.manager init failed: "};
      error.append(exc.what());
      return order_feedback_shm_detail::Failure<OrderFeedbackShmManager>(
          std::move(error));
    } catch (...) {
      return order_feedback_shm_detail::Failure<OrderFeedbackShmManager>(
          "order_feedback_shm.manager init failed");
    }
  }

  std::string shm_name_;
  std::string channel_name_;
  std::unique_ptr<nova::ShmAllocator<kOrderFeedbackShmAllocatorInstances>>
      allocator_;
  OrderFeedbackShmChannel* channel_{nullptr};
};

class OrderFeedbackShmPublisher {
 public:
  explicit OrderFeedbackShmPublisher(OrderFeedbackShmChannel& channel) noexcept
      : channel_(channel) {}

  [[nodiscard]] bool Publish(const OrderFeedbackEvent& event) noexcept {
    if (event.kind == OrderFeedbackKind::kGap || event.local_order_id == 0) {
      RecordInvalidRoute();
      return false;
    }

    const std::uint8_t strategy_id =
        LocalOrderIdCodec::StrategyId(event.local_order_id);
    if (strategy_id >= kMaxOrderFeedbackStrategies) {
      RecordInvalidRoute();
      return false;
    }

    if (!FlushPendingGapForLane(strategy_id)) {
      RecordLaneQueueFull(strategy_id);
      return false;
    }

    OrderFeedbackLane& lane = channel_.lanes[strategy_id];
    if (lane.queue.TryPush(event)) {
      ++published_count_;
      return true;
    }

    RecordLaneQueueFull(strategy_id);
    SetPendingLaneGap(strategy_id, event.local_receive_ns);
    return false;
  }

  [[nodiscard]] bool PublishGlobalGap(OrderFeedbackGapReason reason,
                                      std::int64_t local_receive_ns) noexcept {
    const std::uint64_t sequence = ++gap_sequence_;
    bool all_published = true;

    for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
      OrderFeedbackEvent gap = MakeGapEvent(OrderFeedbackGapScope::kGlobal,
                                            reason, sequence, local_receive_ns);
      OrderFeedbackLane& lane = channel_.lanes[i];
      if (lane.queue.TryPush(gap)) {
        pending_gaps_[i].pending = false;
        ++published_count_;
        continue;
      }

      pending_gaps_[i].pending = true;
      pending_gaps_[i].event = gap;
      RecordLaneQueueFullOnly(static_cast<std::uint8_t>(i));
      all_published = false;
    }

    return all_published;
  }

  [[nodiscard]] std::size_t FlushPendingGapEvents() noexcept {
    std::size_t flushed_count = 0;
    for (std::uint32_t i = 0; i < kMaxOrderFeedbackStrategies; ++i) {
      bool flushed = false;
      if (FlushPendingGapForLane(static_cast<std::uint8_t>(i), &flushed) &&
          flushed) {
        ++flushed_count;
      }
    }
    return flushed_count;
  }

  [[nodiscard]] std::uint64_t published_count() const noexcept {
    return published_count_;
  }

  [[nodiscard]] std::uint64_t invalid_route_count() const noexcept {
    return invalid_route_count_;
  }

 private:
  struct PendingGap {
    bool pending{false};
    OrderFeedbackEvent event{};
  };

  [[nodiscard]] static OrderFeedbackEvent MakeGapEvent(
      OrderFeedbackGapScope scope, OrderFeedbackGapReason reason,
      std::uint64_t sequence, std::int64_t local_receive_ns) noexcept {
    OrderFeedbackEvent gap{};
    gap.kind = OrderFeedbackKind::kGap;
    gap.local_order_id = 0;
    gap.gap_scope = scope;
    gap.gap_reason = reason;
    gap.gap_sequence = sequence;
    gap.local_receive_ns = local_receive_ns;
    return gap;
  }

  void RecordInvalidRoute() noexcept {
    ++invalid_route_count_;
    channel_.header.invalid_route_count.fetch_add(1, std::memory_order_relaxed);
  }

  void RecordLaneQueueFull(std::uint8_t strategy_id) noexcept {
    OrderFeedbackLane& lane = channel_.lanes[strategy_id];
    lane.header.queue_full_count.fetch_add(1, std::memory_order_relaxed);
    lane.header.dropped_count.fetch_add(1, std::memory_order_relaxed);
  }

  void RecordLaneQueueFullOnly(std::uint8_t strategy_id) noexcept {
    OrderFeedbackLane& lane = channel_.lanes[strategy_id];
    lane.header.queue_full_count.fetch_add(1, std::memory_order_relaxed);
  }

  void SetPendingLaneGap(std::uint8_t strategy_id,
                         std::int64_t local_receive_ns) noexcept {
    PendingGap& pending_gap = pending_gaps_[strategy_id];
    if (pending_gap.pending &&
        pending_gap.event.gap_scope == OrderFeedbackGapScope::kGlobal) {
      return;
    }

    pending_gap.pending = true;
    pending_gap.event = MakeGapEvent(OrderFeedbackGapScope::kLane,
                                     OrderFeedbackGapReason::kLaneQueueFull,
                                     ++gap_sequence_, local_receive_ns);
  }

  [[nodiscard]] bool FlushPendingGapForLane(std::uint8_t strategy_id,
                                            bool* flushed = nullptr) noexcept {
    if (flushed != nullptr) {
      *flushed = false;
    }
    PendingGap& pending_gap = pending_gaps_[strategy_id];
    if (!pending_gap.pending) {
      return true;
    }

    OrderFeedbackLane& lane = channel_.lanes[strategy_id];
    if (!lane.queue.TryPush(pending_gap.event)) {
      return false;
    }

    pending_gap.pending = false;
    ++published_count_;
    if (flushed != nullptr) {
      *flushed = true;
    }
    return true;
  }

  OrderFeedbackShmChannel& channel_;
  std::array<PendingGap, kMaxOrderFeedbackStrategies> pending_gaps_{};
  std::uint64_t published_count_{0};
  std::uint64_t invalid_route_count_{0};
  std::uint64_t gap_sequence_{0};
};

class OrderFeedbackShmReader {
 public:
  [[nodiscard]] static Result<OrderFeedbackShmReader> Claim(
      OrderFeedbackShmChannel& channel, std::uint8_t strategy_id,
      std::uint64_t consumer_run_id, bool force_claim = false) {
    if (strategy_id >= kMaxOrderFeedbackStrategies) {
      return Failure("order_feedback_shm.reader strategy_id out of range");
    }
    if (consumer_run_id == 0) {
      return Failure("order_feedback_shm.reader consumer_run_id is required");
    }

    OrderFeedbackLane& lane = channel.lanes[strategy_id];
    OrderFeedbackLaneHeader& header = lane.header;
    const std::uint64_t consumer_pid = static_cast<std::uint64_t>(::getpid());

    if (!force_claim) {
      std::uint64_t expected_run_id = 0;
      if (!header.consumer_run_id.compare_exchange_strong(
              expected_run_id, consumer_run_id, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return Failure("order_feedback_shm.reader lane already claimed");
      }
      header.consumer_pid.store(consumer_pid, std::memory_order_release);
    } else {
      header.consumer_run_id.store(consumer_run_id, std::memory_order_release);
      header.consumer_pid.store(consumer_pid, std::memory_order_release);
    }

    return Success(OrderFeedbackShmReader(lane, consumer_pid, consumer_run_id));
  }

  OrderFeedbackShmReader(OrderFeedbackShmReader&& other) noexcept {
    MoveFrom(other);
  }

  OrderFeedbackShmReader& operator=(OrderFeedbackShmReader&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    Release();
    MoveFrom(other);
    return *this;
  }

  OrderFeedbackShmReader(const OrderFeedbackShmReader&) = delete;
  OrderFeedbackShmReader& operator=(const OrderFeedbackShmReader&) = delete;

  ~OrderFeedbackShmReader() {
    Release();
  }

  void Release() noexcept {
    if (!claimed_ || lane_ == nullptr) {
      return;
    }

    OrderFeedbackLaneHeader& header = lane_->header;
    std::uint64_t expected_run_id = consumer_run_id_;
    if (header.consumer_run_id.compare_exchange_strong(
            expected_run_id, 0, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      header.consumer_pid.store(0, std::memory_order_release);
    }

    Reset();
  }

  template <typename Handler>
  std::size_t Poll(std::size_t max_events, Handler&& handler) noexcept {
    if (max_events == 0 || lane_ == nullptr) {
      return 0;
    }

    std::size_t consumed = 0;
    while (consumed < max_events) {
      OrderFeedbackEvent event{};
      if (!lane_->queue.TryPop(event)) {
        break;
      }
      handler(event);
      ++consumed;
      ++consumed_count_;
    }
    return consumed;
  }

  [[nodiscard]] std::uint64_t consumed_count() const noexcept {
    return consumed_count_;
  }

 private:
  template <typename T>
  friend struct Result;

  OrderFeedbackShmReader() noexcept = default;

  OrderFeedbackShmReader(OrderFeedbackLane& lane, std::uint64_t consumer_pid,
                         std::uint64_t consumer_run_id) noexcept
      : lane_(&lane),
        consumer_pid_(consumer_pid),
        consumer_run_id_(consumer_run_id),
        claimed_(true) {}

  [[nodiscard]] static Result<OrderFeedbackShmReader> Failure(
      std::string error) {
    Result<OrderFeedbackShmReader> result;
    result.error = std::move(error);
    return result;
  }

  [[nodiscard]] static Result<OrderFeedbackShmReader> Success(
      OrderFeedbackShmReader reader) {
    Result<OrderFeedbackShmReader> result;
    result.value = std::move(reader);
    result.ok = true;
    return result;
  }

  void MoveFrom(OrderFeedbackShmReader& other) noexcept {
    lane_ = other.lane_;
    consumer_pid_ = other.consumer_pid_;
    consumer_run_id_ = other.consumer_run_id_;
    consumed_count_ = other.consumed_count_;
    claimed_ = other.claimed_;
    other.Reset();
  }

  void Reset() noexcept {
    lane_ = nullptr;
    consumer_pid_ = 0;
    consumer_run_id_ = 0;
    consumed_count_ = 0;
    claimed_ = false;
  }

  OrderFeedbackLane* lane_{nullptr};
  std::uint64_t consumer_pid_{0};
  std::uint64_t consumer_run_id_{0};
  std::uint64_t consumed_count_{0};
  bool claimed_{false};
};

}  // namespace aquila

#endif  // AQUILA_CORE_TRADING_ORDER_FEEDBACK_SHM_H_
