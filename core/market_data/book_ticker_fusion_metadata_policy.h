#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_POLICY_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_POLICY_H_

#include <type_traits>

#include "core/common/fusion_metadata_mode.h"
#include "core/market_data/book_ticker_fusion.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/fusion_metadata_policy.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

struct BookTickerFusionMetadataTraits {
  using Config = BookTickerFusionConfig;
  using Record = BookTicker;

  [[nodiscard]] static std::int64_t EventNs(const BookTicker& ticker) noexcept {
    return BookTickerFusionTraits::EventNs(ticker);
  }
};

using FileBookTickerFusionMetadataPolicy =
    FileFusionMetadataPolicy<BookTickerFusionMetadataTraits>;
using NoopBookTickerFusionMetadataPolicy =
    NoopFusionMetadataPolicy<BookTickerFusionConfig>;
using DefaultBookTickerFusionMetadataPolicy =
    std::conditional_t<aquila::kFusionMetadataEnabled,
                       FileBookTickerFusionMetadataPolicy,
                       NoopBookTickerFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_POLICY_H_
