#ifndef AQUILA_CORE_COMMON_ORDER_ACK_DIAGNOSTIC_LEVEL_H_
#define AQUILA_CORE_COMMON_ORDER_ACK_DIAGNOSTIC_LEVEL_H_

#ifndef AQUILA_ORDER_ACK_DIAG_LEVEL
#define AQUILA_ORDER_ACK_DIAG_LEVEL 4
#endif

#if AQUILA_ORDER_ACK_DIAG_LEVEL < 0 || AQUILA_ORDER_ACK_DIAG_LEVEL > 5
#error "AQUILA_ORDER_ACK_DIAG_LEVEL must be in [0, 5]"
#endif

namespace aquila::core {

inline constexpr int kOrderAckDiagnosticLevel = AQUILA_ORDER_ACK_DIAG_LEVEL;

inline constexpr bool kOrderAckDiagnosticCorrelationEnabled =
    kOrderAckDiagnosticLevel >= 1;
inline constexpr bool kOrderAckDiagnosticRuntimeWritePathEnabled =
    kOrderAckDiagnosticLevel >= 2;
inline constexpr bool kOrderAckDiagnosticTcpInfoEnabled =
    kOrderAckDiagnosticLevel >= 3;
inline constexpr bool kOrderAckDiagnosticSocketTimestampingEnabled =
    kOrderAckDiagnosticLevel >= 4;
inline constexpr bool kOrderAckDiagnosticPcapGateHeaderEnabled =
    kOrderAckDiagnosticLevel >= 5;

[[nodiscard]] inline constexpr int
RequiredOrderAckDiagnosticLevelForTcpInfo() noexcept {
  return 3;
}

[[nodiscard]] inline constexpr int
RequiredOrderAckDiagnosticLevelForSocketTimestamping() noexcept {
  return 4;
}

[[nodiscard]] inline constexpr bool OrderAckDiagnosticLevelSupports(
    int required_level) noexcept {
  return required_level >= 0 && required_level <= kOrderAckDiagnosticLevel;
}

[[nodiscard]] inline constexpr const char* OrderAckDiagnosticLevelName(
    int level) noexcept {
  switch (level) {
    case 0:
      return "L0";
    case 1:
      return "L1";
    case 2:
      return "L2";
    case 3:
      return "L3";
    case 4:
      return "L4";
    case 5:
      return "L5";
    default:
      return "unknown";
  }
}

}  // namespace aquila::core

#endif  // AQUILA_CORE_COMMON_ORDER_ACK_DIAGNOSTIC_LEVEL_H_
