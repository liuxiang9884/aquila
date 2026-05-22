#include "monitor/market_data/market_data_thread.h"

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/config/data_reader_config.h"
#include "core/market_data/types.h"
#include "monitor/market_data/market_data_store.h"
#include "monitor/model/monitor_spsc_queue.h"

namespace aquila::monitor {
namespace {

class FakeClock {
 public:
  [[nodiscard]] std::int64_t NowNs() const noexcept {
    return now_ns_;
  }

  void set_now_ns(std::int64_t now_ns) noexcept {
    now_ns_ = now_ns;
  }

 private:
  std::int64_t now_ns_{0};
};

class FakeReader {
 public:
  void Push(BookTicker ticker) {
    pending_.push_back(ticker);
  }

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    std::uint64_t drained = 0;
    while (drained < max_events && next_index_ < pending_.size()) {
      handler.OnBookTicker(pending_[next_index_]);
      ++next_index_;
      ++drained;
    }
    return drained;
  }

 private:
  std::vector<BookTicker> pending_;
  std::size_t next_index_{0};
};

[[nodiscard]] std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

[[nodiscard]] BookTicker MakeTicker(Exchange exchange, std::int32_t symbol_id,
                                    std::int64_t id, double bid_price = 100.0,
                                    double ask_price = 101.0) {
  return BookTicker{
      .id = id,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = 1'000 + id,
      .local_ns = 2'000 + id,
      .bid_price = bid_price,
      .bid_volume = 1.5,
      .ask_price = ask_price,
      .ask_volume = 2.5,
  };
}

template <std::size_t Capacity>
[[nodiscard]] bool PopBatch(MonitorSpscQueue<MarketDataBatch, Capacity>* queue,
                            MarketDataBatch* batch) {
  return queue->TryPop(batch);
}

[[nodiscard]] bool ContainsKey(const std::vector<MarketDataKey>& keys,
                               Exchange exchange, std::int32_t symbol_id) {
  for (const MarketDataKey& key : keys) {
    if (key.exchange == exchange && key.symbol_id == symbol_id) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string UniqueMissingShmName() {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream out;
  out << "aquila_missing_md_" << ::getpid() << '_' << suffix;
  return out.str();
}

void UnlinkShm(std::string_view shm_name) {
  std::string normalized{"/"};
  normalized.append(shm_name);
  ::shm_unlink(normalized.c_str());
}

TEST(MarketDataPumpTest, DrainsAndCoalescesUntilPublishInterval) {
  constexpr MarketDataKey kKey{.exchange = Exchange::kGate, .symbol_id = 7};
  FakeReader reader;
  FakeClock clock;
  MarketDataStore store(std::span<const MarketDataKey>{&kKey, 1});
  MonitorSpscQueue<MarketDataBatch, 4> queue;
  MarketDataPump<FakeReader, MonitorSpscQueue<MarketDataBatch, 4>, FakeClock>
      pump(reader, store, queue, clock,
           MarketDataPumpOptions{.drain_budget = 8});

  reader.Push(MakeTicker(Exchange::kGate, 7, 1, 10.0, 11.0));
  reader.Push(MakeTicker(Exchange::kGate, 7, 2, 12.0, 13.0));

  clock.set_now_ns(50'000'000);
  MarketDataPumpResult result = pump.StepOnce();

  EXPECT_EQ(result.drained_count, 2);
  EXPECT_FALSE(result.publish_due);
  EXPECT_TRUE(queue.empty());

  clock.set_now_ns(100'000'000);
  result = pump.StepOnce();

  EXPECT_EQ(result.drained_count, 0);
  EXPECT_TRUE(result.publish_due);
  EXPECT_TRUE(result.batch_pushed);

  MarketDataBatch batch{};
  ASSERT_TRUE(PopBatch(&queue, &batch));
  ASSERT_EQ(batch.row_count, 1);
  EXPECT_EQ(batch.published_ns, 100'000'000);
  EXPECT_EQ(batch.rows[0].id, 2);
  EXPECT_DOUBLE_EQ(batch.rows[0].bid_price, 12.0);
  EXPECT_EQ(batch.drained_count, 2);
}

TEST(MarketDataPumpTest, DuplicateIdDoesNotProduceSecondChangedRow) {
  constexpr MarketDataKey kKey{.exchange = Exchange::kGate, .symbol_id = 7};
  FakeReader reader;
  FakeClock clock;
  MarketDataStore store(std::span<const MarketDataKey>{&kKey, 1});
  MonitorSpscQueue<MarketDataBatch, 4> queue;
  MarketDataPump<FakeReader, MonitorSpscQueue<MarketDataBatch, 4>, FakeClock>
      pump(reader, store, queue, clock,
           MarketDataPumpOptions{.drain_budget = 8});

  reader.Push(MakeTicker(Exchange::kGate, 7, 1, 10.0, 11.0));
  clock.set_now_ns(100'000'000);
  ASSERT_TRUE(pump.StepOnce().batch_pushed);
  MarketDataBatch batch{};
  ASSERT_TRUE(PopBatch(&queue, &batch));
  ASSERT_EQ(batch.row_count, 1);

  reader.Push(MakeTicker(Exchange::kGate, 7, 1, 99.0, 100.0));
  clock.set_now_ns(200'000'000);
  const MarketDataPumpResult result = pump.StepOnce();

  EXPECT_TRUE(result.publish_due);
  EXPECT_FALSE(result.batch_pushed);
  EXPECT_EQ(result.row_count, 0);
  EXPECT_TRUE(queue.empty());
  EXPECT_EQ(store.stats().drained_count, 2);
  EXPECT_EQ(store.stats().changed_count, 1);
}

TEST(MarketDataPumpTest, FullQueueRecordsDroppedBatchWithoutBlocking) {
  constexpr MarketDataKey kKey{.exchange = Exchange::kGate, .symbol_id = 7};
  FakeReader reader;
  FakeClock clock;
  MarketDataStore store(std::span<const MarketDataKey>{&kKey, 1});
  MonitorSpscQueue<MarketDataBatch, 2> queue;
  MarketDataPump<FakeReader, MonitorSpscQueue<MarketDataBatch, 2>, FakeClock>
      pump(reader, store, queue, clock,
           MarketDataPumpOptions{.drain_budget = 8});

  MarketDataBatch filler{};
  ASSERT_TRUE(queue.TryPush(filler));
  ASSERT_TRUE(queue.TryPush(filler));

  reader.Push(MakeTicker(Exchange::kGate, 7, 1));
  clock.set_now_ns(100'000'000);
  const MarketDataPumpResult result = pump.StepOnce();

  EXPECT_TRUE(result.publish_due);
  EXPECT_FALSE(result.batch_pushed);
  EXPECT_TRUE(result.batch_dropped);
  EXPECT_EQ(queue.dropped_push_count(), 1);
  EXPECT_EQ(store.stats().dropped_batch_count, 1);
}

TEST(MarketDataPumpTest, FullQueueKeepsChangedRowForNextPublishAttempt) {
  constexpr MarketDataKey kKey{.exchange = Exchange::kGate, .symbol_id = 7};
  FakeReader reader;
  FakeClock clock;
  MarketDataStore store(std::span<const MarketDataKey>{&kKey, 1});
  MonitorSpscQueue<MarketDataBatch, 2> queue;
  MarketDataPump<FakeReader, MonitorSpscQueue<MarketDataBatch, 2>, FakeClock>
      pump(reader, store, queue, clock,
           MarketDataPumpOptions{.drain_budget = 8});

  MarketDataBatch filler{};
  ASSERT_TRUE(queue.TryPush(filler));
  ASSERT_TRUE(queue.TryPush(filler));

  reader.Push(MakeTicker(Exchange::kGate, 7, 42));
  clock.set_now_ns(100'000'000);
  MarketDataPumpResult result = pump.StepOnce();

  ASSERT_TRUE(result.publish_due);
  ASSERT_FALSE(result.batch_pushed);
  ASSERT_TRUE(result.batch_dropped);

  MarketDataBatch popped{};
  ASSERT_TRUE(PopBatch(&queue, &popped));
  ASSERT_TRUE(PopBatch(&queue, &popped));
  ASSERT_TRUE(queue.empty());

  clock.set_now_ns(200'000'000);
  result = pump.StepOnce();

  EXPECT_EQ(result.drained_count, 0);
  EXPECT_TRUE(result.publish_due);
  ASSERT_TRUE(result.batch_pushed);
  EXPECT_FALSE(result.batch_dropped);
  EXPECT_EQ(result.row_count, 1);

  MarketDataBatch batch{};
  ASSERT_TRUE(PopBatch(&queue, &batch));
  ASSERT_EQ(batch.row_count, 1);
  EXPECT_EQ(batch.rows[0].symbol_id, 7);
  EXPECT_EQ(batch.rows[0].id, 42);
}

TEST(MarketDataThreadTest, BuildMarketDataKeysIncludesSourceExchanges) {
  const config::DataReaderConfigResult config_result =
      config::LoadDataReaderConfigFile(
          SourcePath("config/monitors/gate_account_tui_market_data.toml"));
  ASSERT_TRUE(config_result.ok) << config_result.error;

  const std::vector<MarketDataKey> keys =
      BuildMarketDataKeys(config_result.value);

  EXPECT_TRUE(ContainsKey(keys, Exchange::kGate, 4));
  EXPECT_TRUE(ContainsKey(keys, Exchange::kGate, 6));
  EXPECT_TRUE(ContainsKey(keys, Exchange::kBinance, 4));
  EXPECT_TRUE(ContainsKey(keys, Exchange::kBinance, 6));
}

TEST(MarketDataThreadTest, StartReturnsFalseWhenRealtimeShmIsMissing) {
  const std::string missing_shm_name = UniqueMissingShmName();
  UnlinkShm(missing_shm_name);
  const std::filesystem::path config_path =
      std::filesystem::temp_directory_path() /
      ("aquila_missing_monitor_shm_" + std::to_string(::getpid()) + ".toml");
  {
    std::ofstream out(config_path);
    out << R"toml(
[instrument_catalog]
file = ")toml"
        << SourcePath("config/instruments/usdt_futures.csv").string()
        << R"toml("
schema = "aquila.instrument.v1"

[data_reader]
name = "missing_monitor_shm"
max_events_per_drain = 4

[[data_reader.sources]]
name = "missing_gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = ")toml"
        << missing_shm_name << R"toml("
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "drain"
required = false
)toml";
  }

  MarketDataThreadQueue queue;
  MarketDataThread thread(config_path, queue);

  EXPECT_FALSE(thread.Start());
  EXPECT_FALSE(thread.last_error().empty());

  std::error_code ignored;
  std::filesystem::remove(config_path, ignored);
  UnlinkShm(missing_shm_name);
}

}  // namespace
}  // namespace aquila::monitor
