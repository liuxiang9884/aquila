#ifndef AQUILA_STRATEGY_LEAD_LAG_TYPES_H_
#define AQUILA_STRATEGY_LEAD_LAG_TYPES_H_

#include <cstdint>

namespace aquila::strategy::leadlag {

enum class PairRole : std::uint8_t {
  kNone,
  kLead,
  kLag,
};

struct QuoteSnapshot {
  std::int64_t event_ns{0};
  double bid_price{0.0};
  double ask_price{0.0};
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_TYPES_H_
