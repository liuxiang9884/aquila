#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_THREAD_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_THREAD_H_

#include "core/market_data/fastest_route_fusion_thread.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_runner.h"

namespace aquila::market_data {

using TradeFusionThreadStats = FastestRouteFusionThreadStats;
using TradeFusionThread =
    BasicFastestRouteFusionThread<TradeFusionRunner, TradeFusionConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_THREAD_H_
