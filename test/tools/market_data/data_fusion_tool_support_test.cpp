#include "tools/market_data/data_fusion_tool_support.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/trade_fusion_config.h"
#include "exchange/gate/market_data/data_session_config.h"
#include "tools/gate/gate_data_fusion_config.h"

namespace {

namespace md = aquila::market_data;
namespace support = aquila::tools::market_data;
namespace tool_gate = aquila::tools::gate;

tool_gate::GateDataFusionSourceConfig MakeLaunchSource(std::int32_t source_id,
                                                       std::string shm_name) {
  return tool_gate::GateDataFusionSourceConfig{
      .source_id = source_id,
      .data_session_config = "config/data_sessions/gate_data_session.toml",
      .data_session_name = "gate_source",
      .book_ticker_shm_name = std::move(shm_name),
      .book_ticker_channel_name = "book_ticker_channel",
      .remove_existing_source_shm = true,
      .bind_cpu_id = 17,
  };
}

tool_gate::GateDataFusionSourceConfig MakeTradeLaunchSource(
    std::int32_t source_id, std::string shm_name) {
  return tool_gate::GateDataFusionSourceConfig{
      .source_id = source_id,
      .data_session_config = "config/data_sessions/gate_data_session.toml",
      .data_session_name = "gate_trade_source",
      .book_ticker_shm_name = {},
      .book_ticker_channel_name = "book_ticker_channel",
      .trade_shm_name = std::move(shm_name),
      .trade_channel_name = "trade_channel",
      .remove_existing_source_shm = true,
      .bind_cpu_id = 17,
  };
}

md::BookTickerFusionSourceConfig MakeFusionSource(std::int32_t source_id,
                                                  std::string shm_name) {
  return md::BookTickerFusionSourceConfig{
      .source_id = source_id,
      .name = "source",
      .shm_name = std::move(shm_name),
      .channel_name = "book_ticker_channel",
  };
}

md::TradeFusionSourceConfig MakeTradeFusionSource(std::int32_t source_id,
                                                  std::string shm_name) {
  return md::TradeFusionSourceConfig{
      .source_id = source_id,
      .name = "source",
      .shm_name = std::move(shm_name),
      .channel_name = "trade_channel",
  };
}

TEST(DataFusionToolSupportTest, ValidatesFusionSourceAlignment) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion",
      .fusion_config = "fusion.toml",
      .sources = {MakeLaunchSource(0, "src0"), MakeLaunchSource(1, "src1")},
  };
  const md::BookTickerFusionConfig fusion_config{
      .name = "fusion",
      .max_events_per_source = 8,
      .bind_cpu_id = -1,
      .max_symbol_id = 128,
      .output =
          md::BookTickerFusionOutputConfig{
              .shm_name = "output",
              .channel_name = "book_ticker_channel",
              .remove_existing = true,
              .metadata_bin = "/home/liuxiang/tmp/fusion_metadata.bin",
          },
      .sources = {MakeFusionSource(0, "src0"), MakeFusionSource(1, "src1")},
  };

  std::string error;
  EXPECT_TRUE(support::ValidateBookTickerFusionAlignment(
      launch_config, fusion_config, &error));
  EXPECT_TRUE(error.empty());
}

TEST(DataFusionToolSupportTest, ValidatesTradeFusionSourceAlignment) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion_trade",
      .fusion_config = "fusion.toml",
      .feed = support::DataFusionFeed::kTrade,
      .sources = {MakeTradeLaunchSource(0, "src0"),
                  MakeTradeLaunchSource(1, "src1")},
  };
  const md::TradeFusionConfig fusion_config{
      .name = "fusion",
      .max_events_per_source = 8,
      .bind_cpu_id = -1,
      .max_symbol_id = 128,
      .output =
          md::TradeFusionOutputConfig{
              .shm_name = "output",
              .channel_name = "trade_channel",
              .remove_existing = true,
              .metadata_bin = "/home/liuxiang/tmp/trade_fusion_metadata.bin",
          },
      .sources = {MakeTradeFusionSource(0, "src0"),
                  MakeTradeFusionSource(1, "src1")},
  };

  std::string error;
  EXPECT_TRUE(
      support::ValidateTradeFusionAlignment(launch_config, fusion_config,
                                            &error));
  EXPECT_TRUE(error.empty());
}

