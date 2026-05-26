#include "exchange/gate/trading/order_latency_diagnostics.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace aquila::gate {
namespace {

TEST(OrderLatencyDiagnosticsTest,
     RecordsSendToDriveReadAndAckThresholdsPerOrder) {
  OrderAckLatencyDiagnostics diagnostics;
  std::vector<OrderLatencyDiagnosticLogRecord> records;

  diagnostics.Arm(OrderLatencyDiagnosticWindow{
      .local_order_id = 123,
      .request_sequence = 40,
      .request_send_local_ns = 1'000,
      .inflight_at_send = 7,
  });

  EXPECT_EQ(diagnostics.active_count(), 1U);
  EXPECT_EQ(diagnostics.RecordAfterRuntimeHook(
                2'000, [&](const auto& record) { records.push_back(record); }),
            0U);

  EXPECT_EQ(
      diagnostics.RecordBeforeDriveRead(
          3'500'001, [&](const auto& record) { records.push_back(record); }),
      1U);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records.back().reason,
            OrderLatencyDiagnosticReason::kSendToDriveReadThreshold);
  EXPECT_EQ(records.back().local_order_id, 123U);
  EXPECT_EQ(records.back().request_sequence, 40U);
  EXPECT_EQ(records.back().send_to_first_drive_read_ns, 3'499'001);
  EXPECT_EQ(records.back().inflight_at_send, 7U);

  EXPECT_EQ(
      diagnostics.RecordAfterDriveRead(
          4'800'002, [&](const auto& record) { records.push_back(record); }),
      1U);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records.back().reason,
            OrderLatencyDiagnosticReason::kDriveReadDurationThreshold);
  EXPECT_EQ(records.back().drive_read_duration_ns, 1'300'001);
  EXPECT_EQ(records.back().max_observed_drive_read_duration_ns, 1'300'001);

  EXPECT_TRUE(diagnostics.RecordAck(
      40, 25'000'000, 5'000,
      [&](const auto& record) { records.push_back(record); }));
  ASSERT_EQ(records.size(), 3U);
  EXPECT_EQ(records.back().reason,
            OrderLatencyDiagnosticReason::kAckRttThreshold);
  EXPECT_EQ(records.back().ack_rtt_ns, 24'999'000);
  EXPECT_EQ(records.back().ack_exchange_ns, 5'000);
  EXPECT_EQ(records.back().send_to_first_after_hook_ns, 1'000);
  EXPECT_EQ(diagnostics.active_count(), 0U);
}

TEST(OrderLatencyDiagnosticsTest, RateLimitsRepeatedLoopThresholdRecords) {
  OrderAckLatencyDiagnostics diagnostics;
  diagnostics.Arm(OrderLatencyDiagnosticWindow{
      .local_order_id = 123,
      .request_sequence = 40,
      .request_send_local_ns = 1'000,
      .inflight_at_send = 1,
  });

  std::vector<OrderLatencyDiagnosticLogRecord> records;
  EXPECT_EQ(
      diagnostics.RecordBeforeDriveRead(
          4'000'000, [&](const auto& record) { records.push_back(record); }),
      1U);
  EXPECT_EQ(
      diagnostics.RecordBeforeDriveRead(
          5'000'000, [&](const auto& record) { records.push_back(record); }),
      0U);
  EXPECT_EQ(records.size(), 1U);
}

TEST(OrderLatencyDiagnosticsTest, ClearRemovesActiveDiagnosticWindows) {
  OrderAckLatencyDiagnostics diagnostics;
  diagnostics.Arm(OrderLatencyDiagnosticWindow{
      .local_order_id = 123,
      .request_sequence = 40,
      .request_send_local_ns = 1'000,
      .inflight_at_send = 1,
  });

  diagnostics.clear();

  EXPECT_TRUE(diagnostics.empty());
  std::vector<OrderLatencyDiagnosticLogRecord> records;
  EXPECT_FALSE(diagnostics.RecordAck(
      40, 25'000'000, 5'000,
      [&](const auto& record) { records.push_back(record); }));
  EXPECT_TRUE(records.empty());
}

}  // namespace
}  // namespace aquila::gate
