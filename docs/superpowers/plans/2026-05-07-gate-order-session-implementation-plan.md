# Gate OrderSession Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `aquila::gate::OrderSession`，支持 Gate futures WS login、常规下单、命令撤单和轻量 submit response 关联。

**Architecture:** Strategy 与 `OrderSession` 在同一线程内运行；Strategy 负责风控、订单对象、状态机和 Gate wire fields 缓存，`OrderSession` 只负责 Gate 协议适配、固定缓冲区编码、WebSocket 写入、`request_id` 关联和同步 `OrderResponse` 回调。私有订单 / 成交 / 仓位回报留给后续 `OrderFeedbackSession`，不进入本计划。

**Tech Stack:** C++20, CMake, GoogleTest, benchmark, simdjson ondemand, fmtlib header-only, Abseil flat_hash_map, OpenSSL HMAC-SHA512, existing `core/websocket` client.

---

## 执行状态

截至 2026-05-07，Task 1 到 Task 6 已完成并按阶段提交。当前实现入口包括 `exchange/gate/trading/order_types.h`、
`order_codecs.h`、`order_signature.h/.cpp`、`order_request_encoder.h`、`submit_response_parser.h`、
`order_session.h`、对应 `test/exchange/gate/trading/*` 测试和
`benchmark/exchange/gate/trading/order_session_benchmark.cpp`。

最新事实源以当前代码、`doc/project_onboarding_guide.md` 和
`doc/agent-handoff-gate-trade-architecture.md` 为准；本计划保留为任务拆分、设计约束和验证命令追溯。

## 事实源

- Design spec: `docs/superpowers/specs/2026-05-07-gate-order-session-design.md`
- Existing parser: `exchange/gate/trading/submit_response_parser.h`
- Existing tests: `test/exchange/gate/trading/submit_response_parser_test.cpp`
- Existing benchmark: `benchmark/exchange/gate/trading/submit_response_parse_benchmark.cpp`
- Existing session style: `exchange/gate/market_data/data_session.h`
- Sirius reference: `third_party/sirius/exchange/gate/trade/trade_engine.cpp`

## 文件结构

- Create `exchange/gate/trading/order_types.h`: submit/cancel 请求、响应、本地 send status、diagnostics stats 的轻量类型。
- Create `exchange/gate/trading/order_codecs.h`: `RequestIdCodec` 和 `OrderTextCodec`，不依赖 WebSocket。
- Create `exchange/gate/trading/order_signature.h`: Gate WS API login 签名 API。
- Create `exchange/gate/trading/order_signature.cpp`: OpenSSL HMAC-SHA512 hex 实现。
- Create `exchange/gate/trading/order_request_encoder.h`: login/place/cancel JSON request 固定缓冲区编码。
- Modify `exchange/gate/trading/submit_response_parser.h`: 从 hash-only correlation 升级到 decoded request id / local order id，同时保留现有 hash 字段，避免一次性改动 benchmark 对照。
- Create `exchange/gate/trading/order_session.h`: 模板化 `OrderSession<Handler, WebSocketPolicy, DiagnosticsPolicy>`，持有 `BasicWebSocketClient`，处理 login、place、cancel 和 text response。
- Modify `exchange/gate/CMakeLists.txt`: 把 signature 源文件和 OpenSSL::Crypto 链接加入 `aquila_gate`。
- Create `test/exchange/gate/trading/order_codecs_test.cpp`: codec 单元测试。
- Create `test/exchange/gate/trading/order_request_encoder_test.cpp`: request encoder 单元测试。
- Modify `test/exchange/gate/trading/submit_response_parser_test.cpp`: 增加 decoded correlation 测试。
- Create `test/exchange/gate/trading/order_session_test.cpp`: fake handler + fake phase/message 驱动的 session 行为测试。
- Modify `test/exchange/gate/trading/CMakeLists.txt`: 增加三个 test target。
- Create `benchmark/exchange/gate/trading/order_session_benchmark.cpp`: encode、parse、dispatch microbenchmark。
- Modify `benchmark/exchange/gate/trading/CMakeLists.txt`: 增加 benchmark target。
- Modify `doc/project_onboarding_guide.md`: 增加 OrderSession 代码入口、验证命令和下一步建议。
- Modify `doc/agent-handoff-gate-trade-architecture.md`: 同步 OrderSession 第一版边界和实现状态。

---

### Task 1: Order Types And Codecs

**Files:**
- Create: `exchange/gate/trading/order_types.h`
- Create: `exchange/gate/trading/order_codecs.h`
- Create: `test/exchange/gate/trading/order_codecs_test.cpp`
- Modify: `test/exchange/gate/trading/CMakeLists.txt`

- [ ] **Step 1: 写失败测试**

Create `test/exchange/gate/trading/order_codecs_test.cpp`:

```cpp
#include "exchange/gate/trading/order_codecs.h"

#include <array>
#include <cstdint>
#include <string_view>

#include <gtest/gtest.h>

namespace aquila::gate {
namespace {

TEST(RequestIdCodecTest, EncodesTypeInHighEightBits) {
  constexpr std::uint64_t sequence = 42;

  const std::uint64_t encoded =
      RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
  const DecodedRequestId decoded = RequestIdCodec::Decode(encoded);

  EXPECT_TRUE(decoded.ok);
  EXPECT_EQ(decoded.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(decoded.sequence, sequence);
  EXPECT_EQ(encoded >> 56, 2U);
}

TEST(RequestIdCodecTest, MasksSequenceToLowFiftySixBits) {
  constexpr std::uint64_t sequence = 0x1FFFFFFFFFFFFFFULL;

  const DecodedRequestId decoded = RequestIdCodec::Decode(
      RequestIdCodec::Encode(OrderRequestType::kCancelOrder, sequence));

  EXPECT_TRUE(decoded.ok);
  EXPECT_EQ(decoded.type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(decoded.sequence, 0x00FFFFFFFFFFFFFFULL);
}

TEST(RequestIdCodecTest, RejectsUnknownRequestType) {
  const DecodedRequestId decoded = RequestIdCodec::Decode(0x7F00000000000001ULL);

  EXPECT_FALSE(decoded.ok);
  EXPECT_EQ(decoded.type, OrderRequestType::kUnknown);
  EXPECT_EQ(decoded.sequence, 1U);
}

TEST(OrderTextCodecTest, FormatsAndParsesStandardOrderText) {
  std::array<char, 32> buffer{};

  const std::string_view text = OrderTextCodec::Format(123456789, buffer);
  const ParsedOrderText parsed = OrderTextCodec::Parse(text);

  EXPECT_EQ(text, "t-123456789");
  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 123456789);
}

TEST(OrderTextCodecTest, RejectsUnsupportedPrefix) {
  const ParsedOrderText parsed = OrderTextCodec::Parse("ao-100");

  EXPECT_FALSE(parsed.ok);
  EXPECT_EQ(parsed.local_order_id, 0);
}

TEST(OrderTextCodecTest, ReportsTooSmallFormatBuffer) {
  std::array<char, 4> buffer{};

  const std::string_view text = OrderTextCodec::Format(123456789, buffer);

  EXPECT_TRUE(text.empty());
}

}  // namespace
}  // namespace aquila::gate
```

- [ ] **Step 2: 加入 test target 并确认失败**

Modify `test/exchange/gate/trading/CMakeLists.txt`:

```cmake
add_executable(gate_order_codecs_test
    order_codecs_test.cpp
)

target_link_libraries(gate_order_codecs_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_codecs_test
         COMMAND gate_order_codecs_test)
```

