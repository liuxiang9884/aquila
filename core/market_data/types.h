#ifndef AQUILA_CORE_MARKET_DATA_TYPES_H_
#define AQUILA_CORE_MARKET_DATA_TYPES_H_

#include "core/common/types.h"

#include <cstdint>

namespace aquila {

struct BookTicker {
  std::int64_t id;
  std::int32_t symbol_id;
  Exchange exchange;
  std::int64_t exchange_ns;
  std::int64_t event_ns;
  std::int64_t local_ns;

  double bid_price;
  double bid_volume;
  double ask_price;
  double ask_volume;
};

struct Trade {
  std::int64_t id;
  std::int32_t symbol_id;
  Exchange exchange;
  OrderSide side;
  std::uint16_t reserved;

  std::int64_t exchange_ns;
  std::int64_t event_ns;
  std::int64_t local_ns;

  double price;
  double volume;

  std::uint32_t batch_index;
  std::uint32_t batch_count;
};

}  // namespace aquila

#endif  // AQUILA_CORE_MARKET_DATA_TYPES_H_
