#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <quill/CsvWriter.h>

#include "nova/utils/log.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct OrderSessionRttSampleCsvSchema {
  static constexpr char const* header =
      "run_id,connect_ip,order_session_id,connection_generation,round_index,"
      "sample_index,contract,quantity_text,sample_start_ns,sample_end_ns,"
      "gtc_bbo_ticker_id,gtc_bbo_local_ns,gtc_price_text,ioc_bbo_ticker_id,"
      "ioc_bbo_local_ns,ioc_price_text,gtc_place_ack_receive_local_ns,"
      "gtc_place_ack_rtt_ns,gtc_cancel_ack_receive_local_ns,"
      "gtc_cancel_ack_rtt_ns,ioc_place_ack_receive_local_ns,"
      "ioc_place_ack_rtt_ns,gtc_close_submitted,"
      "gtc_close_ack_receive_local_ns,gtc_close_ack_rtt_ns,gtc_close_status,"
      "ioc_close_submitted,ioc_close_ack_receive_local_ns,"
      "ioc_close_ack_rtt_ns,ioc_close_status,gtc_place_status,"
      "gtc_cancel_status,ioc_place_status,unexpected_fill,"
      "invalid_for_rtt_distribution,invalid_reason";
  static constexpr char const* format =
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
      "{},{},{},{},{},{},{},{},{},{},{},{},{}";
};

struct ProbeSampleCsvRow {
  std::string run_id;
  std::string connect_ip;
  std::uint64_t order_session_id{0};
  std::uint64_t connection_generation{0};
  std::uint64_t round_index{0};
  std::uint64_t sample_index{0};
  std::string contract;
  std::string quantity_text;
  std::int64_t sample_start_ns{0};
  std::int64_t sample_end_ns{0};
  std::int64_t gtc_bbo_ticker_id{0};
  std::int64_t gtc_bbo_local_ns{0};
  std::string gtc_price_text;
  std::int64_t ioc_bbo_ticker_id{0};
  std::int64_t ioc_bbo_local_ns{0};
  std::string ioc_price_text;
  std::int64_t gtc_place_ack_receive_local_ns{0};
  std::int64_t gtc_place_ack_rtt_ns{-1};
  std::int64_t gtc_cancel_ack_receive_local_ns{0};
  std::int64_t gtc_cancel_ack_rtt_ns{-1};
  std::int64_t ioc_place_ack_receive_local_ns{0};
  std::int64_t ioc_place_ack_rtt_ns{-1};
  bool gtc_close_submitted{false};
  std::int64_t gtc_close_ack_receive_local_ns{0};
  std::int64_t gtc_close_ack_rtt_ns{-1};
  std::string gtc_close_status;
  bool ioc_close_submitted{false};
  std::int64_t ioc_close_ack_receive_local_ns{0};
  std::int64_t ioc_close_ack_rtt_ns{-1};
  std::string ioc_close_status;
  std::string gtc_place_status;
  std::string gtc_cancel_status;
  std::string ioc_place_status;
  bool unexpected_fill{false};
  bool invalid_for_rtt_distribution{false};
  std::string invalid_reason;
};

class SampleCsvWriter {
 public:
  using Writer = quill::CsvWriter<OrderSessionRttSampleCsvSchema,
                                  nova::LogManager::NovaFrontendOptions>;

  SampleCsvWriter() = default;
  ~SampleCsvWriter() = default;

  SampleCsvWriter(const SampleCsvWriter&) = delete;
  SampleCsvWriter& operator=(const SampleCsvWriter&) = delete;

  [[nodiscard]] bool Open(const std::filesystem::path& path,
                          std::string* error);
  void Write(const ProbeSampleCsvRow& row) noexcept;
  void Close();

 private:
  std::unique_ptr<Writer> writer_;
};

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_