Run:

```bash
./build.sh debug
```

Expected: configure or compile fails because `exchange/gate/trading/order_codecs.h` does not exist.

- [ ] **Step 3: 实现轻量类型**

Create `exchange/gate/trading/order_types.h`:

```cpp
#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_TYPES_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_TYPES_H_

#include <cstdint>
#include <string_view>

namespace aquila::gate {

enum class OrderRequestType : std::uint8_t {
  kUnknown = 0,
  kLogin = 1,
  kPlaceOrder = 2,
  kCancelOrder = 3,
};

struct DecodedRequestId {
  bool ok{false};
  OrderRequestType type{OrderRequestType::kUnknown};
  std::uint64_t sequence{0};
};

struct ParsedOrderText {
  bool ok{false};
  std::int64_t local_order_id{0};
};

struct OrderWireFields {
  std::int64_t local_order_id{0};
  std::string_view contract{};
  std::int64_t signed_size{0};
  std::string_view price_text{};
  std::string_view tif{};
  std::string_view text{};
  bool reduce_only{false};
};

struct PlaceOrderRequest {
  OrderWireFields wire{};
};

struct CancelOrderRequest {
  std::int64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
};

enum class OrderSendStatus : std::uint8_t {
  kOk,
  kNotLoggedIn,
  kNotActive,
  kInflightFull,
  kEncodeBufferTooSmall,
  kNoPreparedWriteSlot,
  kWriteUnavailable,
};

struct OrderSendResult {
  OrderSendStatus status{OrderSendStatus::kNotActive};
  std::uint64_t request_sequence{0};
  std::uint64_t encoded_request_id{0};
};

enum class OrderResponseKind : std::uint8_t {
  kAck,
  kAccepted,
  kRejected,
  kCancelAccepted,
  kCancelRejected,
};

struct OrderResponse {
  OrderResponseKind kind{OrderResponseKind::kAck};
  std::int64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint16_t http_status{0};
  std::uint64_t error_label_hash{0};
};

struct OrderSessionStats {
  std::uint64_t text_messages{0};
  std::uint64_t parse_errors{0};
  std::uint64_t ignored_messages{0};
  std::uint64_t login_sent{0};
  std::uint64_t login_accepted{0};
  std::uint64_t login_rejected{0};
  std::uint64_t place_sent{0};
  std::uint64_t cancel_sent{0};
  std::uint64_t responses{0};
  std::uint64_t unknown_request_ids{0};
  std::uint64_t local_send_failures{0};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_TYPES_H_
```

- [ ] **Step 4: 实现 codecs**

Create `exchange/gate/trading/order_codecs.h`:

```cpp
#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_CODECS_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_CODECS_H_

#include <array>
#include <charconv>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include "exchange/gate/trading/order_types.h"
#include <fmt/format.h>

namespace aquila::gate {

class RequestIdCodec {
 public:
  static constexpr std::uint64_t kSequenceMask = 0x00FFFFFFFFFFFFFFULL;

  [[nodiscard]] static constexpr std::uint64_t Encode(
      OrderRequestType type, std::uint64_t sequence) noexcept {
    return (static_cast<std::uint64_t>(type) << 56) |
           (sequence & kSequenceMask);
  }

  [[nodiscard]] static constexpr DecodedRequestId Decode(
      std::uint64_t encoded) noexcept {
    const auto type_value = static_cast<std::uint8_t>(encoded >> 56);
    const std::uint64_t sequence = encoded & kSequenceMask;
    switch (static_cast<OrderRequestType>(type_value)) {
      case OrderRequestType::kLogin:
      case OrderRequestType::kPlaceOrder:
      case OrderRequestType::kCancelOrder:
        return {.ok = true,
                .type = static_cast<OrderRequestType>(type_value),
                .sequence = sequence};
      case OrderRequestType::kUnknown:
        return {.ok = false,
                .type = OrderRequestType::kUnknown,
                .sequence = sequence};
    }
    return {.ok = false,
            .type = OrderRequestType::kUnknown,
            .sequence = sequence};
  }
};

class OrderTextCodec {
 public:
  template <std::size_t N>
  [[nodiscard]] static std::string_view Format(
      std::int64_t local_order_id, std::array<char, N>& output) noexcept {
    const auto result =
        fmt::format_to_n(output.data(), output.size(), "t-{}", local_order_id);
    if (result.size > output.size()) {
      return {};
    }
    return std::string_view(output.data(), result.size);
  }

  [[nodiscard]] static ParsedOrderText Parse(std::string_view text) noexcept {
    if (text.size() <= 2 || text[0] != 't' || text[1] != '-') {
      return {};
    }
    std::int64_t local_order_id = 0;
    const char* first = text.data() + 2;
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, local_order_id);
    if (result.ec != std::errc{} || result.ptr != last ||
        local_order_id <= 0) {
      return {};
    }
    return {.ok = true, .local_order_id = local_order_id};
  }
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_CODECS_H_
```

- [ ] **Step 5: 跑测试并提交**

Run:

```bash
./build.sh debug
./build/debug/test/exchange/gate/trading/gate_order_codecs_test
```

Expected: both commands pass.

Commit:

```bash
git add exchange/gate/trading/order_types.h exchange/gate/trading/order_codecs.h \
  test/exchange/gate/trading/order_codecs_test.cpp test/exchange/gate/trading/CMakeLists.txt
git commit -m "feat: add gate order codecs"
```

---

### Task 2: Login Signature And Request Encoder

**Files:**
- Create: `exchange/gate/trading/order_signature.h`
- Create: `exchange/gate/trading/order_signature.cpp`
- Create: `exchange/gate/trading/order_request_encoder.h`
- Create: `test/exchange/gate/trading/order_request_encoder_test.cpp`
- Modify: `exchange/gate/CMakeLists.txt`
- Modify: `test/exchange/gate/trading/CMakeLists.txt`

- [ ] **Step 1: 写失败测试**

Create `test/exchange/gate/trading/order_request_encoder_test.cpp`:

