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
    const strategy::leadlag::SignalDecision& decision) noexcept {
  if (writer_ == nullptr) {
    return;
  }
  writer_->append_row(ticker.id, ticker.symbol_id, ticker.exchange_ns,
                      ticker.local_ns, magic_enum::enum_name(decision.action),
                      magic_enum::enum_name(decision.intent.side),
                      decision.intent.price, decision.intent.reduce_only);
}

void SignalCsvWriter::Close() {
  writer_.reset();
}

}  // namespace aquila::tools::lead_lag
