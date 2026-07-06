#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_THREAD_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_THREAD_H_

#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_runner.h"
#include "core/market_data/fastest_route_fusion_thread.h"

namespace aquila::market_data {

using BookTickerFusionThreadStats = FastestRouteFusionThreadStats;
using BookTickerFusionThread =
    BasicFastestRouteFusionThread<BookTickerFusionRunner,
                                  BookTickerFusionConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_THREAD_H_
