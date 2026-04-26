#include "core/websocket/frame_codec.h"
#include "core/websocket/queued_frame_codec.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace aquila::websocket;

namespace {

std::vector<std::byte> BuildServerTextFrame(std::string_view payload) {
  const size_t header_bytes = payload.size() <= 125 ? 2 : 4;
  std::vector<std::byte> frame(header_bytes + payload.size());
  frame[0] = std::byte{0x81};
  if (payload.size() <= 125) {
    frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  } else {
    frame[1] = std::byte{126};
    frame[2] = std::byte{
        static_cast<unsigned char>((payload.size() >> 8U) & 0xFFU)};
    frame[3] = std::byte{static_cast<unsigned char>(payload.size() & 0xFFU)};
  }
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[header_bytes + i] = static_cast<std::byte>(payload[i]);
  }
  return frame;
}

std::vector<std::byte> BuildServerBinaryFrame(std::string_view payload) {
  const size_t header_bytes = payload.size() <= 125 ? 2 : 4;
  std::vector<std::byte> frame(header_bytes + payload.size());
  frame[0] = std::byte{0x82};
  if (payload.size() <= 125) {
    frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  } else {
    frame[1] = std::byte{126};
    frame[2] = std::byte{
        static_cast<unsigned char>((payload.size() >> 8U) & 0xFFU)};
    frame[3] = std::byte{static_cast<unsigned char>(payload.size() & 0xFFU)};
  }
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[header_bytes + i] = static_cast<std::byte>(payload[i]);
  }
  return frame;
}

bool PayloadEquals(std::span<const std::byte> payload,
                   std::string_view expected) {
  if (payload.size() != expected.size()) {
    return false;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    if (payload[i] != static_cast<std::byte>(expected[i])) {
      return false;
    }
  }
  return true;
}

bool BytesEqual(std::span<const std::byte> lhs,
                std::span<const std::byte> rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (lhs[i] != rhs[i]) {
      return false;
    }
  }
  return true;
}

std::span<const std::byte> AsBytes(std::string_view text) {
  return std::as_bytes(std::span(text.data(), text.size()));
}

}  // namespace

TEST(WebsocketFrameCodecTest, EncodesMaskedFramesAndDecodesCoalescedReads) {
  FrameCodec encode_codec(1024);
  constexpr std::string_view kMaskProbePayload = "tick";
  std::array<std::array<std::byte, 128>, 8> encode_storage{};
  std::array<std::byte, 4> first_mask_key{};
  bool saw_different_mask_key = false;
  for (size_t i = 0; i < encode_storage.size(); ++i) {
    const auto encoded =
        encode_codec.EncodeText(AsBytes(kMaskProbePayload), encode_storage[i]);
    ASSERT_TRUE(encoded.ok);
    EXPECT_NE((std::to_integer<unsigned char>(encoded.bytes[1]) & 0x80U), 0U);

    const auto mask_key = encoded.bytes.subspan(2, first_mask_key.size());
    if (i == 0) {
      for (size_t j = 0; j < first_mask_key.size(); ++j) {
        first_mask_key[j] = mask_key[j];
      }
      continue;
    }
    if (!BytesEqual(mask_key, std::span<const std::byte>(first_mask_key))) {
      saw_different_mask_key = true;
    }
  }
  EXPECT_TRUE(saw_different_mask_key);

  FrameCodec decode_codec(11);
  auto first_frame = BuildServerTextFrame("hello world");
  auto second_frame = BuildServerTextFrame("market-data");
  std::vector<std::byte> coalesced;
  coalesced.reserve(first_frame.size() + second_frame.size());
  coalesced.insert(coalesced.end(), first_frame.begin(), first_frame.end());
  coalesced.insert(coalesced.end(), second_frame.begin(), second_frame.end());

  auto decoded = decode_codec.Feed(coalesced);
  ASSERT_EQ(decoded.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(decoded.view.payload, "hello world"));

  auto drained = decode_codec.Feed({});
  ASSERT_EQ(drained.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(drained.view.payload, "market-data"));

  FrameCodec masked_inbound_codec(32);
  std::array<std::byte, 128> masked_storage{};
  const auto masked_frame =
      encode_codec.EncodeText(AsBytes("masked"), masked_storage);
  ASSERT_TRUE(masked_frame.ok);

  auto protocol_error = masked_inbound_codec.Feed(masked_frame.bytes);
  EXPECT_EQ(protocol_error.status, DecodeStatus::kProtocolError);
}

