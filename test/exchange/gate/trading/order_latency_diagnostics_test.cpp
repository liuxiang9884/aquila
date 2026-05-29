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

TEST(OrderLatencyDiagnosticsTest, AckRecordIncludesCurrentDriveReadDuration) {
  OrderAckLatencyDiagnostics diagnostics(OrderLatencyDiagnosticConfig{
      .ack_rtt_threshold_ns = 1'000,
      .send_to_first_drive_read_threshold_ns = 100'000'000,
      .drive_read_duration_threshold_ns = 100'000'000,
      .diagnostic_window_timeout_ns = 250'000'000,
      .max_logs_per_second = 10,
  });
  std::vector<OrderLatencyDiagnosticLogRecord> records;

  diagnostics.Arm(OrderLatencyDiagnosticWindow{
      .local_order_id = 123,
      .request_sequence = 40,
      .request_send_local_ns = 1'000,
      .inflight_at_send = 1,
  });
  EXPECT_EQ(
      diagnostics.RecordBeforeDriveRead(
          24'000'000, [&](const auto& record) { records.push_back(record); }),
      0U);

  EXPECT_TRUE(diagnostics.RecordAck(
      40, 25'500'000, 5'000, 24'000'000,
      [&](const auto& record) { records.push_back(record); }));

  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records.back().reason,
            OrderLatencyDiagnosticReason::kAckRttThreshold);
  EXPECT_EQ(records.back().drive_read_duration_ns, 1'500'000);
  EXPECT_EQ(records.back().max_observed_drive_read_duration_ns, 1'500'000);
}

TEST(OrderLatencyDiagnosticsTest, AckDriveReadDurationCanEmitThresholdRecord) {
  OrderAckLatencyDiagnostics diagnostics(OrderLatencyDiagnosticConfig{
      .ack_rtt_threshold_ns = 100'000'000,
      .send_to_first_drive_read_threshold_ns = 100'000'000,
      .drive_read_duration_threshold_ns = 1'000'000,
      .diagnostic_window_timeout_ns = 250'000'000,
      .max_logs_per_second = 10,
  });
  std::vector<OrderLatencyDiagnosticLogRecord> records;

  diagnostics.Arm(OrderLatencyDiagnosticWindow{
      .local_order_id = 123,
      .request_sequence = 40,
      .request_send_local_ns = 1'000,
      .inflight_at_send = 1,
  });
  EXPECT_EQ(
      diagnostics.RecordBeforeDriveRead(
          24'000'000, [&](const auto& record) { records.push_back(record); }),
      0U);

  EXPECT_TRUE(diagnostics.RecordAck(
      40, 25'500'000, 5'000, 24'000'000,
      [&](const auto& record) { records.push_back(record); }));

  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records.back().reason,
            OrderLatencyDiagnosticReason::kDriveReadDurationThreshold);
  EXPECT_EQ(records.back().drive_read_duration_ns, 1'500'000);
  EXPECT_EQ(records.back().max_observed_drive_read_duration_ns, 1'500'000);
  EXPECT_TRUE(diagnostics.empty());
}

TEST(OrderLatencyDiagnosticsTest, AckRecordIncludesWriteAndSocketDiagnostics) {
  OrderAckLatencyDiagnostics diagnostics(OrderLatencyDiagnosticConfig{
      .ack_rtt_threshold_ns = 1'000,
      .send_to_first_drive_read_threshold_ns = 100'000'000,
      .drive_read_duration_threshold_ns = 100'000'000,
      .diagnostic_window_timeout_ns = 250'000'000,
      .max_logs_per_second = 10,
  });
  std::vector<OrderLatencyDiagnosticLogRecord> records;

  OrderLatencyDiagnosticWindow window{
      .local_order_id = 123,
      .request_sequence = 40,
      .request_send_local_ns = 1'000,
      .inflight_at_send = 1,
  };
  window.owner_thread_tid = 2468;
  window.write_path.order_encode_done_ns = 1'100;
  window.write_path.ws_frame_encode_done_ns = 1'200;
  window.write_path.write_enqueue_ns = 1'300;
  window.write_path.drive_write_enter_ns = 1'350;
  window.write_path.write_some_enter_ns = 1'400;
  window.write_path.write_some_return_ns = 1'450;
  window.write_path.write_complete_ns = 1'500;
  window.write_path.write_some_bytes = 64;
  window.write_path.write_complete_bytes = 64;
  window.write_path.write_errno = 0;
  window.write_path.write_eagain = false;
  window.write_path.pending_write_count_after = 0;
  window.socket_send_queue.available = true;
  window.socket_send_queue.sendq_bytes = 8;
  window.socket_send_queue.notsent_bytes = 4;
  diagnostics.Arm(window);

  EXPECT_TRUE(diagnostics.RecordAck(
      40, 25'500'000, 5'000,
      [&](const auto& record) { records.push_back(record); }));

  ASSERT_EQ(records.size(), 1U);
  const OrderLatencyDiagnosticLogRecord& record = records.back();
  EXPECT_EQ(record.owner_thread_tid, 2468);
  EXPECT_EQ(record.order_encode_done_ns, 1'100);
  EXPECT_EQ(record.ws_frame_encode_done_ns, 1'200);
  EXPECT_EQ(record.write_enqueue_ns, 1'300);
  EXPECT_EQ(record.drive_write_enter_ns, 1'350);
  EXPECT_EQ(record.write_some_enter_ns, 1'400);
  EXPECT_EQ(record.write_some_return_ns, 1'450);
  EXPECT_EQ(record.write_complete_ns, 1'500);
  EXPECT_EQ(record.write_some_bytes, 64);
  EXPECT_EQ(record.write_complete_bytes, 64);
  EXPECT_EQ(record.write_errno, 0);
  EXPECT_FALSE(record.write_eagain);
  EXPECT_EQ(record.pending_write_count_after, 0U);
  EXPECT_TRUE(record.socket_send_queue_available);
  EXPECT_EQ(record.tcp_sendq_bytes, 8);
  EXPECT_EQ(record.tcp_notsent_bytes, 4);
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
