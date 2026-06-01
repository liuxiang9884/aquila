#include "core/common/order_ack_diagnostic_level.h"

#include <gtest/gtest.h>

namespace aquila::core {
namespace {

TEST(OrderAckDiagnosticLevelTest, ExposesCompileTimeLevelAndCapabilities) {
  EXPECT_EQ(kOrderAckDiagnosticLevel, AQUILA_ORDER_ACK_DIAG_LEVEL);
  EXPECT_EQ(OrderAckDiagnosticLevelName(0), "L0");
  EXPECT_EQ(OrderAckDiagnosticLevelName(5), "L5");
  EXPECT_EQ(OrderAckDiagnosticLevelName(6), "unknown");

  EXPECT_EQ(kOrderAckDiagnosticCorrelationEnabled,
            AQUILA_ORDER_ACK_DIAG_LEVEL >= 1);
  EXPECT_EQ(kOrderAckDiagnosticRuntimeWritePathEnabled,
            AQUILA_ORDER_ACK_DIAG_LEVEL >= 2);
  EXPECT_EQ(kOrderAckDiagnosticTcpInfoEnabled,
            AQUILA_ORDER_ACK_DIAG_LEVEL >= 3);
  EXPECT_EQ(kOrderAckDiagnosticSocketTimestampingEnabled,
            AQUILA_ORDER_ACK_DIAG_LEVEL >= 4);
  EXPECT_EQ(kOrderAckDiagnosticPcapGateHeaderEnabled,
            AQUILA_ORDER_ACK_DIAG_LEVEL >= 5);
}

TEST(OrderAckDiagnosticLevelTest, ReportsMissingRuntimeCapabilities) {
  EXPECT_EQ(RequiredOrderAckDiagnosticLevelForTcpInfo(), 3);
  EXPECT_EQ(RequiredOrderAckDiagnosticLevelForSocketTimestamping(), 4);
  EXPECT_TRUE(OrderAckDiagnosticLevelSupports(0));
  EXPECT_TRUE(OrderAckDiagnosticLevelSupports(kOrderAckDiagnosticLevel));
  EXPECT_FALSE(OrderAckDiagnosticLevelSupports(kOrderAckDiagnosticLevel + 1));
}

}  // namespace
}  // namespace aquila::core
