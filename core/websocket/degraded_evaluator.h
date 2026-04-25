#ifndef AQUILA_CORE_WEBSOCKET_DEGRADED_EVALUATOR_H_
#define AQUILA_CORE_WEBSOCKET_DEGRADED_EVALUATOR_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/websocket/types.h"

namespace aquila::websocket {

struct DegradedSample {
  std::uint64_t now_ns{0};
  std::uint64_t pending_write_count{0};
  std::uint64_t prepared_write_slots{0};
  std::uint64_t consumer_backpressure_drops{0};
  std::uint64_t frame_codec_capacity_exhaustions{0};
  bool awaiting_pong{false};
  std::uint64_t last_ping_ns{0};
};

struct DegradedEvaluation {
  bool active{false};
  bool entered{false};
  bool exited{false};
};

class BackpressureWindow {
 public:
  static constexpr size_t kSlots = 16;
  static constexpr std::uint64_t kWindowNs = 1'000'000'000ULL;

  std::uint64_t Snapshot(std::uint64_t now_ns,
                         std::uint64_t counter) noexcept {
    counter_snapshot_[write_index_] = counter;
    timestamp_ns_[write_index_] = now_ns;
    write_index_ = (write_index_ + 1U) & (kSlots - 1U);
    if (filled_slots_ < kSlots) {
      ++filled_slots_;
    }
    return DeltaSince(now_ns, counter);
  }

 private:
  std::uint64_t DeltaSince(std::uint64_t now_ns,
                           std::uint64_t counter) const noexcept {
    if (filled_slots_ < 2) {
      return 0;
    }

    const std::uint64_t window_begin =
        now_ns > kWindowNs ? now_ns - kWindowNs : 0;
    bool found = false;
    std::uint64_t oldest_timestamp = 0;
    std::uint64_t oldest_counter = counter;
    for (size_t i = 0; i < filled_slots_; ++i) {
      const std::uint64_t timestamp = timestamp_ns_[i];
      if (timestamp < window_begin || timestamp > now_ns) {
        continue;
      }
      if (!found || timestamp < oldest_timestamp) {
        found = true;
        oldest_timestamp = timestamp;
        oldest_counter = counter_snapshot_[i];
      }
    }
    if (!found || counter < oldest_counter) {
      return 0;
    }
    return counter - oldest_counter;
  }

  std::array<std::uint64_t, kSlots> counter_snapshot_{};
  std::array<std::uint64_t, kSlots> timestamp_ns_{};
  std::uint32_t write_index_{0};
  std::uint32_t filled_slots_{0};
};

class DegradedEvaluator {
 public:
  explicit DegradedEvaluator(DegradedThresholds thresholds) noexcept
      : thresholds_(thresholds) {}

  DegradedEvaluation Evaluate(const DegradedSample& sample) noexcept {
    const bool triggered = IsTriggered(sample);
    DegradedEvaluation result{};
    if (active_) {
      if (triggered) {
        recover_ticks_ = 0;
      } else {
        ++recover_ticks_;
        if (recover_ticks_ >= RequiredRecoverTicks()) {
          active_ = false;
          recover_ticks_ = 0;
          result.exited = true;
        }
      }
    } else {
      recover_ticks_ = 0;
      if (triggered) {
        active_ = true;
        result.entered = true;
      }
    }
    result.active = active_;
    return result;
  }

  bool active() const noexcept { return active_; }

  void Reset() noexcept {
    backpressure_window_ = BackpressureWindow{};
    frame_codec_capacity_window_ = BackpressureWindow{};
    high_watermark_ticks_ = 0;
    recover_ticks_ = 0;
    active_ = false;
  }

 private:
  bool IsTriggered(const DegradedSample& sample) noexcept {
    bool triggered = false;
    if (HighWatermarkTriggered(sample)) {
      triggered = true;
    }
    if (BackpressureTriggered(sample)) {
      triggered = true;
    }
    if (FrameCodecCapacityTriggered(sample)) {
      triggered = true;
    }
    if (AwaitingPongTriggered(sample)) {
      triggered = true;
    }
    return triggered;
  }

  bool HighWatermarkTriggered(const DegradedSample& sample) noexcept {
    if (thresholds_.high_watermark_percent == 0 ||
        sample.prepared_write_slots == 0) {
      high_watermark_ticks_ = 0;
      return false;
    }
    const std::uint64_t used_percent =
        sample.pending_write_count * 100U / sample.prepared_write_slots;
    if (used_percent < thresholds_.high_watermark_percent) {
      high_watermark_ticks_ = 0;
      return false;
    }
    ++high_watermark_ticks_;
    return high_watermark_ticks_ >= RequiredHighWatermarkTicks();
  }

  bool BackpressureTriggered(const DegradedSample& sample) noexcept {
    const std::uint64_t drops =
        backpressure_window_.Snapshot(sample.now_ns,
                                      sample.consumer_backpressure_drops);
    return thresholds_.backpressure_drops_per_second != 0 &&
           drops >= thresholds_.backpressure_drops_per_second;
  }

  bool FrameCodecCapacityTriggered(const DegradedSample& sample) noexcept {
    const std::uint64_t events =
        frame_codec_capacity_window_.Snapshot(
            sample.now_ns, sample.frame_codec_capacity_exhaustions);
    return thresholds_.frame_codec_capacity_events_per_second != 0 &&
           events >= thresholds_.frame_codec_capacity_events_per_second;
  }

  bool AwaitingPongTriggered(const DegradedSample& sample) const noexcept {
    if (thresholds_.awaiting_pong_timeout_ms == 0 || !sample.awaiting_pong ||
        sample.last_ping_ns == 0 || sample.now_ns <= sample.last_ping_ns) {
      return false;
    }
    const std::uint64_t timeout_ns =
        static_cast<std::uint64_t>(thresholds_.awaiting_pong_timeout_ms) *
        1'000'000ULL;
    return sample.now_ns - sample.last_ping_ns > timeout_ns;
  }

  std::uint32_t RequiredHighWatermarkTicks() const noexcept {
    return thresholds_.high_watermark_hold_ticks == 0
               ? 1
               : thresholds_.high_watermark_hold_ticks;
  }

  std::uint32_t RequiredRecoverTicks() const noexcept {
    return thresholds_.recover_ticks == 0 ? 1 : thresholds_.recover_ticks;
  }

  DegradedThresholds thresholds_{};
  BackpressureWindow backpressure_window_{};
  BackpressureWindow frame_codec_capacity_window_{};
  std::uint32_t high_watermark_ticks_{0};
  std::uint32_t recover_ticks_{0};
  bool active_{false};
};

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_DEGRADED_EVALUATOR_H_
