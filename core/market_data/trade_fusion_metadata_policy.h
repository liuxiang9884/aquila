#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_

#include <type_traits>

#include "core/common/book_ticker_fusion_metadata_mode.h"
#include "core/market_data/trade_fusion.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_metadata.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

class FileTradeFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = true;

  explicit FileTradeFusionMetadataPolicy(const TradeFusionConfig& config)
      : writer_(config.output.metadata_bin) {}

  [[nodiscard]] bool Write(const TradeFusionDecision& decision,
                           const Trade& trade) noexcept {
    const TradeFusionMetadataRecord record{
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .trade_id = decision.trade_id,
        .exchange_ns = trade.exchange_ns,
        .trade_ns = trade.trade_ns,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
    return writer_.Write(record);
  }

  [[nodiscard]] bool Flush() noexcept {
    return writer_.Flush();
  }

 private:
  TradeFusionMetadataWriter writer_;
};

class NoopTradeFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopTradeFusionMetadataPolicy(
      const TradeFusionConfig& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept {
    return true;
  }
};

using DefaultTradeFusionMetadataPolicy = std::conditional_t<
    aquila::kBookTickerFusionMetadataEnabled, FileTradeFusionMetadataPolicy,
    NoopTradeFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_POLICY_H_
