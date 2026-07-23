#include "tools/gate/fill_probe/csv_writer.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace aquila::tools::gate::fill_probe {
namespace {

[[nodiscard]] CsvOpenResult Failure(std::string error) {
  CsvOpenResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] CsvOpenResult Success() {
  CsvOpenResult result;
  result.value = true;
  result.ok = true;
  return result;
}

[[nodiscard]] std::string EscapeCsv(std::string_view text) {
  if (text.find_first_of(",\"\n\r") == std::string_view::npos) {
    return std::string{text};
  }
  std::string escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back('"');
  for (const char ch : text) {
    if (ch == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

[[nodiscard]] std::FILE* OpenFile(const std::filesystem::path& path,
                                  std::string* error) {
  std::FILE* file = std::fopen(path.string().c_str(), "w");
  if (file == nullptr) {
    *error = fmt::format("failed to open {}: {}", path.string(),
                         std::strerror(errno));
  }
  return file;
}

void CloseFile(std::FILE** file) {
  if (*file != nullptr) {
    std::fclose(*file);
    *file = nullptr;
  }
}

}  // namespace

CsvWriters::CsvWriters(std::filesystem::path run_dir)
    : run_dir_(std::move(run_dir)) {}

CsvWriters::~CsvWriters() {
  Close();
}

CsvOpenResult CsvWriters::Open() {
  Close();
  std::error_code ec;
  std::filesystem::create_directories(run_dir_, ec);
  if (ec) {
    return Failure(fmt::format("failed to create {}: {}", run_dir_.string(),
                               ec.message()));
  }

  std::string error;
  node_file_ = OpenFile(run_dir_ / "node.csv", &error);
  if (node_file_ == nullptr) {
    return Failure(std::move(error));
  }
  lifecycle_file_ = OpenFile(run_dir_ / "lifecycle.csv", &error);
  if (lifecycle_file_ == nullptr) {
    Close();
    return Failure(std::move(error));
  }
  order_event_file_ = OpenFile(run_dir_ / "order_event.csv", &error);
  if (order_event_file_ == nullptr) {
    Close();
    return Failure(std::move(error));
  }

  fmt::print(node_file_,
             "run_id,node_id,side,trigger_mode,binance_bbo_id,"
             "binance_exchange_ns,binance_local_ns,gate_bbo_id,"
             "gate_exchange_ns,gate_local_ns,bbo_id,bbo_exchange_ns,"
             "bbo_local_ns,decision_ns,submit_ns,finish_ns,"
             "local_freshness_ns,exchange_freshness_ns,"
             "binance_freshness_ns,gate_freshness_ns,"
             "gate_exchange_delta_ns,gate_local_delta_ns,trigger_to_send_ns,"
             "bid_price,bid_volume,ask_price,ask_volume,entry_quantity,"
             "entry_notional_usdt,status,skip_reason,unresolved_reason\n");
  fmt::print(lifecycle_file_,
             "run_id,node_id,entry_kind,entry_route_id,"
             "entry_local_order_id,entry_side,entry_tif,entry_price,"
             "entry_quantity,entry_submit_ns,entry_ack_ns,entry_finish_ns,"
             "entry_result,entry_filled_qty,entry_avg_fill_price,"
             "close_route_id,close_attempts,close_filled_qty,"
             "close_avg_fill_price,close_attribution,pnl_usdt,fee_usdt\n");
  fmt::print(order_event_file_,
             "run_id,node_id,lifecycle_kind,order_role,local_order_id,"
             "group_id,route_id,event_kind,response_kind,"
             "feedback_kind,"
             "exchange_order_id,exchange_ns,local_ns,price,quantity,"
             "cumulative_filled_quantity,left_quantity,finish_reason,"
             "reject_reason\n");
  return Success();
}

void CsvWriters::Close() {
  CloseFile(&node_file_);
  CloseFile(&lifecycle_file_);
  CloseFile(&order_event_file_);
}

void CsvWriters::WriteNode(const NodeCsvRow& row) {
  if (node_file_ == nullptr) {
    return;
  }
  fmt::print(node_file_,
             "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
             "{},{},{},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},"
             "{},{},{}\n",
             EscapeCsv(row.run_id), row.node_id, EscapeCsv(row.side),
             EscapeCsv(row.trigger_mode), row.binance_bbo_id,
             row.binance_exchange_ns, row.binance_local_ns, row.gate_bbo_id,
             row.gate_exchange_ns, row.gate_local_ns, row.bbo_id,
             row.bbo_exchange_ns, row.bbo_local_ns, row.decision_ns,
             row.submit_ns, row.finish_ns, row.local_freshness_ns,
             row.exchange_freshness_ns, row.binance_freshness_ns,
             row.gate_freshness_ns, row.gate_exchange_delta_ns,
             row.gate_local_delta_ns, row.trigger_to_send_ns, row.bid_price,
             row.bid_volume, row.ask_price, row.ask_volume, row.entry_quantity,
             row.entry_notional_usdt, EscapeCsv(row.status),
             EscapeCsv(row.skip_reason), EscapeCsv(row.unresolved_reason));
  std::fflush(node_file_);
}

void CsvWriters::WriteLifecycle(const LifecycleCsvRow& row) {
  if (lifecycle_file_ == nullptr) {
    return;
  }
  fmt::print(lifecycle_file_,
             "{},{},{},{},{},{},{},{},{},{},{},{},{},{:.12g},{:.12g},{},"
             "{},{:.12g},{:.12g},{},{:.12g},{:.12g}\n",
             EscapeCsv(row.run_id), row.node_id, EscapeCsv(row.entry_kind),
             row.entry_route_id, row.entry_local_order_id,
             EscapeCsv(row.entry_side), EscapeCsv(row.entry_tif),
             EscapeCsv(row.entry_price), EscapeCsv(row.entry_quantity),
             row.entry_submit_ns, row.entry_ack_ns, row.entry_finish_ns,
             EscapeCsv(row.entry_result), row.entry_filled_qty,
             row.entry_avg_fill_price, row.close_route_id, row.close_attempts,
             row.close_filled_qty, row.close_avg_fill_price,
             EscapeCsv(row.close_attribution), row.pnl_usdt, row.fee_usdt);
  std::fflush(lifecycle_file_);
}

void CsvWriters::WriteOrderEvent(const OrderEventCsvRow& row) {
  if (order_event_file_ == nullptr) {
    return;
  }
  fmt::print(order_event_file_,
             "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{:.12g},{:.12g},"
             "{:.12g},{},{}\n",
             EscapeCsv(row.run_id), row.node_id, EscapeCsv(row.lifecycle_kind),
             EscapeCsv(row.order_role), row.local_order_id, row.group_id,
             row.route_id, EscapeCsv(row.event_kind),
             EscapeCsv(row.response_kind), EscapeCsv(row.feedback_kind),
             row.exchange_order_id, row.exchange_ns, row.local_ns,
             EscapeCsv(row.price), row.quantity, row.cumulative_filled_quantity,
             row.left_quantity, EscapeCsv(row.finish_reason),
             EscapeCsv(row.reject_reason));
  std::fflush(order_event_file_);
}

}  // namespace aquila::tools::gate::fill_probe
