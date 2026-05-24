#include "core/websocket/handshake.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>
#include <openssl/evp.h>

using namespace aquila::websocket;

TEST(WebsocketHandshakeTest, BuildsClientHandshakeAndValidatesServerResponse) {
  constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  std::array<char, 512> output{};
  auto built = BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, output);
  ASSERT_TRUE(built.ok);
  EXPECT_NE(built.bytes.find("Upgrade: websocket"), std::string_view::npos);

  constexpr std::string_view kValidResponse =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
      "\r\n";
  EXPECT_TRUE(ValidateServerHandshake(kValidResponse, kClientKey));

  constexpr std::string_view kInvalidStatusResponse =
      "HTTP/1.1 1010 Not Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
      "\r\n";
  EXPECT_FALSE(ValidateServerHandshake(kInvalidStatusResponse, kClientKey));

  constexpr std::string_view kInvalidAcceptResponse =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: invalid-accept-value=\r\n"
      "\r\n";
  EXPECT_FALSE(ValidateServerHandshake(kInvalidAcceptResponse, kClientKey));
}

TEST(WebsocketHandshakeTest, BuildsClientHandshakeWithExtraHeaders) {
  constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  const std::vector<HttpHeader> extra_headers{
      {.name = "X-Gate-Size-Decimal", .value = "1"},
      {.name = "X-Test-Mode", .value = "decimal"},
  };
  std::array<char, 512> output{};

  auto built = BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, extra_headers, output);

  ASSERT_TRUE(built.ok);
  EXPECT_NE(built.bytes.find("Sec-WebSocket-Version: 13\r\n"
                             "X-Gate-Size-Decimal: 1\r\n"
                             "X-Test-Mode: decimal\r\n"
                             "\r\n"),
            std::string_view::npos);
}

TEST(WebsocketHandshakeTest, RejectsUnsafeExtraHeaderNamesAndValues) {
  constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  std::array<char, 512> output{};

  const std::vector<HttpHeader> bad_name{
      {.name = "X-Gate-Size-Decimal\r\nInjected", .value = "1"},
  };
  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, bad_name, output)
                   .ok);

  const std::vector<HttpHeader> bad_value{
      {.name = "X-Gate-Size-Decimal", .value = "1\r\nInjected: true"},
  };
  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, bad_value, output)
                   .ok);

  const std::vector<HttpHeader> non_ascii_value{
      {.name = "X-Gate-Size-Decimal", .value = std::string("1") + '\x80'},
  };
  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, non_ascii_value, output)
                   .ok);
}

TEST(WebsocketHandshakeTest, RejectsReservedAndDuplicateExtraHeaders) {
  constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  std::array<char, 512> output{};

  const std::vector<HttpHeader> reserved_header{
      {.name = "host", .value = "example.com"},
  };
  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, reserved_header, output)
                   .ok);

  const std::vector<HttpHeader> websocket_extension_header{
      {.name = "Sec-WebSocket-Extensions", .value = "permessage-deflate"},
  };
  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, websocket_extension_header,
                                    output)
                   .ok);

  const std::vector<HttpHeader> websocket_protocol_header{
      {.name = "Sec-WebSocket-Protocol", .value = "chat"},
  };
  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, websocket_protocol_header,
                                    output)
                   .ok);

  const std::vector<HttpHeader> duplicate_header{
      {.name = "X-Gate-Size-Decimal", .value = "1"},
      {.name = "x-gate-size-decimal", .value = "1"},
  };
  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, duplicate_header, output)
                   .ok);
}

TEST(WebsocketHandshakeTest, RejectsTooSmallHandshakeOutputBuffer) {
  constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  const std::vector<HttpHeader> extra_headers{
      {.name = "X-Gate-Size-Decimal", .value = "1"},
  };
  std::array<char, 32> output{};

  EXPECT_FALSE(BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt",
                                    kClientKey, extra_headers, output)
                   .ok);
}

TEST(WebsocketHandshakeTest, GenerateClientKeyProducesUniqueBase64Keys) {
  std::array<char, 32> first_storage{};
  std::array<char, 32> second_storage{};

  const auto first = GenerateClientKey(first_storage);
  const auto second = GenerateClientKey(second_storage);

  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  EXPECT_EQ(first.size(), size_t{24});
  EXPECT_EQ(second.size(), size_t{24});
  EXPECT_NE(first, second);

  // Base64 round-trip must yield 16 raw bytes per RFC 6455 §4.1.
  std::array<std::uint8_t, 32> decoded{};
  const int decoded_len = EVP_DecodeBlock(
      decoded.data(), reinterpret_cast<const unsigned char*>(first.data()),
      static_cast<int>(first.size()));
  ASSERT_GT(decoded_len, 0);
  // EVP_DecodeBlock returns the padded length; subtract trailing '=' bytes.
  size_t pad = 0;
  for (auto it = first.rbegin(); it != first.rend() && *it == '='; ++it) {
    ++pad;
  }
  EXPECT_EQ(static_cast<size_t>(decoded_len) - pad, size_t{16});
}
