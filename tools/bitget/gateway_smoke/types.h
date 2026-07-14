#ifndef AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_TYPES_H_
#define AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_TYPES_H_

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/common/types.h"

namespace aquila::tools::bitget::gateway_smoke {

struct MarketDataConfig {
  std::string shm_name;
  std::string channel_name{"book_ticker_channel"};
};

struct OrderGatewayConfig {
  std::string shm_name;
  std::uint16_t route_count{1};
  std::uint32_t command_queue_capacity{4096};
  std::uint32_t event_queue_capacity{8192};
  std::uint32_t startup_ready_timeout_s{30};
};

struct FeedbackConfig {
  std::string shm_name;
  std::string channel_name{"orders"};
  bool force_claim{false};
  std::uint32_t poll_budget{64};
};

struct GatewaySmokeConfig {
  std::string name;
  std::string run_id;
  std::string symbol;
  std::string exchange_symbol;
  std::int32_t symbol_id{0};
  std::uint8_t strategy_id{0};
  OrderSide side{OrderSide::kBuy};
  double quantity{0.0};
  double passive_price_limit_fraction{0.5};
  std::uint32_t close_slippage_bps{100};
  std::uint64_t bbo_freshness_ns{1'000'000'000};
  std::uint32_t ack_timeout_ms{5'000};
  std::uint32_t terminal_timeout_ms{10'000};
  std::uint16_t route_id{0};
  std::filesystem::path instrument_catalog_file;
  MarketDataConfig market_data;
  OrderGatewayConfig order_gateway;
  FeedbackConfig feedback;
  std::filesystem::path run_dir;
};

struct WireOrder {
  OrderSide side{OrderSide::kBuy};
  OrderType order_type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kImmediateOrCancel};
  double price{0.0};
  double quantity{0.0};
  std::string price_text;
  std::string quantity_text;
  bool reduce_only{false};
};

enum class SmokeResult : std::uint8_t {
  kPending,
  kNoFill,
  kClosed,
};

enum class SmokeFailure : std::uint8_t {
  kNone,
  kInvalidTransition,
  kGatewayRejected,
  kGatewayUnknown,
  kFeedbackRejected,
  kFeedbackContinuityLost,
  kAckTimeout,
  kTerminalTimeout,
  kCloseResidual,
  kQuantityInvariant,
};

struct LegEvidence {
  std::uint64_t local_order_id{0};
  std::int64_t submit_ns{0};
  std::int64_t ack_ns{0};
  std::int64_t terminal_ns{0};
  double requested_quantity{0.0};
  double cumulative_filled_quantity{0.0};
  bool submitted{false};
  bool acked{false};
  bool terminal{false};
};

}  // namespace aquila::tools::bitget::gateway_smoke

#endif  // AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_TYPES_H_
