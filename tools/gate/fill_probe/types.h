#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_TYPES_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_TYPES_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace aquila::tools::gate::fill_probe {

enum class TriggerMode : std::uint8_t {
  kGateDirect,
  kBinanceTriggerGateQuote,
};

struct ExchangeMarketDataConfig {
  std::string shm_name;
  std::string channel_name{"book_ticker_channel"};
};

struct MarketDataConfig {
  ExchangeMarketDataConfig gate;
  ExchangeMarketDataConfig binance;
};

struct ProbeConfig {
  std::string name;
  std::string symbol;
  std::string exchange_symbol;
  std::int32_t symbol_id{0};
  std::uint8_t strategy_id{0};
  TriggerMode trigger_mode{TriggerMode::kGateDirect};
  std::uint64_t max_nodes{1000};
  std::uint64_t duration_ms{1800000};
  std::uint64_t node_pause_ms{1000};
  std::uint64_t gtc_cancel_after_ms{1000};
  std::uint64_t unresolved_timeout_ms{30000};
  double max_entry_notional_usdt{10.0};
  std::uint32_t max_close_retries{3};
  std::uint32_t close_slippage_bps{100};
  std::int64_t max_local_freshness_ns{1000000};
  std::int64_t max_exchange_freshness_ns{2000000};
  std::int64_t max_binance_freshness_ns{2000000};
  std::int64_t max_gate_freshness_ns{50000000};
};

struct ProbeOrderGatewayConfig {
  std::string shm_name;
  std::uint16_t route_count{2};
  std::uint32_t command_queue_capacity{4096};
  std::uint32_t event_queue_capacity{8192};
  std::uint32_t startup_ready_timeout_s{30};
  std::uint16_t gtc_route_id{0};
  std::uint16_t ioc_route_id{1};
};

struct FeedbackConfig {
  std::string shm_name;
  std::string channel_name{"orders"};
  bool force_claim{true};
};

struct OutputConfig {
  std::filesystem::path run_dir;
};

struct FillProbeConfig {
  ProbeConfig probe;
  std::filesystem::path instrument_catalog_file;
  MarketDataConfig market_data;
  ProbeOrderGatewayConfig order_gateway;
  FeedbackConfig feedback;
  OutputConfig output;
};

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_TYPES_H_
