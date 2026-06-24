#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  output << text;
  ASSERT_TRUE(output.good()) << path;
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

TEST(LeadLagFreshnessPreflightCliTest, RequiresLeadLagConfigBeforeOpeningShm) {
  const std::filesystem::path config_path =
      std::filesystem::path{AQUILA_MISSING_FRESHNESS_PREFLIGHT_CONFIG}
          .parent_path() /
      "freshness_preflight_valid_data_reader_missing_lead_lag.toml";
  WriteTextFile(config_path, R"toml(
[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260602.csv"
schema = "aquila.instrument.v1"

[log]
log_level = "info"
file_sink_name = "/home/liuxiang/tmp/freshness_preflight_cli_test.log"
console_sink_name = "freshness_preflight_cli_test_console"
backend_thread_name = "freshness_preflight_cli_test_log"
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[data_reader]
name = "freshness_preflight_cli_test"
max_events_per_drain = 1

[data_reader.execution_policy]
idle_policy = "spin"

[[data_reader.sources]]
name = "missing_gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_missing_freshness_preflight_cli_test"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "drain"
required = true
)toml");

  const std::string command =
      ShellQuote(AQUILA_LEAD_LAG_FRESHNESS_PREFLIGHT_BINARY) +
      " --data-reader-config " + ShellQuote(config_path.string()) +
      " --duration-sec 0 2>&1";

  const CommandResult result = RunCommand(command);
  EXPECT_NE(result.exit_code, 0) << result.output;
  EXPECT_NE(result.output.find("--lead-lag-config-in is required"),
            std::string::npos)
      << result.output;
}
