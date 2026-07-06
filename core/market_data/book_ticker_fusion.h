#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_H_

#include <cstddef>
#include <cstdint>

#include "core/market_data/fastest_route_fusion.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

struct BookTickerFusionDecision {
  bool publish{false};
  // Metadata fields are valid only when publish is true.
  std::int32_t source_id{-1};
  std::int32_t symbol_id{-1};
  std::int64_t book_ticker_id{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

struct BookTickerFusionTraits {
  using Record = BookTicker;

  [[nodiscard]] static std::int32_t SymbolId(
      const BookTicker& ticker) noexcept {
    return ticker.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(
      const BookTicker& ticker) noexcept {
    return ticker.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(
      const BookTicker& ticker) noexcept {
    return ticker.local_ns;
  }
};

class BookTickerFusionCore {
 public:
  explicit BookTickerFusionCore(std::size_t max_symbol_id)
      : core_(max_symbol_id + 1) {}

  [[nodiscard]] BookTickerFusionDecision OnBookTicker(
      std::int32_t source_id, const BookTicker& ticker,
      std::int64_t fusion_publish_ns) noexcept {
    const FastestRouteFusionDecision decision =
        core_.OnRecord(source_id, ticker, fusion_publish_ns);
    if (!decision.publish) {
      return {};
    }
    return BookTickerFusionDecision{
        .publish = true,
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .book_ticker_id = decision.record_id,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
  }

 private:
  BasicFastestRouteFusionCore<BookTickerFusionTraits> core_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_H_
