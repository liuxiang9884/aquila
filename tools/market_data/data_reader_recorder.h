#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include <fmt/core.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace aquila::tools::market_data {

static_assert(std::is_trivially_copyable_v<BookTicker>);
static_assert(std::is_standard_layout_v<BookTicker>);

enum class RecorderWriteMode : std::uint8_t {
  kTruncate,
  kAppend,
};

inline constexpr std::array<Exchange, 6> kRecorderTrackedExchanges{
    Exchange::kBinance, Exchange::kOkx,    Exchange::kGate,
    Exchange::kBybit,   Exchange::kBitget, Exchange::kCoinbase,
};

[[nodiscard]] inline std::optional<std::size_t> RecorderExchangeIndex(
    Exchange exchange) noexcept {
  switch (exchange) {
    case Exchange::kBinance:
      return 0;
    case Exchange::kOkx:
      return 1;
    case Exchange::kGate:
      return 2;
    case Exchange::kBybit:
      return 3;
    case Exchange::kBitget:
      return 4;
    case Exchange::kCoinbase:
      return 5;
  }
  return std::nullopt;
}

struct RecorderStats {
  std::uint64_t total_records{0};
  std::array<std::uint64_t, kRecorderTrackedExchanges.size()>
      records_by_exchange{};
  std::optional<std::int64_t> first_exchange_ns;
  std::optional<std::int64_t> first_local_ns;
  std::optional<std::int64_t> last_exchange_ns;
  std::optional<std::int64_t> last_local_ns;

  void Record(const BookTicker& book_ticker) noexcept {
    if (!first_exchange_ns.has_value()) {
      first_exchange_ns = book_ticker.exchange_ns;
      first_local_ns = book_ticker.local_ns;
    }
    last_exchange_ns = book_ticker.exchange_ns;
    last_local_ns = book_ticker.local_ns;
    ++total_records;

    const std::optional<std::size_t> index =
        RecorderExchangeIndex(book_ticker.exchange);
    if (index.has_value()) {
      ++records_by_exchange[*index];
    }
  }

  [[nodiscard]] std::uint64_t RecordsForExchange(
      Exchange exchange) const noexcept {
    const std::optional<std::size_t> index = RecorderExchangeIndex(exchange);
    if (!index.has_value()) {
      return 0;
    }
    return records_by_exchange[*index];
  }
};

class BookTickerBinaryRecorder {
 public:
  BookTickerBinaryRecorder(std::filesystem::path output_path,
                           RecorderWriteMode write_mode)
      : output_path_(std::move(output_path)) {
    if (output_path_.empty()) {
      throw std::invalid_argument("output path must not be empty");
    }
    if (output_path_.has_parent_path()) {
      std::filesystem::create_directories(output_path_.parent_path());
    }

    std::ios::openmode mode = std::ios::binary;
    switch (write_mode) {
      case RecorderWriteMode::kTruncate:
        mode |= std::ios::trunc;
        break;
      case RecorderWriteMode::kAppend:
        mode |= std::ios::app;
        break;
    }

    output_.open(output_path_, mode);
    if (!output_.is_open()) {
      throw std::runtime_error(fmt::format("failed to open output file '{}'",
                                           output_path_.string()));
    }
  }

  BookTickerBinaryRecorder(const BookTickerBinaryRecorder&) = delete;
  BookTickerBinaryRecorder& operator=(const BookTickerBinaryRecorder&) = delete;

  void OnBookTicker(const BookTicker& book_ticker) noexcept {
    if (write_error_) {
      return;
    }

    output_.write(reinterpret_cast<const char*>(&book_ticker),
                  sizeof(book_ticker));
    if (!output_.good()) {
      write_error_ = true;
      return;
    }
    stats_.Record(book_ticker);
  }

  [[nodiscard]] bool Flush() noexcept {
    if (write_error_) {
      return false;
    }
    output_.flush();
    if (!output_.good()) {
      write_error_ = true;
      return false;
    }
    return true;
  }

  [[nodiscard]] const RecorderStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] bool write_error() const noexcept {
    return write_error_;
  }

  [[nodiscard]] const std::filesystem::path& output_path() const noexcept {
    return output_path_;
  }

 private:
  std::filesystem::path output_path_;
  std::ofstream output_;
  RecorderStats stats_;
  bool write_error_{false};
};

}  // namespace aquila::tools::market_data
