#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include <gtest/gtest.h>

namespace {

struct CommandResult {
  int exit_code{-1};
  std::string output;
};

[[nodiscard]] std::string ShellQuote(std::string_view value) {
  std::string quoted{"'"};
  for (const char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted += ch;
    }
  }
  quoted += "'";
  return quoted;
}

[[nodiscard]] CommandResult RunCommand(const std::string& command) {
  CommandResult result;
  FILE* pipe = ::popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return result;
  }

  std::array<char, 4096> buffer{};
  while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) !=
         nullptr) {
    result.output += buffer.data();
  }

  const int status = ::pclose(pipe);
  if (status == -1) {
    return result;
  }
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
  }
  return result;
}

}  // namespace

TEST(LeadLagFreshnessPreflightCliTest, LogsMissingDataReaderConfigError) {
  const std::filesystem::path missing_config{
      AQUILA_MISSING_FRESHNESS_PREFLIGHT_CONFIG};
  std::error_code error;
  std::filesystem::remove(missing_config, error);
  ASSERT_FALSE(std::filesystem::exists(missing_config));

  const std::string command =
      ShellQuote(AQUILA_LEAD_LAG_FRESHNESS_PREFLIGHT_BINARY) +
      " --data-reader-config " + ShellQuote(missing_config.string()) +
      " --duration-sec 0 2>&1";

  const CommandResult result = RunCommand(command);
  EXPECT_NE(result.exit_code, 0) << result.output;
  EXPECT_NE(result.output.find("config_error="), std::string::npos)
      << result.output;
}
