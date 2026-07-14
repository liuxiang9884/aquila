#ifndef AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_ORDER_MATH_H_
#define AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_ORDER_MATH_H_

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "core/common/result.h"
#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "tools/bitget/gateway_smoke/types.h"

namespace aquila::tools::bitget::gateway_smoke {

using WireOrderResult = Result<WireOrder>;

namespace order_math_detail {

[[nodiscard]] inline WireOrderResult Failure(std::string error) {
  WireOrderResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] inline std::string FormatDecimal(double value,
                                               std::int32_t places) {
  return places <= 0 ? fmt::format("{:.0f}", value)
                     : fmt::format("{:.{}f}", value, places);
}

[[nodiscard]] inline bool Validate(const BookTicker& ticker,
                                   const config::InstrumentInfo& instrument,
                                   double quantity, std::string* error) {
  if (ticker.exchange != Exchange::kBitget ||
      ticker.symbol_id != instrument.symbol_id ||
      !std::isfinite(ticker.bid_price) || ticker.bid_price <= 0.0 ||
      !std::isfinite(ticker.ask_price) || ticker.ask_price <= 0.0 ||
      ticker.bid_price > ticker.ask_price) {
    *error = "Bitget book ticker must be positive and ordered";
    return false;
  }
  if (instrument.exchange != Exchange::kBitget ||
      instrument.price_tick <= 0.0 || !instrument.quantity_step.has_value() ||
      !instrument.quantity_decimal_places.has_value() ||
      *instrument.quantity_step <= 0.0 || quantity <= 0.0) {
    *error = "Bitget instrument trading metadata must be positive";
    return false;
  }
  const double quantity_units = quantity / *instrument.quantity_step;
  if (std::fabs(quantity_units - std::round(quantity_units)) > 1e-9) {
    *error = "order quantity must align to instrument quantity_step";
    return false;
  }
  return true;
}

[[nodiscard]] inline WireOrderResult Build(
    const config::InstrumentInfo& instrument, OrderSide side, double price,
    double quantity, bool reduce_only) {
  if (!std::isfinite(price) || price <= 0.0) {
    return Failure("computed order price must be positive");
  }
  WireOrderResult result;
  result.value = WireOrder{
      .side = side,
      .order_type = OrderType::kLimit,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .price = price,
      .quantity = quantity,
      .price_text = FormatDecimal(price, instrument.price_decimal_places),
      .quantity_text =
          FormatDecimal(quantity, *instrument.quantity_decimal_places),
      .reduce_only = reduce_only,
  };
  result.ok = true;
  return result;
}

}  // namespace order_math_detail

[[nodiscard]] inline WireOrderResult BuildEntryOrder(
    const BookTicker& ticker, const config::InstrumentInfo& instrument,
    OrderSide side, double quantity, double passive_price_limit_fraction) {
  std::string error;
  if (!order_math_detail::Validate(ticker, instrument, quantity, &error)) {
    return order_math_detail::Failure(std::move(error));
  }
  if (!std::isfinite(passive_price_limit_fraction) ||
      passive_price_limit_fraction <= 0.0 ||
      passive_price_limit_fraction > 1.0) {
    return order_math_detail::Failure(
        "passive price limit fraction must be in (0, 1]");
  }

  double price = 0.0;
  if (side == OrderSide::kBuy) {
    if (!instrument.price_limit_down.has_value()) {
      return order_math_detail::Failure(
          "instrument price_limit_down is required");
    }
    const double raw =
        ticker.bid_price *
        (1.0 - *instrument.price_limit_down * passive_price_limit_fraction);
    price =
        std::floor(raw / instrument.price_tick + 1e-12) * instrument.price_tick;
  } else {
    if (!instrument.price_limit_up.has_value()) {
      return order_math_detail::Failure(
          "instrument price_limit_up is required");
    }
    const double raw =
        ticker.ask_price *
        (1.0 + *instrument.price_limit_up * passive_price_limit_fraction);
    price =
        std::ceil(raw / instrument.price_tick - 1e-12) * instrument.price_tick;
  }
  return order_math_detail::Build(instrument, side, price, quantity, false);
}

[[nodiscard]] inline WireOrderResult BuildCloseOrder(
    const BookTicker& ticker, const config::InstrumentInfo& instrument,
    OrderSide side, double quantity, std::uint32_t close_slippage_bps) {
  std::string error;
  if (!order_math_detail::Validate(ticker, instrument, quantity, &error)) {
    return order_math_detail::Failure(std::move(error));
  }
  if (close_slippage_bps == 0 || close_slippage_bps > 1'000) {
    return order_math_detail::Failure(
        "close slippage bps must be in [1, 1000]");
  }
  const double fraction = static_cast<double>(close_slippage_bps) / 10'000.0;
  double price = 0.0;
  if (side == OrderSide::kBuy) {
    const double raw = ticker.ask_price * (1.0 + fraction);
    price =
        std::ceil(raw / instrument.price_tick - 1e-12) * instrument.price_tick;
  } else {
    const double raw = ticker.bid_price * (1.0 - fraction);
    price =
        std::floor(raw / instrument.price_tick + 1e-12) * instrument.price_tick;
  }
  return order_math_detail::Build(instrument, side, price, quantity, true);
}

}  // namespace aquila::tools::bitget::gateway_smoke

#endif  // AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_ORDER_MATH_H_
