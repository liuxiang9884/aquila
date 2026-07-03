#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_ORDER_MATH_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_ORDER_MATH_H_

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "core/common/result.h"
#include "core/common/types.h"
#include "core/config/instrument_catalog.h"

namespace aquila::tools::gate::fill_probe {

struct BboSnapshot {
  std::uint64_t id{0};
  std::int32_t symbol_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t local_ns{0};
  double bid_price{0.0};
  double bid_volume{0.0};
  double ask_price{0.0};
  double ask_volume{0.0};
  double price_tick{0.0};
};

struct OrderSizing {
  double quantity{0.0};
  std::string quantity_text;
  double notional_usdt{0.0};
};

struct PriceText {
  double price{0.0};
  std::string price_text;
};

using OrderSizingResult = Result<OrderSizing>;

namespace order_math_detail {

[[nodiscard]] inline OrderSizingResult Failure(std::string error) {
  OrderSizingResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] inline std::int32_t DecimalPlacesFromTick(double tick) {
  if (!std::isfinite(tick) || tick <= 0.0) {
    return 0;
  }
  double scaled = tick;
  for (std::int32_t places = 0; places <= 12; ++places) {
    if (std::fabs(scaled - std::round(scaled)) < 1e-9) {
      return places;
    }
    scaled *= 10.0;
  }
  return 12;
}

[[nodiscard]] inline std::string TrimDecimalZeros(std::string text) {
  const std::size_t dot = text.find('.');
  if (dot == std::string::npos) {
    return text;
  }
  while (!text.empty() && text.back() == '0') {
    text.pop_back();
  }
  if (!text.empty() && text.back() == '.') {
    text.pop_back();
  }
  return text;
}

[[nodiscard]] inline std::string FormatDecimal(double value,
                                               std::int32_t decimal_places) {
  if (std::fabs(value) < 1e-12) {
    value = 0.0;
  }
  if (decimal_places <= 0) {
    return fmt::format("{:.0f}", value);
  }
  return TrimDecimalZeros(fmt::format("{:.{}f}", value, decimal_places));
}

[[nodiscard]] inline double FloorToTick(double value, double tick) {
  return std::floor(value / tick + 1e-12) * tick;
}

[[nodiscard]] inline double CeilToTick(double value, double tick) {
  return std::ceil(value / tick - 1e-12) * tick;
}

[[nodiscard]] inline PriceText MakePriceText(double price, double tick) {
  const std::int32_t decimal_places = DecimalPlacesFromTick(tick);
  return PriceText{.price = price,
                   .price_text = FormatDecimal(price, decimal_places)};
}

}  // namespace order_math_detail

[[nodiscard]] inline OrderSizingResult BuildOrderSizing(
    const config::InstrumentInfo& instrument, double reference_price) {
  if (!std::isfinite(reference_price) || reference_price <= 0.0) {
    return order_math_detail::Failure("reference price must be positive");
  }
  if (!std::isfinite(instrument.min_quantity) ||
      instrument.min_quantity <= 0.0) {
    return order_math_detail::Failure("instrument min_quantity must be positive");
  }
  if (!std::isfinite(instrument.notional_multiplier) ||
      instrument.notional_multiplier <= 0.0) {
    return order_math_detail::Failure(
        "instrument notional_multiplier must be positive");
  }
  if (!instrument.quantity_decimal_places ||
      *instrument.quantity_decimal_places < 0) {
    return order_math_detail::Failure(
        "instrument quantity_decimal_places is required");
  }

  OrderSizingResult result;
  result.value.quantity = instrument.min_quantity;
  result.value.quantity_text = order_math_detail::FormatDecimal(
      instrument.min_quantity, *instrument.quantity_decimal_places);
  result.value.notional_usdt = reference_price * instrument.min_quantity *
                               instrument.notional_multiplier;
  result.ok = true;
  return result;
}

[[nodiscard]] inline PriceText EntryPrice(OrderSide side,
                                          const BboSnapshot& bbo) {
  if (side == OrderSide::kBuy) {
    return order_math_detail::MakePriceText(
        order_math_detail::CeilToTick(bbo.ask_price, bbo.price_tick),
        bbo.price_tick);
  }
  return order_math_detail::MakePriceText(
      order_math_detail::FloorToTick(bbo.bid_price, bbo.price_tick),
      bbo.price_tick);
}

[[nodiscard]] inline PriceText ClosePrice(OrderSide close_side,
                                          const BboSnapshot& bbo,
                                          std::uint32_t slippage_bps) {
  const double slippage_fraction =
      static_cast<double>(slippage_bps) / 10000.0;
  if (close_side == OrderSide::kBuy) {
    return order_math_detail::MakePriceText(
        order_math_detail::CeilToTick(bbo.ask_price * (1.0 + slippage_fraction),
                                      bbo.price_tick),
        bbo.price_tick);
  }
  return order_math_detail::MakePriceText(
      order_math_detail::CeilToTick(bbo.bid_price * (1.0 - slippage_fraction),
                                    bbo.price_tick),
      bbo.price_tick);
}

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_ORDER_MATH_H_
