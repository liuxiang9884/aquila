#ifndef AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_EVIDENCE_WRITER_H_
#define AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_EVIDENCE_WRITER_H_

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/common/result.h"

namespace aquila::tools::bitget::gateway_smoke {

struct EvidenceEventRow {
  std::string run_id;
  std::string event_source;
  std::string event_kind;
  std::string order_role;
  std::uint64_t local_order_id{0};
  std::uint64_t group_id{0};
  std::uint16_t route_id{0};
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

struct SmokeSummary {
  std::string run_id;
  std::string final_result;
  std::string failure_reason{"none"};
  std::uint64_t entry_local_order_id{0};
  bool entry_acked{false};
  bool entry_terminal{false};
  double entry_filled_quantity{0.0};
  bool close_required{false};
  std::uint64_t close_local_order_id{0};
  bool close_acked{false};
  bool close_terminal{false};
  double close_filled_quantity{0.0};
};

class EvidenceWriter {
 public:
  explicit EvidenceWriter(std::filesystem::path run_dir);
  EvidenceWriter(const EvidenceWriter&) = delete;
  EvidenceWriter& operator=(const EvidenceWriter&) = delete;
  ~EvidenceWriter();

  [[nodiscard]] Result<bool> Open();
  void Close();
  void WriteEvent(const EvidenceEventRow& row);
  [[nodiscard]] Result<bool> WriteSummary(const SmokeSummary& summary) const;

 private:
  std::filesystem::path run_dir_;
  std::FILE* event_file_{nullptr};
};

}  // namespace aquila::tools::bitget::gateway_smoke

#endif  // AQUILA_TOOLS_BITGET_GATEWAY_SMOKE_EVIDENCE_WRITER_H_
