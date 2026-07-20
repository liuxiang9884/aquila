#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_PASSIVE_ORDER_BUILDER_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_PASSIVE_ORDER_BUILDER_H_

#include <cmath>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include "core/config/instrument_catalog.h"
#include "core/market_data/types.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct PassiveOrderOptions {
  double passive_price_limit_fraction{0.5};
};

struct PassiveOrderBuildResult {
  bool ok{false};
  std::string contract;
  double price{0.0};
  double quantity{0.0};
  std::string price_text;
  std::string quantity_text;
  std::uint8_t price_decimal_places{0};
  std::uint8_t quantity_decimal_places{0};
  std::int64_t bbo_ticker_id{0};
  std::int64_t bbo_local_ns{0};
  std::string error;
};

namespace passive_order_builder_detail {

[[nodiscard]] inline std::string FormatFixedDecimal(
    double value, std::int32_t decimal_places) {
  if (decimal_places <= 0) {
    return fmt::format("{:.0f}", value);
  }
  return fmt::format("{:.{}f}", value, decimal_places);
}

}  // namespace passive_order_builder_detail

[[nodiscard]] inline PassiveOrderBuildResult BuildPassiveBuyOrder(
    const BookTicker& ticker, const config::InstrumentInfo& instrument,
    PassiveOrderOptions options = {}) {
  PassiveOrderBuildResult result{
      .contract = instrument.symbol,
      .bbo_ticker_id = ticker.id,
      .bbo_local_ns = ticker.local_ns,
  };

  if (ticker.exchange != Exchange::kGate) {
    result.error = "book ticker exchange must be Gate";
    return result;
  }
  if (!std::isfinite(ticker.bid_price) || ticker.bid_price <= 0.0) {
    result.error = "book ticker bid price must be positive";
    return result;
  }
  if (!std::isfinite(instrument.price_tick) || instrument.price_tick <= 0.0) {
    result.error = "instrument price_tick must be positive";
    return result;
  }
  if (!instrument.price_limit_down ||
      !std::isfinite(*instrument.price_limit_down) ||
      *instrument.price_limit_down <= 0.0) {
    result.error = "instrument price_limit_down must be positive";
    return result;
  }
  if (!std::isfinite(options.passive_price_limit_fraction) ||
      options.passive_price_limit_fraction <= 0.0 ||
      options.passive_price_limit_fraction > 1.0) {
    result.error = "passive price limit fraction must be in (0, 1]";
    return result;
  }
  if (!instrument.quantity_step || !instrument.quantity_decimal_places ||
      !std::isfinite(*instrument.quantity_step) ||
      *instrument.quantity_step <= 0.0 || instrument.min_quantity <= 0.0) {
    result.error = "instrument quantity metadata must be positive";
    return result;
  }

  const double max_deviation =
      *instrument.price_limit_down * options.passive_price_limit_fraction;
  const double raw_price = ticker.bid_price * (1.0 - max_deviation);
  const double units = std::floor(raw_price / instrument.price_tick + 1e-12);
  const double price = units * instrument.price_tick;
  if (!std::isfinite(price) || price <= 0.0) {
    result.error = "computed passive price must be positive";
    return result;
  }

  result.price_text = passive_order_builder_detail::FormatFixedDecimal(
      price, instrument.price_decimal_places);
  result.quantity_text = passive_order_builder_detail::FormatFixedDecimal(
      instrument.min_quantity, *instrument.quantity_decimal_places);
  result.price = price;
  result.quantity = instrument.min_quantity;
  result.price_decimal_places =
      static_cast<std::uint8_t>(instrument.price_decimal_places);
  result.quantity_decimal_places =
      static_cast<std::uint8_t>(*instrument.quantity_decimal_places);
  result.ok = true;
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_PASSIVE_ORDER_BUILDER_H_
