#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "core/market_data/types.h"

namespace aquila::market_data {

struct BookTickerFusionDecision {
  bool publish{false};
  std::int32_t source_id{-1};
  std::int32_t symbol_id{-1};
  std::int64_t book_ticker_id{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
  BookTicker ticker{};
};

class BookTickerFusionCore {
 public:
  explicit BookTickerFusionCore(std::size_t max_symbol_id)
      : states_(max_symbol_id + 1) {}

  [[nodiscard]] BookTickerFusionDecision OnBookTicker(
      std::int32_t source_id, const BookTicker& ticker,
      std::int64_t fusion_publish_ns) noexcept {
    BookTickerFusionDecision decision{
        .source_id = source_id,
        .symbol_id = ticker.symbol_id,
        .book_ticker_id = ticker.id,
        .source_local_ns = ticker.local_ns,
        .fusion_publish_ns = fusion_publish_ns,
    };

    if (ticker.symbol_id < 0 ||
        static_cast<std::size_t>(ticker.symbol_id) >= states_.size()) {
      return decision;
    }

    SymbolFusionState& state =
        states_[static_cast<std::size_t>(ticker.symbol_id)];
    if (ticker.id <= state.last_published_id) {
      return decision;
    }

    state.last_published_id = ticker.id;
    state.last_published_source = source_id;

    decision.publish = true;
    decision.ticker = ticker;
    decision.ticker.local_ns = fusion_publish_ns;
    return decision;
  }

 private:
  struct SymbolFusionState {
    std::int64_t last_published_id{std::numeric_limits<std::int64_t>::min()};
    std::int32_t last_published_source{-1};
  };

  std::vector<SymbolFusionState> states_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_H_