TEST(WebsocketFrameCodecTest, DecodesPayloadAcrossMirroredBoundary) {
  FrameCodec codec(128, 4096);
  const auto filler = BuildServerTextFrame("x");
  const size_t target_offset = codec.ReceiveCapacity() - 5U;
  const size_t filler_count = target_offset / filler.size();
  for (size_t i = 0; i < filler_count; ++i) {
    auto filled = codec.Feed(filler);
    ASSERT_EQ(filled.status, DecodeStatus::kMessageReady);
    EXPECT_TRUE(PayloadEquals(filled.view.payload, "x"));
    EXPECT_EQ(codec.Poll().status, DecodeStatus::kNeedMore);
  }

  const auto wrapped = BuildServerTextFrame("abcdef");
  auto writable = codec.WritableSpan();
  ASSERT_GE(writable.size(), wrapped.size());
  std::copy(wrapped.begin(), wrapped.end(), writable.begin());
  codec.CommitWritten(wrapped.size());

  const auto decoded = codec.Poll();
  ASSERT_EQ(decoded.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(decoded.view.payload, "abcdef"));
}

TEST(WebsocketFrameCodecTest, DecodesCoalescedFramesByRepeatedPoll) {
  FrameCodec codec(128, 4096);
  const auto first_frame = BuildServerTextFrame("q");
  const auto second_frame = BuildServerTextFrame("r");
  std::vector<std::byte> coalesced;
  coalesced.reserve(first_frame.size() + second_frame.size());
  coalesced.insert(coalesced.end(), first_frame.begin(), first_frame.end());
  coalesced.insert(coalesced.end(), second_frame.begin(), second_frame.end());

  const auto decoded = codec.Feed(coalesced);
  ASSERT_EQ(decoded.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(decoded.view.payload, "q"));

  const auto next = codec.Poll();
  ASSERT_EQ(next.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(next.view.payload, "r"));
  EXPECT_EQ(codec.Poll().status, DecodeStatus::kNeedMore);
}

TEST(WebsocketFrameCodecTest, DecodesDirectBinaryAndExtendedTextDataFrames) {
  FrameCodec codec(256, 4096);
  const auto binary_frame = BuildServerBinaryFrame("bin");
  const std::string extended_payload(126, 'x');
  const auto extended_text_frame = BuildServerTextFrame(extended_payload);
  std::vector<std::byte> coalesced;
  coalesced.reserve(binary_frame.size() + extended_text_frame.size());
  coalesced.insert(coalesced.end(), binary_frame.begin(), binary_frame.end());
  coalesced.insert(coalesced.end(), extended_text_frame.begin(),
                   extended_text_frame.end());

  const auto binary = codec.Feed(coalesced);
  ASSERT_EQ(binary.status, DecodeStatus::kMessageReady);
  EXPECT_EQ(binary.view.kind, PayloadKind::kBinary);
  EXPECT_TRUE(PayloadEquals(binary.view.payload, "bin"));

  const auto text = codec.Poll();
  ASSERT_EQ(text.status, DecodeStatus::kMessageReady);
  EXPECT_EQ(text.view.kind, PayloadKind::kText);
  EXPECT_TRUE(PayloadEquals(text.view.payload, extended_payload));
  EXPECT_EQ(codec.Poll().status, DecodeStatus::kNeedMore);
}

