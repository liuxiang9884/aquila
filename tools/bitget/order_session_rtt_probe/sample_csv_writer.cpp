#include "tools/bitget/order_session_rtt_probe/sample_csv_writer.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace aquila::tools::bitget_order_session_rtt_probe {
namespace {

[[nodiscard]] std::string CsvEscape(std::string_view value) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string(value);
  }
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      escaped.push_back('"');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('"');
  return escaped;
}

[[nodiscard]] std::string JsonEscape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        escaped.append("\\\\");
        break;
      case '"':
        escaped.append("\\\"");
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
                                         std::string* error) {
  try {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    return true;
  } catch (const std::exception& ex) {
    if (error != nullptr) {
      *error = fmt::format("failed to create parent directory for '{}': {}",
                           path.string(), ex.what());
    }
    return false;
  }
}

[[nodiscard]] bool WriteText(std::FILE* file, std::string_view text,
                             std::string_view description, std::string* error) {
  if (file == nullptr) {
    if (error != nullptr) {
      *error = fmt::format("{} is not open", description);
    }
    return false;
  }
  if (std::fwrite(text.data(), 1, text.size(), file) != text.size()) {
    if (error != nullptr) {
      *error = fmt::format("failed to write {}: {}", description,
                           std::strerror(errno));
    }
    return false;
  }
  return true;
}

[[nodiscard]] bool OpenCsv(const std::filesystem::path& path,
                           std::string_view header, std::FILE** file,
                           std::string* error) {
  if (!EnsureParentDirectory(path, error)) {
    return false;
  }
  *file = std::fopen(path.c_str(), "wb");
  if (*file == nullptr) {
    if (error != nullptr) {
      *error = fmt::format("failed to open '{}': {}", path.string(),
                           std::strerror(errno));
    }
    return false;
  }
  const std::string header_line = fmt::format("{}\n", header);
  if (!WriteText(*file, header_line, "CSV header", error)) {
    std::fclose(*file);
    *file = nullptr;
    return false;
  }
  return true;
}

}  // namespace

SampleCsvWriter::~SampleCsvWriter() {
  Close();
}

bool SampleCsvWriter::Open(const std::filesystem::path& path,
                           std::string* error) {
  Close();
  return OpenCsv(path, SampleCsvSchema::kHeader, &file_, error);
}

bool SampleCsvWriter::Write(const SampleCsvRow& row, std::string* error) {
  const std::string line = fmt::format(
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n",
      CsvEscape(row.run_id), CsvEscape(row.session_name), CsvEscape(row.group),
      CsvEscape(row.host), CsvEscape(row.connect_ip), CsvEscape(row.port),
      row.worker_cpu, row.owner_thread_cpu, row.owner_thread_tid,
      row.cycle_index, row.sample_index, row.local_order_id,
      row.close_local_order_id, row.request_sequence, row.exchange_order_id,
      CsvEscape(row.symbol), CsvEscape(row.side), CsvEscape(row.quantity_text),
      CsvEscape(row.price_text), row.bbo_ticker_id, row.bbo_local_ns,
      row.request_send_ns, row.response_receive_ns, row.response_exchange_ns,
      row.ack_rtt_ns, CsvEscape(row.response_kind), row.error_code,
      row.connection_id_hash, CsvEscape(row.terminal_feedback_kind),
      row.terminal_feedback_local_ns, row.terminal_feedback_exchange_ns,
      CsvEscape(row.terminal_finish_reason), row.cumulative_fill,
      CsvEscape(row.outcome), row.invalid, row.unexpected_fill,
      row.safety_close_requested, row.safety_close_sent,
      row.safety_close_confirmed, row.safety_close_filled_quantity,
      CsvEscape(row.invalid_reason));
  return WriteText(file_, line, "Bitget RTT sample CSV", error);
}

void SampleCsvWriter::Close() noexcept {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

ConnectionObservedCsvWriter::~ConnectionObservedCsvWriter() {
  Close();
}

bool ConnectionObservedCsvWriter::Open(const std::filesystem::path& path,
                                       std::string* error) {
  Close();
  return OpenCsv(path, ConnectionObservedCsvSchema::kHeader, &file_, error);
}

bool ConnectionObservedCsvWriter::Write(const ConnectionObservedCsvRow& row,
                                        std::string* error) {
  const std::string line = fmt::format(
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}\n", CsvEscape(row.run_id),
      CsvEscape(row.session_name), CsvEscape(row.group),
      CsvEscape(row.configured_host), CsvEscape(row.configured_connect_ip),
      CsvEscape(row.configured_port), row.worker_cpu, row.connected_at_ns,
      row.endpoint_available, CsvEscape(row.local_ip), row.local_port,
      CsvEscape(row.remote_ip), row.remote_port, row.owner_thread_cpu,
      row.owner_thread_tid);
  return WriteText(file_, line, "Bitget RTT connection CSV", error);
}

void ConnectionObservedCsvWriter::Close() noexcept {
  if (file_ != nullptr) {
    std::fflush(file_);
    std::fclose(file_);
    file_ = nullptr;
  }
}

bool WriteRunMetadata(const std::filesystem::path& path,
                      const ProbeConfig& config, std::size_t session_count,
                      std::string_view sample_csv_path,
                      std::string_view connection_observed_csv_path,
                      std::string* error) {
  if (!EnsureParentDirectory(path, error)) {
    return false;
  }
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    if (error != nullptr) {
      *error = fmt::format("failed to open '{}': {}", path.string(),
                           std::strerror(errno));
    }
    return false;
  }
  const std::string metadata = fmt::format(
      "{{\n"
      "  \"sample_csv_schema_version\": {},\n"
      "  \"run_id\": \"{}\",\n"
      "  \"probe_name\": \"{}\",\n"
      "  \"session_count\": {},\n"
      "  \"samples_per_session\": {},\n"
      "  \"feedback_strategy_id\": {},\n"
      "  \"order_mode\": \"{}\",\n"
      "  \"rest_guard_implemented\": false,\n"
      "  \"sample_csv_path\": \"{}\",\n"
      "  \"connection_observed_csv_path\": \"{}\"\n"
      "}}\n",
      SampleCsvSchema::kVersion, JsonEscape(config.run_id),
      JsonEscape(config.name), session_count,
      config.sampling.samples_per_session, config.feedback.strategy_id,
      JsonEscape(config.order.order_mode), JsonEscape(sample_csv_path),
      JsonEscape(connection_observed_csv_path));
  const bool ok = WriteText(file, metadata, "Bitget RTT run metadata", error);
  if (std::fclose(file) != 0 && ok) {
    if (error != nullptr) {
      *error = fmt::format("failed to close '{}': {}", path.string(),
                           std::strerror(errno));
    }
    return false;
  }
  return ok;
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe
