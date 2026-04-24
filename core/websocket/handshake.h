#ifndef AQUILA_CORE_WEBSOCKET_HANDSHAKE_H_
#define AQUILA_CORE_WEBSOCKET_HANDSHAKE_H_

#include <array>
#include <cctype>
#include <cstddef>
#include <span>
#include <string_view>

#include <fmt/compile.h>
#include <fmt/format.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace aquila::websocket {

struct HandshakeBuildResult {
  bool ok{false};
  std::string_view bytes{};
};

namespace detail {

inline constexpr std::string_view kWebsocketGuid =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

inline bool AsciiEqualsIgnoreCase(std::string_view lhs,
                                  std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    const auto left =
        static_cast<unsigned char>(static_cast<unsigned char>(lhs[i]));
    const auto right =
        static_cast<unsigned char>(static_cast<unsigned char>(rhs[i]));
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

inline std::string_view TrimAsciiWhitespace(std::string_view value) noexcept {
  size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return value.substr(begin, end - begin);
}

inline bool ContainsTokenIgnoreCase(std::string_view header_value,
                                    std::string_view token) noexcept {
  size_t cursor = 0;
  while (cursor < header_value.size()) {
    const size_t comma = header_value.find(',', cursor);
    const size_t token_end =
        comma == std::string_view::npos ? header_value.size() : comma;
    const auto candidate =
        TrimAsciiWhitespace(header_value.substr(cursor, token_end - cursor));
    if (AsciiEqualsIgnoreCase(candidate, token)) {
      return true;
    }
    if (comma == std::string_view::npos) {
      break;
    }
    cursor = comma + 1;
  }
  return false;
}

inline bool HasSwitchingProtocolsStatus(std::string_view status_line) noexcept {
  constexpr std::string_view kHttp11Prefix = "HTTP/1.1 ";
  constexpr std::string_view kHttp10Prefix = "HTTP/1.0 ";

  std::string_view status_code{};
  if (status_line.rfind(kHttp11Prefix, 0) == 0) {
    status_code = status_line.substr(kHttp11Prefix.size(), 3);
    if (status_line.size() < kHttp11Prefix.size() + 4) {
      return false;
    }
    return status_code == "101" &&
           status_line[kHttp11Prefix.size() + 3] == ' ';
  }
  if (status_line.rfind(kHttp10Prefix, 0) == 0) {
    status_code = status_line.substr(kHttp10Prefix.size(), 3);
    if (status_line.size() < kHttp10Prefix.size() + 4) {
      return false;
    }
    return status_code == "101" &&
           status_line[kHttp10Prefix.size() + 3] == ' ';
  }
  return false;
}

inline bool ComputeAcceptKey(std::string_view client_key,
                             std::array<char, 64>& output,
                             std::string_view& accept_key) noexcept {
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_size = 0;

  EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
  if (md_ctx == nullptr) {
    return false;
  }
  const bool digest_ok =
      EVP_DigestInit_ex(md_ctx, EVP_sha1(), nullptr) == 1 &&
      EVP_DigestUpdate(md_ctx, client_key.data(), client_key.size()) == 1 &&
      EVP_DigestUpdate(md_ctx, kWebsocketGuid.data(), kWebsocketGuid.size()) == 1 &&
      EVP_DigestFinal_ex(md_ctx, digest, &digest_size) == 1;
  EVP_MD_CTX_free(md_ctx);
  if (!digest_ok) {
    return false;
  }

  const int encoded_size =
      EVP_EncodeBlock(reinterpret_cast<unsigned char*>(output.data()), digest,
                      static_cast<int>(digest_size));
  if (encoded_size <= 0 ||
      static_cast<size_t>(encoded_size) > output.size()) {
    return false;
  }

  accept_key = std::string_view(output.data(),
                                static_cast<size_t>(encoded_size));
  return true;
}

}  // namespace detail

// Generate a per-connection 16-byte random Sec-WebSocket-Key as base64.
// Output must hold at least 24 bytes; returns a view into `output` on success
// or an empty view on RNG / encoding failure.
inline std::string_view GenerateClientKey(std::span<char> output) noexcept {
  constexpr size_t kRawKeyBytes = 16;
  constexpr size_t kEncodedBytes = 24;
  if (output.size() < kEncodedBytes) {
    return {};
  }

  unsigned char raw[kRawKeyBytes];
  if (RAND_bytes(raw, static_cast<int>(kRawKeyBytes)) != 1) {
    return {};
  }

  const int encoded_size = EVP_EncodeBlock(
      reinterpret_cast<unsigned char*>(output.data()), raw,
      static_cast<int>(kRawKeyBytes));
  if (encoded_size != static_cast<int>(kEncodedBytes)) {
    return {};
  }
  return std::string_view(output.data(), kEncodedBytes);
}

inline HandshakeBuildResult BuildClientHandshake(std::string_view host,
                                                 std::string_view target,
                                                 std::string_view client_key,
                                                 std::span<char> output) noexcept {
  if (host.empty() || target.empty() || client_key.empty() || output.empty()) {
    return {};
  }

  const auto result = fmt::format_to_n(
      output.data(), output.size(),
      FMT_COMPILE("GET {} HTTP/1.1\r\n"
                  "Host: {}\r\n"
                  "Upgrade: websocket\r\n"
                  "Connection: Upgrade\r\n"
                  "Sec-WebSocket-Key: {}\r\n"
                  "Sec-WebSocket-Version: 13\r\n"
                  "\r\n"),
      target, host, client_key);
  if (result.size == 0 || result.size > output.size()) {
    return {};
  }

  return {true,
          std::string_view(output.data(), result.size)};
}

inline bool ValidateServerHandshake(std::string_view response,
                                    std::string_view client_key) noexcept {
  if (response.empty() || client_key.empty()) {
    return false;
  }

  const size_t status_end = response.find("\r\n");
  if (status_end == std::string_view::npos) {
    return false;
  }
  const std::string_view status_line = response.substr(0, status_end);
  if (!detail::HasSwitchingProtocolsStatus(status_line)) {
    return false;
  }

  std::array<char, 64> accept_storage{};
  std::string_view expected_accept{};
  if (!detail::ComputeAcceptKey(client_key, accept_storage, expected_accept)) {
    return false;
  }

  bool saw_upgrade = false;
  bool saw_connection = false;
  bool saw_accept = false;

  size_t line_begin = status_end + 2;
  while (line_begin <= response.size()) {
    const size_t line_end = response.find("\r\n", line_begin);
    if (line_end == std::string_view::npos) {
      return false;
    }
    if (line_end == line_begin) {
      break;
    }

    const std::string_view line = response.substr(line_begin, line_end - line_begin);
    const size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
      return false;
    }
    const auto name = detail::TrimAsciiWhitespace(line.substr(0, colon));
    const auto value = detail::TrimAsciiWhitespace(line.substr(colon + 1));

    if (detail::AsciiEqualsIgnoreCase(name, "Upgrade")) {
      saw_upgrade = detail::AsciiEqualsIgnoreCase(value, "websocket");
    } else if (detail::AsciiEqualsIgnoreCase(name, "Connection")) {
      saw_connection = detail::ContainsTokenIgnoreCase(value, "Upgrade");
    } else if (detail::AsciiEqualsIgnoreCase(name, "Sec-WebSocket-Accept")) {
      saw_accept = value == expected_accept;
    }

    line_begin = line_end + 2;
  }

  return saw_upgrade && saw_connection && saw_accept;
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_HANDSHAKE_H_
