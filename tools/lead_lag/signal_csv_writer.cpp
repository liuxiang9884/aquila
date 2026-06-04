#include "tools/lead_lag/signal_csv_writer.h"

#include <exception>

#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

namespace aquila::tools::lead_lag {

bool SignalCsvWriter::Open(const std::filesystem::path& path,
                           std::string* error) {
  try {
    writer_ = std::make_unique<Writer>(path.string());
  } catch (const std::exception& ex) {
    writer_.reset();
    if (error != nullptr) {
      *error = fmt::format("failed to open signals output '{}': {}",
                           path.string(), ex.what());
    }
    return false;
  }
  return true;
}

void SignalCsvWriter::Write(
    const BookTicker& ticker,
    const strategy::leadlag::SignalDecision& decision,
    const strategy::leadlag::SignalDiagnostics& diagnostics) noexcept {
  if (writer_ == nullptr) {
    return;
  }
  writer_->append_row(
      ticker.symbol_id, magic_enum::enum_name(ticker.exchange),
      magic_enum::enum_name(diagnostics.role), ticker.exchange_ns,
      ticker.local_ns, diagnostics.event_ns, diagnostics.price_changed,
      magic_enum::enum_name(decision.action),
      magic_enum::enum_name(decision.intent.side), decision.intent.price,
      decision.intent.reduce_only, diagnostics.lead_raw.exchange_ns,
      diagnostics.lead_raw.bid_price, diagnostics.lead_raw.ask_price,
      diagnostics.lead_drifted.event_ns, diagnostics.lead_drifted.bid_price,
      diagnostics.lead_drifted.ask_price, diagnostics.lag.exchange_ns,
      diagnostics.lag.bid_price, diagnostics.lag.ask_price,
      diagnostics.alignment.drift_mean, diagnostics.alignment.drift_ready,
      diagnostics.alignment.drift_deviation, diagnostics.threshold.up_entry,
      diagnostics.threshold.down_entry, diagnostics.threshold.up_exit,
      diagnostics.threshold.down_exit, diagnostics.recorder.lag_spread_mean,
      diagnostics.recorder.lead_noise, diagnostics.recorder.lag_noise,
      diagnostics.active_group_count, diagnostics.group_id,
      magic_enum::enum_name(diagnostics.position_direction),
      diagnostics.trailing_price);
}

void SignalCsvWriter::Close() {
  writer_.reset();
}

}  // namespace aquila::tools::lead_lag
