#ifndef AQUILA_CORE_MARKET_DATA_TYPES_H_
#define AQUILA_CORE_MARKET_DATA_TYPES_H_

#include "core/common/types.h"

#include <cstdint>

namespace aquila {

struct BookTicker {
  Exchange exchange;
  std::int32_t symbol_id;
  std::int64_t exchange_time_ns;
  std::int64_t local_time_ns;
  std::int64_t elapsed_ns;
  std::int64_t sequence;
  double bid_price;
  double bid_volume;
  double ask_price;
  double ask_volume;
};

}  // namespace aquila

#endif  // AQUILA_CORE_MARKET_DATA_TYPES_H_
