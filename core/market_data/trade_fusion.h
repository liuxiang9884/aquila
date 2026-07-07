#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_H_

#include <cstddef>
#include <cstdint>

#include "core/market_data/fastest_route_fusion.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

using TradeFusionDecision = FastestRouteFusionDecision;

struct TradeFusionTraits {
  using Record = Trade;

  [[nodiscard]] static std::int32_t SymbolId(const Trade& trade) noexcept {
    return trade.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(const Trade& trade) noexcept {
    return trade.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(const Trade& trade) noexcept {
    return trade.local_ns;
  }

  [[nodiscard]] static std::int64_t EventNs(const Trade& trade) noexcept {
    return trade.trade_ns;
  }
};

class TradeFusionCore {
 public:
  explicit TradeFusionCore(std::size_t max_symbol_id)
      : core_(max_symbol_id + 1) {}

  [[nodiscard]] TradeFusionDecision OnTrade(
      std::int32_t source_id, const Trade& trade,
      std::int64_t fusion_publish_ns) noexcept {
    const FastestRouteFusionDecision decision =
        core_.OnRecord(source_id, trade, fusion_publish_ns);
    if (!decision.publish) {
      return {};
    }
    return TradeFusionDecision{
        .publish = true,
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .record_id = decision.record_id,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
  }

 private:
  BasicFastestRouteFusionCore<TradeFusionTraits> core_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_H_
