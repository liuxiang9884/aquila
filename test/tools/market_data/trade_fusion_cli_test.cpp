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

TEST(TradeFusionCliTest, LogsMissingConfigLoadError) {
  const std::filesystem::path missing_config{AQUILA_MISSING_TRADE_FUSION_CONFIG};
  std::error_code error;
  std::filesystem::remove(missing_config, error);
  ASSERT_FALSE(std::filesystem::exists(missing_config));

  const std::string command =
      ShellQuote(AQUILA_GATE_TRADE_FUSION_BINARY) + " --config " +
      ShellQuote(missing_config.string()) + " --max-polls 1 2>&1";

  const CommandResult result = RunCommand(command);
  EXPECT_NE(result.exit_code, 0) << result.output;
  EXPECT_NE(result.output.find("config_error="), std::string::npos)
      << result.output;
}
