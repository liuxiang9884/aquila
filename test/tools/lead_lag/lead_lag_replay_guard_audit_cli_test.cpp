#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/market_data_binary_format.h"
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

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  output << text;
  ASSERT_TRUE(output.good()) << path;
}

void WriteBookTickerFile(const std::filesystem::path& path,
                         const std::vector<aquila::BookTicker>& tickers) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(output.is_open()) << path;
  ASSERT_TRUE(aquila::market_data::WriteMarketDataBinaryHeader(
      output, aquila::config::DataReaderFeed::kBookTicker));
  for (const aquila::BookTicker& ticker : tickers) {
    output.write(reinterpret_cast<const char*>(&ticker), sizeof(ticker));
  }
  ASSERT_TRUE(output.good()) << path;
}

[[nodiscard]] std::vector<std::string> ReadLines(
    const std::filesystem::path& path) {
  std::ifstream input(path);
  EXPECT_TRUE(input.is_open()) << path;
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    lines.push_back(line);
  }
  return lines;
}

[[nodiscard]] std::vector<std::string> SplitCsvLine(std::string_view line) {
  std::vector<std::string> fields;
  std::size_t offset = 0;
  while (offset <= line.size()) {
    const std::size_t comma = line.find(',', offset);
    if (comma == std::string_view::npos) {
      fields.emplace_back(line.substr(offset));
      break;
    }
    fields.emplace_back(line.substr(offset, comma - offset));
    offset = comma + 1;
  }
  return fields;
}

[[nodiscard]] std::size_t ColumnIndex(const std::vector<std::string>& header,
                                      std::string_view name) {
  for (std::size_t index = 0; index < header.size(); ++index) {
    if (header[index] == name) {
      return index;
    }
  }
  ADD_FAILURE() << "missing CSV column " << name;
  return header.size();
}

[[nodiscard]] std::int64_t CurrentEpochNs() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] aquila::BookTicker Ticker(std::int64_t id,
                                        aquila::Exchange exchange,
                                        std::int64_t base_ns, double bid,
                                        double ask) {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = 3,
      .exchange = exchange,
      .exchange_ns = base_ns + id * 1'000'000 - 10,
      .local_ns = base_ns + id * 1'000'000,
      .bid_price = bid,
      .bid_volume = 1.0,
      .ask_price = ask,
      .ask_volume = 1.0,
  };
}

[[nodiscard]] std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  EXPECT_TRUE(input.is_open()) << path;
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string ReadGeneratedLogFile(
    const std::filesystem::path& configured_path) {
  if (std::filesystem::exists(configured_path)) {
    return ReadTextFile(configured_path);
  }

  std::vector<std::filesystem::path> candidates;
  const std::string generated_prefix = configured_path.stem().string() + "_";
  for (const auto& entry :
       std::filesystem::directory_iterator(configured_path.parent_path())) {
    const std::filesystem::path& path = entry.path();
    const std::string filename = path.filename().string();
    if (entry.is_regular_file() &&
        filename.starts_with(generated_prefix) &&
        path.extension() == configured_path.extension()) {
      candidates.push_back(path);
    }
  }
  EXPECT_EQ(candidates.size(), 1U) << configured_path;
  return candidates.size() == 1U ? ReadTextFile(candidates.front())
                                 : std::string{};
}

