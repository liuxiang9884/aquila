#ifndef AQUILA_STRATEGY_LEAD_LAG_MARKET_CALC_CSV_WRITER_H_
#define AQUILA_STRATEGY_LEAD_LAG_MARKET_CALC_CSV_WRITER_H_

#include <filesystem>
#include <memory>
#include <string>

#include <quill/CsvWriter.h>

#include "nova/utils/log.h"
#include "strategy/lead_lag/market_calc_diagnostics.h"

namespace aquila::strategy::leadlag {

struct LeadMarketCalcCsvSchema {
  static constexpr char const* header =
      "row_index,role,symbol,symbol_id,book_ticker_id,exchange,exchange_ns,"
      "local_ns,event_ns,price_changed,both_sides_valid,active,lead_bid,"
      "lead_ask,lag_bid,lag_ask,drift_mean,drift_std_ema,drifted_lead_bid,"
      "drifted_lead_ask,up_entry,down_entry,up_exit,down_exit,lead_noise,"
      "lag_noise,lag_spread_mean,long_lead_move,long_price_diff,"
      "long_lag_part_ratio,long_target_space,long_required_edge,"
      "short_lead_move,short_price_diff,short_lag_part_ratio,"
      "short_target_space,short_required_edge,lag_spread_pct";
  static constexpr char const* format =
      "{},{},{},{},{},{},{},{},{},{},{},{},{:.12g},{:.12g},{:.12g},"
      "{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},"
      "{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},"
      "{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},"
      "{:.12g},{:.12g}";
};

struct LagMarketCalcCsvSchema {
  static constexpr char const* header =
      "row_index,role,symbol,symbol_id,book_ticker_id,exchange,exchange_ns,"
      "local_ns,event_ns,price_changed,both_sides_valid,active,lead_bid,"
      "lead_ask,lag_bid,lag_ask,drift_mean,drift_std_ema,lag_spread,"
      "lag_spread_mean,lag_spread_buffer,lag_spread_pct,lag_noise";
  static constexpr char const* format =
      "{},{},{},{},{},{},{},{},{},{},{},{},{:.12g},{:.12g},{:.12g},"
      "{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},"
      "{:.12g}";
};

class MarketCalcCsvWriter {
 public:
  using LeadWriter = quill::CsvWriter<LeadMarketCalcCsvSchema,
                                      nova::LogManager::NovaFrontendOptions>;
  using LagWriter = quill::CsvWriter<LagMarketCalcCsvSchema,
                                     nova::LogManager::NovaFrontendOptions>;

  MarketCalcCsvWriter() = default;
  ~MarketCalcCsvWriter() = default;

  MarketCalcCsvWriter(const MarketCalcCsvWriter&) = delete;
  MarketCalcCsvWriter& operator=(const MarketCalcCsvWriter&) = delete;

  [[nodiscard]] bool Open(const std::filesystem::path& output_dir,
                          std::string* error);
  void Write(const MarketCalcRow& row) noexcept;
  void Close();

 private:
  std::unique_ptr<LeadWriter> lead_writer_;
  std::unique_ptr<LagWriter> lag_writer_;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_MARKET_CALC_CSV_WRITER_H_
