#ifndef AQUILA_CORE_WEBSOCKET_RECONNECT_CLASSIFIER_H_
#define AQUILA_CORE_WEBSOCKET_RECONNECT_CLASSIFIER_H_

#include <algorithm>
#include <cstdint>
#include <limits>

#include "core/websocket/types.h"

namespace aquila::websocket {

enum class FailureClass : std::uint8_t {
  kTransient,
  kFatal,
};

inline FailureClass Classify(ConnectionError error) noexcept {
  switch (error) {
    case ConnectionError::kResolveFailure:
    case ConnectionError::kHandshakeFailure:
    case ConnectionError::kConsumerFatal:
      return FailureClass::kFatal;
    case ConnectionError::kNone:
    case ConnectionError::kSocketError:
    case ConnectionError::kConnectTimeout:
    case ConnectionError::kTlsFailure:
    case ConnectionError::kProtocolError:
    case ConnectionError::kHeartbeatTimeout:
    case ConnectionError::kPeerClosed:
      return FailureClass::kTransient;
  }
  return FailureClass::kFatal;
}

class BackoffRng {
 public:
  explicit BackoffRng(std::uint64_t seed) noexcept
      : state_(seed == 0 ? kDefaultSeed : seed) {}

  std::uint64_t Next() noexcept {
    std::uint64_t x = state_;
    x ^= x << 13U;
    x ^= x >> 7U;
    x ^= x << 17U;
    state_ = x == 0 ? kDefaultSeed : x;
    return state_;
  }

 private:
  static constexpr std::uint64_t kDefaultSeed = 0x9e37'79b9'7f4a'7c15ULL;

  std::uint64_t state_{kDefaultSeed};
};

inline std::uint32_t ComputeBackoffMs(std::uint32_t attempt,
                                      const ReconnectPolicy& policy,
                                      BackoffRng& rng) noexcept {
  const std::uint64_t max_backoff = policy.max_backoff_ms;
  std::uint64_t raw = policy.initial_backoff_ms;
  if (policy.backoff_shift_bits != 0) {
    const std::uint64_t shift =
        static_cast<std::uint64_t>(attempt) * policy.backoff_shift_bits;
    if (shift >= 63U) {
      raw = max_backoff;
    } else {
      raw = static_cast<std::uint64_t>(policy.initial_backoff_ms) << shift;
    }
  }
  raw = std::min(raw, max_backoff);

  if (policy.jitter_percent == 0 || raw == 0) {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(raw, std::numeric_limits<std::uint32_t>::max()));
  }

  const std::uint32_t range =
      static_cast<std::uint32_t>(policy.jitter_percent) * 2U + 1U;
  const std::int32_t jitter =
      static_cast<std::int32_t>(rng.Next() % range) -
      static_cast<std::int32_t>(policy.jitter_percent);
  const std::uint64_t adjusted =
      raw * static_cast<std::uint64_t>(100 + jitter) / 100U;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      adjusted, std::numeric_limits<std::uint32_t>::max()));
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_RECONNECT_CLASSIFIER_H_
