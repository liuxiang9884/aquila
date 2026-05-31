#include "tools/gate/order_session_rtt_probe/sample_csv_writer.h"

#include <exception>
#include <filesystem>

#include <fmt/core.h>

namespace aquila::tools::gate_order_session_rtt_probe {

bool SampleCsvWriter::Open(const std::filesystem::path& path,
                           std::string* error) {
  try {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    writer_ = std::make_unique<Writer>(path.string());
  } catch (const std::exception& ex) {
    writer_.reset();
    if (error != nullptr) {
      *error = fmt::format("failed to open RTT sample CSV '{}': {}",
                           path.string(), ex.what());
    }
    return false;
  }
  return true;
}

void SampleCsvWriter::Write(const ProbeSampleCsvRow& row) noexcept {
  if (writer_ == nullptr) {
    return;
  }
  writer_->append_row(
      row.run_id, row.session_name, row.group, row.connect_ip,
      row.order_session_id, row.round_index, row.sample_index, row.contract,
      row.quantity_text, row.price_text, row.probe_order_type, row.order_action,
      row.local_order_id, row.request_sequence, row.bbo_ticker_id,
      row.bbo_local_ns, row.request_send_local_ns, row.ack_receive_local_ns,
      row.ack_exchange_ns, row.ack_exchange_to_local_ns, row.ack_rtt_ns,
      row.response_receive_local_ns, row.response_exchange_ns,
      row.response_exchange_to_local_ns, row.response_rtt_ns, row.status,
      row.terminal_feedback_kind, row.unexpected_fill,
      row.invalid_for_rtt_distribution, row.invalid_reason);
}

void SampleCsvWriter::Close() {
  writer_.reset();
}

}  // namespace aquila::tools::gate_order_session_rtt_probe
