#ifndef AQUILA_STRATEGY_LEAD_LAG_SHADOW_EVALUATION_H_
#define AQUILA_STRATEGY_LEAD_LAG_SHADOW_EVALUATION_H_

#include <cmath>

#include "core/common/types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/signal.h"

namespace aquila::strategy::leadlag {

inline constexpr double kShadowOrderPriceEpsilon = 1e-12;

[[nodiscard]] inline double ReferenceBufferPctForAction(
    SignalAction action, const TakerBufferConfig& buffer) noexcept {
  if (buffer.mode == FeatureMode::kOff) {
    return 0.0;
  }
  switch (action) {
    case SignalAction::kOpenLong:
    case SignalAction::kOpenShort:
      return buffer.entry_fixed_pct;
    case SignalAction::kCloseLong:
    case SignalAction::kCloseShort:
      return buffer.normal_close_fixed_pct;
    case SignalAction::kStoplossLong:
    case SignalAction::kStoplossShort:
    case SignalAction::kNone:
      return 0.0;
  }
  return 0.0;
}

[[nodiscard]] inline double RoundShadowOrderPrice(
    double price, const InstrumentMetadata& instrument,
    OrderSide side) noexcept {
  if (!std::isfinite(price) || price <= 0.0 ||
      !std::isfinite(instrument.price_tick) || instrument.price_tick <= 0.0) {
    return 0.0;
  }
  const double scaled = price / instrument.price_tick;
  if (!std::isfinite(scaled)) {
    return 0.0;
  }
  const double units = side == OrderSide::kBuy
                           ? std::ceil(scaled - kShadowOrderPriceEpsilon)
                           : std::floor(scaled + kShadowOrderPriceEpsilon);
  const double rounded = units * instrument.price_tick;
  return std::isfinite(rounded) && rounded > 0.0 ? rounded : 0.0;
}

[[nodiscard]] inline double ReferenceShadowOrderPrice(
    SignalAction action, OrderSide side, double raw_price,
    const InstrumentMetadata& instrument,
    const TakerBufferConfig& buffer) noexcept {
  const double buffer_pct = ReferenceBufferPctForAction(action, buffer);
  if (buffer.mode == FeatureMode::kOff) {
    return 0.0;
  }

  double adjusted_price = raw_price;
  switch (side) {
    case OrderSide::kBuy:
      adjusted_price = raw_price * (1.0 + buffer_pct);
      break;
    case OrderSide::kSell:
      adjusted_price = raw_price * (1.0 - buffer_pct);
      break;
  }
  return RoundShadowOrderPrice(adjusted_price, instrument, side);
}

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_SHADOW_EVALUATION_H_
