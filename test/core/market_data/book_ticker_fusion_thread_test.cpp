#include "core/market_data/book_ticker_fusion_thread.h"

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

#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_metadata.h"
#include "core/market_data/data_shm.h"

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
  return fmt::format("/aquila_book_ticker_fusion_thread_test_{}_{}", ::getpid(),
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
         fmt::format("aquila_book_ticker_fusion_thread_test_{}.bin",
                     ::getpid());
}

aquila::BookTicker MakeTicker(std::int32_t symbol_id, std::int64_t id) {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = aquila::Exchange::kGate,
      .exchange_ns = 1'780'000'000'000'000'000 + id,
      .local_ns = 1'780'000'000'000'100'000 + id,
      .bid_price = 100.0 + static_cast<double>(id),
      .bid_volume = 1.0,
      .ask_price = 101.0 + static_cast<double>(id),
      .ask_volume = 2.0,
  };
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
[[gnu::noinline]] void PublishToShm(
    md::DataShmPublisher* publisher,
    const aquila::BookTicker& book_ticker) noexcept {
  publisher->OnBookTicker(book_ticker);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

}  // namespace

TEST(BookTickerFusionThreadTest, PublishesAndStops) {
  const md::BookTickerShmConfig source = MakeCreateConfig("source");
  const md::BookTickerShmConfig output = MakeCreateConfig("output");
  ShmCleanup source_cleanup(source.shm_name);
  ShmCleanup output_cleanup(output.shm_name);
  const std::filesystem::path metadata_path = UniqueMetadataPath();
  std::filesystem::remove(metadata_path);

  md::DataShmPublisher source_publisher(source);
  md::BookTickerFusionConfig config{
      .name = "fusion_thread_test",
      .max_events_per_source = 4,
      .bind_cpu_id = -1,
      .max_symbol_id = 128,
      .output =
          md::BookTickerFusionOutputConfig{
              .shm_name = output.shm_name,
              .channel_name = output.channel_name,
              .remove_existing = true,
              .metadata_bin = metadata_path,
          },
      .sources =
          {
              md::BookTickerFusionSourceConfig{
                  .source_id = 0,
                  .name = "source",
                  .shm_name = source.shm_name,
                  .channel_name = source.channel_name,
              },
          },
  };

  md::BookTickerFusionThread fusion_thread(config);
  fusion_thread.Start();

  md::BookTickerShmReader output_reader(MakeAttachConfig(output));
  output_reader.SeekLatest();
  PublishToShm(&source_publisher, MakeTicker(42, 100));

  aquila::BookTicker canonical{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!output_reader.TryReadOne(&canonical) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  fusion_thread.Stop();
  const md::BookTickerFusionThreadStats stats = fusion_thread.Join();

  ASSERT_TRUE(stats.ok) << stats.error;
  EXPECT_TRUE(stats.flush_ok);
  EXPECT_GE(stats.total_read_count, 1U);
  EXPECT_GE(stats.total_published_count, 1U);
  EXPECT_EQ(stats.total_metadata_write_errors, 0U);
  EXPECT_EQ(canonical.id, 100);
  EXPECT_EQ(canonical.symbol_id, 42);
  EXPECT_EQ(canonical.exchange_ns, 1'780'000'000'000'000'100);

  ASSERT_TRUE(std::filesystem::exists(metadata_path));
  EXPECT_EQ(std::filesystem::file_size(metadata_path),
            sizeof(md::FusionMetadataRecord));
  std::filesystem::remove(metadata_path);
}
