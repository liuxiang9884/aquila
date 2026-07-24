#include "exchange/bitget/trading/order_session_config.h"

#include <filesystem>
#include <string>
#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace aquila::bitget {
namespace {

std::string BaseToml() {
  return R"toml(
[order_session]
name = "bitget_order_session_high_availability"
category = "usdt-futures"
position_mode = "one_way_mode"
margin_mode = "crossed"
client_oid_run_namespace = "0123456789AB"
request_map_capacity = 16384
order_id_cache_capacity = 8192

[order_session.credentials]
api_key_env = "BITGET_TEST_KEY"
api_secret_env = "BITGET_TEST_SECRET"
api_passphrase_env = "BITGET_TEST_PASSPHRASE"

[order_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"
port = "443"
target = "/v3/ws/private"
enable_tls = true

[order_session.websocket.execution_policy]
bind_cpu_id = -1

[order_session.websocket.heartbeat]
interval_ms = 30000
timeout_ms = 10000
)toml";
}

void Replace(std::string* text, std::string_view before,
             std::string_view after) {
  const std::size_t position = text->find(before);
  ASSERT_NE(position, std::string::npos);
  text->replace(position, before.size(), after);
}

OrderSessionConfigResult Parse(std::string_view text) {
  return ParseOrderSessionConfig(toml::parse(text));
}

TEST(BitgetOrderSessionConfigTest, ParsesHighAvailabilityPrivateEndpoint) {
  const OrderSessionConfigResult result = Parse(BaseToml());

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_order_session_high_availability");
  EXPECT_EQ(result.value.category, "usdt-futures");
  EXPECT_EQ(result.value.position_mode, "one_way_mode");
  EXPECT_EQ(result.value.margin_mode, "crossed");
  EXPECT_EQ(result.value.client_oid_run_namespace.View(), "0123456789AB");
  EXPECT_EQ(result.value.request_map_capacity, 16384U);
  EXPECT_EQ(result.value.order_id_cache_capacity, 8192U);
  EXPECT_EQ(result.value.credentials.api_key_env, "BITGET_TEST_KEY");
  EXPECT_EQ(result.value.credentials.api_secret_env, "BITGET_TEST_SECRET");
  EXPECT_EQ(result.value.credentials.api_passphrase_env,
            "BITGET_TEST_PASSPHRASE");
  EXPECT_EQ(result.value.connection.host, "vip-ws-uta.bitget.com");
  EXPECT_EQ(result.value.connection.port, "443");
  EXPECT_EQ(result.value.connection.target, "/v3/ws/private");
  EXPECT_TRUE(result.value.connection.enable_tls);
  EXPECT_EQ(result.value.connection.heartbeat_interval_ms, 30000U);
  EXPECT_EQ(result.value.connection.heartbeat_timeout_ms, 10000U);
}

TEST(BitgetOrderSessionConfigTest, LoadsCheckedInConfig) {
  const std::filesystem::path path =
      std::filesystem::path{AQUILA_SOURCE_DIR} /
      "config/order_sessions/bitget_order_session.toml";

  const OrderSessionConfigResult result = LoadOrderSessionConfigFile(path);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_order_session_high_speed_private");
  EXPECT_EQ(result.value.connection.host, "vip-ws-uta-pri-a.bitget.com");
  EXPECT_EQ(result.value.connection.target, "/v3/ws/private");
  EXPECT_EQ(result.value.client_oid_run_namespace.View(), "000000000000");
  EXPECT_FALSE(result.value.client_oid_run_namespace.IsConfigured());
}

TEST(BitgetOrderSessionConfigTest, RejectsMissingOrInvalidRunNamespace) {
  for (const std::string_view replacement : {
           "",
           "client_oid_run_namespace = \"0123456789A\"",
           "client_oid_run_namespace = \"0123456789AI\"",
           "client_oid_run_namespace = \"0123456789ab\"",
       }) {
    std::string text = BaseToml();
    Replace(&text, "client_oid_run_namespace = \"0123456789AB\"", replacement);
    EXPECT_FALSE(Parse(text).ok) << replacement;
  }
}

TEST(BitgetOrderSessionConfigTest, RejectsUnsupportedTradingModes) {
  for (const auto& replacement : {
           std::pair{"category = \"usdt-futures\"",
                     "category = \"coin-futures\""},
           std::pair{"position_mode = \"one_way_mode\"",
                     "position_mode = \"hedge_mode\""},
           std::pair{"margin_mode = \"crossed\"", "margin_mode = \"isolated\""},
       }) {
    std::string text = BaseToml();
    Replace(&text, replacement.first, replacement.second);
    const OrderSessionConfigResult result = Parse(text);
    EXPECT_FALSE(result.ok);
  }
}

TEST(BitgetOrderSessionConfigTest, RejectsMissingCredentialEnvNames) {
  for (const std::string_view line : {
           "api_key_env = \"BITGET_TEST_KEY\"",
           "api_secret_env = \"BITGET_TEST_SECRET\"",
           "api_passphrase_env = \"BITGET_TEST_PASSPHRASE\"",
       }) {
    std::string text = BaseToml();
    Replace(&text, line, "");
    const OrderSessionConfigResult result = Parse(text);
    EXPECT_FALSE(result.ok);
  }
}

TEST(BitgetOrderSessionConfigTest, RejectsZeroCapacities) {
  for (const auto& replacement : {
           std::pair{"request_map_capacity = 16384",
                     "request_map_capacity = 0"},
           std::pair{"order_id_cache_capacity = 8192",
                     "order_id_cache_capacity = 0"},
       }) {
    std::string text = BaseToml();
    Replace(&text, replacement.first, replacement.second);
    const OrderSessionConfigResult result = Parse(text);
    EXPECT_FALSE(result.ok);
  }
}

TEST(BitgetOrderSessionConfigTest, RejectsNonPrivateOrNonTlsEndpoint) {
  for (const auto& replacement : {
           std::pair{"target = \"/v3/ws/private\"",
                     "target = \"/v3/ws/public\""},
           std::pair{"enable_tls = true", "enable_tls = false"},
           std::pair{"port = \"443\"", "port = \"80\""},
       }) {
    std::string text = BaseToml();
    Replace(&text, replacement.first, replacement.second);
    const OrderSessionConfigResult result = Parse(text);
    EXPECT_FALSE(result.ok);
  }
}

TEST(BitgetOrderSessionConfigTest, RejectsMissingNameAndInvalidHeartbeat) {
  std::string missing_name = BaseToml();
  Replace(&missing_name, "name = \"bitget_order_session_high_availability\"",
          "");
  EXPECT_FALSE(Parse(missing_name).ok);

  std::string long_interval = BaseToml();
  Replace(&long_interval, "interval_ms = 30000", "interval_ms = 30001");
  EXPECT_FALSE(Parse(long_interval).ok);

  std::string zero_timeout = BaseToml();
  Replace(&zero_timeout, "timeout_ms = 10000", "timeout_ms = 0");
  EXPECT_FALSE(Parse(zero_timeout).ok);
}

}  // namespace
}  // namespace aquila::bitget
