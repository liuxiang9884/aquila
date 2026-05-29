#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_ORDER_MODE_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_ORDER_MODE_H_

#include <cstdint>

namespace aquila::tools::gate_order_session_rtt_probe {

enum class ProbeOrderMode : std::uint8_t {
  kIoc,
  kGtc,
  kIocAndGtc,
};

[[nodiscard]] constexpr bool ProbeOrderModeUsesIoc(
    ProbeOrderMode mode) noexcept {
  return mode == ProbeOrderMode::kIoc || mode == ProbeOrderMode::kIocAndGtc;
}

[[nodiscard]] constexpr bool ProbeOrderModeUsesGtc(
    ProbeOrderMode mode) noexcept {
  return mode == ProbeOrderMode::kGtc || mode == ProbeOrderMode::kIocAndGtc;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_ORDER_MODE_H_
