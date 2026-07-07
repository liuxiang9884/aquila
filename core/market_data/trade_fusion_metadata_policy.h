#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_

#include <type_traits>

#include "core/common/book_ticker_fusion_metadata_mode.h"
#include "core/market_data/fusion_metadata_policy.h"
#include "core/market_data/trade_fusion.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

struct TradeFusionMetadataTraits {
  using Config = TradeFusionConfig;
  using Record = Trade;

  [[nodiscard]] static std::int64_t EventNs(const Trade& trade) noexcept {
    return TradeFusionTraits::EventNs(trade);
  }
};

using FileTradeFusionMetadataPolicy =
    FileFusionMetadataPolicy<TradeFusionMetadataTraits>;
using NoopTradeFusionMetadataPolicy =
    NoopFusionMetadataPolicy<TradeFusionConfig>;
using DefaultTradeFusionMetadataPolicy =
    std::conditional_t<aquila::kBookTickerFusionMetadataEnabled,
                       FileTradeFusionMetadataPolicy,
                       NoopTradeFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_