```cpp
#include "exchange/gate/trading/order_request_encoder.h"

#include <array>
#include <string_view>

#include <gtest/gtest.h>

namespace aquila::gate {
namespace {

TEST(OrderSignatureTest, MatchesGateLoginSignatureShape) {
  std::array<char, kGateSignatureHexSize> signature{};

  const bool ok = GenerateGateApiSignatureHex(
      "secret", "futures.login", "", 1700000000, signature);

  EXPECT_TRUE(ok);
  EXPECT_EQ(std::string_view(signature.data(), signature.size()),
            "f39035057b3528fc2c5aff4b9cfa9f43673c88d3ff823c55468608173205809999a8b45d7ed898ebf49c15a4f6e5131de175ded143be5eeb58431f600e1d4085");
}

TEST(OrderRequestEncoderTest, EncodesLoginRequest) {
  std::array<char, kLoginRequestBufferSize> buffer{};

  const EncodedTextRequest encoded = EncodeLoginRequest(
      LoginRequestFields{.api_key = "key",
                         .api_secret = "secret",
                         .timestamp = 1700000000,
                         .encoded_request_id = 72057594037927937ULL},
      buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_NE(encoded.text.find("\"channel\":\"futures.login\""),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find("\"api_key\":\"key\""), std::string_view::npos);
  EXPECT_NE(encoded.text.find("\"timestamp\":\"1700000000\""),
            std::string_view::npos);
  EXPECT_NE(encoded.text.find("\"req_id\":\"72057594037927937\""),
            std::string_view::npos);
}

TEST(OrderRequestEncoderTest, EncodesPlaceOrderRequest) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      PlaceOrderEncodeFields{
          .timestamp = 1700000001,
          .encoded_request_id = 144115188075855873ULL,
          .wire = OrderWireFields{.local_order_id = 9,
                                  .contract = "BTC_USDT",
                                  .signed_size = 1,
                                  .price_text = "81000",
                                  .tif = "gtc",
                                  .text = "t-9",
                                  .reduce_only = false}},
      buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(encoded.text,
            R"({"time":1700000001,"channel":"futures.order_place","event":"api","payload":{"req_id":"144115188075855873","req_param":{"contract":"BTC_USDT","size":1,"price":"81000","tif":"gtc","text":"t-9","reduce_only":false}}})");
}

TEST(OrderRequestEncoderTest, EncodesCancelByExchangeOrderId) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(
      CancelOrderEncodeFields{.timestamp = 1700000002,
                              .encoded_request_id = 216172782113783810ULL,
                              .local_order_id = 10,
                              .exchange_order_id = 36028827892199865ULL},
      buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_EQ(encoded.text,
            R"({"time":1700000002,"channel":"futures.order_cancel","event":"api","payload":{"req_id":"216172782113783810","req_param":{"order_id":"36028827892199865"}}})");
}

TEST(OrderRequestEncoderTest, EncodesCancelByOrderTextFallback) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};

  const EncodedTextRequest encoded = EncodeCancelOrderRequest(
      CancelOrderEncodeFields{.timestamp = 1700000003,
                              .encoded_request_id = 216172782113783811ULL,
                              .local_order_id = 11,
                              .exchange_order_id = 0},
      buffer);

  ASSERT_EQ(encoded.status, OrderEncodeStatus::kOk);
  EXPECT_NE(encoded.text.find("\"order_id\":\"t-11\""), std::string_view::npos);
}

TEST(OrderRequestEncoderTest, ReportsSmallBuffer) {
  std::array<char, 16> buffer{};

  const EncodedTextRequest encoded = EncodePlaceOrderRequest(
      PlaceOrderEncodeFields{
          .timestamp = 1700000001,
          .encoded_request_id = 144115188075855873ULL,
          .wire = OrderWireFields{.local_order_id = 9,
                                  .contract = "BTC_USDT",
                                  .signed_size = 1,
                                  .price_text = "81000",
                                  .tif = "gtc",
                                  .text = "t-9",
                                  .reduce_only = false}},
      buffer);

  EXPECT_EQ(encoded.status, OrderEncodeStatus::kBufferTooSmall);
  EXPECT_TRUE(encoded.text.empty());
}

}  // namespace
}  // namespace aquila::gate
```

- [ ] **Step 2: 更新 CMake 并确认失败**

Modify `test/exchange/gate/trading/CMakeLists.txt`:

```cmake
add_executable(gate_order_request_encoder_test
    order_request_encoder_test.cpp
)

target_link_libraries(gate_order_request_encoder_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_request_encoder_test
         COMMAND gate_order_request_encoder_test)
```

Run:

```bash
./build.sh debug
```

Expected: compile fails because encoder headers do not exist.

- [ ] **Step 3: 实现 HMAC 签名**

Create `exchange/gate/trading/order_signature.h`:

```cpp
#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_SIGNATURE_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_SIGNATURE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aquila::gate {

inline constexpr std::size_t kGateSignatureHexSize = 128;

[[nodiscard]] bool GenerateGateApiSignatureHex(
    std::string_view api_secret, std::string_view channel,
    std::string_view request_param, std::int64_t timestamp,
    std::array<char, kGateSignatureHexSize>& output) noexcept;

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SIGNATURE_H_
```

Create `exchange/gate/trading/order_signature.cpp`:

```cpp
#include "exchange/gate/trading/order_signature.h"

#include <array>
#include <cstddef>
#include <string_view>

#include <fmt/format.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace aquila::gate {
namespace {

constexpr std::size_t kGateApiSignInputSize = 256;
constexpr char kHexDigits[] = "0123456789abcdef";

}  // namespace

bool GenerateGateApiSignatureHex(
    std::string_view api_secret, std::string_view channel,
    std::string_view request_param, std::int64_t timestamp,
    std::array<char, kGateSignatureHexSize>& output) noexcept {
  std::array<char, kGateApiSignInputSize> sign_input{};
  const auto formatted = fmt::format_to_n(
      sign_input.data(), sign_input.size(), "api\n{}\n{}\n{}", channel,
      request_param, timestamp);
  if (formatted.size > sign_input.size()) {
    return false;
  }

  unsigned char digest[EVP_MAX_MD_SIZE]{};
  unsigned int digest_len = 0;
  const std::string_view message(sign_input.data(), formatted.size);
  if (HMAC(EVP_sha512(), api_secret.data(),
           static_cast<int>(api_secret.size()),
           reinterpret_cast<const unsigned char*>(message.data()),
           message.size(), digest, &digest_len) == nullptr ||
      digest_len * 2 != output.size()) {
    return false;
  }

  for (unsigned int i = 0; i < digest_len; ++i) {
    output[i * 2] = kHexDigits[digest[i] >> 4];
    output[i * 2 + 1] = kHexDigits[digest[i] & 0x0F];
  }
  return true;
}

}  // namespace aquila::gate
```

Modify `exchange/gate/CMakeLists.txt`:

```cmake
add_library(aquila_gate STATIC
    market_data/data_session_config.cpp
    market_data/data_session_config.h
    trading/order_signature.cpp
    trading/order_signature.h
)

target_link_libraries(aquila_gate
    PUBLIC
        aquila_config
        fmt::fmt-header-only
        simdjson::simdjson
        FastFloat::fast_float
        absl::flat_hash_map
        OpenSSL::Crypto
    PRIVATE
        nova
)
```

- [ ] **Step 4: 实现固定缓冲区 encoder**

Create `exchange/gate/trading/order_request_encoder.h`:

