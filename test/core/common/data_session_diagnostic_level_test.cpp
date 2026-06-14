#include "core/common/data_session_diagnostic_level.h"

#include <gtest/gtest.h>

namespace aquila::core {
namespace {

TEST(DataSessionDiagnosticLevelTest, ExposesCompileTimeLevelAndCapabilities) {
  EXPECT_EQ(kDataSessionDiagnosticLevel, AQUILA_DATA_SESSION_DIAG_LEVEL);
  EXPECT_EQ(DataSessionDiagnosticLevelName(0), "L0");
  EXPECT_EQ(DataSessionDiagnosticLevelName(4), "L4");
  EXPECT_EQ(DataSessionDiagnosticLevelName(5), "unknown");

  EXPECT_EQ(kDataSessionDiagnosticCorrelationEnabled,
            AQUILA_DATA_SESSION_DIAG_LEVEL >= 1);
  EXPECT_EQ(kDataSessionDiagnosticUserPathEnabled,
            AQUILA_DATA_SESSION_DIAG_LEVEL >= 2);
  EXPECT_EQ(kDataSessionDiagnosticTcpInfoEnabled,
            AQUILA_DATA_SESSION_DIAG_LEVEL >= 3);
  EXPECT_EQ(kDataSessionDiagnosticSocketTimestampingEnabled,
            AQUILA_DATA_SESSION_DIAG_LEVEL >= 4);
}

TEST(DataSessionDiagnosticLevelTest, ReportsMissingRuntimeCapabilities) {
  EXPECT_EQ(RequiredDataSessionDiagnosticLevelForTcpInfo(), 3);
  EXPECT_EQ(RequiredDataSessionDiagnosticLevelForSocketTimestamping(), 4);
  EXPECT_FALSE(DataSessionDiagnosticLevelSupports(-1));
  EXPECT_TRUE(DataSessionDiagnosticLevelSupports(0));
  EXPECT_TRUE(DataSessionDiagnosticLevelSupports(kDataSessionDiagnosticLevel));
  EXPECT_FALSE(
      DataSessionDiagnosticLevelSupports(kDataSessionDiagnosticLevel + 1));
}

}  // namespace
}  // namespace aquila::core
