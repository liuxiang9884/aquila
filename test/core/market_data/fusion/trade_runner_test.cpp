#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/common/fusion_metadata_mode.h"
#include "core/common/types.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/metadata.h"
#include "core/market_data/fusion/trade.h"

namespace {

namespace md = aquila::market_data;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_trade_fusion_runner_test_{}_{}", ::getpid(),
                     suffix);
}

md::TradeShmConfig MakeCreateConfig(std::string_view suffix) {
  return md::TradeShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .channel_name = "trade_channel",
      .create = true,
      .remove_existing = true,
  };
}

md::TradeShmConfig MakeAttachConfig(const md::TradeShmConfig& create_config) {
  md::TradeShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

std::filesystem::path UniqueMetadataPath() {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("aquila_trade_fusion_runner_test_{}.bin", ::getpid());
}

aquila::Trade MakeTrade(std::int32_t symbol_id, std::int64_t id,
                        std::int64_t exchange_ns, std::int64_t event_ns,
                        std::int64_t local_ns) {
  return aquila::Trade{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = aquila::Exchange::kGate,
      .side = aquila::OrderSide::kSell,
      .reserved = 0,
      .exchange_ns = exchange_ns,
      .event_ns = event_ns,
      .local_ns = local_ns,
      .price = 100.0 + static_cast<double>(id),
      .volume = 0.01 * static_cast<double>(symbol_id + 1),
      .batch_index = 2,
      .batch_count = 4,
  };
}

std::vector<md::TradeFusionMetadataRecord> ReadMetadata(
    const std::filesystem::path& path) {
  const std::uintmax_t size = std::filesystem::file_size(path);
  EXPECT_EQ(size % sizeof(md::TradeFusionMetadataRecord), 0U);
  std::vector<md::TradeFusionMetadataRecord> records(
      size / sizeof(md::TradeFusionMetadataRecord));
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(
                   records.size() * sizeof(md::TradeFusionMetadataRecord)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

TEST(TradeFusionRunnerTest, PublishesCanonicalShmAndOptionalMetadata) {
  const md::TradeShmConfig source0 = MakeCreateConfig("source0");
  const md::TradeShmConfig source1 = MakeCreateConfig("source1");
  const md::TradeShmConfig output = MakeCreateConfig("output");
  ShmCleanup cleanup0(source0.shm_name);
  ShmCleanup cleanup1(source1.shm_name);
  ShmCleanup cleanup_output(output.shm_name);
#if AQUILA_FUSION_METADATA_ENABLED
  const std::filesystem::path metadata_path = UniqueMetadataPath();
  std::filesystem::remove(metadata_path);
#else
  const std::filesystem::path metadata_path;
#endif

  md::DataShmPublisher source0_publisher(source0);
  md::DataShmPublisher source1_publisher(source1);

  md::TradeFusionConfig config{
      .name = "trade_runner_test",
      .max_events_per_source = 8,
      .bind_cpu_id = -1,
      .max_symbol_id = 128,
      .output =
          md::TradeFusionOutputConfig{
              .shm_name = output.shm_name,
              .channel_name = output.channel_name,
              .remove_existing = true,
              .metadata_bin = metadata_path,
          },
      .sources =
          {
              md::TradeFusionSourceConfig{
                  .source_id = 0,
                  .name = "source0",
                  .shm_name = source0.shm_name,
                  .channel_name = source0.channel_name,
              },
              md::TradeFusionSourceConfig{
                  .source_id = 1,
                  .name = "source1",
                  .shm_name = source1.shm_name,
                  .channel_name = source1.channel_name,
              },
          },
  };

  md::TradeFusionRunner runner(config);
  md::TradeShmReader output_reader(MakeAttachConfig(output));
  output_reader.SeekLatest();

  const aquila::Trade source0_first = MakeTrade(42, 100, 1'000, 1'010, 2'000);
  const aquila::Trade source1_duplicate =
      MakeTrade(42, 100, 1'000, 1'010, 1'900);
  const aquila::Trade source1_next = MakeTrade(42, 101, 1'100, 1'110, 2'100);
  source0_publisher.OnTrade(source0_first);
  source1_publisher.OnTrade(source1_duplicate);
  source1_publisher.OnTrade(source1_next);

  const md::TradeFusionPollStats stats = runner.PollOnce();

  EXPECT_EQ(stats.read_count, 3U);
  EXPECT_EQ(stats.published_count, 2U);
  EXPECT_EQ(stats.metadata_write_errors, 0U);
  ASSERT_TRUE(runner.Flush());
  EXPECT_EQ(runner.total_metadata_write_errors(), 0U);

  aquila::Trade first{};
  aquila::Trade second{};
  ASSERT_TRUE(output_reader.TryReadOne(&first));
  ASSERT_TRUE(output_reader.TryReadOne(&second));
  EXPECT_FALSE(output_reader.TryReadOne(&second));
  EXPECT_EQ(first.id, source0_first.id);
  EXPECT_EQ(first.symbol_id, source0_first.symbol_id);
  EXPECT_EQ(first.exchange_ns, source0_first.exchange_ns);
  EXPECT_EQ(first.event_ns, source0_first.event_ns);
  EXPECT_GE(first.local_ns, source0_first.local_ns);
  EXPECT_EQ(second.id, source1_next.id);
  EXPECT_EQ(second.symbol_id, source1_next.symbol_id);
  EXPECT_EQ(second.exchange_ns, source1_next.exchange_ns);
  EXPECT_EQ(second.event_ns, source1_next.event_ns);
  EXPECT_GE(second.local_ns, source1_next.local_ns);

#if AQUILA_FUSION_METADATA_ENABLED
  const std::vector<md::TradeFusionMetadataRecord> metadata =
      ReadMetadata(metadata_path);
  ASSERT_EQ(metadata.size(), 2U);
  EXPECT_EQ(metadata[0].source_id, 0);
  EXPECT_EQ(metadata[0].symbol_id, source0_first.symbol_id);
  EXPECT_EQ(metadata[0].record_id, source0_first.id);
  EXPECT_EQ(metadata[0].exchange_ns, source0_first.exchange_ns);
  EXPECT_EQ(metadata[0].event_ns, source0_first.event_ns);
  EXPECT_EQ(metadata[0].source_local_ns, source0_first.local_ns);
  EXPECT_EQ(metadata[0].fusion_publish_ns, first.local_ns);
  EXPECT_EQ(metadata[1].source_id, 1);
  EXPECT_EQ(metadata[1].record_id, source1_next.id);
  EXPECT_EQ(metadata[1].event_ns, source1_next.event_ns);
  EXPECT_EQ(metadata[1].source_local_ns, source1_next.local_ns);
  EXPECT_EQ(metadata[1].fusion_publish_ns, second.local_ns);

  std::filesystem::remove(metadata_path);
#else
  EXPECT_TRUE(metadata_path.empty());
#endif
}

}  // namespace
