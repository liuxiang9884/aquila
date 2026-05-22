#ifndef AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_STORE_H_
#define AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_STORE_H_

#include <cstdint>
#include <span>
#include <vector>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "monitor/market_data/market_data_update.h"

namespace aquila::monitor {

struct MarketDataKey {
  Exchange exchange;
  std::int32_t symbol_id;
};

struct MarketDataStoreStats {
  std::uint64_t drained_count{0};
  std::uint64_t changed_count{0};
  std::uint64_t unknown_symbol_count{0};
  std::uint64_t overrun_count{0};
  std::uint64_t dropped_batch_count{0};
};

class MarketDataStore {
 public:
  explicit MarketDataStore(std::span<const MarketDataKey> keys) {
    entries_.reserve(keys.size());
    for (const MarketDataKey& key : keys) {
      entries_.push_back(Entry{.key = key});
    }
  }

  void OnBookTicker(const BookTicker& ticker) {
    ++stats_.drained_count;

    Entry* entry = FindEntry(ticker.exchange, ticker.symbol_id);
    if (entry == nullptr) {
      ++stats_.unknown_symbol_count;
      return;
    }

    if (entry->has_latest && ticker.id == entry->latest.id) {
      return;
    }

    entry->latest = MarketDataRowUpdate{
        .exchange = ticker.exchange,
        .symbol_id = ticker.symbol_id,
        .id = ticker.id,
        .exchange_ns = ticker.exchange_ns,
        .local_ns = ticker.local_ns,
        .bid_price = ticker.bid_price,
        .bid_volume = ticker.bid_volume,
        .ask_price = ticker.ask_price,
        .ask_volume = ticker.ask_volume,
    };
    entry->has_latest = true;
    entry->changed = true;
    ++stats_.changed_count;
  }

  [[nodiscard]] MarketDataBatch BuildChangedBatch(std::int64_t published_ns) {
    MarketDataBatch batch{};
    batch.published_ns = published_ns;
    batch.drained_count = stats_.drained_count;
    batch.overrun_count = stats_.overrun_count;
    batch.dropped_batch_count = stats_.dropped_batch_count;

    for (Entry& entry : entries_) {
      if (!entry.changed) {
        continue;
      }
      if (batch.row_count < kMarketDataBatchCapacity) {
        batch.rows[batch.row_count] = entry.latest;
        ++batch.row_count;
      }
      entry.changed = false;
    }

    return batch;
  }

  void RecordOverrun(std::uint64_t delta) noexcept {
    stats_.overrun_count += delta;
  }

  void RecordDroppedBatch() noexcept {
    ++stats_.dropped_batch_count;
  }

  [[nodiscard]] const MarketDataStoreStats& stats() const noexcept {
    return stats_;
  }

 private:
  struct Entry {
    MarketDataKey key;
    MarketDataRowUpdate latest{};
    bool has_latest{false};
    bool changed{false};
  };

  [[nodiscard]] Entry* FindEntry(Exchange exchange,
                                 std::int32_t symbol_id) noexcept {
    for (Entry& entry : entries_) {
      if (entry.key.exchange == exchange && entry.key.symbol_id == symbol_id) {
        return &entry;
      }
    }
    return nullptr;
  }

  std::vector<Entry> entries_;
  MarketDataStoreStats stats_;
};

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_STORE_H_
