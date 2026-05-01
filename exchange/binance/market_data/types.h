#ifndef AQUILA_EXCHANGE_BINANCE_MARKET_DATA_TYPES_H_
#define AQUILA_EXCHANGE_BINANCE_MARKET_DATA_TYPES_H_

#include <cstdint>
#include <string_view>

namespace aquila::binance {

struct SymbolBinding {
  // The symbol text storage must outlive clients and sessions built from this
  // binding; market data lookup keeps string_view keys on purpose.
  std::string_view symbol{};
  std::int32_t symbol_id{-1};
};

}  // namespace aquila::binance

#endif  // AQUILA_EXCHANGE_BINANCE_MARKET_DATA_TYPES_H_