```cpp
#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "exchange/gate/trading/order_codecs.h"
#include "exchange/gate/trading/order_signature.h"
#include "exchange/gate/trading/order_types.h"
#include <fmt/format.h>

namespace aquila::gate {

inline constexpr std::size_t kLoginRequestBufferSize = 1024;
inline constexpr std::size_t kPlaceOrderRequestBufferSize = 1024;
inline constexpr std::size_t kCancelOrderRequestBufferSize = 512;

enum class OrderEncodeStatus : std::uint8_t {
  kOk,
  kBufferTooSmall,
  kSignatureFailed,
  kInvalidOrderText,
};

struct EncodedTextRequest {
  OrderEncodeStatus status{OrderEncodeStatus::kBufferTooSmall};
  std::string_view text{};
};

struct LoginRequestFields {
  std::string_view api_key{};
  std::string_view api_secret{};
  std::int64_t timestamp{0};
  std::uint64_t encoded_request_id{0};
};

struct PlaceOrderEncodeFields {
  std::int64_t timestamp{0};
  std::uint64_t encoded_request_id{0};
  OrderWireFields wire{};
};

struct CancelOrderEncodeFields {
  std::int64_t timestamp{0};
  std::uint64_t encoded_request_id{0};
  std::int64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
};

template <std::size_t N, typename... Args>
[[nodiscard]] EncodedTextRequest FormatJsonToBuffer(
    std::array<char, N>& output, fmt::format_string<Args...> format,
    Args&&... args) noexcept {
  const auto result = fmt::format_to_n(output.data(), output.size(), format,
                                       std::forward<Args>(args)...);
  if (result.size > output.size()) {
    return {.status = OrderEncodeStatus::kBufferTooSmall, .text = {}};
  }
  return {.status = OrderEncodeStatus::kOk,
          .text = std::string_view(output.data(), result.size)};
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeLoginRequest(
    const LoginRequestFields& fields, std::array<char, N>& output) noexcept {
  std::array<char, kGateSignatureHexSize> signature{};
  if (!GenerateGateApiSignatureHex(fields.api_secret, "futures.login", "",
                                   fields.timestamp, signature)) {
    return {.status = OrderEncodeStatus::kSignatureFailed, .text = {}};
  }

  return FormatJsonToBuffer(
      output,
      R"({{"time":{},"channel":"futures.login","event":"api","payload":{{"api_key":"{}","signature":"{}","timestamp":"{}","req_id":"{}"}}}})",
      fields.timestamp, fields.api_key,
      std::string_view(signature.data(), signature.size()), fields.timestamp,
      fields.encoded_request_id);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodePlaceOrderRequest(
    const PlaceOrderEncodeFields& fields,
    std::array<char, N>& output) noexcept {
  return FormatJsonToBuffer(
      output,
      R"({{"time":{},"channel":"futures.order_place","event":"api","payload":{{"req_id":"{}","req_param":{{"contract":"{}","size":{},"price":"{}","tif":"{}","text":"{}","reduce_only":{}}}}}}})",
      fields.timestamp, fields.encoded_request_id, fields.wire.contract,
      fields.wire.signed_size, fields.wire.price_text, fields.wire.tif,
      fields.wire.text, fields.wire.reduce_only);
}

template <std::size_t N>
[[nodiscard]] EncodedTextRequest EncodeCancelOrderRequest(
    const CancelOrderEncodeFields& fields,
    std::array<char, N>& output) noexcept {
  if (fields.exchange_order_id != 0) {
    return FormatJsonToBuffer(
        output,
        R"({{"time":{},"channel":"futures.order_cancel","event":"api","payload":{{"req_id":"{}","req_param":{{"order_id":"{}"}}}}}})",
        fields.timestamp, fields.encoded_request_id, fields.exchange_order_id);
  }

  std::array<char, 32> text_buffer{};
  const std::string_view order_text =
      OrderTextCodec::Format(fields.local_order_id, text_buffer);
  if (order_text.empty()) {
    return {.status = OrderEncodeStatus::kInvalidOrderText, .text = {}};
  }
  return FormatJsonToBuffer(
      output,
      R"({{"time":{},"channel":"futures.order_cancel","event":"api","payload":{{"req_id":"{}","req_param":{{"order_id":"{}"}}}}}})",
      fields.timestamp, fields.encoded_request_id, order_text);
}

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_REQUEST_ENCODER_H_
```

- [ ] **Step 5: 跑测试并提交**

Run:

```bash
./build.sh debug
./build/debug/test/exchange/gate/trading/gate_order_request_encoder_test
```

Expected: both commands pass.

Commit:

```bash
git add exchange/gate/CMakeLists.txt \
  exchange/gate/trading/order_signature.h exchange/gate/trading/order_signature.cpp \
  exchange/gate/trading/order_request_encoder.h \
  test/exchange/gate/trading/order_request_encoder_test.cpp \
  test/exchange/gate/trading/CMakeLists.txt
git commit -m "feat: add gate order request encoder"
```

---

### Task 3: Submit Response Parser Correlation

**Files:**
- Modify: `exchange/gate/trading/submit_response_parser.h`
- Modify: `test/exchange/gate/trading/submit_response_parser_test.cpp`

- [ ] **Step 1: 写失败测试**

Append to `test/exchange/gate/trading/submit_response_parser_test.cpp`:

```cpp
TEST(GateSubmitResponseParserTest, DecodesRequestIdAndOrderText) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "144115188075855881",
    "ack": false,
    "header": {
      "status": "200",
      "channel": "futures.order_place",
      "event": "api"
    },
    "data": {
      "result": {
        "id": "36028827892199865",
        "text": "t-12345"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kResult);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kPlaceOrder);
  EXPECT_EQ(parsed.request_id.sequence, 9U);
  EXPECT_TRUE(parsed.has_local_order_id);
  EXPECT_EQ(parsed.local_order_id, 12345);
  EXPECT_EQ(parsed.exchange_order_id, 36028827892199865U);
}

TEST(GateSubmitResponseParserTest, DecodesCancelErrorRequestId) {
  static constexpr std::string_view kPayload = R"json({
    "request_id": "216172782113783818",
    "ack": false,
    "header": {
      "status": "400",
      "channel": "futures.order_cancel",
      "event": "api"
    },
    "data": {
      "errs": {
        "label": "ORDER_NOT_FOUND",
        "message": "order not found"
      }
    }
  })json";

  const auto parsed = ParseGateSubmitResponse(kPayload);

  ASSERT_EQ(parsed.parse_status, GateSubmitParseStatus::kOk);
  EXPECT_EQ(parsed.kind, GateSubmitResponseKind::kError);
  EXPECT_TRUE(parsed.request_id.ok);
  EXPECT_EQ(parsed.request_id.type, OrderRequestType::kCancelOrder);
  EXPECT_EQ(parsed.request_id.sequence, 10U);
  EXPECT_EQ(parsed.error_label_hash, HashGateSubmitString("ORDER_NOT_FOUND"));
}
```

Run:

```bash
./build.sh debug
```

Expected: compile fails because `GateSubmitResponse` does not expose decoded fields.

- [ ] **Step 2: 扩展 parser 类型**

Modify `exchange/gate/trading/submit_response_parser.h`:

```cpp
#include "exchange/gate/trading/order_codecs.h"
```

Extend `GateSubmitResponse`:

```cpp
enum class GateSubmitChannel : std::uint8_t {
  kUnknown,
  kFuturesLogin,
  kFuturesOrderPlace,
  kFuturesOrderCancel,
};

struct GateSubmitResponse {
  GateSubmitParseStatus parse_status{GateSubmitParseStatus::kUnexpectedShape};
  GateSubmitResponseKind kind{GateSubmitResponseKind::kUnknown};
  bool has_ack{false};
  bool ack{false};
  GateSubmitChannel channel{GateSubmitChannel::kUnknown};
  bool channel_is_order_place{false};
  std::uint16_t http_status{0};
  DecodedRequestId request_id{};
  bool has_req_id{false};
  DecodedRequestId req_id{};
  bool has_local_order_id{false};
  std::int64_t local_order_id{0};
  std::uint64_t request_id_hash{0};
  std::uint64_t req_id_hash{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t text_hash{0};
  std::uint64_t error_label_hash{0};
};
```

Add helpers:

```cpp
inline GateSubmitChannel ParseSubmitChannel(std::string_view channel) noexcept {
  if (channel == "futures.login") {
    return GateSubmitChannel::kFuturesLogin;
  }
  if (channel == "futures.order_place") {
    return GateSubmitChannel::kFuturesOrderPlace;
  }
  if (channel == "futures.order_cancel") {
    return GateSubmitChannel::kFuturesOrderCancel;
  }
  return GateSubmitChannel::kUnknown;
}

inline DecodedRequestId DecodeSimdjsonRequestId(
    simdjson::ondemand::value value) noexcept {
  std::uint64_t encoded = 0;
  if (!ReadSimdjsonUint64(value, &encoded)) {
    return {};
  }
  return RequestIdCodec::Decode(encoded);
}
```

