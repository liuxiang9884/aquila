#ifndef AQUILA_CORE_COMMON_DATA_SESSION_DIAGNOSTIC_LEVEL_H_
#define AQUILA_CORE_COMMON_DATA_SESSION_DIAGNOSTIC_LEVEL_H_

#ifndef AQUILA_DATA_SESSION_DIAG_LEVEL
#define AQUILA_DATA_SESSION_DIAG_LEVEL 0
#endif

#if AQUILA_DATA_SESSION_DIAG_LEVEL < 0 || AQUILA_DATA_SESSION_DIAG_LEVEL > 4
#error "AQUILA_DATA_SESSION_DIAG_LEVEL must be in [0, 4]"
#endif

namespace aquila::core {

inline constexpr int kDataSessionDiagnosticLevel =
    AQUILA_DATA_SESSION_DIAG_LEVEL;

inline constexpr bool kDataSessionDiagnosticCorrelationEnabled =
    kDataSessionDiagnosticLevel >= 1;
inline constexpr bool kDataSessionDiagnosticUserPathEnabled =
    kDataSessionDiagnosticLevel >= 2;
inline constexpr bool kDataSessionDiagnosticTcpInfoEnabled =
    kDataSessionDiagnosticLevel >= 3;
inline constexpr bool kDataSessionDiagnosticSocketTimestampingEnabled =
    kDataSessionDiagnosticLevel >= 4;

[[nodiscard]] inline constexpr int
RequiredDataSessionDiagnosticLevelForTcpInfo() noexcept {
  return 3;
}

[[nodiscard]] inline constexpr int
RequiredDataSessionDiagnosticLevelForSocketTimestamping() noexcept {
  return 4;
}

[[nodiscard]] inline constexpr bool DataSessionDiagnosticLevelSupports(
    int required_level) noexcept {
  return required_level >= 0 && required_level <= kDataSessionDiagnosticLevel;
}

[[nodiscard]] inline constexpr const char* DataSessionDiagnosticLevelName(
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
    default:
      return "unknown";
  }
}

}  // namespace aquila::core

#endif  // AQUILA_CORE_COMMON_DATA_SESSION_DIAGNOSTIC_LEVEL_H_
