#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <quill/CsvWriter.h>

#include "nova/utils/log.h"

namespace aquila::tools::gate_order_session_rtt_probe {

struct OrderSessionRttSampleCsvSchema {
  static constexpr char const* header =
      "run,session,group,ip,sid,round,sample,contract,qty,price,type,action,"
      "local_id,"
      "req_seq,bbo_id,bbo_ns,send_ns,ack_recv_ns,ack_ex_ns,"
      "ack_ex2local_ns,ack_rtt_ns,diag,diag_reason,"
      "send_to_after_hook_ns,send_to_drive_read_ns,drive_read_ns,"
      "max_drive_read_ns,inflight_at_send,max_loop_gap_ns,"
      "loop_iters_before_ack,owner_tid,order_encode_done_ns,"
      "ws_frame_encode_done_ns,write_enqueue_ns,drive_write_enter_ns,"
      "write_some_enter_ns,write_some_return_ns,write_complete_ns,"
      "write_some_bytes,write_complete_bytes,write_errno,write_eagain,"
      "pending_write_count_after,socket_sendq_available,tcp_sendq_bytes,"
      "tcp_notsent_bytes,tcp_info_requested,tcp_info_available,"
      "tcp_info_rtt_us,tcp_info_rttvar_us,tcp_info_retrans,"
      "tcp_info_total_retrans,tcp_info_unacked,tcp_info_snd_cwnd,"
      "ts_write_complete_ns,ts_tx_sched_ns,"
      "ts_tx_software_ns,ts_tx_ack_ns,ts_rx_software_ns,"
      "ts_write_to_tx_software_ns,ts_tx_software_to_tx_ack_ns,"
      "ts_tx_ack_to_rx_software_ns,ts_rx_software_to_ack_receive_ns,"
      "resp_recv_ns,resp_ex_ns,"
      "resp_ex2local_ns,resp_rtt_ns,status,term_fb,fill,invalid,"
      "inv_reason";
  static constexpr char const* format =
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
      "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{}";
};

struct ProbeSampleCsvRow {
  std::string run_id;
  std::string session_name;
  std::string group;
  std::string connect_ip;
  std::uint64_t order_session_id{0};
  std::uint64_t round_index{0};
  std::uint64_t sample_index{0};
  std::string contract;
  std::string quantity_text;
  std::string price_text;
  std::string probe_order_type;
  std::string order_action;
  std::uint64_t local_order_id{0};
  std::uint64_t request_sequence{0};
  std::int64_t bbo_ticker_id{0};
  std::int64_t bbo_local_ns{0};
  std::int64_t request_send_local_ns{0};
  std::int64_t ack_receive_local_ns{0};
  std::int64_t ack_exchange_ns{0};
  std::int64_t ack_exchange_to_local_ns{0};
  std::int64_t ack_rtt_ns{-1};
  bool ack_diagnostic_available{false};
  std::string ack_diagnostic_reason;
  std::int64_t send_to_first_after_hook_ns{0};
  std::int64_t send_to_first_drive_read_ns{0};
  std::int64_t drive_read_duration_ns{0};
  std::int64_t max_observed_drive_read_duration_ns{0};
  std::size_t inflight_at_send{0};
  std::int64_t max_runtime_loop_gap_ns{0};
  std::uint64_t runtime_loop_iterations_before_ack{0};
  int owner_thread_tid{-1};
  std::int64_t order_encode_done_ns{0};
  std::int64_t ws_frame_encode_done_ns{0};
  std::int64_t write_enqueue_ns{0};
  std::int64_t drive_write_enter_ns{0};
  std::int64_t write_some_enter_ns{0};
  std::int64_t write_some_return_ns{0};
  std::int64_t write_complete_ns{0};
  std::int64_t write_some_bytes{0};
  std::int64_t write_complete_bytes{0};
  int write_errno{0};
  bool write_eagain{false};
  std::size_t pending_write_count_after{0};
  bool socket_send_queue_available{false};
  std::uint32_t tcp_sendq_bytes{0};
  std::uint32_t tcp_notsent_bytes{0};
  bool tcp_info_requested{false};
  bool tcp_info_available{false};
  std::uint32_t tcp_info_rtt_us{0};
  std::uint32_t tcp_info_rttvar_us{0};
  std::uint32_t tcp_info_retrans{0};
  std::uint32_t tcp_info_total_retrans{0};
  std::uint32_t tcp_info_unacked{0};
  std::uint32_t tcp_info_snd_cwnd{0};
  std::int64_t ts_write_complete_ns{0};
  std::int64_t ts_tx_sched_ns{0};
  std::int64_t ts_tx_software_ns{0};
  std::int64_t ts_tx_ack_ns{0};
  std::int64_t ts_rx_software_ns{0};
  std::int64_t ts_write_to_tx_software_ns{-1};
  std::int64_t ts_tx_software_to_tx_ack_ns{-1};
  std::int64_t ts_tx_ack_to_rx_software_ns{-1};
  std::int64_t ts_rx_software_to_ack_receive_ns{-1};
  std::int64_t response_receive_local_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t response_exchange_to_local_ns{0};
  std::int64_t response_rtt_ns{-1};
  std::string status;
  std::string terminal_feedback_kind;
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
