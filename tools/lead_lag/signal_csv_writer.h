#ifndef AQUILA_TOOLS_LEAD_LAG_SIGNAL_CSV_WRITER_H_
#define AQUILA_TOOLS_LEAD_LAG_SIGNAL_CSV_WRITER_H_

#include <filesystem>
#include <memory>
#include <string>

#include <quill/CsvWriter.h>

#include "core/market_data/types.h"
#include "nova/utils/log.h"
#include "strategy/lead_lag/signal.h"

namespace aquila::tools::lead_lag {

struct SignalCsvSchema {
  static constexpr char const* header =
      "ticker_id,symbol_id,exchange_ns,local_ns,action,side,price,reduce_only";
  static constexpr char const* format = "{},{},{},{},{},{},{:.12g},{}";
};

class SignalCsvWriter {
 public:
  using Writer =
      quill::CsvWriter<SignalCsvSchema,
                       nova::LogManager::NovaFrontendOptions>;

  SignalCsvWriter() = default;
  ~SignalCsvWriter() = default;

  SignalCsvWriter(const SignalCsvWriter&) = delete;
  SignalCsvWriter& operator=(const SignalCsvWriter&) = delete;

  [[nodiscard]] bool Open(const std::filesystem::path& path,
                          std::string* error);
  void Write(const BookTicker& ticker,
             const strategy::leadlag::SignalDecision& decision) noexcept;
  void Close();

 private:
  std::unique_ptr<Writer> writer_;
};

}  // namespace aquila::tools::lead_lag

#endif  // AQUILA_TOOLS_LEAD_LAG_SIGNAL_CSV_WRITER_H_
