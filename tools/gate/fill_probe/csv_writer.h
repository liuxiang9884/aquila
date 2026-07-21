#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_CSV_WRITER_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_CSV_WRITER_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/common/result.h"

namespace aquila::tools::gate::fill_probe {

struct NodeCsvRow {
  std::string run_id;
  std::uint64_t node_id{0};
  std::string side;
  std::string trigger_mode;
  std::uint64_t binance_bbo_id{0};
  std::int64_t binance_exchange_ns{0};
  std::int64_t binance_local_ns{0};
  std::uint64_t gate_bbo_id{0};
  std::int64_t gate_exchange_ns{0};
  std::int64_t gate_local_ns{0};
  std::uint64_t bbo_id{0};
  std::int64_t bbo_exchange_ns{0};
  std::int64_t bbo_local_ns{0};
  std::int64_t decision_ns{0};
  std::int64_t submit_ns{0};
  std::int64_t finish_ns{0};
  std::int64_t local_freshness_ns{0};
  std::int64_t exchange_freshness_ns{0};
  std::int64_t binance_freshness_ns{0};
  std::int64_t gate_freshness_ns{0};
  std::int64_t gate_exchange_delta_ns{0};
  std::int64_t gate_local_delta_ns{0};
  std::int64_t trigger_to_send_ns{0};
  double bid_price{0.0};
  double bid_volume{0.0};
  double ask_price{0.0};
  double ask_volume{0.0};
  double entry_quantity{0.0};
  double entry_notional_usdt{0.0};
  std::string status;
  std::string skip_reason;
  std::string unresolved_reason;
};

struct LifecycleCsvRow {
  std::string run_id;
  std::uint64_t node_id{0};
  std::string entry_kind;
  std::uint16_t entry_route_id{0};
  std::uint64_t entry_local_order_id{0};
  std::string entry_side;
  std::string entry_tif;
  std::string entry_price;
  std::string entry_quantity;
  std::int64_t entry_submit_ns{0};
  std::int64_t entry_ack_ns{0};
  std::int64_t entry_finish_ns{0};
  std::string entry_result;
  double entry_filled_qty{0.0};
  double entry_avg_fill_price{0.0};
  std::uint16_t close_route_id{0};
  std::uint32_t close_attempts{0};
  double close_filled_qty{0.0};
  double close_avg_fill_price{0.0};
  std::string close_attribution;
  double pnl_usdt{0.0};
  double fee_usdt{0.0};
};

struct OrderEventCsvRow {
  std::string run_id;
  std::uint64_t node_id{0};
  std::string lifecycle_kind;
  std::string order_role;
  std::uint64_t local_order_id{0};
  std::uint64_t parent_id{0};
  std::uint64_t group_id{0};
  std::uint16_t route_id{0};
  std::string event_kind;
  std::string response_kind;
  std::string feedback_kind;
  std::uint64_t exchange_order_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t local_ns{0};
  std::string price;
  double quantity{0.0};
  double cumulative_filled_quantity{0.0};
  double left_quantity{0.0};
  std::string finish_reason;
  std::string reject_reason;
};

using CsvOpenResult = Result<bool>;

class CsvWriters {
 public:
  explicit CsvWriters(std::filesystem::path run_dir);
  CsvWriters(const CsvWriters&) = delete;
  CsvWriters& operator=(const CsvWriters&) = delete;
  CsvWriters(CsvWriters&&) = delete;
  CsvWriters& operator=(CsvWriters&&) = delete;
  ~CsvWriters();

  [[nodiscard]] CsvOpenResult Open();
  void Close();

  void WriteNode(const NodeCsvRow& row);
  void WriteLifecycle(const LifecycleCsvRow& row);
  void WriteOrderEvent(const OrderEventCsvRow& row);

 private:
  std::filesystem::path run_dir_;
  std::FILE* node_file_{nullptr};
  std::FILE* lifecycle_file_{nullptr};
  std::FILE* order_event_file_{nullptr};
};

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_CSV_WRITER_H_
