#ifndef AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_H_
#define AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_H_

#include <cstdint>
#include <type_traits>

#include "core/market_data/fusion_metadata_writer.h"

namespace aquila::market_data {

struct TradeFusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t trade_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t trade_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

static_assert(std::is_standard_layout_v<TradeFusionMetadataRecord>);
static_assert(std::is_trivially_copyable_v<TradeFusionMetadataRecord>);

using TradeFusionMetadataWriter =
    BasicFusionMetadataWriter<TradeFusionMetadataRecord>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_TRADE_FUSION_METADATA_H_
