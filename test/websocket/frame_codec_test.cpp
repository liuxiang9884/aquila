#include "core/websocket/frame_codec.h"

#include <array>
#include <span>
#include <string_view>
#include <vector>

using namespace aquila::websocket;

namespace {

std::vector<std::byte> BuildServerTextFrame(std::string_view payload) {
  std::vector<std::byte> frame(2 + payload.size());
  frame[0] = std::byte{0x81};
  frame[1] = std::byte{static_cast<unsigned char>(payload.size())};
  for (size_t i = 0; i < payload.size(); ++i) {
    frame[2 + i] = static_cast<std::byte>(payload[i]);
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

int main() {
  FrameCodec encode_codec(1024);
  constexpr std::string_view kMaskProbePayload = "tick";
  std::array<std::array<std::byte, 128>, 8> encode_storage{};
  std::array<std::byte, 4> first_mask_key{};
  bool saw_different_mask_key = false;
  for (size_t i = 0; i < encode_storage.size(); ++i) {
    const auto encoded =
        encode_codec.EncodeText(AsBytes(kMaskProbePayload), encode_storage[i]);
    if (!encoded.ok) {
      return 1;
    }
    if ((std::to_integer<unsigned char>(encoded.bytes[1]) & 0x80U) == 0) {
      return 1;
    }

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
  if (!saw_different_mask_key) {
    return 1;
  }

  FrameCodec decode_codec(11);
  auto first_frame = BuildServerTextFrame("hello world");
  auto second_frame = BuildServerTextFrame("market-data");
  std::vector<std::byte> coalesced;
  coalesced.reserve(first_frame.size() + second_frame.size());
  coalesced.insert(coalesced.end(), first_frame.begin(), first_frame.end());
  coalesced.insert(coalesced.end(), second_frame.begin(), second_frame.end());

  auto decoded = decode_codec.Feed(coalesced);
  if (decoded.status != DecodeStatus::kMessageReady ||
      !PayloadEquals(decoded.view.payload, "hello world")) {
    return 1;
  }

  auto drained = decode_codec.Feed({});
  if (drained.status != DecodeStatus::kMessageReady ||
      !PayloadEquals(drained.view.payload, "market-data")) {
    return 1;
  }

  FrameCodec masked_inbound_codec(32);
  std::array<std::byte, 128> masked_storage{};
  const auto masked_frame =
      encode_codec.EncodeText(AsBytes("masked"), masked_storage);
  if (!masked_frame.ok) {
    return 1;
  }

  auto protocol_error = masked_inbound_codec.Feed(masked_frame.bytes);
  return protocol_error.status == DecodeStatus::kProtocolError ? 0 : 1;
}
