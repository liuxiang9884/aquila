#include "tools/gate/order_session_rtt_probe/sample_csv_writer.h"

#include <exception>

#include <fmt/core.h>

namespace aquila::tools::gate_order_session_rtt_probe {

bool SampleCsvWriter::Open(const std::filesystem::path& path,
                           std::string* error) {
  try {
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
      row.run_id, row.connect_ip, row.order_session_id,
      row.connection_generation, row.round_index, row.sample_index,
      row.contract, row.quantity_text, row.sample_start_ns, row.sample_end_ns,
      row.gtc_bbo_ticker_id, row.gtc_bbo_local_ns, row.gtc_price_text,
      row.ioc_bbo_ticker_id, row.ioc_bbo_local_ns, row.ioc_price_text,
      row.gtc_place_ack_receive_local_ns, row.gtc_place_ack_rtt_ns,
      row.gtc_cancel_ack_receive_local_ns, row.gtc_cancel_ack_rtt_ns,
      row.ioc_place_ack_receive_local_ns, row.ioc_place_ack_rtt_ns,
      row.gtc_close_submitted, row.gtc_close_ack_receive_local_ns,
      row.gtc_close_ack_rtt_ns, row.gtc_close_status, row.ioc_close_submitted,
      row.ioc_close_ack_receive_local_ns, row.ioc_close_ack_rtt_ns,
      row.ioc_close_status, row.gtc_place_status, row.gtc_cancel_status,
      row.ioc_place_status, row.unexpected_fill,
      row.invalid_for_rtt_distribution, row.invalid_reason);
}

void SampleCsvWriter::Close() {
  writer_.reset();
}

}  // namespace aquila::tools::gate_order_session_rtt_probe
