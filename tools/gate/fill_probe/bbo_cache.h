#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_BBO_CACHE_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_BBO_CACHE_H_

#include <cmath>
#include <cstdint>
#include <optional>

#include "core/market_data/types.h"
#include "tools/gate/fill_probe/order_math.h"

namespace aquila::tools::gate::fill_probe {

class BboCache {
 public:
  BboCache(std::int32_t symbol_id, double price_tick)
      : symbol_id_(symbol_id), price_tick_(price_tick) {}

  void OnBookTicker(const aquila::BookTicker& ticker) noexcept {
    if (ticker.symbol_id != symbol_id_ || !Sane(ticker)) {
      return;
    }
    latest_ = BboSnapshot{
        .id = static_cast<std::uint64_t>(ticker.id),
        .symbol_id = ticker.symbol_id,
        .exchange_ns = ticker.exchange_ns,
        .local_ns = ticker.local_ns,
        .bid_price = ticker.bid_price,
        .bid_volume = ticker.bid_volume,
        .ask_price = ticker.ask_price,
        .ask_volume = ticker.ask_volume,
        .price_tick = price_tick_,
    };
  }

  [[nodiscard]] const std::optional<BboSnapshot>& latest() const noexcept {
    return latest_;
  }

 private:
  [[nodiscard]] static bool Sane(const aquila::BookTicker& ticker) noexcept {
    return std::isfinite(ticker.bid_price) && ticker.bid_price > 0.0 &&
           std::isfinite(ticker.ask_price) && ticker.ask_price > 0.0 &&
           ticker.bid_price <= ticker.ask_price;
  }

  std::int32_t symbol_id_{0};
  double price_tick_{0.0};
  std::optional<BboSnapshot> latest_;
};

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_BBO_CACHE_H_