In `ParseSimdjsonDocument()`:

```cpp
if (FindSimdjsonField(root, "request_id", &value)) {
  response.request_id_hash = HashSimdjsonString(value);
  response.request_id = DecodeSimdjsonRequestId(value);
}
```

When reading header channel:

```cpp
std::string_view channel{};
if (ReadSimdjsonString(value, &channel)) {
  response.channel = ParseSubmitChannel(channel);
  response.channel_is_order_place =
      response.channel == GateSubmitChannel::kFuturesOrderPlace;
}
```

When reading ack `req_id`:

```cpp
if (FindSimdjsonField(result, "req_id", &value)) {
  response.req_id_hash = HashSimdjsonString(value);
  response.req_id = DecodeSimdjsonRequestId(value);
  response.has_req_id = response.req_id.ok;
}
```

When reading result `text`:

```cpp
if (FindSimdjsonField(result, "text", &value)) {
  std::string_view text{};
  if (ReadSimdjsonString(value, &text)) {
    response.text_hash = HashGateSubmitString(text);
    const ParsedOrderText parsed_text = OrderTextCodec::Parse(text);
    response.has_local_order_id = parsed_text.ok;
    response.local_order_id = parsed_text.local_order_id;
  }
}
```

Add a no-copy padded-view overload for WebSocket message handling:

```cpp
inline GateSubmitResponse ParseGateSubmitResponse(
    std::string_view payload, size_t readable_tail_bytes,
    simdjson::ondemand::parser& parser) noexcept {
  if (payload.empty()) {
    return detail::InvalidJsonSubmitResponse();
  }

  if (readable_tail_bytes >= simdjson::SIMDJSON_PADDING) {
    simdjson::padded_string_view view(payload.data(), payload.size(),
                                      payload.size() + readable_tail_bytes);
    simdjson::ondemand::document document;
    if (parser.iterate(view).get(document) != simdjson::SUCCESS) {
      return detail::InvalidJsonSubmitResponse();
    }
    return detail::ParseSimdjsonDocument(std::move(document));
  }

  simdjson::padded_string padded(payload);
  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document) != simdjson::SUCCESS) {
    return detail::InvalidJsonSubmitResponse();
  }
  return detail::ParseSimdjsonDocument(std::move(document));
}
```

- [ ] **Step 3: 跑 parser 测试并提交**

Run:

```bash
./build.sh debug
./build/debug/test/exchange/gate/trading/gate_submit_response_parser_test
```

Expected: both commands pass; existing hash-only tests still pass.

Commit:

```bash
git add exchange/gate/trading/submit_response_parser.h \
  test/exchange/gate/trading/submit_response_parser_test.cpp
git commit -m "feat: decode gate submit correlation fields"
```

---

### Task 4: OrderSession Core

**Files:**
- Create: `exchange/gate/trading/order_session.h`
- Create: `test/exchange/gate/trading/order_session_test.cpp`
- Modify: `test/exchange/gate/trading/CMakeLists.txt`

- [ ] **Step 1: 写 session 行为测试**

Create `test/exchange/gate/trading/order_session_test.cpp`:

```cpp
#include "exchange/gate/trading/order_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/websocket/message_view.h"

namespace aquila::gate {
namespace {

aquila::websocket::MessageView TextView(std::string_view payload) noexcept {
  return {.kind = aquila::websocket::PayloadKind::kText,
          .payload = std::as_bytes(std::span(payload.data(), payload.size())),
          .sequence = 1,
          .fin = true,
          .readable_tail_bytes = 0};
}

struct RecordingHandler {
  std::vector<OrderResponse> responses;

  void OnOrderResponse(const OrderResponse& response) noexcept {
    responses.push_back(response);
  }
};

template <typename Handler>
class TestOrderSession
    : public OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                          OrderSessionDiagnostics> {
 public:
  explicit TestOrderSession(Handler& handler)
      : OrderSession<Handler, OrderSessionDefaultPlainWebSocketPolicy,
                     OrderSessionDiagnostics>(
            MakeConfig(),
            LoginCredentials{.api_key = "key", .api_secret = "secret"},
            handler) {}

  static aquila::websocket::ConnectionConfig MakeConfig() {
    aquila::websocket::ConnectionConfig config{};
    config.host = "localhost";
    config.service = "80";
    config.target = "/v4/ws/usdt";
    config.prepared_write_slots = 8;
    config.prepared_write_bytes = 4096;
    return config;
  }
};

TEST(OrderSessionTest, RejectsPlaceBeforeLoginReady) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);

  const OrderSendResult result = session.PlaceOrder(
      PlaceOrderRequest{.wire = OrderWireFields{.local_order_id = 1,
                                                .contract = "BTC_USDT",
                                                .signed_size = 1,
                                                .price_text = "81000",
                                                .tif = "gtc",
                                                .text = "t-1",
                                                .reduce_only = false}});

  EXPECT_EQ(result.status, OrderSendStatus::kNotLoggedIn);
  EXPECT_TRUE(handler.responses.empty());
}

TEST(OrderSessionTest, SendsLoginOnActiveAndMarksReadyOnSuccess) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);
  const auto result = session.Handle(TextView(
      R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.login","event":"api"},"data":{"result":{"uid":"1"}}})"));

  EXPECT_EQ(result, aquila::websocket::DeliveryResult::kAccepted);
  EXPECT_TRUE(session.login_ready());
  EXPECT_EQ(session.stats().login_sent, 1U);
  EXPECT_EQ(session.stats().login_accepted, 1U);
}

template <typename Handler>
void ActivateAndLogin(TestOrderSession<Handler>& session) {
  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kActive);
  session.Handle(TextView(
      R"({"request_id":"72057594037927937","ack":false,"header":{"status":"200","channel":"futures.login","event":"api"},"data":{"result":{"uid":"1"}}})"));
  ASSERT_TRUE(session.login_ready());
}

TEST(OrderSessionTest, PlaceAckDoesNotEraseCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(
      PlaceOrderRequest{.wire = OrderWireFields{.local_order_id = 123,
                                                .contract = "BTC_USDT",
                                                .signed_size = 1,
                                                .price_text = "81000",
                                                .tif = "gtc",
                                                .text = "t-123",
                                                .reduce_only = false}});
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":true,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"req_id":"144115188075855874"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kAck);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(session.inflight_count(), 1U);
}

TEST(OrderSessionTest, PlaceResultMapsExchangeOrderIdAndErasesCorrelation) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.PlaceOrder(
      PlaceOrderRequest{.wire = OrderWireFields{.local_order_id = 123,
                                                .contract = "BTC_USDT",
                                                .signed_size = 1,
                                                .price_text = "81000",
                                                .tif = "gtc",
                                                .text = "t-123",
                                                .reduce_only = false}});
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.Handle(TextView(
      R"({"request_id":"144115188075855874","ack":false,"header":{"status":"200","channel":"futures.order_place","event":"api"},"data":{"result":{"id":"36028827892199865","text":"t-123"}}})"));

  ASSERT_EQ(handler.responses.size(), 1U);
  EXPECT_EQ(handler.responses[0].kind, OrderResponseKind::kAccepted);
  EXPECT_EQ(handler.responses[0].local_order_id, 123);
  EXPECT_EQ(handler.responses[0].exchange_order_id, 36028827892199865U);
  EXPECT_EQ(session.inflight_count(), 0U);
}

TEST(OrderSessionTest, DisconnectClearsInflightWithoutFakeResponses) {
  RecordingHandler handler;
  TestOrderSession<RecordingHandler> session(handler);
  ActivateAndLogin(session);

  const OrderSendResult sent = session.CancelOrder(
      CancelOrderRequest{.local_order_id = 123, .exchange_order_id = 0});
  ASSERT_EQ(sent.status, OrderSendStatus::kOk);

  session.OnConnectionPhase(aquila::websocket::ConnectionPhase::kDisconnected);

  EXPECT_FALSE(session.login_ready());
  EXPECT_EQ(session.inflight_count(), 0U);
  EXPECT_TRUE(handler.responses.empty());
}

}  // namespace
}  // namespace aquila::gate
```

