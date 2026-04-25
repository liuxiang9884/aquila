#include "core/websocket/reconnect_classifier.h"

#include <gtest/gtest.h>
#include <magic_enum/magic_enum.hpp>

#include <array>
#include <optional>
#include <utility>

using namespace aquila::websocket;

namespace {

constexpr std::array<std::pair<ConnectionError, FailureClass>, 9>
    kExpectedClasses{{
        {ConnectionError::kResolveFailure, FailureClass::kFatal},
        {ConnectionError::kSocketError, FailureClass::kTransient},
        {ConnectionError::kConnectTimeout, FailureClass::kTransient},
        {ConnectionError::kTlsFailure, FailureClass::kTransient},
        {ConnectionError::kHandshakeFailure, FailureClass::kFatal},
        {ConnectionError::kProtocolError, FailureClass::kTransient},
        {ConnectionError::kHeartbeatTimeout, FailureClass::kTransient},
        {ConnectionError::kPeerClosed, FailureClass::kTransient},
        {ConnectionError::kConsumerFatal, FailureClass::kFatal},
    }};

std::optional<FailureClass> ExpectedClass(ConnectionError error) noexcept {
  for (const auto& [candidate, failure_class] : kExpectedClasses) {
    if (candidate == error) {
      return failure_class;
    }
  }
  return std::nullopt;
}

}  // namespace

TEST(WebsocketReconnectClassifierTest, ClassifiesEveryFailureReason) {
  size_t classified = 0;
  for (const auto error : magic_enum::enum_values<ConnectionError>()) {
    if (error == ConnectionError::kNone) {
      continue;
    }
    const auto expected = ExpectedClass(error);
    ASSERT_TRUE(expected.has_value()) << magic_enum::enum_name(error);
    EXPECT_EQ(Classify(error), *expected) << magic_enum::enum_name(error);
    ++classified;
  }

  EXPECT_EQ(classified, kExpectedClasses.size());
}
