#include "exchange/bitget/trading/order_feedback_session_config.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

namespace aquila::bitget {
namespace {

std::string BaseToml() {
  return R"toml(
[order_feedback_session]
name = "bitget_order_feedback_high_availability"
category = "usdt-futures"
position_mode = "one_way_mode"
margin_mode = "crossed"
client_oid_run_namespace = "0123456789AB"

[order_feedback_session.credentials]
api_key_env = "BITGET_TEST_KEY"
api_secret_env = "BITGET_TEST_SECRET"
api_passphrase_env = "BITGET_TEST_PASSPHRASE"

[order_feedback_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"
port = "443"
target = "/v3/ws/private"
enable_tls = true

[order_feedback_session.websocket.execution_policy]
bind_cpu_id = -1

[order_feedback_session.websocket.heartbeat]
interval_ms = 30000
timeout_ms = 10000

[order_feedback_session.shm]
shm_name = "aquila_bitget_order_feedback"
channel_name = "orders"
max_strategy_count = 8
queue_capacity = 65536
create = true
remove_existing = false
)toml";
}

void Replace(std::string* text, std::string_view before,
             std::string_view after) {
  const std::size_t position = text->find(before);
  ASSERT_NE(position, std::string::npos);
  text->replace(position, before.size(), after);
}

OrderFeedbackSessionConfigResult Parse(std::string_view text) {
  return ParseOrderFeedbackSessionConfig(toml::parse(text));
}

TEST(BitgetOrderFeedbackSessionConfigTest, ParsesV1ScopeEndpointAndShm) {
  const OrderFeedbackSessionConfigResult result = Parse(BaseToml());

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_order_feedback_high_availability");
  EXPECT_EQ(result.value.category, "usdt-futures");
  EXPECT_EQ(result.value.position_mode, "one_way_mode");
  EXPECT_EQ(result.value.margin_mode, "crossed");
  EXPECT_EQ(result.value.client_oid_run_namespace.View(), "0123456789AB");
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
  EXPECT_EQ(result.value.shm.shm_name, "aquila_bitget_order_feedback");
  EXPECT_EQ(result.value.shm.channel_name, "orders");
  EXPECT_EQ(result.value.shm.max_strategy_count, 8U);
  EXPECT_EQ(result.value.shm.queue_capacity, 65536U);
  EXPECT_TRUE(result.value.shm.create);
  EXPECT_FALSE(result.value.shm.remove_existing);
}

TEST(BitgetOrderFeedbackSessionConfigTest, LoadsCheckedInConfig) {
  const std::filesystem::path path =
      std::filesystem::path{AQUILA_SOURCE_DIR} /
      "config/order_feedback/bitget_order_feedback_session.toml";

  const OrderFeedbackSessionConfigResult result =
      LoadOrderFeedbackSessionConfigFile(path);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.name, "bitget_order_feedback_high_speed_private");
  EXPECT_EQ(result.value.connection.host, "vip-ws-uta-pri-a.bitget.com");
  EXPECT_EQ(result.value.connection.target, "/v3/ws/private");
  EXPECT_EQ(result.value.client_oid_run_namespace.View(), "000000000000");
  EXPECT_FALSE(result.value.client_oid_run_namespace.IsConfigured());
}

TEST(BitgetOrderFeedbackSessionConfigTest,
     RejectsMissingOrInvalidRunNamespace) {
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

TEST(BitgetOrderFeedbackSessionConfigTest, RejectsUnsupportedScope) {
  for (const auto& [before, after] : {
           std::pair{"category = \"usdt-futures\"",
                     "category = \"coin-futures\""},
           std::pair{"position_mode = \"one_way_mode\"",
                     "position_mode = \"hedge_mode\""},
           std::pair{"margin_mode = \"crossed\"", "margin_mode = \"isolated\""},
       }) {
    std::string text = BaseToml();
    Replace(&text, before, after);
    EXPECT_FALSE(Parse(text).ok) << after;
  }
}

TEST(BitgetOrderFeedbackSessionConfigTest, RejectsMissingCredentialEnvNames) {
  for (const std::string_view line : {
           "api_key_env = \"BITGET_TEST_KEY\"",
           "api_secret_env = \"BITGET_TEST_SECRET\"",
           "api_passphrase_env = \"BITGET_TEST_PASSPHRASE\"",
       }) {
    std::string text = BaseToml();
    Replace(&text, line, "");
    EXPECT_FALSE(Parse(text).ok) << line;
  }
}

TEST(BitgetOrderFeedbackSessionConfigTest,
     RejectsNonPrivateEndpointAndInvalidHeartbeat) {
  for (const auto& [before, after] : {
           std::pair{"target = \"/v3/ws/private\"",
                     "target = \"/v3/ws/public\""},
           std::pair{"enable_tls = true", "enable_tls = false"},
           std::pair{"port = \"443\"", "port = \"80\""},
           std::pair{"interval_ms = 30000", "interval_ms = 30001"},
           std::pair{"timeout_ms = 10000", "timeout_ms = 0"},
       }) {
    std::string text = BaseToml();
    Replace(&text, before, after);
    EXPECT_FALSE(Parse(text).ok) << after;
  }
}

TEST(BitgetOrderFeedbackSessionConfigTest, RejectsShmAbiMismatch) {
  for (const auto& [before, after] : {
           std::pair{"max_strategy_count = 8", "max_strategy_count = 7"},
           std::pair{"queue_capacity = 65536", "queue_capacity = 32768"},
           std::pair{"shm_name = \"aquila_bitget_order_feedback\"",
                     "shm_name = \"\""},
           std::pair{"channel_name = \"orders\"", "channel_name = \"\""},
       }) {
    std::string text = BaseToml();
    Replace(&text, before, after);
    EXPECT_FALSE(Parse(text).ok) << after;
  }

  std::string remove_without_create = BaseToml();
  Replace(&remove_without_create, "create = true", "create = false");
  Replace(&remove_without_create, "remove_existing = false",
          "remove_existing = true");
  EXPECT_FALSE(Parse(remove_without_create).ok);
}

TEST(BitgetOrderFeedbackSessionConfigTest, RejectsMissingName) {
  std::string text = BaseToml();
  Replace(&text, "name = \"bitget_order_feedback_high_availability\"", "");
  EXPECT_FALSE(Parse(text).ok);
}

}  // namespace
}  // namespace aquila::bitget
