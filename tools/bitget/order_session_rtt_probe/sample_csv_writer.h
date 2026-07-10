#ifndef AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_
#define AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

#include "tools/bitget/order_session_rtt_probe/config.h"

namespace aquila::tools::bitget_order_session_rtt_probe {

struct SampleCsvSchema {
  static constexpr std::uint32_t kVersion = 1;
  static constexpr char kHeader[] =
      "run_id,session_name,group,host,connect_ip,port,worker_cpu,"
      "owner_thread_cpu,owner_thread_tid,cycle_index,sample_index,"
      "local_order_id,close_local_order_id,request_sequence,"
      "close_request_sequence,exchange_order_id,close_exchange_order_id,"
      "symbol,side,quantity,price,bbo_ticker_id,"
      "bbo_local_ns,request_send_ns,response_receive_ns,"
      "response_exchange_ns,ack_rtt_ns,response_kind,error_code,"
      "connection_id_hash,close_request_send_ns,close_response_receive_ns,"
      "close_response_exchange_ns,close_ack_rtt_ns,close_response_kind,"
      "close_error_code,close_connection_id_hash,terminal_feedback_kind,"
      "terminal_feedback_local_ns,terminal_feedback_exchange_ns,"
      "terminal_finish_reason,cumulative_fill,outcome,invalid,"
      "unexpected_fill,safety_close_requested,safety_close_sent,"
      "safety_close_confirmed,safety_close_filled_quantity,invalid_reason";
};

struct SampleCsvRow {
  std::string run_id;
  std::string session_name;
  std::string group;
  std::string host;
  std::string connect_ip;
  std::string port;
  int worker_cpu{-1};
  int owner_thread_cpu{-1};
  int owner_thread_tid{-1};
  std::uint64_t cycle_index{0};
  std::uint64_t sample_index{0};
  std::uint64_t local_order_id{0};
  std::uint64_t close_local_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint64_t close_request_sequence{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t close_exchange_order_id{0};
  std::string symbol;
  std::string side;
  std::string quantity_text;
  std::string price_text;
  std::int64_t bbo_ticker_id{0};
  std::int64_t bbo_local_ns{0};
  std::int64_t request_send_ns{0};
  std::int64_t response_receive_ns{0};
  std::int64_t response_exchange_ns{0};
  std::int64_t ack_rtt_ns{-1};
  std::string response_kind;
  std::uint32_t error_code{0};
  std::uint64_t connection_id_hash{0};
  std::int64_t close_request_send_ns{0};
  std::int64_t close_response_receive_ns{0};
  std::int64_t close_response_exchange_ns{0};
  std::int64_t close_ack_rtt_ns{-1};
  std::string close_response_kind;
  std::uint32_t close_error_code{0};
  std::uint64_t close_connection_id_hash{0};
  std::string terminal_feedback_kind;
  std::int64_t terminal_feedback_local_ns{0};
  std::int64_t terminal_feedback_exchange_ns{0};
  std::string terminal_finish_reason;
  double cumulative_fill{0.0};
  std::string outcome;
  bool invalid{false};
  bool unexpected_fill{false};
  bool safety_close_requested{false};
  bool safety_close_sent{false};
  bool safety_close_confirmed{false};
  double safety_close_filled_quantity{0.0};
  std::string invalid_reason;
};

class SampleCsvWriter {
 public:
  SampleCsvWriter() = default;
  ~SampleCsvWriter();

  SampleCsvWriter(const SampleCsvWriter&) = delete;
  SampleCsvWriter& operator=(const SampleCsvWriter&) = delete;

  [[nodiscard]] bool Open(const std::filesystem::path& path,
                          std::string* error);
  [[nodiscard]] bool Write(const SampleCsvRow& row, std::string* error);
  void Close() noexcept;

 private:
  std::FILE* file_{nullptr};
};

struct ConnectionObservedCsvSchema {
  static constexpr char kHeader[] =
      "run_id,session_name,group,configured_host,configured_connect_ip,"
      "configured_port,worker_cpu,connected_at_ns,endpoint_available,"
      "local_ip,local_port,remote_ip,remote_port,owner_thread_cpu,"
      "owner_thread_tid";
};

struct ConnectionObservedCsvRow {
  std::string run_id;
  std::string session_name;
  std::string group;
  std::string configured_host;
  std::string configured_connect_ip;
  std::string configured_port;
  int worker_cpu{-1};
  std::int64_t connected_at_ns{0};
  bool endpoint_available{false};
  std::string local_ip;
  std::uint16_t local_port{0};
  std::string remote_ip;
  std::uint16_t remote_port{0};
  int owner_thread_cpu{-1};
  int owner_thread_tid{-1};
};

class ConnectionObservedCsvWriter {
 public:
  ConnectionObservedCsvWriter() = default;
  ~ConnectionObservedCsvWriter();

  ConnectionObservedCsvWriter(const ConnectionObservedCsvWriter&) = delete;
  ConnectionObservedCsvWriter& operator=(const ConnectionObservedCsvWriter&) =
      delete;

  [[nodiscard]] bool Open(const std::filesystem::path& path,
                          std::string* error);
  [[nodiscard]] bool Write(const ConnectionObservedCsvRow& row,
                           std::string* error);
  void Close() noexcept;

 private:
  std::FILE* file_{nullptr};
};

[[nodiscard]] bool WriteRunMetadata(
    const std::filesystem::path& path, const ProbeConfig& config,
    std::size_t session_count, std::string_view sample_csv_path,
    std::string_view connection_observed_csv_path, std::string* error);

}  // namespace aquila::tools::bitget_order_session_rtt_probe

#endif  // AQUILA_TOOLS_BITGET_ORDER_SESSION_RTT_PROBE_SAMPLE_CSV_WRITER_H_
