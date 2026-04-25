#include "core/websocket/reconnect_classifier.h"

#include <gtest/gtest.h>

#include <cstdint>

using namespace aquila::websocket;

namespace {

std::uint32_t RawBackoffMs(const ReconnectPolicy& policy,
                           std::uint32_t attempt) noexcept {
  std::uint32_t raw = policy.initial_backoff_ms;
  const std::uint32_t shift =
      static_cast<std::uint32_t>(policy.backoff_shift_bits) * attempt;
  if (policy.backoff_shift_bits != 0 && shift < 31U) {
    raw = policy.initial_backoff_ms << shift;
  } else if (policy.backoff_shift_bits != 0) {
    raw = policy.max_backoff_ms;
  }
  return raw > policy.max_backoff_ms ? policy.max_backoff_ms : raw;
}

}  // namespace

TEST(WebsocketBackoffComputeTest, ComputesIntegerExponentialBackoffWithoutJitter) {
  ReconnectPolicy policy{};
  policy.initial_backoff_ms = 100;
  policy.max_backoff_ms = 1'000;
  policy.backoff_shift_bits = 1;
  policy.jitter_percent = 0;

  BackoffRng rng(0x1234'5678'9abc'def0ULL);
  EXPECT_EQ(ComputeBackoffMs(0, policy, rng), 100U);
  EXPECT_EQ(ComputeBackoffMs(1, policy, rng), 200U);
  EXPECT_EQ(ComputeBackoffMs(2, policy, rng), 400U);
  EXPECT_EQ(ComputeBackoffMs(3, policy, rng), 800U);
  EXPECT_EQ(ComputeBackoffMs(4, policy, rng), 1'000U);
  EXPECT_EQ(ComputeBackoffMs(8, policy, rng), 1'000U);
}

TEST(WebsocketBackoffComputeTest, AppliesDeterministicJitterWithinRange) {
  ReconnectPolicy policy{};
  policy.initial_backoff_ms = 80;
  policy.max_backoff_ms = 10'000;
  policy.backoff_shift_bits = 1;
  policy.jitter_percent = 25;

  BackoffRng first(0xfeed'face'cafe'beefULL);
  BackoffRng second(0xfeed'face'cafe'beefULL);
  for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
    const std::uint32_t raw = RawBackoffMs(policy, attempt);
    const std::uint32_t min_ms = raw * 75U / 100U;
    const std::uint32_t max_ms = raw * 125U / 100U;
    const std::uint32_t first_value = ComputeBackoffMs(attempt, policy, first);
    const std::uint32_t second_value = ComputeBackoffMs(attempt, policy, second);

    EXPECT_EQ(first_value, second_value);
    EXPECT_GE(first_value, min_ms);
    EXPECT_LE(first_value, max_ms);
  }
}

TEST(WebsocketBackoffComputeTest, ShiftBitsZeroKeepsInitialBackoffConstant) {
  ReconnectPolicy policy{};
  policy.initial_backoff_ms = 125;
  policy.max_backoff_ms = 10'000;
  policy.backoff_shift_bits = 0;
  policy.jitter_percent = 0;

  BackoffRng rng(0x0102'0304'0506'0708ULL);
  for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
    EXPECT_EQ(ComputeBackoffMs(attempt, policy, rng), 125U);
  }
}
