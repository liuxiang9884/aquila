#include "tools/market_data/data_fusion_tool_support.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/market_data/fusion/config.h"
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
      .data_shm_name = std::move(shm_name),
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
      .data_shm_name = std::move(shm_name),
      .book_ticker_channel_name = "book_ticker_channel",
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
  EXPECT_TRUE(
      support::ValidateFusionAlignment<support::BookTickerDataFusionFeedTraits>(
          launch_config, fusion_config, &error));
  EXPECT_TRUE(error.empty());
}

TEST(DataFusionToolSupportTest, ValidatesTradeFusionSourceAlignment) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion_trade",
      .feeds = {support::DataFusionFeed::kTrade},
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
      support::ValidateFusionAlignment<support::TradeDataFusionFeedTraits>(
          launch_config, fusion_config, &error));
  EXPECT_TRUE(error.empty());
}

TEST(DataFusionToolSupportTest, ReportsTradeFusionChannelMismatch) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion_trade",
      .feeds = {support::DataFusionFeed::kTrade},
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
      support::ValidateFusionAlignment<support::TradeDataFusionFeedTraits>(
          launch_config, fusion_config, &error));
  EXPECT_NE(error.find("channel mismatch"), std::string::npos);
}

TEST(DataFusionToolSupportTest, ReportsUnexpectedBookTickerFusionSource) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion",
      .sources = {MakeLaunchSource(0, "src0")},
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
  EXPECT_FALSE(
      support::ValidateFusionAlignment<support::BookTickerDataFusionFeedTraits>(
          launch_config, fusion_config, &error));
  EXPECT_NE(error.find("unexpected fusion source_id=1"), std::string::npos);
}

TEST(DataFusionToolSupportTest, ReportsUnexpectedTradeFusionSource) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion_trade",
      .feeds = {support::DataFusionFeed::kTrade},
      .sources = {MakeTradeLaunchSource(0, "src0")},
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
  EXPECT_FALSE(
      support::ValidateFusionAlignment<support::TradeDataFusionFeedTraits>(
          launch_config, fusion_config, &error));
  EXPECT_NE(error.find("unexpected fusion source_id=1"), std::string::npos);
}

TEST(DataFusionToolSupportTest, ReportsFusionSourceMismatch) {
  const tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion",
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
  EXPECT_FALSE(
      support::ValidateFusionAlignment<support::BookTickerDataFusionFeedTraits>(
          launch_config, fusion_config, &error));
  EXPECT_NE(error.find("shm mismatch"), std::string::npos);
}

TEST(DataFusionToolSupportTest, FormatsBookTickerFusionMetadataOutput) {
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
      .sources = {MakeFusionSource(0, "src0")},
  };

  if constexpr (aquila::kFusionMetadataEnabled) {
    EXPECT_EQ(support::FormatFusionMetadataOutput<
                  support::BookTickerDataFusionFeedTraits>(fusion_config),
              "/home/liuxiang/tmp/fusion_metadata.bin");
  } else {
    EXPECT_EQ(support::FormatFusionMetadataOutput<
                  support::BookTickerDataFusionFeedTraits>(fusion_config),
              "disabled");
  }
}

TEST(DataFusionToolSupportTest, FormatsTradeFusionMetadataOutput) {
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
      .sources = {MakeTradeFusionSource(0, "src0")},
  };

  if constexpr (aquila::kFusionMetadataEnabled) {
    EXPECT_EQ(
        support::FormatFusionMetadataOutput<support::TradeDataFusionFeedTraits>(
            fusion_config),
        "/home/liuxiang/tmp/trade_fusion_metadata.bin");
  } else {
    EXPECT_EQ(
        support::FormatFusionMetadataOutput<support::TradeDataFusionFeedTraits>(
            fusion_config),
        "disabled");
  }
}

TEST(DataFusionToolSupportTest, AppliesBookTickerSourceOverrides) {
  const tool_gate::GateDataFusionSourceConfig source =
      MakeLaunchSource(3, "override_src");
  aquila::gate::DataSessionConfig config;
  config.connection.runtime_policy.io_cpu_id = 2;

  support::ApplyFusionSourceOverrides({support::DataFusionFeed::kBookTicker},
                                      source, &config);

  EXPECT_EQ(config.name, source.data_session_name);
  EXPECT_TRUE(config.feeds.book_ticker);
  EXPECT_FALSE(config.feeds.trade);
  EXPECT_TRUE(config.data_shm.enabled);
  EXPECT_TRUE(config.data_shm.book_ticker_enabled);
  EXPECT_FALSE(config.data_shm.trade_enabled);
  EXPECT_EQ(config.data_shm.shm_name, source.data_shm_name);
  EXPECT_TRUE(config.book_ticker_shm.enabled);
  EXPECT_EQ(config.book_ticker_shm.shm_name, source.data_shm_name);
  EXPECT_EQ(config.book_ticker_shm.channel_name,
            source.book_ticker_channel_name);
  EXPECT_TRUE(config.book_ticker_shm.create);
  EXPECT_TRUE(config.book_ticker_shm.remove_existing);
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 17);
  EXPECT_EQ(config.diagnostics.latency_outlier.source_id, 3);
}

