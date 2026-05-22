#ifndef AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_UPDATE_H_
#define AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_UPDATE_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "core/common/types.h"

namespace aquila::monitor {

inline constexpr std::size_t kMarketDataBatchCapacity = 32;

struct MarketDataRowUpdate {
  Exchange exchange;
  std::int32_t symbol_id;
  std::int64_t id;
  std::int64_t exchange_ns;
  std::int64_t local_ns;
  double bid_price;
  double bid_volume;
  double ask_price;
  double ask_volume;
};

struct MarketDataBatch {
  std::int64_t published_ns;
  std::uint16_t row_count;
  std::array<MarketDataRowUpdate, kMarketDataBatchCapacity> rows;
  std::uint64_t drained_count;
  std::uint64_t overrun_count;
  std::uint64_t dropped_batch_count;
};

}  // namespace aquila::monitor

#endif  // AQUILA_MONITOR_MARKET_DATA_MARKET_DATA_UPDATE_H_
