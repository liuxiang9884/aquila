#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_PASSIVE_ORDER_BUILDER_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_PASSIVE_ORDER_BUILDER_H_

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"
#include "core/trading/order_types.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

struct ProbeWireOrder {
  std::uint64_t local_order_id{0};
  std::string symbol;
  OrderSide side{OrderSide::kBuy};
  OrderType type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kImmediateOrCancel};
  double price{0.0};
  double quantity{0.0};
  std::string quantity_text;
  std::string price_text;
  std::uint8_t price_decimal_places{0};
  std::uint8_t quantity_decimal_places{0};
  bool reduce_only{false};
};

[[nodiscard]] inline core::OrderPlaceRequest ToOrderPlaceRequest(
    const ProbeWireOrder& order) noexcept {
  core::OrderPlaceRequest request{
      .local_order_id = order.local_order_id,
      .price = order.price,
      .quantity = order.quantity,
      .exchange = Exchange::kBitget,
      .side = order.side,
      .order_type = order.type,
      .time_in_force = order.time_in_force,
      .price_decimal_places = order.price_decimal_places,
      .quantity_decimal_places = order.quantity_decimal_places,
      .reduce_only = order.reduce_only,
  };
  core::SetOrderSymbol(&request, order.symbol);
  return request;
}

struct ProbeOrderBuildResult {
  bool ok{false};
  ProbeWireOrder order;
  std::int64_t bbo_ticker_id{0};
  std::int64_t bbo_local_ns{0};
  std::string error;
};

namespace passive_order_builder_detail {

[[nodiscard]] inline std::string FormatDecimal(double value,
                                               std::int32_t decimals) {
  return decimals <= 0 ? fmt::format("{:.0f}", value)
                       : fmt::format("{:.{}f}", value, decimals);
}

[[nodiscard]] inline ProbeOrderBuildResult Failure(std::string error) {
  ProbeOrderBuildResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] inline bool ValidateInputs(
    const BookTicker& ticker, const config::InstrumentInfo& instrument,
    double price_limit_fraction, std::string* error) {
  if (ticker.exchange != Exchange::kBitget) {
    *error = "book ticker exchange must be Bitget";
    return false;
  }
  if (!std::isfinite(ticker.bid_price) || ticker.bid_price <= 0.0 ||
      !std::isfinite(ticker.ask_price) || ticker.ask_price <= 0.0 ||
      ticker.ask_price < ticker.bid_price) {
    *error = "book ticker prices must be positive and ordered";
    return false;
  }
  if (instrument.exchange != Exchange::kBitget ||
      instrument.exchange_symbol.empty()) {
    *error = "instrument must contain a Bitget exchange symbol";
    return false;
  }
  if (!std::isfinite(instrument.price_tick) || instrument.price_tick <= 0.0 ||
      instrument.price_decimal_places < 0) {
    *error = "instrument price metadata must be positive";
    return false;
  }
  if (!instrument.price_limit_down ||
      !std::isfinite(*instrument.price_limit_down) ||
      *instrument.price_limit_down <= 0.0 ||
      *instrument.price_limit_down >= 1.0) {
    *error = "instrument price_limit_down must be in (0, 1)";
    return false;
  }
  if (!std::isfinite(price_limit_fraction) || price_limit_fraction <= 0.0 ||
      price_limit_fraction > 1.0) {
    *error = "passive price limit fraction must be in (0, 1]";
    return false;
  }
  if (!instrument.quantity_step || !instrument.quantity_decimal_places ||
      !std::isfinite(*instrument.quantity_step) ||
      *instrument.quantity_step <= 0.0 ||
      *instrument.quantity_decimal_places < 0 ||
      !std::isfinite(instrument.min_quantity) ||
      instrument.min_quantity <= 0.0) {
    *error = "instrument quantity metadata must be positive";
    return false;
  }
  return true;
}

[[nodiscard]] inline double ProbePrice(const BookTicker& ticker,
                                       const config::InstrumentInfo& instrument,
                                       double price_limit_fraction) noexcept {
  const double raw_price =
      ticker.bid_price *
      (1.0 - *instrument.price_limit_down * price_limit_fraction);
  const double tick_units =
      std::floor(raw_price / instrument.price_tick + 1e-12);
  return tick_units * instrument.price_tick;
}

[[nodiscard]] inline ProbeOrderBuildResult Build(
    const BookTicker& ticker, const config::InstrumentInfo& instrument,
    double quantity, double price_limit_fraction, OrderSide side,
    bool reduce_only) {
  std::string error;
  if (!ValidateInputs(ticker, instrument, price_limit_fraction, &error)) {
    return Failure(std::move(error));
  }
  const double price = ProbePrice(ticker, instrument, price_limit_fraction);
  if (!std::isfinite(price) || price <= 0.0 || !std::isfinite(quantity) ||
      quantity <= 0.0) {
    return Failure("computed order price and quantity must be positive");
  }
  ProbeOrderBuildResult result;
  result.ok = true;
  result.bbo_ticker_id = ticker.id;
  result.bbo_local_ns = ticker.local_ns;
  result.order = ProbeWireOrder{
      .symbol = instrument.exchange_symbol,
      .side = side,
      .type = OrderType::kLimit,
      .time_in_force = TimeInForce::kImmediateOrCancel,
      .price = price,
      .quantity = quantity,
      .quantity_text =
          FormatDecimal(quantity, *instrument.quantity_decimal_places),
      .price_text = FormatDecimal(price, instrument.price_decimal_places),
      .price_decimal_places =
          static_cast<std::uint8_t>(instrument.price_decimal_places),
      .quantity_decimal_places =
          static_cast<std::uint8_t>(*instrument.quantity_decimal_places),
      .reduce_only = reduce_only,
  };
  return result;
}

}  // namespace passive_order_builder_detail

[[nodiscard]] inline ProbeOrderBuildResult BuildPassiveBuyIoc(
    const BookTicker& ticker, const config::InstrumentInfo& instrument,
    double price_limit_fraction) {
  return passive_order_builder_detail::Build(
      ticker, instrument, instrument.min_quantity, price_limit_fraction,
      OrderSide::kBuy, false);
}

[[nodiscard]] inline ProbeOrderBuildResult BuildSafetyCloseSellIoc(
    const BookTicker& ticker, const config::InstrumentInfo& instrument,
    double filled_quantity, double price_limit_fraction) {
  std::string error;
  if (!passive_order_builder_detail::ValidateInputs(
          ticker, instrument, price_limit_fraction, &error)) {
    return passive_order_builder_detail::Failure(std::move(error));
  }
  if (!std::isfinite(filled_quantity) || filled_quantity <= 0.0) {
    return passive_order_builder_detail::Failure(
        "filled quantity must be positive");
  }
  const double step = *instrument.quantity_step;
  const double quantity = std::floor(filled_quantity / step + 1e-12) * step;
  if (quantity + 1e-12 < instrument.min_quantity) {
    return passive_order_builder_detail::Failure(
        "safety close quantity is below instrument minimum");
  }
  return passive_order_builder_detail::Build(ticker, instrument, quantity,
                                             price_limit_fraction,
                                             OrderSide::kSell, true);
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_PASSIVE_ORDER_BUILDER_H_
