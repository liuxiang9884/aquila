#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_POLICY_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_POLICY_H_

#include <type_traits>

#include "core/common/book_ticker_fusion_metadata_mode.h"
#include "core/market_data/book_ticker_fusion.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_metadata.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

class FileBookTickerFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = true;

  explicit FileBookTickerFusionMetadataPolicy(
      const BookTickerFusionConfig& config)
      : writer_(config.output.metadata_bin) {}

  [[nodiscard]] bool Write(const BookTickerFusionDecision& decision,
                           const BookTicker& ticker) noexcept {
    const FusionMetadataRecord record{
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .book_ticker_id = decision.book_ticker_id,
        .exchange_ns = ticker.exchange_ns,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
    return writer_.Write(record);
  }

  [[nodiscard]] bool Flush() noexcept {
    return writer_.Flush();
  }

 private:
  FusionMetadataWriter writer_;
};

class NoopBookTickerFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopBookTickerFusionMetadataPolicy(
      const BookTickerFusionConfig& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept {
    return true;
  }
};

using DefaultBookTickerFusionMetadataPolicy = std::conditional_t<
    aquila::kBookTickerFusionMetadataEnabled, FileBookTickerFusionMetadataPolicy,
    NoopBookTickerFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_POLICY_H_
