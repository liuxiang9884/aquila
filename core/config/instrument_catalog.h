#ifndef AQUILA_CORE_CONFIG_INSTRUMENT_CATALOG_H_
#define AQUILA_CORE_CONFIG_INSTRUMENT_CATALOG_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "core/common/types.h"

namespace aquila::config {

struct InstrumentInfo {
  std::int32_t symbol_id{-1};
  Exchange exchange{Exchange::kGate};
  std::string symbol;
  std::string exchange_symbol;
  std::string base_asset;
  std::string quote_asset;
  std::string settle_asset;
  std::string product_type;
  std::string status;
  std::string contract_type;
  double price_tick{0.0};
  std::int32_t price_decimal_places{0};
  std::optional<double> quantity_step;
  std::optional<std::int32_t> quantity_decimal_places;
  double min_quantity{0.0};
  double max_quantity{0.0};
  std::optional<double> max_market_quantity;
  std::optional<double> min_notional;
  double notional_multiplier{0.0};
  std::optional<double> price_limit_up;
  std::optional<double> price_limit_down;
  std::optional<double> market_price_bound;
};

namespace detail {

struct InstrumentLookupKey {
  Exchange exchange{Exchange::kGate};
  std::string symbol;

  friend bool operator==(const InstrumentLookupKey& lhs,
                         const InstrumentLookupKey& rhs) {
    return lhs.exchange == rhs.exchange && lhs.symbol == rhs.symbol;
  }

  template <typename H>
  friend H AbslHashValue(H h, const InstrumentLookupKey& key) {
    return H::combine(std::move(h), static_cast<std::uint8_t>(key.exchange),
                      key.symbol);
  }
};

}  // namespace detail

class InstrumentCatalog {
 public:
  void Add(InstrumentInfo info);

  [[nodiscard]] const InstrumentInfo* Find(Exchange exchange,
                                           std::string_view symbol) const;

  [[nodiscard]] const std::vector<InstrumentInfo>& instruments()
      const noexcept {
    return instruments_;
  }

 private:
  std::vector<InstrumentInfo> instruments_;
  absl::flat_hash_map<detail::InstrumentLookupKey, std::size_t> lookup_;
};

struct InstrumentCatalogLoadResult {
  InstrumentCatalog catalog;
  std::string error;
  bool ok{false};
};

[[nodiscard]] InstrumentCatalogLoadResult LoadInstrumentCatalogFromCsv(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_INSTRUMENT_CATALOG_H_
