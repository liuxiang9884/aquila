#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_CONFIG_H_

#include "core/market_data/fusion_config.h"

namespace aquila::market_data {

using TradeFusionSourceConfig = FusionSourceConfig;
using TradeFusionOutputConfig = FusionOutputConfig;
using TradeFusionConfig =
    BasicFusionConfig<TradeFusionSourceConfig, TradeFusionOutputConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_CONFIG_H_