[[nodiscard]] std::size_t CountOccurrences(std::string_view text,
                                           std::string_view needle) {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

[[nodiscard]] std::vector<aquila::BookTicker> ParallelOpenTickers(
    std::size_t capacity, std::int64_t base_ns) {
  std::vector<aquila::BookTicker> tickers{
      Ticker(100, aquila::Exchange::kGate, base_ns, 101.5, 102.0),
      Ticker(100, aquila::Exchange::kBinance, base_ns, 100.0, 101.0),
      Ticker(101, aquila::Exchange::kBinance, base_ns, 112.0, 113.0),
  };
  for (std::size_t signal_index = 1; signal_index <= capacity; ++signal_index) {
    if (signal_index == 1) {
      tickers.push_back(
          Ticker(102, aquila::Exchange::kGate, base_ns, 105.0, 106.0));
      tickers.push_back(
          Ticker(103, aquila::Exchange::kBinance, base_ns, 170.0, 171.0));
      continue;
    }
    const std::int64_t ticker_id =
        102 + static_cast<std::int64_t>(signal_index);
    const double bid = 170.0 + 50.0 * static_cast<double>(signal_index - 1);
    tickers.push_back(
        Ticker(ticker_id, aquila::Exchange::kBinance, base_ns, bid, bid + 1.0));
  }
  return tickers;
}

struct ParallelReplayResult {
  CommandResult command;
  std::vector<std::string> signal_lines;
  std::string replay_log;
};

void RunParallelReplay(const std::filesystem::path& run_dir,
                       std::size_t capacity, std::int64_t base_ns,
                       ParallelReplayResult* result) {
  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(std::filesystem::create_directories(run_dir)) << run_dir;
  const std::filesystem::path book_ticker_path = run_dir / "book_ticker.bin";
  const std::filesystem::path catalog_path = run_dir / "instruments.csv";
  const std::filesystem::path data_reader_path = run_dir / "data_reader.toml";
  const std::filesystem::path lead_lag_path = run_dir / "lead_lag.toml";
  const std::filesystem::path replay_path = run_dir / "replay.toml";
  const std::filesystem::path signals_path = run_dir / "signals.csv";
  const std::filesystem::path replay_log_path = run_dir / "replay.log";

  WriteBookTickerFile(book_ticker_path, ParallelOpenTickers(capacity, base_ns));
  WriteTextFile(
      catalog_path,
      R"csv(symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,settle_asset,product_type,status,contract_type,price_tick,price_decimal_places,quantity_step,quantity_decimal_places,min_quantity,max_quantity,max_market_quantity,min_notional,notional_multiplier,price_limit_up,price_limit_down,market_price_bound
3,BTC_USDT,gate,BTC_USDT_GATE,BTC,USDT,USDT,linear_perpetual,TRADING,direct,0.1,1,1,0,1,100000,,,1,0.5,0.5,
3,BTC_USDT,binance,BTCUSDT,BTC,USDT,USDT,linear_perpetual,TRADING,PERPETUAL,0.1,1,0.001,3,0.001,1000,120,100,1,0.05,0.05,0.05
)csv");
  WriteTextFile(data_reader_path, std::string{R"toml(
[instrument_catalog]
file = ")toml"} + catalog_path.string() +
                                      R"toml("
schema = "aquila.instrument.v1"

[data_reader]
name = "lead_lag_parallel_replay_cli_test"
max_events_per_drain = 64

[data_reader.execution_policy]
bind_cpu_id = -1
idle_policy = "spin"

[[data_reader.sources]]
name = "book_ticker"
type = "binary_file"
feed = "book_ticker"
files = [")toml" + book_ticker_path.string() +
                                      R"toml("]
start_position = "earliest_visible"
read_mode = "drain"
required = true
)toml");
  WriteTextFile(lead_lag_path, std::string{R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 3
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.0
max_lead_freshness_ms = 2000000000
max_lag_freshness_ms = 2000000000

[lead_lag.pairs.trigger]
lead = 0.02
close = 0.005
lag_part = 0.5
target_profit_rate = 0.0
drift_period = "1s"
drift_min_samples = 1
drift_warmup = "1ns"

[lead_lag.pairs.trigger.drift_guard]
enabled = false
drift_instant = 10.0
ratio_std = 10.0
ratio_std_window = "1m"
drift_mean = 10.0
drift_mean_window = "1m"

[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.10
down_min = -0.10
down_max = 0.0
precision = 0.000001

[lead_lag.pairs.execute]
open_notional = 1000.0
trailing_stop = 0.05
max_entry_spread = 0.02
open_slippage_ticks = 0
close_slippage_ticks = 0
stoploss_slippage_ticks = 0
close_retry_times = 0
close_retry_slippage_step_ticks = 0
parallel = )toml"} + std::to_string(capacity) +
                                   R"toml(

[lead_lag.pairs.bbo_record]
window = "1s"
stats_window = "1s"
)toml");
  WriteTextFile(replay_path, std::string{R"toml(
[instrument_catalog]
file = ")toml"} + catalog_path.string() +
                                 R"toml("
schema = "aquila.instrument.v1"

[log]
log_level = "info"
file_sink_name = ")toml" + replay_log_path.string() +
                                 R"toml("
console_sink_name = ""
backend_thread_name = "parallel_replay_cli_test_log"
backend_cpu_affinity = -1
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[strategy]
name = "lead_lag"
strategy_id = 4
mode = "dry_run"
order_capacity = 64
config = ")toml" + lead_lag_path.string() +
                                 R"toml("

[strategy.loop]
idle_policy = "spin"
bind_cpu_id = -1
max_loop_seconds = 0

[strategy.data_reader]
config = ")toml" + data_reader_path.string() +
                                 R"toml("

[strategy.order_session]
config = "unused_order_session.toml"

[strategy.feedback]
enabled = false
poll_budget = 32
force_claim = false
)toml");

  const std::string command = ShellQuote(AQUILA_LEAD_LAG_REPLAY_BINARY) +
                              " --config " + ShellQuote(replay_path.string()) +
                              " --signals-output " +
                              ShellQuote(signals_path.string()) + " 2>&1";
  result->command = RunCommand(command);
  result->signal_lines = ReadLines(signals_path);
  result->replay_log = ReadGeneratedLogFile(replay_log_path);
}

}  // namespace

TEST(LeadLagReplayGuardAuditCliTest,
     WritesDriftGuardBlockedOpenSignalToSignalsAndAuditCsv) {
  const std::filesystem::path run_dir =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      ("lead_lag_replay_guard_audit_cli_test_" +
       std::to_string(static_cast<long long>(::getpid())));
  std::error_code error;
  std::filesystem::remove_all(run_dir, error);
  ASSERT_TRUE(std::filesystem::create_directories(run_dir)) << run_dir;

  const std::filesystem::path book_ticker_path = run_dir / "book_ticker.bin";
  const std::filesystem::path catalog_path = run_dir / "instruments.csv";
  const std::filesystem::path data_reader_path = run_dir / "data_reader.toml";
  const std::filesystem::path lead_lag_path = run_dir / "lead_lag.toml";
  const std::filesystem::path replay_path = run_dir / "replay.toml";
  const std::filesystem::path signals_path = run_dir / "signals.csv";
  const std::filesystem::path audit_path = run_dir / "lag_vol_guard_audit.csv";

  constexpr double kLagBid = 101.57;
  constexpr double kLagAsk = 102.02;
  constexpr double kExpectedDriftInstant = 1.01;
  constexpr double kLagMid = (kLagBid + kLagAsk) * 0.5;
  constexpr double kFinalLeadMid = kLagMid / kExpectedDriftInstant;
  const std::int64_t base_ns = CurrentEpochNs() - 10'000'000;
  WriteBookTickerFile(
      book_ticker_path,
      {
          Ticker(100, aquila::Exchange::kGate, base_ns, kLagBid, kLagAsk),
          Ticker(101, aquila::Exchange::kBinance, base_ns, 94.5, 95.5),
          Ticker(102, aquila::Exchange::kGate, base_ns, kLagBid, kLagAsk),
          Ticker(103, aquila::Exchange::kBinance, base_ns, kFinalLeadMid - 0.5,
                 kFinalLeadMid + 0.5),
      });

  WriteTextFile(
      catalog_path,
      R"csv(symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,settle_asset,product_type,status,contract_type,price_tick,price_decimal_places,quantity_step,quantity_decimal_places,min_quantity,max_quantity,max_market_quantity,min_notional,notional_multiplier,price_limit_up,price_limit_down,market_price_bound
3,BTC_USDT,gate,BTC_USDT,BTC,USDT,USDT,linear_perpetual,TRADING,direct,0.1,1,1,0,1,100000,,,1,0.5,0.5,
3,BTC_USDT,binance,BTCUSDT,BTC,USDT,USDT,linear_perpetual,TRADING,PERPETUAL,0.1,1,0.001,3,0.001,1000,120,100,1,0.05,0.05,0.05
)csv");

  WriteTextFile(data_reader_path, std::string{R"toml(
[instrument_catalog]
file = ")toml"} + catalog_path.string() +
                                      R"toml("
schema = "aquila.instrument.v1"

[data_reader]
name = "lead_lag_replay_guard_audit_cli_test"
max_events_per_drain = 16

[data_reader.execution_policy]
bind_cpu_id = -1
idle_policy = "spin"

[[data_reader.sources]]
name = "book_ticker"
type = "binary_file"
feed = "book_ticker"
files = [")toml" + book_ticker_path.string() +
                                      R"toml("]
start_position = "earliest_visible"
read_mode = "drain"
required = true
)toml");

  WriteTextFile(lead_lag_path, R"toml(
[lead_lag]
name = "lead_lag"
version = "1.0"

[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 3
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.00016
max_lead_freshness_ms = 2000000000
max_lag_freshness_ms = 2000000000

[lead_lag.pairs.trigger]
lead = 0.02
close = 0.005
lag_part = 0.5
target_profit_rate = 0.0
drift_period = "1s"
drift_min_samples = 1
drift_warmup = "1ns"

[lead_lag.pairs.trigger.drift_guard]
enabled = true
drift_instant = 0.001
ratio_std = 100.0
ratio_std_window = "1m"
drift_mean = 100.0
drift_mean_window = "1m"

[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.10
down_min = -0.10
down_max = 0.0
precision = 0.000001

[lead_lag.pairs.execute]
open_notional = 1000.0
trailing_stop = 0.05
max_entry_spread = 0.02
open_slippage_ticks = 0
close_slippage_ticks = 0
stoploss_slippage_ticks = 0
close_retry_times = 0
close_retry_slippage_step_ticks = 0
parallel = 1

[lead_lag.pairs.bbo_record]
window = "1s"
stats_window = "1s"
)toml");

  WriteTextFile(replay_path, std::string{R"toml(
[instrument_catalog]
file = ")toml"} + catalog_path.string() +
                                 R"toml("
schema = "aquila.instrument.v1"

[log]
log_level = "info"
file_sink_name = ")toml" + (run_dir / "replay.log").string() +
                                 R"toml("
console_sink_name = ""
backend_thread_name = "replay_guard_audit_cli_test_log"
backend_cpu_affinity = -1
format_pattern = "%(log_level_short_code)%(time) %(process_id):%(thread_id) %(file_name):%(caller_function):%(line_number)] %(message)"
timestamp_pattern = "%Y-%m-%d %H:%M:%S.%Qns"

[strategy]
name = "lead_lag"
strategy_id = 4
mode = "dry_run"
order_capacity = 8
config = ")toml" + lead_lag_path.string() +
                                 R"toml("

[strategy.loop]
idle_policy = "spin"
bind_cpu_id = -1
max_loop_seconds = 0

[strategy.data_reader]
config = ")toml" + data_reader_path.string() +
                                 R"toml("

[strategy.order_session]
config = "unused_order_session.toml"

[strategy.feedback]
enabled = false
poll_budget = 32
force_claim = false
)toml");

  const std::string command =
      ShellQuote(AQUILA_LEAD_LAG_REPLAY_BINARY) + " --config " +
      ShellQuote(replay_path.string()) + " --signals-output " +
      ShellQuote(signals_path.string()) + " --lag-vol-guard-audit-output " +
      ShellQuote(audit_path.string()) + " 2>&1";
  const CommandResult result = RunCommand(command);

  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_NE(result.output.find("signals=1 open=1"), std::string::npos)
      << result.output;

  const std::vector<std::string> signal_lines = ReadLines(signals_path);
  ASSERT_EQ(signal_lines.size(), 2U) << result.output;
  const std::vector<std::string> signal_header = SplitCsvLine(signal_lines[0]);
  const std::vector<std::string> signal_row = SplitCsvLine(signal_lines[1]);
  ASSERT_EQ(signal_row.size(), signal_header.size());
  EXPECT_EQ(signal_row[ColumnIndex(signal_header, "action")], "kOpenLong");

  const std::vector<std::string> audit_lines = ReadLines(audit_path);
  ASSERT_EQ(audit_lines.size(), 2U) << result.output;
  const std::vector<std::string> audit_header = SplitCsvLine(audit_lines[0]);
  const std::vector<std::string> audit_row = SplitCsvLine(audit_lines[1]);
  ASSERT_EQ(audit_row.size(), audit_header.size());
  EXPECT_EQ(audit_row[ColumnIndex(audit_header, "drift_guard_outcome")],
            "blocked:instant");
  const std::string& drift_instant =
      audit_row[ColumnIndex(audit_header, "drift_instant")];
  ASSERT_NE(drift_instant, "nan");
  ASSERT_FALSE(drift_instant.empty());
  const double drift_instant_value = std::stod(drift_instant);
  ASSERT_TRUE(std::isfinite(drift_instant_value)) << drift_instant;
  EXPECT_NEAR(drift_instant_value, kExpectedDriftInstant, 1e-9);
}

TEST(LeadLagReplayGuardAuditCliTest,
     ParallelCapacityUsesMonotonicGroupsAndDeterministicSignals) {
  const std::filesystem::path run_root =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      ("lead_lag_parallel_replay_cli_test_" +
       std::to_string(static_cast<long long>(::getpid())));
  std::error_code error;
  std::filesystem::remove_all(run_root, error);
  ASSERT_TRUE(std::filesystem::create_directories(run_root)) << run_root;
  const std::int64_t base_ns = CurrentEpochNs() - 100'000'000;

  constexpr std::array<std::size_t, 5> kCapacities{1, 2, 4, 8, 16};
  for (const std::size_t capacity : kCapacities) {
    SCOPED_TRACE(::testing::Message() << "parallel=" << capacity);
    ParallelReplayResult first;
    ParallelReplayResult second;
    RunParallelReplay(
        run_root / ("parallel_" + std::to_string(capacity) + "_first"),
        capacity, base_ns, &first);
    RunParallelReplay(
        run_root / ("parallel_" + std::to_string(capacity) + "_second"),
        capacity, base_ns, &second);

    ASSERT_EQ(first.command.exit_code, 0) << first.command.output;
    ASSERT_EQ(second.command.exit_code, 0) << second.command.output;
    const std::string expected_summary =
        "signals=" + std::to_string(capacity + 1) +
        " open=" + std::to_string(capacity + 1) + " close=0 stoploss=0";
    EXPECT_NE(first.command.output.find(expected_summary), std::string::npos)
        << first.command.output;
    EXPECT_NE(second.command.output.find(expected_summary), std::string::npos)
        << second.command.output;
    ASSERT_EQ(first.signal_lines, second.signal_lines);
    ASSERT_EQ(first.signal_lines.size(), capacity + 2);

    const std::vector<std::string> header =
        SplitCsvLine(first.signal_lines.front());
    const std::size_t action_column = ColumnIndex(header, "action");
    const std::size_t group_column = ColumnIndex(header, "group_id");
    const std::size_t active_column = ColumnIndex(header, "active_group_count");
    ASSERT_LT(action_column, header.size());
    ASSERT_LT(group_column, header.size());
    ASSERT_LT(active_column, header.size());
    for (std::size_t index = 0; index < capacity; ++index) {
      const std::vector<std::string> row =
          SplitCsvLine(first.signal_lines[index + 1]);
      ASSERT_EQ(row.size(), header.size());
      EXPECT_EQ(row[action_column], "kOpenLong");
      EXPECT_EQ(row[group_column], std::to_string(index + 1));
      EXPECT_EQ(row[active_column], std::to_string(index + 1));
    }
    const std::vector<std::string> rejected =
        SplitCsvLine(first.signal_lines.back());
    ASSERT_EQ(rejected.size(), header.size());
    EXPECT_EQ(rejected[action_column], "kOpenLong");
    EXPECT_EQ(rejected[group_column], "0");
    EXPECT_EQ(rejected[active_column], std::to_string(capacity));
    EXPECT_EQ(CountOccurrences(first.replay_log, "reason=parallel_limit"), 1U);
    EXPECT_EQ(CountOccurrences(second.replay_log, "reason=parallel_limit"), 1U);
  }
}
