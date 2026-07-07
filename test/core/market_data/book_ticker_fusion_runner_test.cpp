#include "core/market_data/book_ticker_fusion_runner.h"

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
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_metadata.h"
#include "core/market_data/data_shm.h"

namespace {

namespace md = aquila::market_data;
namespace tool = aquila::market_data;

struct ShmCleanup {
  explicit ShmCleanup(std::string shm_name_in)
      : shm_name(std::move(shm_name_in)) {}

  ~ShmCleanup() {
    ::shm_unlink(shm_name.c_str());
  }

  std::string shm_name;
};

std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("/aquila_book_ticker_fusion_runner_test_{}_{}", ::getpid(),
                     suffix);
}

md::BookTickerShmConfig MakeCreateConfig(std::string_view suffix) {
  return md::BookTickerShmConfig{
      .enabled = true,
      .shm_name = UniqueShmName(suffix),
      .channel_name = "book_ticker_channel",
      .create = true,
      .remove_existing = true,
  };
}

md::BookTickerShmConfig MakeAttachConfig(
    const md::BookTickerShmConfig& create_config) {
  md::BookTickerShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

std::filesystem::path UniqueMetadataPath() {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("aquila_book_ticker_fusion_runner_test_{}.bin",
                     ::getpid());
}

aquila::BookTicker MakeTicker(std::int32_t symbol_id, std::int64_t id,
                              std::int64_t exchange_ns, std::int64_t local_ns) {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = aquila::Exchange::kGate,
      .exchange_ns = exchange_ns,
      .local_ns = local_ns,
      .bid_price = 100.0 + static_cast<double>(id),
      .bid_volume = 1.0 + static_cast<double>(symbol_id),
      .ask_price = 101.0 + static_cast<double>(id),
      .ask_volume = 2.0 + static_cast<double>(symbol_id),
  };
}

std::vector<tool::FusionMetadataRecord> ReadMetadata(
    const std::filesystem::path& path) {
  const std::uintmax_t size = std::filesystem::file_size(path);
  EXPECT_EQ(size % sizeof(tool::FusionMetadataRecord), 0U);
  std::vector<tool::FusionMetadataRecord> records(
      size / sizeof(tool::FusionMetadataRecord));
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(
                   records.size() * sizeof(tool::FusionMetadataRecord)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

TEST(BookTickerFusionRunnerTest, PublishesCanonicalShmAndOptionalMetadata) {
  const md::BookTickerShmConfig source0 = MakeCreateConfig("source0");
  const md::BookTickerShmConfig source1 = MakeCreateConfig("source1");
  const md::BookTickerShmConfig output = MakeCreateConfig("output");
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

  tool::BookTickerFusionConfig config{
      .name = "runner_test",
      .max_events_per_source = 8,
      .bind_cpu_id = -1,
      .max_symbol_id = 128,
      .output =
          tool::BookTickerFusionOutputConfig{
              .shm_name = output.shm_name,
              .channel_name = output.channel_name,
              .remove_existing = true,
              .metadata_bin = metadata_path,
          },
      .sources =
          {
              tool::BookTickerFusionSourceConfig{
                  .source_id = 0,
                  .name = "source0",
                  .shm_name = source0.shm_name,
                  .channel_name = source0.channel_name,
              },
              tool::BookTickerFusionSourceConfig{
                  .source_id = 1,
                  .name = "source1",
                  .shm_name = source1.shm_name,
                  .channel_name = source1.channel_name,
              },
          },
  };

  tool::BookTickerFusionRunner runner(config);
  md::BookTickerShmReader output_reader(MakeAttachConfig(output));
  output_reader.SeekLatest();

  const aquila::BookTicker source0_first = MakeTicker(42, 100, 1'000, 2'000);
  const aquila::BookTicker source1_duplicate =
      MakeTicker(42, 100, 1'000, 1'900);
  const aquila::BookTicker source1_next = MakeTicker(42, 101, 1'010, 2'100);
  source0_publisher.OnBookTicker(source0_first);
  source1_publisher.OnBookTicker(source1_duplicate);
  source1_publisher.OnBookTicker(source1_next);

  const tool::BookTickerFusionPollStats stats = runner.PollOnce();

  EXPECT_EQ(stats.read_count, 3U);
  EXPECT_EQ(stats.published_count, 2U);
  EXPECT_EQ(stats.metadata_write_errors, 0U);
  ASSERT_TRUE(runner.Flush());
  EXPECT_EQ(runner.total_metadata_write_errors(), 0U);

  aquila::BookTicker first{};
  aquila::BookTicker second{};
  ASSERT_TRUE(output_reader.TryReadOne(&first));
  ASSERT_TRUE(output_reader.TryReadOne(&second));
  EXPECT_FALSE(output_reader.TryReadOne(&second));
  EXPECT_EQ(first.id, source0_first.id);
  EXPECT_EQ(first.symbol_id, source0_first.symbol_id);
  EXPECT_EQ(first.exchange_ns, source0_first.exchange_ns);
  EXPECT_GE(first.local_ns, source0_first.local_ns);
  EXPECT_EQ(second.id, source1_next.id);
  EXPECT_EQ(second.symbol_id, source1_next.symbol_id);
  EXPECT_EQ(second.exchange_ns, source1_next.exchange_ns);
  EXPECT_GE(second.local_ns, source1_next.local_ns);

#if AQUILA_FUSION_METADATA_ENABLED
  const std::vector<tool::FusionMetadataRecord> metadata =
      ReadMetadata(metadata_path);
  ASSERT_EQ(metadata.size(), 2U);
  EXPECT_EQ(metadata[0].source_id, 0);
  EXPECT_EQ(metadata[0].symbol_id, source0_first.symbol_id);
  EXPECT_EQ(metadata[0].record_id, source0_first.id);
  EXPECT_EQ(metadata[0].exchange_ns, source0_first.exchange_ns);
  EXPECT_EQ(metadata[0].event_ns, source0_first.exchange_ns);
  EXPECT_EQ(metadata[0].source_local_ns, source0_first.local_ns);
  EXPECT_EQ(metadata[0].fusion_publish_ns, first.local_ns);
  EXPECT_EQ(metadata[1].source_id, 1);
  EXPECT_EQ(metadata[1].record_id, source1_next.id);
  EXPECT_EQ(metadata[1].event_ns, source1_next.exchange_ns);
  EXPECT_EQ(metadata[1].source_local_ns, source1_next.local_ns);
  EXPECT_EQ(metadata[1].fusion_publish_ns, second.local_ns);

  std::filesystem::remove(metadata_path);
#else
  EXPECT_TRUE(metadata_path.empty());
#endif
}

}  // namespace
