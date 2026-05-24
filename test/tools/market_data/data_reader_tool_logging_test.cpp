#include "tools/market_data/data_reader_tool_logging.h"

#include <filesystem>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/types.h"

namespace {

namespace cfg = aquila::config;
namespace md_tools = aquila::tools::market_data;

cfg::DataReaderSourceConfig MakeShmSource() {
  return cfg::DataReaderSourceConfig{
      .name = "gate_book_ticker",
      .type = cfg::DataReaderSourceType::kShm,
      .exchange = aquila::Exchange::kGate,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = "aquila_gate_market_data",
      .channel_name = "book_ticker_channel",
      .files = {},
      .start_position = cfg::DataReaderStartPosition::kLatest,
      .read_mode = cfg::DataReaderReadMode::kDrain,
      .required = true,
  };
}

cfg::DataReaderSourceConfig MakeBinarySource() {
  return cfg::DataReaderSourceConfig{
      .name = "recorded_book_ticker",
      .type = cfg::DataReaderSourceType::kBinaryFile,
      .exchange = aquila::Exchange::kGate,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = {},
      .channel_name = {},
      .files = {std::filesystem::path{"/home/liuxiang/tmp/live.bin"}},
      .start_position = cfg::DataReaderStartPosition::kEarliestVisible,
      .read_mode = cfg::DataReaderReadMode::kDrain,
      .required = true,
  };
}

TEST(DataReaderToolLoggingTest, FormatsShmSourceConfig) {
  const std::string line = md_tools::FormatSourceConfigLog(2, MakeShmSource());
  EXPECT_NE(line.find("source_config index=2"), std::string::npos);
  EXPECT_NE(line.find("name=gate_book_ticker"), std::string::npos);
  EXPECT_NE(line.find("exchange=kGate"), std::string::npos);
  EXPECT_NE(line.find("type=kShm"), std::string::npos);
  EXPECT_NE(line.find("start_position=latest"), std::string::npos);
  EXPECT_NE(line.find("read_mode=drain"), std::string::npos);
  EXPECT_NE(line.find("shm_name=aquila_gate_market_data"), std::string::npos);
  EXPECT_NE(line.find("channel_name=book_ticker_channel"), std::string::npos);
}

TEST(DataReaderToolLoggingTest, FormatsBinarySourceConfig) {
  const std::string line =
      md_tools::FormatSourceConfigLog(0, MakeBinarySource());
  EXPECT_NE(line.find("source_config index=0"), std::string::npos);
  EXPECT_NE(line.find("exchange=record_embedded"), std::string::npos);
  EXPECT_EQ(line.find("exchange=kGate"), std::string::npos);
  EXPECT_NE(line.find("type=kBinaryFile"), std::string::npos);
  EXPECT_NE(line.find("start_position=earliest_visible"), std::string::npos);
  EXPECT_NE(line.find("files=[/home/liuxiang/tmp/live.bin]"),
            std::string::npos);
}

TEST(DataReaderToolLoggingTest, FormatsStartupWithAndWithoutOutputPath) {
  const std::string recorder = md_tools::FormatToolStartupLog(
      "data_reader_recorder", "realtime", "config.toml",
      std::filesystem::path{"/home/liuxiang/tmp/out.bin"}, 100, 64,
      sizeof(aquila::BookTicker));
  EXPECT_NE(recorder.find("tool=data_reader_recorder"), std::string::npos);
  EXPECT_NE(recorder.find("mode=realtime"), std::string::npos);
  EXPECT_NE(recorder.find("output=/home/liuxiang/tmp/out.bin"),
            std::string::npos);
  EXPECT_NE(recorder.find("book_ticker_abi_size="), std::string::npos);

  const std::string probe = md_tools::FormatToolStartupLog(
      "data_reader_probe", "historical", "binary.toml", std::nullopt, 0, 4096,
      sizeof(aquila::BookTicker));
  EXPECT_NE(probe.find("tool=data_reader_probe"), std::string::npos);
  EXPECT_NE(probe.find("mode=historical"), std::string::npos);
  EXPECT_NE(probe.find("output=none"), std::string::npos);
}

}  // namespace
