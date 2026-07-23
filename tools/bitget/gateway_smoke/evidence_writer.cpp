#include "tools/bitget/gateway_smoke/evidence_writer.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace aquila::tools::bitget::gateway_smoke {
namespace {

[[nodiscard]] Result<bool> Failure(std::string error) {
  Result<bool> result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] Result<bool> Success() {
  Result<bool> result;
  result.value = true;
  result.ok = true;
  return result;
}

[[nodiscard]] std::string EscapeCsv(std::string_view text) {
  if (text.find_first_of(",\"\n\r") == std::string_view::npos) {
    return std::string{text};
  }
  std::string output;
  output.reserve(text.size() + 2);
  output.push_back('"');
  for (const char ch : text) {
    if (ch == '"') {
      output.push_back('"');
    }
    output.push_back(ch);
  }
  output.push_back('"');
  return output;
}

[[nodiscard]] std::string EscapeJson(std::string_view text) {
  std::string output;
  output.reserve(text.size());
  for (const char ch : text) {
    switch (ch) {
      case '\\':
        output.append("\\\\");
        break;
      case '"':
        output.append("\\\"");
        break;
      case '\n':
        output.append("\\n");
        break;
      case '\r':
        output.append("\\r");
        break;
      case '\t':
        output.append("\\t");
        break;
      default:
        output.push_back(ch);
        break;
    }
  }
  return output;
}

[[nodiscard]] const char* JsonBool(bool value) noexcept {
  return value ? "true" : "false";
}

}  // namespace

EvidenceWriter::EvidenceWriter(std::filesystem::path run_dir)
    : run_dir_(std::move(run_dir)) {}

EvidenceWriter::~EvidenceWriter() {
  Close();
}

Result<bool> EvidenceWriter::Open() {
  Close();
  std::error_code error;
  std::filesystem::create_directories(run_dir_, error);
  if (error) {
    return Failure(fmt::format("failed to create {}: {}", run_dir_.string(),
                               error.message()));
  }
  const std::filesystem::path path = run_dir_ / "order_event.csv";
  event_file_ = std::fopen(path.string().c_str(), "w");
  if (event_file_ == nullptr) {
    return Failure(fmt::format("failed to open {}: {}", path.string(),
                               std::strerror(errno)));
  }
  fmt::print(event_file_,
             "run_id,event_source,event_kind,order_role,local_order_id,"
             "group_id,route_id,response_kind,feedback_kind,"
             "exchange_order_id,exchange_ns,local_ns,price,quantity,"
             "cumulative_filled_quantity,left_quantity,finish_reason,"
             "reject_reason\n");
  std::fflush(event_file_);
  return Success();
}

void EvidenceWriter::Close() {
  if (event_file_ != nullptr) {
    std::fclose(event_file_);
    event_file_ = nullptr;
  }
}

void EvidenceWriter::WriteEvent(const EvidenceEventRow& row) {
  if (event_file_ == nullptr) {
    return;
  }
  fmt::print(event_file_,
             "{},{},{},{},{},{},{},{},{},{},{},{},{},{:.12g},{:.12g},"
             "{:.12g},{},{}\n",
             EscapeCsv(row.run_id), EscapeCsv(row.event_source),
             EscapeCsv(row.event_kind), EscapeCsv(row.order_role),
             row.local_order_id, row.group_id, row.route_id,
             EscapeCsv(row.response_kind), EscapeCsv(row.feedback_kind),
             row.exchange_order_id, row.exchange_ns, row.local_ns,
             EscapeCsv(row.price), row.quantity, row.cumulative_filled_quantity,
             row.left_quantity, EscapeCsv(row.finish_reason),
             EscapeCsv(row.reject_reason));
  std::fflush(event_file_);
}

Result<bool> EvidenceWriter::WriteSummary(const SmokeSummary& summary) const {
  const std::filesystem::path temporary = run_dir_ / "summary.json.tmp";
  const std::filesystem::path output = run_dir_ / "summary.json";
  std::FILE* file = std::fopen(temporary.string().c_str(), "w");
  if (file == nullptr) {
    return Failure(fmt::format("failed to open {}: {}", temporary.string(),
                               std::strerror(errno)));
  }
  fmt::print(file,
             "{{\n"
             "  \"run_id\": \"{}\",\n"
             "  \"final_result\": \"{}\",\n"
             "  \"failure_reason\": \"{}\",\n"
             "  \"entry_local_order_id\": {},\n"
             "  \"entry_acked\": {},\n"
             "  \"entry_terminal\": {},\n"
             "  \"entry_filled_quantity\": {:.12g},\n"
             "  \"close_required\": {},\n"
             "  \"close_local_order_id\": {},\n"
             "  \"close_acked\": {},\n"
             "  \"close_terminal\": {},\n"
             "  \"close_filled_quantity\": {:.12g}\n"
             "}}\n",
             EscapeJson(summary.run_id), EscapeJson(summary.final_result),
             EscapeJson(summary.failure_reason), summary.entry_local_order_id,
             JsonBool(summary.entry_acked), JsonBool(summary.entry_terminal),
             summary.entry_filled_quantity, JsonBool(summary.close_required),
             summary.close_local_order_id, JsonBool(summary.close_acked),
             JsonBool(summary.close_terminal), summary.close_filled_quantity);
  if (std::fclose(file) != 0) {
    std::filesystem::remove(temporary);
    return Failure(fmt::format("failed to flush {}", temporary.string()));
  }
  std::error_code error;
  std::filesystem::rename(temporary, output, error);
  if (error) {
    std::filesystem::remove(temporary);
    return Failure(fmt::format("failed to replace {}: {}", output.string(),
                               error.message()));
  }
  return Success();
}

}  // namespace aquila::tools::bitget::gateway_smoke
