#include "strategy/lead_lag/market_calc_csv_writer.h"

#include <exception>

#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

namespace aquila::strategy::leadlag {

bool MarketCalcCsvWriter::Open(const std::filesystem::path& output_dir,
                               std::string* error) {
  try {
    std::filesystem::create_directories(output_dir);
    lead_writer_ =
        std::make_unique<LeadWriter>((output_dir / "lead_calc.csv").string());
    lag_writer_ =
        std::make_unique<LagWriter>((output_dir / "lag_calc.csv").string());
  } catch (const std::exception& ex) {
    Close();
    if (error != nullptr) {
      *error = fmt::format("failed to open market calc output '{}': {}",
                           output_dir.string(), ex.what());
    }
    return false;
  }
  return true;
}

void MarketCalcCsvWriter::Write(const MarketCalcRow& row) noexcept {
  switch (row.role) {
    case PairRole::kLead:
      if (lead_writer_ == nullptr) {
        return;
      }
      lead_writer_->append_row(
          row.row_index, magic_enum::enum_name(row.role), row.symbol,
          row.symbol_id, row.book_ticker_id,
          magic_enum::enum_name(row.exchange), row.exchange_ns, row.local_ns,
          row.event_ns, row.price_changed, row.both_sides_valid, row.active,
          row.lead_bid, row.lead_ask, row.lag_bid, row.lag_ask, row.drift_mean,
          row.drift_std_ema, row.drifted_lead_bid, row.drifted_lead_ask,
          row.up_entry, row.down_entry, row.up_exit, row.down_exit,
          row.lead_noise, row.lag_noise, row.lag_spread_mean,
          row.long_lead_move, row.long_price_diff, row.long_lag_part_ratio,
          row.long_target_space, row.long_required_edge, row.short_lead_move,
          row.short_price_diff, row.short_lag_part_ratio,
          row.short_target_space, row.short_required_edge, row.lag_spread_pct);
      break;
    case PairRole::kLag:
      if (lag_writer_ == nullptr) {
        return;
      }
      lag_writer_->append_row(
          row.row_index, magic_enum::enum_name(row.role), row.symbol,
          row.symbol_id, row.book_ticker_id,
          magic_enum::enum_name(row.exchange), row.exchange_ns, row.local_ns,
          row.event_ns, row.price_changed, row.both_sides_valid, row.active,
          row.lead_bid, row.lead_ask, row.lag_bid, row.lag_ask, row.drift_mean,
          row.drift_std_ema, row.lag_spread, row.lag_spread_mean,
          row.lag_spread_buffer, row.lag_spread_pct, row.lag_noise);
      break;
    case PairRole::kNone:
      break;
  }
}

void MarketCalcCsvWriter::Close() {
  lead_writer_.reset();
  lag_writer_.reset();
}

}  // namespace aquila::strategy::leadlag
