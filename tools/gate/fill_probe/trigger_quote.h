#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_TRIGGER_QUOTE_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_TRIGGER_QUOTE_H_

#include <cstdint>
#include <string>

#include "tools/gate/fill_probe/order_math.h"

namespace aquila::tools::gate::fill_probe {

struct FreshnessLimits {
  std::int64_t max_binance_freshness_ns{2'000'000};
  std::int64_t max_gate_freshness_ns{50'000'000};
};

struct TriggerQuoteDecision {
  bool accepted{false};
  std::int64_t binance_freshness_ns{0};
  std::int64_t gate_freshness_ns{0};
  std::int64_t gate_exchange_delta_ns{0};
  std::int64_t gate_local_delta_ns{0};
  std::string skip_reason;
};

[[nodiscard]] inline TriggerQuoteDecision EvaluateTriggerQuote(
    const BboSnapshot& binance, const BboSnapshot& gate,
    std::int64_t decision_ns, FreshnessLimits limits) {
  TriggerQuoteDecision decision;
  decision.binance_freshness_ns = decision_ns - binance.local_ns;
  decision.gate_freshness_ns = decision_ns - gate.local_ns;
  decision.gate_exchange_delta_ns = gate.exchange_ns - binance.exchange_ns;
  decision.gate_local_delta_ns = gate.local_ns - binance.local_ns;

  if (decision.binance_freshness_ns < 0 ||
      decision.binance_freshness_ns >= limits.max_binance_freshness_ns) {
    decision.skip_reason = "stale_binance_trigger";
    return decision;
  }
  if (decision.gate_freshness_ns < 0 ||
      decision.gate_freshness_ns >= limits.max_gate_freshness_ns) {
    decision.skip_reason = "stale_gate_quote";
    return decision;
  }
  decision.accepted = true;
  return decision;
}

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_TRIGGER_QUOTE_H_
