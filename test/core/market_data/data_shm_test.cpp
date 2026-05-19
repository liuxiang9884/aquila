#include "core/market_data/data_shm.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <gtest/gtest.h>

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
  return fmt::format("/aquila_data_shm_test_{}_{}", ::getpid(), suffix);
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

aquila::BookTicker MakeBookTicker(std::int64_t id) {
  return aquila::BookTicker{
      .id = id,
      .symbol_id = 42,
      .exchange = aquila::Exchange::kGate,
      .exchange_ns = 1'770'000'000'000'000'000 + id,
      .local_ns = 1'770'000'000'000'100'000 + id,
      .bid_price = 65'000.0 + static_cast<double>(id),
      .bid_volume = 10.0 + static_cast<double>(id),
      .ask_price = 65'001.0 + static_cast<double>(id),
      .ask_volume = 11.0 + static_cast<double>(id),
  };
}

void ExpectBookTickerEq(const aquila::BookTicker& actual,
                        const aquila::BookTicker& expected) {
  EXPECT_EQ(actual.id, expected.id);
  EXPECT_EQ(actual.symbol_id, expected.symbol_id);
  EXPECT_EQ(actual.exchange, expected.exchange);
  EXPECT_EQ(actual.exchange_ns, expected.exchange_ns);
  EXPECT_EQ(actual.local_ns, expected.local_ns);
  EXPECT_DOUBLE_EQ(actual.bid_price, expected.bid_price);
  EXPECT_DOUBLE_EQ(actual.bid_volume, expected.bid_volume);
  EXPECT_DOUBLE_EQ(actual.ask_price, expected.ask_price);
  EXPECT_DOUBLE_EQ(actual.ask_volume, expected.ask_volume);
}

TEST(DataShmTest, PublisherWritesAndReaderReadsBookTicker) {
  const md::BookTickerShmConfig config = MakeCreateConfig("publish_read");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  const aquila::BookTicker expected = MakeBookTicker(7);
  publisher.OnBookTicker(expected);

  aquila::BookTicker actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  ExpectBookTickerEq(actual, expected);
  EXPECT_FALSE(reader.TryReadOne(&actual));
  EXPECT_EQ(publisher.published_count(), 1U);
}

TEST(DataShmTest, PublisherEmplaceWithWritesAndReaderReadsBookTicker) {
  const md::BookTickerShmConfig config = MakeCreateConfig("emplace_read");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  const aquila::BookTicker expected = MakeBookTicker(8);
  publisher.EmplaceBookTickerWith(
      [&expected](aquila::BookTicker& out) noexcept { out = expected; });

  aquila::BookTicker actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  ExpectBookTickerEq(actual, expected);
  EXPECT_FALSE(reader.TryReadOne(&actual));
  EXPECT_EQ(publisher.published_count(), 1U);
}

TEST(DataShmTest, ReaderStartsAtLatestWhenRequested) {
  const md::BookTickerShmConfig config = MakeCreateConfig("seek_latest");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  publisher.OnBookTicker(MakeBookTicker(1));

  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();
  publisher.OnBookTicker(MakeBookTicker(2));

  aquila::BookTicker actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  EXPECT_EQ(actual.id, 2);
  EXPECT_FALSE(reader.TryReadOne(&actual));
}

TEST(DataShmTest, ReaderCanSeekEarliestVisible) {
  const md::BookTickerShmConfig config = MakeCreateConfig("seek_earliest");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  publisher.OnBookTicker(MakeBookTicker(1));
  publisher.OnBookTicker(MakeBookTicker(2));

  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekEarliestVisible();

  aquila::BookTicker actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  EXPECT_EQ(actual.id, 1);
  ASSERT_TRUE(reader.TryReadOne(&actual));
  EXPECT_EQ(actual.id, 2);
  EXPECT_FALSE(reader.TryReadOne(&actual));
}

TEST(DataShmTest, ReaderSeekEarliestVisibleUsesConservativeWindow) {
  const md::BookTickerShmConfig config = MakeCreateConfig("seek_earliest_full");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  for (std::uint64_t id = 0; id < md::kBookTickerShmCapacity; ++id) {
    publisher.OnBookTicker(MakeBookTicker(static_cast<std::int64_t>(id)));
  }

  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekEarliestVisible();

  aquila::BookTicker actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  EXPECT_EQ(actual.id, 1);
  EXPECT_EQ(reader.overrun_count(), 0U);
}

