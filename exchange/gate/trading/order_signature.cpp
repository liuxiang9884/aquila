#include "exchange/gate/trading/order_signature.h"

#include <array>
#include <cstddef>

#include <fmt/format.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace aquila::gate {
namespace {

constexpr std::size_t kGateSignatureInputBufferSize = 512;
constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

bool GenerateGateApiSignatureHex(
    std::string_view api_secret, std::string_view channel,
    std::string_view request_param, std::int64_t timestamp,
    std::array<char, kGateSignatureHexSize>& output) noexcept {
  std::array<char, kGateSignatureInputBufferSize> sign_input;
  const auto sign_input_result =
      fmt::format_to_n(sign_input.data(), sign_input.size(), "api\n{}\n{}\n{}",
                       channel, request_param, timestamp);
  if (sign_input_result.size > sign_input.size()) {
    return false;
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
  unsigned int digest_size = 0;
  const unsigned char* const hmac =
      HMAC(EVP_sha512(), api_secret.data(), static_cast<int>(api_secret.size()),
           reinterpret_cast<const unsigned char*>(sign_input.data()),
           sign_input_result.size, digest.data(), &digest_size);
  if (hmac == nullptr || digest_size * 2 != output.size()) {
    return false;
  }

  for (std::size_t i = 0; i < digest_size; ++i) {
    output[i * 2] = kHexDigits[(digest[i] >> 4) & 0x0F];
    output[i * 2 + 1] = kHexDigits[digest[i] & 0x0F];
  }
  return true;
}

}  // namespace aquila::gate
