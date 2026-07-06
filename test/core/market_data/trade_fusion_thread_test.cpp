#include "core/market_data/trade_fusion_thread.h"

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/common/book_ticker_fusion_metadata_mode.h"
#include "core/common/types.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_metadata.h"

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
  return fmt::format("/aquila_trade_fusion_thread_test_{}_{}", ::getpid(),
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

md::TradeShmConfig MakeAttachConfig(
    const md::TradeShmConfig& create_config) {
  md::TradeShmConfig config = create_config;
  config.create = false;
  config.remove_existing = false;
  return config;
}

std::filesystem::path UniqueMetadataPath() {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("aquila_trade_fusion_thread_test_{}.bin", ::getpid());
}

aquila::Trade MakeTrade(std::int32_t symbol_id, std::int64_t id) {
  return aquila::Trade{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = aquila::Exchange::kGate,
      .side = aquila::OrderSide::kBuy,
      .reserved = 0,
      .exchange_ns = 1'780'000'000'000'000'000 + id,
      .trade_ns = 1'780'000'000'000'100'000 + id,
      .local_ns = 1'780'000'000'000'200'000 + id,
      .price = 100.0 + static_cast<double>(id),
      .volume = 1.0,
      .batch_index = 0,
      .batch_count = 1,
  };
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
[[gnu::noinline]] void PublishToShm(md::DataShmPublisher* publisher,
                                    const aquila::Trade& trade) noexcept {
  publisher->OnTrade(trade);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace

TEST(TradeFusionThreadTest, PublishesAndStops) {
  const md::TradeShmConfig source = MakeCreateConfig("source");
  const md::TradeShmConfig output = MakeCreateConfig("output");
  ShmCleanup source_cleanup(source.shm_name);
  ShmCleanup output_cleanup(output.shm_name);
#if AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED
  const std::filesystem::path metadata_path = UniqueMetadataPath();
  std::filesystem::remove(metadata_path);
#else
  const std::filesystem::path metadata_path;
#endif

  md::DataShmPublisher source_publisher(source);
  md::TradeFusionConfig config{
      .name = "trade_fusion_thread_test",
      .max_events_per_source = 4,
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
                  .name = "source",
                  .shm_name = source.shm_name,
                  .channel_name = source.channel_name,
              },
          },
  };

  md::TradeFusionThread fusion_thread(config);
  fusion_thread.Start();

  md::TradeShmReader output_reader(MakeAttachConfig(output));
  output_reader.SeekLatest();
  PublishToShm(&source_publisher, MakeTrade(42, 100));

  aquila::Trade canonical{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!output_reader.TryReadOne(&canonical) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  fusion_thread.Stop();
  const md::TradeFusionThreadStats stats = fusion_thread.Join();

  ASSERT_TRUE(stats.ok) << stats.error;
  EXPECT_TRUE(stats.flush_ok);
  EXPECT_GE(stats.total_read_count, 1U);
  EXPECT_GE(stats.total_published_count, 1U);
  EXPECT_EQ(stats.total_metadata_write_errors, 0U);
  EXPECT_EQ(canonical.id, 100);
  EXPECT_EQ(canonical.symbol_id, 42);
  EXPECT_EQ(canonical.trade_ns, 1'780'000'000'000'100'100);

#if AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED
  ASSERT_TRUE(std::filesystem::exists(metadata_path));
  EXPECT_EQ(std::filesystem::file_size(metadata_path),
            sizeof(md::TradeFusionMetadataRecord));
  std::filesystem::remove(metadata_path);
#else
  EXPECT_TRUE(metadata_path.empty());
#endif
}
