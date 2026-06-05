#ifndef AQUILA_STRATEGY_LEAD_LAG_SIGNAL_CSV_WRITER_H_
#define AQUILA_STRATEGY_LEAD_LAG_SIGNAL_CSV_WRITER_H_

#include <filesystem>
#include <memory>
#include <string>

#include <quill/CsvWriter.h>

#include "core/market_data/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/signal.h"
#include "strategy/lead_lag/strategy.h"

namespace aquila::strategy::leadlag {

struct SignalCsvSchema {
  static constexpr char const* header =
      "symbol_id,exchange,role,exchange_ns,local_ns,event_ns,"
      "price_changed,action,side,raw_price,reduce_only,lead_exchange_ns,"
      "lead_raw_bid,lead_raw_ask,lead_drifted_event_ns,lead_drifted_bid,"
      "lead_drifted_ask,lag_exchange_ns,lag_bid,lag_ask,drift_mean,"
      "drift_ready,drift_deviation,up_entry,down_entry,up_exit,down_exit,"
      "lag_spread_mean,lead_noise,lag_noise,active_group_count,"
      "group_id,position_direction,trailing_price";
  static constexpr char const* format =
      "{},{},{},{},{},{},{},{},{},{:.12g},{},{},{:.12g},{:.12g},{},"
      "{:.12g},{:.12g},{},{:.12g},{:.12g},{:.12g},{},{:.12g},{:.12g},"
      "{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{},{},{},{:.12g}";
};

class SignalCsvWriter {
 public:
  using Writer =
      quill::CsvWriter<SignalCsvSchema, nova::LogManager::NovaFrontendOptions>;

  SignalCsvWriter() = default;
  ~SignalCsvWriter() = default;

  SignalCsvWriter(const SignalCsvWriter&) = delete;
  SignalCsvWriter& operator=(const SignalCsvWriter&) = delete;

  [[nodiscard]] bool Open(const std::filesystem::path& path,
                          std::string* error);
  void Write(const BookTicker& ticker, const SignalDecision& decision,
             const SignalDiagnostics& diagnostics) noexcept;
  void Close();

 private:
  std::unique_ptr<Writer> writer_;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_SIGNAL_CSV_WRITER_H_
