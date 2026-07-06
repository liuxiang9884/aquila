#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

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

[[nodiscard]] std::string TomlQuote(std::string_view value) {
  std::string quoted{"\""};
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') {
      quoted += '\\';
    }
    quoted += ch;
  }
  quoted += '"';
  return quoted;
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  output << text;
  ASSERT_TRUE(output.good()) << path;
}

void WriteTradeFile(const std::filesystem::path& path,
                    const std::vector<aquila::Trade>& trades) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  for (const aquila::Trade& trade : trades) {
    output.write(reinterpret_cast<const char*>(&trade), sizeof(trade));
  }
  ASSERT_TRUE(output.good()) << path;
}

[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

[[nodiscard]] aquila::Trade MakeTrade(std::int64_t id) noexcept {
  return aquila::Trade{
      .id = id,
      .symbol_id = 0,
      .exchange = aquila::Exchange::kGate,
      .side = id % 2 == 0 ? aquila::OrderSide::kBuy : aquila::OrderSide::kSell,
      .reserved = 0,
      .exchange_ns = 1'770'000'000'000'000'000 + id,
      .trade_ns = 1'770'000'000'000'100'000 + id,
      .local_ns = 1'770'000'000'000'200'000 + id,
      .price = 65'000.0 + static_cast<double>(id),
      .volume = 0.001 * static_cast<double>(id),
      .batch_index = 0,
      .batch_count = 1,
  };
}

}  // namespace

TEST(DataReaderProbeCliTest, HistoricalTradeBinaryReportsTradeCount) {
  const std::filesystem::path root =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      fmt::format("aquila_data_reader_probe_cli_test_{}", ::getpid());
  std::error_code error;
  std::filesystem::remove_all(root, error);
  ASSERT_TRUE(std::filesystem::create_directories(root)) << root;

  const std::filesystem::path trade_file = root / "recorded_trade.bin";
  const std::filesystem::path config_file = root / "trade_reader.toml";
  const std::filesystem::path log_file = root / "probe.log";
  WriteTradeFile(trade_file, {MakeTrade(1), MakeTrade(2), MakeTrade(3)});

  WriteTextFile(config_file,
                fmt::format(R"toml(
[instrument_catalog]
file = {}
schema = "aquila.instrument.v1"

[log]
log_level = "info"
file_sink_name = {}
console_sink_name = "data_reader_probe_cli_test_console"
backend_thread_name = "data_reader_probe_cli_test_log"
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[data_reader]
name = "data_reader_probe_cli_trade"
max_events_per_drain = 2

[[data_reader.sources]]
name = "recorded_trade"
type = "binary_file"
feed = "trade"
files = [
  {},
]
start_position = "earliest_visible"
read_mode = "drain"
required = true
)toml",
                            TomlQuote(AQUILA_DATA_READER_PROBE_CATALOG),
                            TomlQuote(log_file.string()),
                            TomlQuote(trade_file.string())));

  const std::string command = ShellQuote(AQUILA_DATA_READER_PROBE_BINARY) +
                              " --config " + ShellQuote(config_file.string()) +
                              " --max-polls 8 --log-every 1 2>&1";
  const CommandResult result = RunCommand(command);
  const std::string logs = result.output + ReadTextFile(log_file);

  EXPECT_EQ(result.exit_code, 0) << logs;
  EXPECT_NE(logs.find("result=ok mode=historical"), std::string::npos) << logs;
  EXPECT_NE(logs.find("handler_book_tickers=0"), std::string::npos) << logs;
  EXPECT_NE(logs.find("handler_trades=3"), std::string::npos) << logs;
  EXPECT_NE(logs.find("diagnostics_total_count=3"), std::string::npos) << logs;
  EXPECT_NE(logs.find("files_completed=1"), std::string::npos) << logs;

  std::filesystem::remove_all(root, error);
}
