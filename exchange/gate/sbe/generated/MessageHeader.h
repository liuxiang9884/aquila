#ifndef AQUILA_EXCHANGE_GATE_SBE_GENERATED_MESSAGE_HEADER_H_
#define AQUILA_EXCHANGE_GATE_SBE_GENERATED_MESSAGE_HEADER_H_

#include <cstddef>
#include <cstdint>

namespace gate::sbe {

class MessageHeader {
 public:
  static constexpr size_t encodedLength() noexcept { return 8; }

  void wrap(const char* buffer,
            size_t offset,
            std::uint16_t /* acting_version */,
            size_t buffer_size) noexcept {
    data_ = buffer;
    offset_ = offset;
    size_ = buffer_size;
  }

  std::uint16_t blockLength() const noexcept { return ReadUint16(0); }
  std::uint16_t templateId() const noexcept { return ReadUint16(2); }
  std::uint16_t schemaId() const noexcept { return ReadUint16(4); }
  std::uint16_t version() const noexcept { return ReadUint16(6); }

 private:
  std::uint16_t ReadUint16(size_t relative_offset) const noexcept {
    if (data_ == nullptr ||
        offset_ + relative_offset + sizeof(std::uint16_t) > size_) {
      return 0;
    }
    const auto* bytes =
        reinterpret_cast<const unsigned char*>(data_ + offset_ +
                                               relative_offset);
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1] << 8U);
  }

  const char* data_{nullptr};
  size_t offset_{0};
  size_t size_{0};
};

}  // namespace gate::sbe

#endif  // AQUILA_EXCHANGE_GATE_SBE_GENERATED_MESSAGE_HEADER_H_
