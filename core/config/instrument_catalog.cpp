#include "core/config/instrument_catalog.h"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/utils/numeric.h"
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

[[nodiscard]] std::string ReadString(csv::CSVRow& row, std::string_view name) {
  return std::string{row[std::string{name}].get<std::string_view>()};
}

[[nodiscard]] std::int32_t ReadInt32(csv::CSVRow& row,
                                     std::string_view name) {
  return ToInt32(row[std::string{name}].get<std::string_view>());
}

[[nodiscard]] double ReadDouble(csv::CSVRow& row, std::string_view name) {
  return ToDouble(row[std::string{name}].get<std::string_view>());
}

[[nodiscard]] std::optional<std::int32_t> ReadOptionalInt32(
    csv::CSVRow& row, std::string_view name) {
  const std::string_view text = row[std::string{name}].get<std::string_view>();
  if (text.empty()) {
    return std::nullopt;
  }
  return ToInt32(text);
}

[[nodiscard]] std::optional<double> ReadOptionalDouble(csv::CSVRow& row,
                                                       std::string_view name) {
  const std::string_view text = row[std::string{name}].get<std::string_view>();
  if (text.empty()) {
    return std::nullopt;
  }
  return ToDouble(text);
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
          .symbol_id = ReadInt32(row, "symbol_id"),
          .exchange = exchange,
          .symbol = ReadString(row, "symbol"),
          .exchange_symbol = ReadString(row, "exchange_symbol"),
          .base_asset = ReadString(row, "base_asset"),
          .quote_asset = ReadString(row, "quote_asset"),
          .settle_asset = ReadString(row, "settle_asset"),
          .product_type = ReadString(row, "product_type"),
          .status = ReadString(row, "status"),
          .contract_type = ReadString(row, "contract_type"),
          .price_tick = ReadDouble(row, "price_tick"),
          .price_decimal_places = ReadInt32(row, "price_decimal_places"),
          .quantity_step = ReadOptionalDouble(row, "quantity_step"),
          .quantity_decimal_places =
              ReadOptionalInt32(row, "quantity_decimal_places"),
          .min_quantity = ReadDouble(row, "min_quantity"),
          .max_quantity = ReadDouble(row, "max_quantity"),
          .max_market_quantity = ReadOptionalDouble(row, "max_market_quantity"),
          .min_notional = ReadOptionalDouble(row, "min_notional"),
          .notional_multiplier = ReadDouble(row, "notional_multiplier"),
          .price_limit_up = ReadOptionalDouble(row, "price_limit_up"),
          .price_limit_down = ReadOptionalDouble(row, "price_limit_down"),
          .market_price_bound = ReadOptionalDouble(row, "market_price_bound"),
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
