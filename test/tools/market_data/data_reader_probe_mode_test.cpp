#include "tools/market_data/data_reader_probe_mode.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/config/data_reader_config.h"

namespace {

namespace cfg = aquila::config;
namespace md_tools = aquila::tools::market_data;

cfg::DataReaderSourceConfig MakeSource(std::string name,
                                       cfg::DataReaderSourceType type) {
  cfg::DataReaderSourceConfig source;
  source.name = std::move(name);
  source.type = type;
  source.exchange = aquila::Exchange::kGate;
  source.feed = cfg::DataReaderFeed::kBookTicker;
  source.shm_name =
      type == cfg::DataReaderSourceType::kShm ? "aquila_gate_market_data" : "";
  source.channel_name =
      type == cfg::DataReaderSourceType::kShm ? "book_ticker_channel" : "";
  source.files = type == cfg::DataReaderSourceType::kBinaryFile
                     ? std::vector<std::filesystem::path>{"/tmp/recorded.bin"}
                     : std::vector<std::filesystem::path>{};
  source.start_position = type == cfg::DataReaderSourceType::kBinaryFile
                              ? cfg::DataReaderStartPosition::kEarliestVisible
                              : cfg::DataReaderStartPosition::kLatest;
  source.read_mode = cfg::DataReaderReadMode::kDrain;
  source.required = true;
  return source;
}

TEST(DataReaderProbeModeTest, DetectsRealtimeForShmSources) {
  cfg::DataReaderConfig config;
  config.sources.push_back(MakeSource("gate", cfg::DataReaderSourceType::kShm));
  config.sources.push_back(
      MakeSource("binance", cfg::DataReaderSourceType::kShm));

  EXPECT_EQ(md_tools::DetectProbeMode(config),
            md_tools::DataReaderProbeMode::kRealtime);
}

TEST(DataReaderProbeModeTest, DetectsHistoricalForSingleBinarySource) {
  cfg::DataReaderConfig config;
  config.sources.push_back(
      MakeSource("recorded", cfg::DataReaderSourceType::kBinaryFile));

  EXPECT_EQ(md_tools::DetectProbeMode(config),
            md_tools::DataReaderProbeMode::kHistorical);
}

TEST(DataReaderProbeModeTest, RejectsMixedSources) {
  cfg::DataReaderConfig config;
  config.sources.push_back(MakeSource("gate", cfg::DataReaderSourceType::kShm));
  config.sources.push_back(
      MakeSource("recorded", cfg::DataReaderSourceType::kBinaryFile));

  EXPECT_THROW((void)md_tools::DetectProbeMode(config), std::invalid_argument);
}

TEST(DataReaderProbeModeTest, RejectsMultipleBinarySources) {
  cfg::DataReaderConfig config;
  config.sources.push_back(
      MakeSource("first", cfg::DataReaderSourceType::kBinaryFile));
  config.sources.push_back(
      MakeSource("second", cfg::DataReaderSourceType::kBinaryFile));

  EXPECT_THROW((void)md_tools::DetectProbeMode(config), std::invalid_argument);
}

}  // namespace
