#ifndef AQUILA_CORE_CONFIG_INSTRUMENT_CATALOG_H_
#define AQUILA_CORE_CONFIG_INSTRUMENT_CATALOG_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
