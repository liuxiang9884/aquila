#include "tools/gate/order_session_rtt_probe/sample_csv_writer.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <string_view>

#include <fmt/core.h>

#include "core/common/order_ack_diagnostic_level.h"

namespace aquila::tools::gate_order_session_rtt_probe {
namespace {

[[nodiscard]] const char* BoolText(bool value) noexcept {
  return value ? "true" : "false";
}

[[nodiscard]] std::string JsonEscape(std::string_view input) {
  std::string escaped;
  escaped.reserve(input.size());
  for (const char ch : input) {
    switch (ch) {
      case '\\':
        escaped.append("\\\\");
        break;
      case '"':
        escaped.append("\\\"");
        break;
      case '\b':
        escaped.append("\\b");
        break;
      case '\f':
        escaped.append("\\f");
        break;
      case '\n':
        escaped.append("\\n");
        break;
      case '\r':
        escaped.append("\\r");
        break;
      case '\t':
        escaped.append("\\t");
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

[[nodiscard]] bool EnsureParentDirectory(const std::filesystem::path& path,
                                         std::string_view description,
                                         std::string* error) {
  try {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = fmt::format("failed to create {} parent for '{}': {}",
                           description, path.string(), ex.what());
    }
    return false;
  }
  return true;
}

}  // namespace

bool WriteRunMetadataJson(const std::filesystem::path& path,
                          const ProbeRunMetadata& metadata,
                          std::string* error) {
  if (!EnsureParentDirectory(path, "RTT run metadata", error)) {
    return false;
  }
  try {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
      if (error != nullptr) {
        *error =
            fmt::format("failed to open RTT run metadata '{}'", path.string());
      }
      return false;
    }
    output << fmt::format(
        "{{\n"
        "  \"sample_csv_schema_version\": {},\n"
        "  \"run_id\": \"{}\",\n"
        "  \"order_ack_diag_level\": {},\n"
        "  \"order_ack_diag_level_name\": \"{}\",\n"
        "  \"tcp_info_compiled\": {},\n"
        "  \"socket_timestamping_compiled\": {},\n"
        "  \"pcap_gate_header_compiled\": {},\n"
        "  \"tcp_info_runtime_requested\": {},\n"
        "  \"socket_timestamping_runtime_requested\": {},\n"
        "  \"sample_csv_path\": \"{}\",\n"
        "  \"connection_observed_csv_path\": \"{}\"\n"
        "}}\n",
        OrderSessionRttSampleCsvSchema::kVersion, JsonEscape(metadata.run_id),
        core::kOrderAckDiagnosticLevel,
        core::OrderAckDiagnosticLevelName(core::kOrderAckDiagnosticLevel),
        BoolText(core::kOrderAckDiagnosticTcpInfoEnabled),
        BoolText(core::kOrderAckDiagnosticSocketTimestampingEnabled),
        BoolText(core::kOrderAckDiagnosticPcapGateHeaderEnabled),
        BoolText(metadata.tcp_info_runtime_requested),
        BoolText(metadata.socket_timestamping_runtime_requested),
        JsonEscape(metadata.sample_csv_path),
        JsonEscape(metadata.connection_observed_csv_path));
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = fmt::format("failed to write RTT run metadata '{}': {}",
                           path.string(), ex.what());
    }
    return false;
  }
  return true;
}

bool SampleCsvWriter::Open(const std::filesystem::path& path,
                           std::string* error) {
  try {
    if (!EnsureParentDirectory(path, "RTT sample CSV", error)) {
      writer_.reset();
      return false;
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
      row.ack_diagnostic_available, row.ack_diagnostic_reason,
      row.send_to_first_after_hook_ns, row.send_to_first_drive_read_ns,
      row.drive_read_duration_ns, row.max_observed_drive_read_duration_ns,
      row.inflight_at_send, row.max_runtime_loop_gap_ns,
      row.runtime_loop_iterations_before_ack, row.owner_thread_tid,
      row.order_encode_done_ns, row.ws_frame_encode_done_ns,
      row.write_enqueue_ns, row.drive_write_enter_ns, row.write_some_enter_ns,
      row.write_some_return_ns, row.write_complete_ns, row.write_some_bytes,
      row.write_complete_bytes, row.write_errno, row.write_eagain,
      row.pending_write_count_after, row.socket_send_queue_available,
      row.tcp_sendq_bytes, row.tcp_notsent_bytes, row.tcp_info_requested,
      row.tcp_info_available, row.tcp_info_rtt_us, row.tcp_info_rttvar_us,
      row.tcp_info_retrans, row.tcp_info_total_retrans, row.tcp_info_unacked,
      row.tcp_info_snd_cwnd, row.ts_available, row.ts_write_complete_ns,
      row.ts_tx_sched_ns, row.ts_tx_software_ns, row.ts_tx_ack_ns,
      row.ts_rx_software_ns, row.ts_write_to_tx_software_ns,
      row.ts_tx_software_to_tx_ack_ns, row.ts_tx_ack_to_rx_software_ns,
      row.ts_rx_software_to_ack_receive_ns, row.response_receive_local_ns,
      row.response_exchange_ns, row.response_exchange_to_local_ns,
      row.response_rtt_ns, row.status, row.terminal_feedback_kind,
      row.unexpected_fill, row.invalid_for_rtt_distribution,
      row.invalid_reason);
}

void SampleCsvWriter::Close() {
  if (writer_ != nullptr) {
    writer_->flush();
  }
  writer_.reset();
}

bool ConnectionObservedCsvWriter::Open(const std::filesystem::path& path,
                                       std::string* error) {
  try {
    if (!EnsureParentDirectory(path, "RTT connection observed CSV", error)) {
      writer_.reset();
      return false;
    }
    writer_ = std::make_unique<Writer>(path.string());
  } catch (const std::exception& ex) {
    writer_.reset();
    if (error != nullptr) {
      *error =
          fmt::format("failed to open RTT connection observed CSV '{}': {}",
                      path.string(), ex.what());
    }
    return false;
  }
  return true;
}

void ConnectionObservedCsvWriter::Write(
    const ProbeConnectionObservedCsvRow& row) noexcept {
  if (writer_ == nullptr) {
    return;
  }
  writer_->append_row(row.run_id, row.session_name, row.group, row.connect_ip,
                      row.order_session_id, row.connected_at_ns,
                      row.endpoint_available, row.local_ip, row.local_port,
                      row.remote_ip, row.remote_port, row.owner_thread_cpu,
                      row.owner_thread_tid);
}

void ConnectionObservedCsvWriter::Close() {
  if (writer_ != nullptr) {
    writer_->flush();
  }
  writer_.reset();
}

}  // namespace aquila::tools::gate_order_session_rtt_probe