TEST(DataFusionToolSupportTest, ReportsTradeFusionChannelMismatch) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion_trade",
      .fusion_config = "fusion.toml",
      .feed = support::DataFusionFeed::kTrade,
      .sources = {MakeTradeLaunchSource(0, "src0")},
  };
  md::TradeFusionConfig fusion_config{
      .name = "fusion",
      .max_events_per_source = 8,
      .bind_cpu_id = -1,
      .max_symbol_id = 128,
      .output =
          md::TradeFusionOutputConfig{
              .shm_name = "output",
              .channel_name = "trade_channel",
              .remove_existing = true,
              .metadata_bin = "/home/liuxiang/tmp/trade_fusion_metadata.bin",
          },
      .sources = {MakeTradeFusionSource(0, "src0")},
  };
  fusion_config.sources[0].channel_name = "wrong_channel";

  std::string error;
  EXPECT_FALSE(
      support::ValidateTradeFusionAlignment(launch_config, fusion_config,
                                            &error));
  EXPECT_NE(error.find("channel mismatch"), std::string::npos);
}

TEST(DataFusionToolSupportTest, ReportsFusionSourceMismatch) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion",
      .fusion_config = "fusion.toml",
      .sources = {MakeLaunchSource(0, "launch_src")},
  };
  const md::BookTickerFusionConfig fusion_config{
      .name = "fusion",
      .max_events_per_source = 8,
      .bind_cpu_id = -1,
      .max_symbol_id = 128,
      .output =
          md::BookTickerFusionOutputConfig{
              .shm_name = "output",
              .channel_name = "book_ticker_channel",
              .remove_existing = true,
              .metadata_bin = "/home/liuxiang/tmp/fusion_metadata.bin",
          },
      .sources = {MakeFusionSource(0, "fusion_src")},
  };

  std::string error;
  EXPECT_FALSE(support::ValidateBookTickerFusionAlignment(
      launch_config, fusion_config, &error));
  EXPECT_NE(error.find("shm mismatch"), std::string::npos);
}

TEST(DataFusionToolSupportTest, AppliesBookTickerSourceOverride) {
  const tool_gate::GateDataFusionSourceConfig source =
      MakeLaunchSource(3, "override_src");
  aquila::gate::DataSessionConfig config;
  config.connection.runtime_policy.io_cpu_id = 2;

  support::ApplyBookTickerSourceOverride(source, &config);

  EXPECT_EQ(config.name, source.data_session_name);
  EXPECT_TRUE(config.book_ticker_shm.enabled);
  EXPECT_EQ(config.book_ticker_shm.shm_name, source.book_ticker_shm_name);
  EXPECT_EQ(config.book_ticker_shm.channel_name,
            source.book_ticker_channel_name);
  EXPECT_TRUE(config.book_ticker_shm.create);
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 17);
  EXPECT_EQ(config.diagnostics.latency_outlier.source_id, 3);
}

TEST(DataFusionToolSupportTest, AppliesTradeSourceOverride) {
  const tool_gate::GateDataFusionSourceConfig source =
      MakeTradeLaunchSource(3, "override_trade_src");
  aquila::gate::DataSessionConfig config;
  config.connection.runtime_policy.io_cpu_id = 2;
  config.feeds.book_ticker = true;
  config.feeds.trade = false;

  support::ApplyTradeSourceOverride(source, &config);

  EXPECT_EQ(config.name, source.data_session_name);
  EXPECT_FALSE(config.feeds.book_ticker);
  EXPECT_TRUE(config.feeds.trade);
  EXPECT_TRUE(config.trade_shm.enabled);
  EXPECT_EQ(config.trade_shm.shm_name, source.trade_shm_name);
  EXPECT_EQ(config.trade_shm.channel_name, source.trade_channel_name);
  EXPECT_TRUE(config.trade_shm.create);
  EXPECT_TRUE(config.trade_shm.remove_existing);
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 17);
  EXPECT_EQ(config.diagnostics.latency_outlier.source_id, 3);
}

struct PreparedSourceForTest {
  aquila::gate::DataSessionConfig data_session_config;
};

TEST(DataFusionToolSupportTest, ChecksHomogeneousTls) {
  PreparedSourceForTest plain_a;
  PreparedSourceForTest plain_b;
  plain_a.data_session_config.connection.enable_tls = false;
  plain_b.data_session_config.connection.enable_tls = false;
  const std::vector<PreparedSourceForTest> homogeneous{plain_a, plain_b};

  std::string error;
  EXPECT_TRUE(support::SourcesUseSameTls(homogeneous, "Gate", &error));
  EXPECT_TRUE(error.empty());

  PreparedSourceForTest tls;
  tls.data_session_config.connection.enable_tls = true;
  const std::vector<PreparedSourceForTest> mixed{plain_a, tls};
  EXPECT_FALSE(support::SourcesUseSameTls(mixed, "Gate", &error));
  EXPECT_NE(error.find("Gate"), std::string::npos);
}

}  // namespace
