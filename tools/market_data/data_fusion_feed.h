#ifndef AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_FEED_H_
#define AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_FEED_H_

#include <cstdint>

namespace aquila::tools::market_data {

enum class DataFusionFeed : std::uint8_t {
  kBookTicker,
  kTrade,
};

[[nodiscard]] inline const char* DataFusionFeedName(
    DataFusionFeed feed) noexcept {
  switch (feed) {
    case DataFusionFeed::kBookTicker:
      return "book_ticker";
    case DataFusionFeed::kTrade:
      return "trade";
  }
  return "unknown";
}

}  // namespace aquila::tools::market_data

#endif  // AQUILA_TOOLS_MARKET_DATA_DATA_FUSION_FEED_H_
