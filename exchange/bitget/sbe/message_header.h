#ifndef AQUILA_EXCHANGE_BITGET_SBE_MESSAGE_HEADER_H_
#define AQUILA_EXCHANGE_BITGET_SBE_MESSAGE_HEADER_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aquila::bitget {

inline constexpr size_t kSbeMessageHeaderBytes = 8;

struct SbeMessageHeader {
  std::uint16_t block_length{0};
  std::uint16_t template_id{0};
  std::uint16_t schema_id{0};
  std::uint16_t version{0};
};

namespace detail {

inline std::uint16_t ReadUint16LittleEndianUnchecked(std::string_view payload,
                                                     size_t offset) noexcept {
  const auto* bytes =
      reinterpret_cast<const unsigned char*>(payload.data() + offset);
  return static_cast<std::uint16_t>(bytes[0]) |
         static_cast<std::uint16_t>(bytes[1] << 8U);
}

}  // namespace detail

inline bool ParseSbeMessageHeader(std::string_view payload,
                                  SbeMessageHeader* out) noexcept {
  assert(out != nullptr);
  if (payload.size() < kSbeMessageHeaderBytes) {
    return false;
  }

  out->block_length = detail::ReadUint16LittleEndianUnchecked(payload, 0);
  out->template_id = detail::ReadUint16LittleEndianUnchecked(payload, 2);
  out->schema_id = detail::ReadUint16LittleEndianUnchecked(payload, 4);
  out->version = detail::ReadUint16LittleEndianUnchecked(payload, 6);
  return true;
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_SBE_MESSAGE_HEADER_H_
