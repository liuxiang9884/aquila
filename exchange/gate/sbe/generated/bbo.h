#ifndef AQUILA_EXCHANGE_GATE_SBE_GENERATED_BBO_H_
#define AQUILA_EXCHANGE_GATE_SBE_GENERATED_BBO_H_

#include "exchange/gate/sbe/generated/Event.h"
#include "exchange/gate/sbe/generated/MessageHeader.h"
#include "exchange/gate/sbe/generated/gate/messages/bbo.hpp"

#include <cstddef>
#include <cstdint>

#include <sbepp/sbepp.hpp>

namespace gate::sbe {

class bbo {
 public:
  static constexpr std::uint16_t sbeBlockLength() noexcept { return 59; }
  static constexpr std::uint16_t sbeTemplateId() noexcept { return 1; }
  static constexpr std::uint16_t sbeSchemaId() noexcept { return 1; }
  static constexpr std::uint16_t sbeSchemaVersion() noexcept { return 1; }

  void wrapForDecode(const char* buffer,
                     size_t offset,
                     std::uint16_t /* acting_block_length */,
                     std::uint16_t /* acting_version */,
                     size_t buffer_size) noexcept {
    if (buffer == nullptr || offset > buffer_size) {
      data_ = nullptr;
      size_ = 0;
      return;
    }

    const size_t message_offset =
        offset >= MessageHeader::encodedLength()
            ? offset - MessageHeader::encodedLength()
            : offset;
    data_ = buffer + message_offset;
    size_ = buffer_size - message_offset;
  }

  std::int64_t time() const noexcept { return View().time().value(); }

  Event e() const noexcept {
    return static_cast<Event>(static_cast<std::int8_t>(View().e()));
  }

  std::int64_t t() const noexcept { return View().t().value(); }
  std::int64_t u() const noexcept { return View().u().value(); }
  std::int8_t pxExponent() const noexcept {
    return View().pxExponent().value();
  }
  std::int8_t szExponent() const noexcept {
    return View().szExponent().value();
  }
  std::int64_t askMantissaPrice() const noexcept {
    return View().askMantissaPrice().value();
  }
  std::int64_t askMantissaSize() const noexcept {
    return View().askMantissaSize().value();
  }
  std::int64_t bidMantissaPrice() const noexcept {
    return View().bidMantissaPrice().value();
  }
  std::int64_t bidMantissaSize() const noexcept {
    return View().bidMantissaSize().value();
  }

 private:
  ::gate::messages::bbo<const char> View() const noexcept {
    return ::sbepp::make_const_view<::gate::messages::bbo>(data_, size_);
  }

  const char* data_{nullptr};
  size_t size_{0};
};

}  // namespace gate::sbe

#endif  // AQUILA_EXCHANGE_GATE_SBE_GENERATED_BBO_H_
