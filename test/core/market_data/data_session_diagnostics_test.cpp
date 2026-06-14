#include "core/market_data/data_session_diagnostics.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/data_session_diagnostic_level.h"

namespace {

namespace md = aquila::market_data;

TEST(CoreMarketDataDataSessionDiagnosticsTest, DefaultsToFiveMsAndHighLogCap) {
  const md::DataSessionLatencyOutlierConfig config;

  EXPECT_FALSE(config.enabled);
  EXPECT_EQ(config.threshold_ns, 5'000'000);
  EXPECT_EQ(config.max_logs_per_second, 1000u);
}

TEST(CoreMarketDataDataSessionDiagnosticsTest,
     OutlierPredicateFollowsCompileTimeLevel) {
  const md::DataSessionLatencyOutlierConfig config{
      .enabled = true,
      .source_id = 3,
      .threshold_ns = 5'000'000,
      .max_logs_per_second = 1000,
  };
  const md::DataSessionBookTickerTiming timing{
      .exchange = aquila::Exchange::kGate,
      .source_id = 3,
      .symbol_id = 11,
      .book_ticker_id = 42,
      .exchange_ns = 100'000'000,
      .book_ticker_local_ns = 106'000'001,
  };

  if constexpr (aquila::core::kDataSessionDiagnosticCorrelationEnabled) {
    EXPECT_TRUE(md::IsDataSessionLatencyOutlier(config, timing));
  } else {
    EXPECT_FALSE(md::IsDataSessionLatencyOutlier(config, timing));
  }
}

TEST(CoreMarketDataDataSessionDiagnosticsTest,
     ComputesUserAndKernelBreakdownWhenAvailable) {
  const md::DataSessionBookTickerTiming timing{
      .exchange = aquila::Exchange::kGate,
      .source_id = 3,
      .symbol_id = 11,
      .book_ticker_id = 42,
      .exchange_ns = 1'000,
      .book_ticker_local_ns = 2'500,
      .parse_done_ns = 2'900,
      .shm_publish_done_ns = 3'200,
      .message =
          {
              .available = true,
              .transport_tls = false,
              .kernel_rx_available = true,
              .kernel_rx_ns = 1'900,
              .drive_read_enter_ns = 2'100,
              .read_return_ns = 2'400,
              .handler_entry_ns = 2'600,
              .read_bytes = 128,
          },
  };

  const md::DataSessionLatencyBreakdown breakdown =
      md::ComputeDataSessionLatencyBreakdown(timing);

  EXPECT_EQ(breakdown.network_or_exchange_ns, 900);
  EXPECT_EQ(breakdown.kernel_queue_ns, 200);
  EXPECT_EQ(breakdown.read_syscall_or_tls_ns, 300);
  EXPECT_EQ(breakdown.ws_dispatch_ns, 200);
  EXPECT_EQ(breakdown.parse_ns, 300);
  EXPECT_EQ(breakdown.shm_publish_ns, 300);
  EXPECT_EQ(breakdown.user_after_read_ns, 800);
}

TEST(CoreMarketDataDataSessionDiagnosticsTest,
     KeepsUnavailableKernelBreakdownAtMinusOne) {
  const md::DataSessionBookTickerTiming timing{
      .exchange_ns = 1'000,
      .book_ticker_local_ns = 2'500,
      .parse_done_ns = 2'900,
      .shm_publish_done_ns = 3'200,
      .message =
          {
              .available = true,
              .read_return_ns = 2'400,
              .handler_entry_ns = 2'600,
          },
  };

  const md::DataSessionLatencyBreakdown breakdown =
      md::ComputeDataSessionLatencyBreakdown(timing);

  EXPECT_EQ(breakdown.network_or_exchange_ns, -1);
  EXPECT_EQ(breakdown.kernel_queue_ns, -1);
  EXPECT_EQ(breakdown.read_syscall_or_tls_ns, -1);
  EXPECT_EQ(breakdown.ws_dispatch_ns, 200);
  EXPECT_EQ(breakdown.parse_ns, 300);
  EXPECT_EQ(breakdown.shm_publish_ns, 300);
  EXPECT_EQ(breakdown.user_after_read_ns, 800);
}

}  // namespace
