#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_RUNNER_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_RUNNER_H_

#include <cstdint>

#include "core/market_data/data_shm.h"
#include "core/market_data/fastest_route_fusion_runner.h"
#include "core/market_data/trade_fusion.h"
#include "core/market_data/trade_fusion_config.h"
#include "core/market_data/trade_fusion_metadata_policy.h"

namespace aquila::market_data {

struct TradeFusionRunnerTraits {
  using Config = TradeFusionConfig;
  using Core = TradeFusionCore;
  using Decision = TradeFusionDecision;
  using Record = Trade;
  using Reader = TradeShmReader;
  using ShmConfig = TradeShmConfig;
  using SourceConfig = TradeFusionSourceConfig;
  using OutputConfig = TradeFusionOutputConfig;

  [[nodiscard]] static TradeShmConfig MakeSourceShmConfig(
      const TradeFusionSourceConfig& source) {
    return TradeShmConfig{.enabled = true,
                          .shm_name = source.shm_name,
                          .channel_name = source.channel_name,
                          .create = false,
                          .remove_existing = false};
  }

  [[nodiscard]] static TradeShmConfig MakeOutputShmConfig(
      const TradeFusionOutputConfig& output) {
    return TradeShmConfig{.enabled = true,
                          .shm_name = output.shm_name,
                          .channel_name = output.channel_name,
                          .create = true,
                          .remove_existing = output.remove_existing};
  }

  [[nodiscard]] static TradeFusionDecision OnRecord(
      TradeFusionCore& core, std::int32_t source_id, const Trade& trade,
      std::int64_t fusion_publish_ns) noexcept {
    return core.OnTrade(source_id, trade, fusion_publish_ns);
  }

  static void SetLocalNs(Trade* trade, std::int64_t local_ns) noexcept {
    trade->local_ns = local_ns;
  }

  static void Publish(DataShmPublisher& publisher,
                      const Trade& trade) noexcept {
    publisher.OnTrade(trade);
  }
};

template <typename MetadataPolicy>
using BasicTradeFusionRunner =
    BasicFastestRouteFusionRunner<TradeFusionRunnerTraits, MetadataPolicy>;

using TradeFusionPollStats = FastestRouteFusionPollStats;
using TradeFusionRunner =
    BasicTradeFusionRunner<DefaultTradeFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_RUNNER_H_
