#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_POLICY_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_POLICY_H_

#include "core/market_data/fastest_route_fusion.h"
#include "core/market_data/fusion_metadata.h"

namespace aquila::market_data {

template <typename Traits>
class FileFusionMetadataPolicy {
 public:
  using Config = typename Traits::Config;
  using Record = typename Traits::Record;
  static constexpr bool kEnabled = true;

  explicit FileFusionMetadataPolicy(const Config& config)
      : writer_(config.output.metadata_bin) {}

  [[nodiscard]] bool Write(const FastestRouteFusionDecision& decision,
                           const Record& record) noexcept {
    const FusionMetadataRecord metadata{
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .record_id = decision.record_id,
        .exchange_ns = record.exchange_ns,
        .event_ns = Traits::EventNs(record),
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
    return writer_.Write(metadata);
  }

  [[nodiscard]] bool Flush() noexcept {
    return writer_.Flush();
  }

 private:
  FusionMetadataWriter writer_;
};

template <typename Config>
class NoopFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopFusionMetadataPolicy(const Config& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept {
    return true;
  }
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_POLICY_H_
