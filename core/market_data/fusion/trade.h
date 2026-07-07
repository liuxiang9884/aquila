#ifndef AQUILA_CORE_MARKET_DATA_FUSION_TRADE_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_TRADE_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/common/fusion_metadata_mode.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/fastest_route.h"
#include "core/market_data/fusion/metadata.h"
#include "core/market_data/types.h"
#include "core/websocket/runtime_clock.h"

namespace aquila::market_data {

using TradeFusionDecision = FastestRouteFusionDecision;

struct TradeFusionTraits {
  using Record = Trade;

  [[nodiscard]] static std::int32_t SymbolId(const Trade& trade) noexcept {
    return trade.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(const Trade& trade) noexcept {
    return trade.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(const Trade& trade) noexcept {
    return trade.local_ns;
  }

  [[nodiscard]] static std::int64_t EventNs(const Trade& trade) noexcept {
    return trade.trade_ns;
  }
};

class TradeFusionCore {
 public:
  explicit TradeFusionCore(std::size_t max_symbol_id)
      : core_(max_symbol_id + 1) {}

  [[nodiscard]] TradeFusionDecision OnTrade(
      std::int32_t source_id, const Trade& trade,
      std::int64_t fusion_publish_ns) noexcept {
    const FastestRouteFusionDecision decision =
        core_.OnRecord(source_id, trade, fusion_publish_ns);
    if (!decision.publish) {
      return {};
    }
    return TradeFusionDecision{
        .publish = true,
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .record_id = decision.record_id,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
  }

 private:
  BasicFastestRouteFusionCore<TradeFusionTraits> core_;
};

struct TradeFusionMetadataTraits {
  using Config = TradeFusionConfig;
  using Record = Trade;

  [[nodiscard]] static std::int64_t EventNs(const Trade& trade) noexcept {
    return TradeFusionTraits::EventNs(trade);
  }
};

using FileTradeFusionMetadataPolicy =
    FileFusionMetadataPolicy<TradeFusionMetadataTraits>;
using NoopTradeFusionMetadataPolicy =
    NoopFusionMetadataPolicy<TradeFusionConfig>;
using DefaultTradeFusionMetadataPolicy =
    std::conditional_t<aquila::kFusionMetadataEnabled,
                       FileTradeFusionMetadataPolicy,
                       NoopTradeFusionMetadataPolicy>;

struct TradeFusionRunnerTraits {
  using Config = TradeFusionConfig;
  using Core = TradeFusionCore;
  using Decision = TradeFusionDecision;
  using OutputConfig = TradeFusionOutputConfig;
  using Publisher = DataShmPublisher;
  using Record = Trade;
  using Reader = TradeShmReader;
  using ShmConfig = TradeShmConfig;
  using SourceConfig = TradeFusionSourceConfig;

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

  [[nodiscard]] static std::int64_t NowNs() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
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

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_TRADE_H_
