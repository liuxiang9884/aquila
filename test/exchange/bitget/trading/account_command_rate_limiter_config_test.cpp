#include <string_view>

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include "exchange/bitget/trading/account_command_rate_limiter.h"

namespace aquila::bitget {
namespace {

AccountCommandRateLimitConfigResult ParseConfig(std::string_view text) {
  const toml::parse_result parsed = toml::parse(text);
  return ParseAccountCommandRateLimitConfig(parsed);
}

TEST(AccountCommandRateLimitConfigTest, ParsesEnabledConfiguration) {
  const auto result = ParseConfig(R"toml(
[bitget_account_command_rate_limit]
enabled = true
max_commands_per_second = 5
reserved_exit_commands = 2
)toml");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.enabled);
  EXPECT_EQ(result.value.max_commands_per_second, 5U);
  EXPECT_EQ(result.value.reserved_exit_commands, 2U);
}

TEST(AccountCommandRateLimitConfigTest, DefaultsToDisabledWhenMissing) {
  const auto result = ParseConfig("");

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_FALSE(result.value.enabled);
}

TEST(AccountCommandRateLimitConfigTest, RejectsInvalidEnabledConfiguration) {
  const auto zero_limit = ParseConfig(R"toml(
[bitget_account_command_rate_limit]
enabled = true
max_commands_per_second = 0
reserved_exit_commands = 0
)toml");
  EXPECT_FALSE(zero_limit.ok);

  const auto excessive_reserve = ParseConfig(R"toml(
[bitget_account_command_rate_limit]
enabled = true
max_commands_per_second = 5
reserved_exit_commands = 5
)toml");
  EXPECT_FALSE(excessive_reserve.ok);

  const auto missing_enabled = ParseConfig(R"toml(
[bitget_account_command_rate_limit]
max_commands_per_second = 5
reserved_exit_commands = 2
)toml");
  EXPECT_FALSE(missing_enabled.ok);

  const auto wrong_section_type =
      ParseConfig("bitget_account_command_rate_limit = \"enabled\"");
  EXPECT_FALSE(wrong_section_type.ok);
}

}  // namespace
}  // namespace aquila::bitget