TEST(DataShmTest, ReaderCountsOverrunAtExactCapacityBoundary) {
  const md::BookTickerShmConfig config =
      MakeCreateConfig("overrun_exact_capacity");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  for (std::uint64_t id = 0; id < md::kBookTickerShmCapacity; ++id) {
    publisher.OnBookTicker(MakeBookTicker(static_cast<std::int64_t>(id)));
  }

  aquila::BookTicker actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  EXPECT_EQ(actual.id, 1);
  EXPECT_EQ(reader.overrun_count(), 1U);
}

TEST(DataShmTest, ReaderCountsOverrunWhenUnreadCountExceedsCapacity) {
  const md::BookTickerShmConfig config = MakeCreateConfig("overrun");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  for (std::uint64_t id = 0; id < md::kBookTickerShmCapacity + 2; ++id) {
    publisher.OnBookTicker(MakeBookTicker(static_cast<std::int64_t>(id)));
  }

  aquila::BookTicker actual{};
  ASSERT_TRUE(reader.TryReadOne(&actual));
  EXPECT_EQ(actual.id, 3);
  EXPECT_EQ(reader.overrun_count(), 1U);
}

TEST(DataShmTest, ReaderTryReadLatestReturnsLastVisibleBookTicker) {
  const md::BookTickerShmConfig config = MakeCreateConfig("read_latest");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  publisher.OnBookTicker(MakeBookTicker(10));
  publisher.OnBookTicker(MakeBookTicker(11));
  publisher.OnBookTicker(MakeBookTicker(12));

  aquila::BookTicker actual{};
  std::uint64_t skipped{0};
  ASSERT_TRUE(reader.TryReadLatest(&actual, &skipped));
  EXPECT_EQ(actual.id, 12);
  EXPECT_EQ(skipped, 2U);
  EXPECT_FALSE(reader.TryReadLatest(&actual, &skipped));
}

TEST(DataShmTest, ReaderTryReadLatestCountsOverrunAndSkipsToLast) {
  const md::BookTickerShmConfig config =
      MakeCreateConfig("read_latest_overrun");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  for (std::uint64_t i = 0; i < md::kBookTickerShmCapacity + 3; ++i) {
    publisher.OnBookTicker(MakeBookTicker(static_cast<std::int64_t>(i)));
  }

  aquila::BookTicker actual{};
  std::uint64_t skipped{0};
  ASSERT_TRUE(reader.TryReadLatest(&actual, &skipped));
  EXPECT_EQ(actual.id,
            static_cast<std::int64_t>(md::kBookTickerShmCapacity + 2));
  EXPECT_EQ(reader.overrun_count(), 1U);
  EXPECT_EQ(skipped, md::kBookTickerShmCapacity - 2);
}

TEST(DataShmTest, RejectsHeaderCapacityMismatchOnAttach) {
  const md::BookTickerShmConfig config = MakeCreateConfig("capacity_mismatch");
  ShmCleanup cleanup(config.shm_name);

  md::BookTickerShmManager manager(config);
  manager.channel().header.capacity = 32768;

  EXPECT_THROW(
      md::BookTickerShmManager attach_manager(MakeAttachConfig(config)),
      std::runtime_error);
}

TEST(DataShmTest, PublisherUpdatesHeartbeatOutsideHotPath) {
  const md::BookTickerShmConfig config = MakeCreateConfig("heartbeat");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmManager manager(MakeAttachConfig(config));

  publisher.OnBookTicker(MakeBookTicker(1));
  EXPECT_EQ(
      manager.channel().header.heartbeat_ns.load(std::memory_order_relaxed),
      0U);

  publisher.UpdateHeartbeatNs(123456789);
  EXPECT_EQ(
      manager.channel().header.heartbeat_ns.load(std::memory_order_relaxed),
      123456789U);
}

TEST(DataShmTest, PublisherFlushesSharedPublishedCountOnColdPath) {
  const md::BookTickerShmConfig config = MakeCreateConfig("published_flush");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmManager manager(MakeAttachConfig(config));

  publisher.OnBookTicker(MakeBookTicker(1));
  EXPECT_EQ(publisher.published_count(), 1U);
  EXPECT_EQ(
      manager.channel().header.published_count.load(std::memory_order_relaxed),
      0U);

  publisher.FlushPublishedCount();
  EXPECT_EQ(
      manager.channel().header.published_count.load(std::memory_order_relaxed),
      1U);

  publisher.OnBookTicker(MakeBookTicker(2));
  publisher.UpdateHeartbeatNs(123456789);
  EXPECT_EQ(
      manager.channel().header.published_count.load(std::memory_order_relaxed),
      2U);
}

}  // namespace
