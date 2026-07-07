#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_

#include <cstdint>
#include <type_traits>

#include "core/market_data/fusion_metadata_writer.h"

namespace aquila::market_data {

struct FusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t record_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t event_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

static_assert(sizeof(FusionMetadataRecord) == 48);
static_assert(std::is_standard_layout_v<FusionMetadataRecord>);
static_assert(std::is_trivially_copyable_v<FusionMetadataRecord>);

using FusionMetadataWriter = BasicFusionMetadataWriter<FusionMetadataRecord>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
