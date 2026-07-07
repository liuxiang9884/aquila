#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_CONFIG_H_

#include "core/market_data/fusion_config.h"

namespace aquila::market_data {

using BookTickerFusionSourceConfig = FusionSourceConfig;
using BookTickerFusionOutputConfig = FusionOutputConfig;
using BookTickerFusionConfig = BasicFusionConfig<BookTickerFusionSourceConfig,
                                                 BookTickerFusionOutputConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_CONFIG_H_