TEST(DataFusionToolSupportTest, AppliesTradeSourceOverrides) {
  const tool_gate::GateDataFusionSourceConfig source =
      MakeTradeLaunchSource(3, "override_trade_src");
  aquila::gate::DataSessionConfig config;
  config.connection.runtime_policy.io_cpu_id = 2;
  config.feeds.book_ticker = true;
  config.feeds.trade = false;

  support::ApplyFusionSourceOverrides({support::DataFusionFeed::kTrade}, source,
                                      &config);

  EXPECT_EQ(config.name, source.data_session_name);
  EXPECT_FALSE(config.feeds.book_ticker);
  EXPECT_TRUE(config.feeds.trade);
  EXPECT_TRUE(config.data_shm.enabled);
  EXPECT_FALSE(config.data_shm.book_ticker_enabled);
  EXPECT_TRUE(config.data_shm.trade_enabled);
  EXPECT_EQ(config.data_shm.shm_name, source.data_shm_name);
  EXPECT_TRUE(config.trade_shm.enabled);
  EXPECT_EQ(config.trade_shm.shm_name, source.data_shm_name);
  EXPECT_EQ(config.trade_shm.channel_name, source.trade_channel_name);
  EXPECT_TRUE(config.trade_shm.create);
  EXPECT_TRUE(config.trade_shm.remove_existing);
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 17);
  EXPECT_EQ(config.diagnostics.latency_outlier.source_id, 3);
}

TEST(DataFusionToolSupportTest, AppliesMultiFeedSourceOverrides) {
  tool_gate::GateDataFusionSourceConfig source =
      MakeLaunchSource(3, "legacy_book_src");
  source.data_shm_name = "combined_src";
  source.trade_channel_name = "trade_channel";
  aquila::gate::DataSessionConfig config;
  config.connection.runtime_policy.io_cpu_id = 2;

  support::ApplyFusionSourceOverrides(
      {support::DataFusionFeed::kBookTicker, support::DataFusionFeed::kTrade},
      source, &config);

  EXPECT_EQ(config.name, source.data_session_name);
  EXPECT_TRUE(config.feeds.book_ticker);
  EXPECT_TRUE(config.feeds.trade);
  EXPECT_TRUE(config.data_shm.enabled);
  EXPECT_TRUE(config.data_shm.book_ticker_enabled);
  EXPECT_TRUE(config.data_shm.trade_enabled);
  EXPECT_EQ(config.data_shm.shm_name, "combined_src");
  EXPECT_EQ(config.data_shm.book_ticker_channel_name,
            source.book_ticker_channel_name);
  EXPECT_EQ(config.data_shm.trade_channel_name, source.trade_channel_name);
  EXPECT_TRUE(config.data_shm.create);
  EXPECT_TRUE(config.data_shm.remove_existing);
  EXPECT_TRUE(config.book_ticker_shm.enabled);
  EXPECT_TRUE(config.trade_shm.enabled);
  EXPECT_EQ(config.connection.runtime_policy.io_cpu_id, 17);
  EXPECT_EQ(config.diagnostics.latency_outlier.source_id, 3);
}

TEST(DataFusionToolSupportTest, AppliesSingleFeedSourceOverrides) {
  tool_gate::GateDataFusionSourceConfig source =
      MakeTradeLaunchSource(3, "legacy_trade_src");
  source.data_shm_name = "trade_only_src";
  aquila::gate::DataSessionConfig config;

  support::ApplyFusionSourceOverrides({support::DataFusionFeed::kTrade}, source,
                                      &config);

  EXPECT_FALSE(config.feeds.book_ticker);
  EXPECT_TRUE(config.feeds.trade);
  EXPECT_TRUE(config.data_shm.enabled);
  EXPECT_FALSE(config.data_shm.book_ticker_enabled);
  EXPECT_TRUE(config.data_shm.trade_enabled);
  EXPECT_EQ(config.data_shm.shm_name, "trade_only_src");
  EXPECT_FALSE(config.book_ticker_shm.enabled);
  EXPECT_TRUE(config.trade_shm.enabled);
}

TEST(DataFusionToolSupportTest, RejectsCpuBindingOverlap) {
  tool_gate::GateDataFusionConfig launch_config{
      .name = "gate_data_fusion",
      .sources = {MakeLaunchSource(0, "src0"), MakeLaunchSource(1, "src1")},
  };
  launch_config.feeds = {support::DataFusionFeed::kBookTicker,
                         support::DataFusionFeed::kTrade};
  launch_config.sources[0].bind_cpu_id = 17;
  launch_config.sources[1].bind_cpu_id = 18;
  launch_config.backend_cpu_affinity = 31;
  md::BookTickerFusionConfig book_ticker_config{
      .name = "book_ticker_fusion",
      .bind_cpu_id = 16,
  };
  md::TradeFusionConfig trade_config{
      .name = "trade_fusion",
      .bind_cpu_id = 16,
  };

  std::string error;
  EXPECT_FALSE(support::ValidateDataFusionCpuBindings(
      launch_config, &book_ticker_config, &trade_config, &error));
  EXPECT_NE(error.find("cpu binding overlap"), std::string::npos);

  trade_config.bind_cpu_id = 19;
  EXPECT_TRUE(support::ValidateDataFusionCpuBindings(
      launch_config, &book_ticker_config, &trade_config, &error));
  EXPECT_TRUE(error.empty());

  launch_config.backend_cpu_affinity = 19;
  EXPECT_FALSE(support::ValidateDataFusionCpuBindings(
      launch_config, &book_ticker_config, &trade_config, &error));
  EXPECT_NE(error.find("log_backend"), std::string::npos);
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