Add test target:

```cmake
add_executable(gate_order_session_test
    order_session_test.cpp
)

target_link_libraries(gate_order_session_test
    PRIVATE
        aquila_gate
        GTest::gtest_main
)

add_test(NAME gate_order_session_test
         COMMAND gate_order_session_test)
```

Run:

```bash
./build.sh debug
```

Expected: compile fails because `order_session.h` does not exist.

- [ ] **Step 2: 实现 OrderSession public surface**

Create `exchange/gate/trading/order_session.h` with this shape:

```cpp
#ifndef AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_
#define AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/websocket/message_view.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/types.h"
#include "core/websocket/websocket_client.h"
#include "exchange/gate/trading/order_request_encoder.h"
#include "exchange/gate/trading/submit_response_parser.h"
#include <absl/container/flat_hash_map.h>
#include <simdjson.h>

namespace aquila::gate {

inline constexpr std::size_t kDefaultOrderInflightCapacity = 16384;

struct LoginCredentials {
  std::string api_key;
  std::string api_secret;
};

class NoopOrderSessionDiagnostics {
 public:
  static constexpr bool kEnabled = false;

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return kStats;
  }

 private:
  inline static constexpr OrderSessionStats kStats{};
};

class OrderSessionDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  void RecordTextMessage() noexcept { ++stats_.text_messages; }
  void RecordParseError() noexcept { ++stats_.parse_errors; }
  void RecordIgnoredMessage() noexcept { ++stats_.ignored_messages; }
  void RecordLoginSent() noexcept { ++stats_.login_sent; }
  void RecordLoginAccepted() noexcept { ++stats_.login_accepted; }
  void RecordLoginRejected() noexcept { ++stats_.login_rejected; }
  void RecordPlaceSent() noexcept { ++stats_.place_sent; }
  void RecordCancelSent() noexcept { ++stats_.cancel_sent; }
  void RecordResponse() noexcept { ++stats_.responses; }
  void RecordUnknownRequestId() noexcept { ++stats_.unknown_request_ids; }
  void RecordLocalSendFailure() noexcept { ++stats_.local_send_failures; }

  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return stats_;
  }

 private:
  OrderSessionStats stats_{};
};

struct OrderSessionDefaultTlsWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::TlsSocket;
};

struct OrderSessionDefaultPlainWebSocketPolicy
    : websocket::DefaultWebSocketOptions {
  using TransportSocket = websocket::PlainSocket;
};
```

Then add the class:

```cpp
template <typename ResponseHandler,
          typename WebSocketPolicy = OrderSessionDefaultTlsWebSocketPolicy,
          typename Diagnostics = NoopOrderSessionDiagnostics>
class OrderSession {
 public:
  using TransportSocket = typename WebSocketPolicy::TransportSocket;
  using MessageHandler = websocket::MessageHandlerRef<OrderSession>;
  using Client =
      websocket::BasicWebSocketClient<TransportSocket, MessageHandler>;
  static constexpr bool DiagnosticsEnabled = Diagnostics::kEnabled;
  static constexpr websocket::ClockSource kClockSource =
      WebSocketPolicy::kClockSource;

  OrderSession(websocket::ConnectionConfig config, LoginCredentials credentials,
               ResponseHandler& response_handler)
      : connection_(ApplyOptions(std::move(config))),
        credentials_(std::move(credentials)),
        response_handler_(response_handler),
        message_handler_(websocket::MakeMessageHandler(*this)),
        client_(connection_, message_handler_) {
    request_id_to_local_order_id_.reserve(kDefaultOrderInflightCapacity);
    client_.SetStateHook(this, &HandleState);
  }

  bool Start() noexcept { return client_.Start(); }
  void Stop() noexcept { client_.Stop(); }

  websocket::DeliveryResult Handle(
      const websocket::MessageView& view) noexcept {
    if (view.kind != websocket::PayloadKind::kText) {
      return websocket::DeliveryResult::kAccepted;
    }
    return HandleText(view);
  }

  void OnConnectionPhase(websocket::ConnectionPhase phase) noexcept {
    if (phase == websocket::ConnectionPhase::kActive) {
      active_ = true;
      (void)SendLogin();
      return;
    }
    if (phase == websocket::ConnectionPhase::kDisconnected ||
        phase == websocket::ConnectionPhase::kReconnectBackoff ||
        phase == websocket::ConnectionPhase::kClosing ||
        phase == websocket::ConnectionPhase::kClosed) {
      active_ = false;
      login_ready_ = false;
      request_id_to_local_order_id_.clear();
    }
  }

  OrderSendResult PlaceOrder(const PlaceOrderRequest& request) noexcept;
  OrderSendResult CancelOrder(const CancelOrderRequest& request) noexcept;

  [[nodiscard]] bool login_ready() const noexcept { return login_ready_; }
  [[nodiscard]] std::size_t inflight_count() const noexcept {
    return request_id_to_local_order_id_.size();
  }
  [[nodiscard]] const OrderSessionStats& stats() const noexcept {
    return diagnostics_.stats();
  }

 private:
  static void HandleState(void* context,
                          websocket::ConnectionPhase phase) noexcept {
    static_cast<OrderSession*>(context)->OnConnectionPhase(phase);
  }

  static websocket::ConnectionConfig ApplyOptions(
      websocket::ConnectionConfig config) {
    config.runtime_policy.clock_source = kClockSource;
    return config;
  }
```

- [ ] **Step 3: 实现 send path**

Add these private helpers and public method bodies:

```cpp
  [[nodiscard]] std::int64_t NowSeconds() const noexcept {
    return static_cast<std::int64_t>(std::time(nullptr));
  }

  [[nodiscard]] std::uint64_t NextRequestSequence() noexcept {
    return request_sequence_++;
  }

  [[nodiscard]] OrderSendStatus MapSendStatus(
      websocket::SendStatus status) noexcept {
    switch (status) {
      case websocket::SendStatus::kOk:
        return OrderSendStatus::kOk;
      case websocket::SendStatus::kNoPreparedWriteSlot:
        return OrderSendStatus::kNoPreparedWriteSlot;
      case websocket::SendStatus::kWriteUnavailable:
        return OrderSendStatus::kWriteUnavailable;
      case websocket::SendStatus::kEncodeFailed:
      case websocket::SendStatus::kPayloadTooLarge:
        return OrderSendStatus::kEncodeBufferTooSmall;
    }
    return OrderSendStatus::kWriteUnavailable;
  }

  [[nodiscard]] websocket::SendStatus SendText(
      std::string_view payload_text) noexcept {
    auto& core = client_.Core();
    const auto payload = std::as_bytes(
        std::span<const char>(payload_text.data(), payload_text.size()));
    return core.SendText(payload, websocket::WriteFlushMode::kTryFlushOne);
  }

  [[nodiscard]] OrderSendResult SendLogin() noexcept {
    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kLogin, sequence);
    std::array<char, kLoginRequestBufferSize> buffer{};
    const EncodedTextRequest encoded = EncodeLoginRequest(
        LoginRequestFields{.api_key = credentials_.api_key,
                           .api_secret = credentials_.api_secret,
                           .timestamp = NowSeconds(),
                           .encoded_request_id = encoded_request_id},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return {.status = OrderSendStatus::kEncodeBufferTooSmall,
              .request_sequence = sequence,
              .encoded_request_id = encoded_request_id};
    }
    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status == OrderSendStatus::kOk) {
      login_request_sequence_ = sequence;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginSent();
      }
    }
    return {.status = status,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  [[nodiscard]] OrderSendResult LocalReject(
      OrderSendStatus status) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLocalSendFailure();
    }
    return {.status = status, .request_sequence = 0, .encoded_request_id = 0};
  }
```