TEST(WebsocketFrameCodecTest, QueuedCodecDrainsCoalescedFrames) {
  QueuedFrameCodec codec(128, 4096, 8);
  const auto first_frame = BuildServerTextFrame("one");
  const auto second_frame = BuildServerTextFrame("two");
  const auto third_frame = BuildServerTextFrame("three");
  std::vector<std::byte> coalesced;
  coalesced.reserve(first_frame.size() + second_frame.size() +
                    third_frame.size());
  coalesced.insert(coalesced.end(), first_frame.begin(), first_frame.end());
  coalesced.insert(coalesced.end(), second_frame.begin(), second_frame.end());
  coalesced.insert(coalesced.end(), third_frame.begin(), third_frame.end());

  const auto first = codec.Feed(coalesced);
  ASSERT_EQ(first.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(first.view.payload, "one"));

  const auto second = codec.Poll();
  ASSERT_EQ(second.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(second.view.payload, "two"));

  const auto third = codec.Poll();
  ASSERT_EQ(third.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(third.view.payload, "three"));
  EXPECT_EQ(codec.Poll().status, DecodeStatus::kNeedMore);
}

TEST(WebsocketFrameCodecTest, QueuedCodecReportsReadyQueueCapacity) {
  QueuedFrameCodec codec(128, 4096, 1);
  const auto first_frame = BuildServerTextFrame("a");
  const auto second_frame = BuildServerTextFrame("b");
  std::vector<std::byte> coalesced;
  coalesced.reserve(first_frame.size() + second_frame.size());
  coalesced.insert(coalesced.end(), first_frame.begin(), first_frame.end());
  coalesced.insert(coalesced.end(), second_frame.begin(), second_frame.end());

  const auto first = codec.Feed(coalesced);
  ASSERT_EQ(first.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(first.view.payload, "a"));
  EXPECT_EQ(codec.Poll().status, DecodeStatus::kCapacityExceeded);
}

TEST(WebsocketFrameCodecTest, QueuedCodecRejectsFastPathPayloadAboveMax) {
  QueuedFrameCodec short_codec(4, 4096, 8);
  const auto short_frame = BuildServerTextFrame("hello");
  EXPECT_EQ(short_codec.Feed(short_frame).status,
            DecodeStatus::kProtocolError);

  QueuedFrameCodec extended_codec(125, 4096, 8);
  const std::string extended_payload(126, 'x');
  const auto extended_frame = BuildServerTextFrame(extended_payload);
  EXPECT_EQ(extended_codec.Feed(extended_frame).status,
            DecodeStatus::kProtocolError);
}

TEST(WebsocketFrameCodecTest, RejectsNonMinimalExtendedPayloadLengths) {
  FrameCodec short_extended_codec(128, 4096);
  std::vector<std::byte> short_extended{
      std::byte{0x81}, std::byte{126}, std::byte{0}, std::byte{5},
      std::byte{'h'},  std::byte{'e'}, std::byte{'l'}, std::byte{'l'},
      std::byte{'o'}};
  EXPECT_EQ(short_extended_codec.Feed(short_extended).status,
            DecodeStatus::kProtocolError);

  FrameCodec long_extended_codec(256, 4096);
  std::vector<std::byte> long_extended(10 + 126, std::byte{'x'});
  long_extended[0] = std::byte{0x81};
  long_extended[1] = std::byte{127};
  for (size_t i = 2; i < 9; ++i) {
    long_extended[i] = std::byte{0};
  }
  long_extended[9] = std::byte{126};
  EXPECT_EQ(long_extended_codec.Feed(long_extended).status,
            DecodeStatus::kProtocolError);
}

TEST(WebsocketFrameCodecTest, ReportsCapacityExceededWhenReceiveRingInvalid) {
  FrameCodec codec(std::numeric_limits<size_t>::max(),
                   std::numeric_limits<size_t>::max());
  EXPECT_TRUE(codec.WritableSpan().empty());
  EXPECT_EQ(codec.Poll().status, DecodeStatus::kCapacityExceeded);
}
