#ifndef AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_H_
#define AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace aquila::market_data {

struct FastestRouteFusionDecision {
  bool publish{false};
  std::int32_t source_id{-1};
  std::int32_t symbol_id{-1};
  std::int64_t record_id{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

template <typename Traits>
class BasicFastestRouteFusionCore {
 public:
  using Record = typename Traits::Record;

  explicit BasicFastestRouteFusionCore(std::size_t symbol_capacity)
      : states_(symbol_capacity) {}

  [[nodiscard]] FastestRouteFusionDecision OnRecord(
      std::int32_t source_id, const Record& record,
      std::int64_t fusion_publish_ns) noexcept {
    const std::int32_t symbol_id = Traits::SymbolId(record);
    if (symbol_id < 0 ||
        static_cast<std::size_t>(symbol_id) >= states_.size()) {
      return {};
    }

    SymbolState& state = states_[static_cast<std::size_t>(symbol_id)];
    const std::int64_t record_id = Traits::RecordId(record);
    if (record_id <= state.last_published_id) {
      return {};
    }

    state.last_published_id = record_id;
    state.last_published_source = source_id;
    return FastestRouteFusionDecision{
        .publish = true,
        .source_id = source_id,
        .symbol_id = symbol_id,
        .record_id = record_id,
        .source_local_ns = Traits::LocalNs(record),
        .fusion_publish_ns = fusion_publish_ns,
    };
  }

 private:
  struct SymbolState {
    std::int64_t last_published_id{std::numeric_limits<std::int64_t>::min()};
    std::int32_t last_published_source{-1};
  };

  std::vector<SymbolState> states_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_H_