Then implement `PlaceOrder()` and `CancelOrder()`:

```cpp
  OrderSendResult PlaceOrder(const PlaceOrderRequest& request) noexcept {
    if (!active_) {
      return LocalReject(OrderSendStatus::kNotActive);
    }
    if (!login_ready_) {
      return LocalReject(OrderSendStatus::kNotLoggedIn);
    }
    if (request_id_to_local_order_id_.size() >= kDefaultOrderInflightCapacity) {
      return LocalReject(OrderSendStatus::kInflightFull);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kPlaceOrder, sequence);
    std::array<char, kPlaceOrderRequestBufferSize> buffer{};
    const EncodedTextRequest encoded = EncodePlaceOrderRequest(
        PlaceOrderEncodeFields{.timestamp = NowSeconds(),
                               .encoded_request_id = encoded_request_id,
                               .wire = request.wire},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return LocalReject(OrderSendStatus::kEncodeBufferTooSmall);
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status != OrderSendStatus::kOk) {
      return LocalReject(status);
    }
    request_id_to_local_order_id_.emplace(sequence,
                                          request.wire.local_order_id);
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordPlaceSent();
    }
    return {.status = status,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }

  OrderSendResult CancelOrder(const CancelOrderRequest& request) noexcept {
    if (!active_) {
      return LocalReject(OrderSendStatus::kNotActive);
    }
    if (!login_ready_) {
      return LocalReject(OrderSendStatus::kNotLoggedIn);
    }
    if (request_id_to_local_order_id_.size() >= kDefaultOrderInflightCapacity) {
      return LocalReject(OrderSendStatus::kInflightFull);
    }

    const std::uint64_t sequence = NextRequestSequence();
    const std::uint64_t encoded_request_id =
        RequestIdCodec::Encode(OrderRequestType::kCancelOrder, sequence);
    std::array<char, kCancelOrderRequestBufferSize> buffer{};
    const EncodedTextRequest encoded = EncodeCancelOrderRequest(
        CancelOrderEncodeFields{.timestamp = NowSeconds(),
                                .encoded_request_id = encoded_request_id,
                                .local_order_id = request.local_order_id,
                                .exchange_order_id =
                                    request.exchange_order_id},
        buffer);
    if (encoded.status != OrderEncodeStatus::kOk) {
      return LocalReject(OrderSendStatus::kEncodeBufferTooSmall);
    }

    const OrderSendStatus status = MapSendStatus(SendText(encoded.text));
    if (status != OrderSendStatus::kOk) {
      return LocalReject(status);
    }
    request_id_to_local_order_id_.emplace(sequence, request.local_order_id);
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordCancelSent();
    }
    return {.status = status,
            .request_sequence = sequence,
            .encoded_request_id = encoded_request_id};
  }
```

- [ ] **Step 4: 实现 response dispatch**

Add:

```cpp
  websocket::DeliveryResult HandleText(
      const websocket::MessageView& view) noexcept {
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordTextMessage();
    }
    const std::string_view payload{
        reinterpret_cast<const char*>(view.payload.data()),
        view.payload.size()};

    const GateSubmitResponse parsed =
        ParseGateSubmitResponse(payload, view.readable_tail_bytes,
                                text_parser_);
    if (parsed.parse_status != GateSubmitParseStatus::kOk ||
        !parsed.request_id.ok) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordParseError();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    if (parsed.request_id.type == OrderRequestType::kLogin) {
      HandleLoginResponse(parsed);
      return websocket::DeliveryResult::kAccepted;
    }

    auto it = request_id_to_local_order_id_.find(parsed.request_id.sequence);
    if (it == request_id_to_local_order_id_.end()) {
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordUnknownRequestId();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    const std::int64_t local_order_id = it->second;
    if (parsed.kind == GateSubmitResponseKind::kAck) {
      response_handler_.OnOrderResponse(
          OrderResponse{.kind = OrderResponseKind::kAck,
                        .local_order_id = local_order_id,
                        .exchange_order_id = 0,
                        .request_sequence = parsed.request_id.sequence,
                        .http_status = parsed.http_status,
                        .error_label_hash = 0});
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordResponse();
      }
      return websocket::DeliveryResult::kAccepted;
    }

    request_id_to_local_order_id_.erase(it);
    const bool is_cancel =
        parsed.request_id.type == OrderRequestType::kCancelOrder;
    const bool is_error = parsed.kind == GateSubmitResponseKind::kError;
    const OrderResponseKind kind =
        is_cancel ? (is_error ? OrderResponseKind::kCancelRejected
                              : OrderResponseKind::kCancelAccepted)
                  : (is_error ? OrderResponseKind::kRejected
                              : OrderResponseKind::kAccepted);
    response_handler_.OnOrderResponse(
        OrderResponse{.kind = kind,
                      .local_order_id = local_order_id,
                      .exchange_order_id = parsed.exchange_order_id,
                      .request_sequence = parsed.request_id.sequence,
                      .http_status = parsed.http_status,
                      .error_label_hash = parsed.error_label_hash});
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordResponse();
    }
    return websocket::DeliveryResult::kAccepted;
  }

  void HandleLoginResponse(const GateSubmitResponse& parsed) noexcept {
    if (parsed.http_status == 200 &&
        parsed.kind != GateSubmitResponseKind::kError) {
      login_ready_ = true;
      if constexpr (DiagnosticsEnabled) {
        diagnostics_.RecordLoginAccepted();
      }
      return;
    }
    login_ready_ = false;
    if constexpr (DiagnosticsEnabled) {
      diagnostics_.RecordLoginRejected();
    }
  }
```

Add members:

```cpp
  websocket::ConnectionConfig connection_;
  LoginCredentials credentials_;
  ResponseHandler& response_handler_;
  MessageHandler message_handler_;
  Client client_;
  [[no_unique_address]] Diagnostics diagnostics_{};
  simdjson::ondemand::parser text_parser_;
  absl::flat_hash_map<std::uint64_t, std::int64_t>
      request_id_to_local_order_id_;
  std::uint64_t request_sequence_{1};
  std::uint64_t login_request_sequence_{0};
  bool active_{false};
  bool login_ready_{false};
};

}  // namespace aquila::gate

#endif  // AQUILA_EXCHANGE_GATE_TRADING_ORDER_SESSION_H_
```

- [ ] **Step 5: Keep production test hooks narrow**

The `TestOrderSession` wrapper lives only in `order_session_test.cpp`. If tests need to inspect state, keep the production surface to narrow read-only accessors:

```cpp
[[nodiscard]] std::size_t inflight_count() const noexcept;
```

Do not add a production method that directly flips login state; drive login through `OnConnectionPhase(kActive)` plus a login response `Handle()`.

- [ ] **Step 6: 跑测试并提交**

Run:

```bash
./build.sh debug
./build/debug/test/exchange/gate/trading/gate_order_session_test
ctest --test-dir build/debug -R 'gate_(order|submit)' --output-on-failure
```

Expected: all Gate trading tests pass.

Commit:

```bash
git add exchange/gate/trading/order_session.h \
  test/exchange/gate/trading/order_session_test.cpp \
  test/exchange/gate/trading/CMakeLists.txt
git commit -m "feat: add gate order session"
```

---

### Task 5: Benchmarks

**Files:**
- Create: `benchmark/exchange/gate/trading/order_session_benchmark.cpp`
- Modify: `benchmark/exchange/gate/trading/CMakeLists.txt`

- [ ] **Step 1: 写 benchmark**

Create `benchmark/exchange/gate/trading/order_session_benchmark.cpp`:

```cpp
#include <array>
#include <string_view>

#include <benchmark/benchmark.h>

#include "exchange/gate/trading/order_request_encoder.h"
#include "exchange/gate/trading/order_session.h"
#include "exchange/gate/trading/submit_response_parser.h"

namespace {

using namespace aquila::gate;

constexpr PlaceOrderEncodeFields kPlaceFields{
    .timestamp = 1700000001,
    .encoded_request_id = 144115188075855873ULL,
    .wire = OrderWireFields{.local_order_id = 9,
                            .contract = "BTC_USDT",
                            .signed_size = 1,
                            .price_text = "81000",
                            .tif = "gtc",
                            .text = "t-9",
                            .reduce_only = false}};

constexpr CancelOrderEncodeFields kCancelFields{
    .timestamp = 1700000002,
    .encoded_request_id = 216172782113783810ULL,
    .local_order_id = 9,
    .exchange_order_id = 36028827892199865ULL};

constexpr std::string_view kPlaceResult = R"json({
  "request_id": "144115188075855873",
  "ack": false,
  "header": {"status": "200", "channel": "futures.order_place", "event": "api"},
  "data": {"result": {"id": "36028827892199865", "text": "t-9"}}
})json";

void BM_EncodePlaceOrder(benchmark::State& state) {
  std::array<char, kPlaceOrderRequestBufferSize> buffer{};
  for (auto _ : state) {
    const EncodedTextRequest encoded =
        EncodePlaceOrderRequest(kPlaceFields, buffer);
    benchmark::DoNotOptimize(encoded.text.data());
    benchmark::DoNotOptimize(encoded.text.size());
  }
}
BENCHMARK(BM_EncodePlaceOrder);

void BM_EncodeCancelOrder(benchmark::State& state) {
  std::array<char, kCancelOrderRequestBufferSize> buffer{};
  for (auto _ : state) {
    const EncodedTextRequest encoded =
        EncodeCancelOrderRequest(kCancelFields, buffer);
    benchmark::DoNotOptimize(encoded.text.data());
    benchmark::DoNotOptimize(encoded.text.size());
  }
}
BENCHMARK(BM_EncodeCancelOrder);

void BM_ParsePlaceResult(benchmark::State& state) {
  for (auto _ : state) {
    const GateSubmitResponse parsed = ParseGateSubmitResponse(kPlaceResult);
    benchmark::DoNotOptimize(parsed.exchange_order_id);
  }
}
BENCHMARK(BM_ParsePlaceResult);

}  // namespace
```

Modify `benchmark/exchange/gate/trading/CMakeLists.txt`:

```cmake
add_executable(gate_order_session_benchmark
    order_session_benchmark.cpp
)

target_link_libraries(gate_order_session_benchmark
    PRIVATE
        aquila_gate
        benchmark::benchmark_main
        fmt::fmt-header-only
)
```

- [ ] **Step 2: 跑 release benchmark smoke 并提交**

Run:

```bash
./build.sh release
./build/release/benchmark/exchange/gate/trading/gate_order_session_benchmark \
  --benchmark_filter='BM_EncodePlaceOrder|BM_EncodeCancelOrder|BM_ParsePlaceResult' \
  --benchmark_min_time=0.01s
```

Expected: benchmark runs and reports timing for all three cases. Do not write performance conclusions into docs from this smoke command.

Commit:

```bash
git add benchmark/exchange/gate/trading/order_session_benchmark.cpp \
  benchmark/exchange/gate/trading/CMakeLists.txt
git commit -m "bench: add gate order session benchmark"
```

---

### Task 6: Documentation And Final Verification

**Files:**
- Modify: `doc/project_onboarding_guide.md`
- Modify: `doc/agent-handoff-gate-trade-architecture.md`

- [ ] **Step 1: 更新 onboarding**

In `doc/project_onboarding_guide.md`:

- 在“最近已完成”增加 `OrderSession` 第一版状态；
- 在“Gate 交易准备代码”代码入口增加：

```text
exchange/gate/trading/order_types.h
exchange/gate/trading/order_codecs.h
exchange/gate/trading/order_signature.h
exchange/gate/trading/order_request_encoder.h
exchange/gate/trading/order_session.h
```

- 在“常用验证命令”增加：

```bash
./build/debug/test/exchange/gate/trading/gate_order_codecs_test
./build/debug/test/exchange/gate/trading/gate_order_request_encoder_test
./build/debug/test/exchange/gate/trading/gate_submit_response_parser_test
./build/debug/test/exchange/gate/trading/gate_order_session_test
ctest --test-dir build/debug -R 'gate_(order|submit)' --output-on-failure
./build/release/benchmark/exchange/gate/trading/gate_order_session_benchmark --benchmark_min_time=0.01s
```

- 在“下一步建议”把 OrderSession 第一版从待设计改为待联通 / 待 live 验证，并保留 `OrderFeedbackSession`、SBE private feedback、REST reconcile 等后续项。

- [ ] **Step 2: 更新 Gate handoff**

In `doc/agent-handoff-gate-trade-architecture.md`:

- 记录 `OrderSession` 已采用无 Gate 前缀命名；
- 记录 `request_sequence -> local_order_id` 是轻量 correlation map；
- 记录 `OrderSession` 不做 Strategy 风控和订单状态；
- 记录第一版未覆盖 `OrderFeedbackSession`；
- 增加本计划对应验证命令。

- [ ] **Step 3: 跑完整验证**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R 'gate_(order|submit)' --output-on-failure
./build.sh release
./build/release/benchmark/exchange/gate/trading/gate_order_session_benchmark --benchmark_min_time=0.01s
git diff --check
```

Expected:

- debug build passes;
- Gate trading tests pass;
- release build passes;
- benchmark smoke runs;
- `git diff --check` prints no output.

- [ ] **Step 4: 提交文档**

Commit:

```bash
git add doc/project_onboarding_guide.md doc/agent-handoff-gate-trade-architecture.md
git commit -m "doc: update gate order session handoff"
```

---

## Execution Choice

Plan complete. Recommended execution mode is `superpowers:subagent-driven-development` with one worker per task boundary:

1. Task 1 worker: types/codecs only.
2. Task 2 worker: signature/encoder only.
3. Task 3 worker: parser upgrade only.
4. Task 4 worker: session only.
5. Task 5 worker: benchmark only.
6. Task 6 worker or inline: docs and final verification.

If executing inline in this same session, use `superpowers:executing-plans` and stop after each task commit for review.
