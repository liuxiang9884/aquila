#include "core/config/instrument_catalog.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include "nova/utils/log.h"
#include <csv.hpp>

namespace aquila::config {
namespace {

void MaybeLogInfo(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_INFO("{}", message);
  }
}

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] InstrumentCatalogLoadResult Failure(std::string error) {
  MaybeLogError(error);
  InstrumentCatalogLoadResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] InstrumentCatalogLoadResult Success(InstrumentCatalog catalog) {
  InstrumentCatalogLoadResult result;
  result.catalog = std::move(catalog);
  result.ok = true;
  return result;
}

[[nodiscard]] bool ParseExchange(std::string_view text, Exchange* exchange) {
  if (text == "gate") {
    *exchange = Exchange::kGate;
    return true;
  }
  if (text == "binance") {
    *exchange = Exchange::kBinance;
    return true;
  }
  return false;
}

}  // namespace

void InstrumentCatalog::Add(InstrumentInfo info) {
  const std::size_t index = instruments_.size();
  detail::InstrumentLookupKey key{.exchange = info.exchange,
                                  .symbol = info.symbol};
  instruments_.push_back(std::move(info));
  lookup_[std::move(key)] = index;
}

const InstrumentInfo* InstrumentCatalog::Find(Exchange exchange,
                                              std::string_view symbol) const {
  const auto found = lookup_.find(detail::InstrumentLookupKey{
      .exchange = exchange, .symbol = std::string{symbol}});
  if (found == lookup_.end()) {
    return nullptr;
  }
  return &instruments_[found->second];
}

InstrumentCatalogLoadResult LoadInstrumentCatalogFromCsv(
    const std::filesystem::path& path) {
  try {
    csv::CSVReader reader(path.string());
    InstrumentCatalog catalog;
    for (csv::CSVRow& row : reader) {
      Exchange exchange{};
      const std::string_view exchange_text =
          row["exchange"].get<std::string_view>();
      if (!ParseExchange(exchange_text, &exchange)) {
        return Failure("unsupported exchange in instrument catalog");
      }

      catalog.Add(InstrumentInfo{
          .symbol_id = row["symbol_id"].get<std::int32_t>(),
          .exchange = exchange,
          .symbol = std::string{row["symbol"].get<std::string_view>()},
          .exchange_symbol =
              std::string{row["exchange_symbol"].get<std::string_view>()},
      });
    }

    MaybeLogInfo("loaded instrument catalog");
    return Success(std::move(catalog));
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load instrument catalog: "} +
                   exc.what());
  }
}

}  // namespace aquila::config
