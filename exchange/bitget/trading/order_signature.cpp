#include "exchange/bitget/trading/order_signature.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>

#include <fmt/format.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace aquila::bitget {

bool GenerateBitgetLoginSignatureBase64(
    std::string_view api_secret, std::int64_t timestamp_seconds,
    std::array<char, kBitgetLoginSignatureBase64Size>& output) noexcept {
  if (api_secret.empty() || api_secret.size() > INT_MAX ||
      timestamp_seconds <= 0) {
    return false;
  }

  std::array<char, 64> sign_input{};
  const auto input_result =
      fmt::format_to_n(sign_input.data(), sign_input.size(),
                       "{}GET/user/verify", timestamp_seconds);
  if (input_result.size > sign_input.size()) {
    return false;
  }

  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  const unsigned char* hmac =
      HMAC(EVP_sha256(), api_secret.data(), static_cast<int>(api_secret.size()),
           reinterpret_cast<const unsigned char*>(sign_input.data()),
           input_result.size, digest.data(), &digest_size);
  if (hmac == nullptr || digest_size != 32) {
    return false;
  }

  std::array<unsigned char, kBitgetLoginSignatureBase64Size + 1> encoded{};
  const int encoded_size =
      EVP_EncodeBlock(encoded.data(), digest.data(), digest_size);
  if (encoded_size != static_cast<int>(output.size())) {
    return false;
  }
  std::copy_n(reinterpret_cast<const char*>(encoded.data()), output.size(),
              output.data());
  return true;
}

}  // namespace aquila::bitget
