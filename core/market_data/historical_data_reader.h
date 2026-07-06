#ifndef AQUILA_CORE_MARKET_DATA_HISTORICAL_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_HISTORICAL_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/market_data_binary_format.h"
#include "core/market_data/types.h"
#include "core/utils/mapped_file.h"

namespace aquila::market_data {

static_assert(std::is_trivially_copyable_v<BookTicker>);
static_assert(std::is_standard_layout_v<BookTicker>);
static_assert(std::is_trivially_copyable_v<Trade>);
static_assert(std::is_standard_layout_v<Trade>);

struct HistoricalDataReaderStats {
  std::uint64_t total_count{0};
  std::uint64_t book_ticker_count{0};
  std::uint64_t trade_count{0};
  std::uint64_t files_completed{0};
};

struct NoopHistoricalDataReaderDiagnostics {
  static constexpr bool kEnabled = false;

  explicit NoopHistoricalDataReaderDiagnostics(
      std::size_t file_count) noexcept {
    (void)file_count;
  }
  void RecordBookTicker(const BookTicker&) noexcept {}
  void RecordTrade(const Trade&) noexcept {}
  void RecordFileCompleted() noexcept {}
};

class HistoricalDataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  explicit HistoricalDataReaderDiagnostics(std::size_t file_count) noexcept {
    (void)file_count;
  }

  void RecordBookTicker(const BookTicker&) noexcept {
    ++stats_.total_count;
    ++stats_.book_ticker_count;
  }

  void RecordTrade(const Trade&) noexcept {
    ++stats_.total_count;
    ++stats_.trade_count;
  }

  void RecordFileCompleted() noexcept {
    ++stats_.files_completed;
  }

  [[nodiscard]] const HistoricalDataReaderStats& stats() const noexcept {
    return stats_;
  }

 private:
  HistoricalDataReaderStats stats_;
};

template <typename Diagnostics = NoopHistoricalDataReaderDiagnostics>
class HistoricalDataReader {
 public:
  static constexpr bool kFiniteDataReader = true;

  explicit HistoricalDataReader(config::DataReaderConfig data_reader_config)
      : diagnostics_(CountFiles(data_reader_config)) {
    if (data_reader_config.sources.size() != 1) {
      throw std::invalid_argument(
          "historical data reader requires exactly one binary_file source");
    }
    files_.reserve(CountFiles(data_reader_config));
    for (const config::DataReaderSourceConfig& source :
         data_reader_config.sources) {
      ValidateSource(source);
      feed_ = source.feed;
      for (const std::filesystem::path& file : source.files) {
        files_.push_back(OpenTypedFile(file, feed_));
      }
    }
    PrepareCurrentFile();
  }

  HistoricalDataReader(const HistoricalDataReader&) = delete;
  HistoricalDataReader& operator=(const HistoricalDataReader&) = delete;
  HistoricalDataReader(HistoricalDataReader&&) = default;
  HistoricalDataReader& operator=(HistoricalDataReader&&) = default;

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept {
    if (finished()) {
      return 0;
    }

    DispatchCurrentRecord(handler);
    --current_records_remaining_;

    if (current_records_remaining_ == 0) {
      CompleteCurrentFile();
    }
    return 1;
  }

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    std::uint64_t count = 0;
    while (count < max_events && !finished()) {
      const std::uint64_t events_left = max_events - count;
      const std::uint64_t batch_count = current_records_remaining_ < events_left
                                            ? current_records_remaining_
                                            : events_left;
      for (std::uint64_t i = 0; i < batch_count; ++i) {
        DispatchCurrentRecord(handler);
      }

      current_records_remaining_ -= batch_count;
      count += batch_count;
      if (current_records_remaining_ == 0) {
        CompleteCurrentFile();
      }
    }
    return count;
  }

  [[nodiscard]] bool finished() const noexcept {
    return current_file_index_ >= files_.size();
  }

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  struct FileState {
    std::filesystem::path path;
    std::uint64_t record_count{0};
    std::size_t payload_offset{0};
    MappedFile mapping;
  };

  [[nodiscard]] static std::size_t CountFiles(
      const config::DataReaderConfig& data_reader_config) noexcept {
    std::size_t file_count = 0;
    for (const config::DataReaderSourceConfig& source :
         data_reader_config.sources) {
      file_count += source.files.size();
    }
    return file_count;
  }

  static void ValidateSource(const config::DataReaderSourceConfig& source) {
    if (source.type != config::DataReaderSourceType::kBinaryFile) {
      throw std::invalid_argument(fmt::format(
          "historical data reader source '{}' must have type binary_file",
          source.name));
    }
    switch (source.feed) {
      case config::DataReaderFeed::kBookTicker:
      case config::DataReaderFeed::kTrade:
        break;
      default:
        throw std::invalid_argument(fmt::format(
            "historical data reader source '{}' must use book_ticker or trade "
            "feed",
            source.name));
    }
    if (source.files.empty()) {
      throw std::invalid_argument(fmt::format(
          "historical data reader source '{}' requires at least one file",
          source.name));
    }
    if (source.start_position == config::DataReaderStartPosition::kLatest) {
      throw std::invalid_argument(fmt::format(
          "historical data reader source '{}' does not support latest "
          "start_position",
          source.name));
    }
    if (source.read_mode != config::DataReaderReadMode::kDrain) {
      throw std::invalid_argument(fmt::format(
          "historical data reader source '{}' must use drain read_mode",
          source.name));
    }
  }

  [[nodiscard]] static FileState OpenTypedFile(
      const std::filesystem::path& file, config::DataReaderFeed feed) {
    if (file.empty()) {
      throw std::invalid_argument("binary data reader file path is empty");
    }

    std::error_code error;
    const std::uintmax_t file_size = std::filesystem::file_size(file, error);
    if (error) {
      throw std::runtime_error(
          fmt::format("failed to inspect binary data file '{}': {}",
                      file.string(), error.message()));
    }
    if (file_size < sizeof(MarketDataBinaryHeader)) {
      throw std::runtime_error(fmt::format(
          "binary data file '{}' is missing market data header",
          file.string()));
    }

    MappedFile mapping(file, MappedFileAccessPattern::kSequential);
    if (mapping.size() != file_size) {
      throw std::runtime_error(fmt::format(
          "binary data file '{}' size changed during open", file.string()));
    }

    const MarketDataBinaryHeader header = ReadMarketDataBinaryHeaderFromData(
        mapping.data(), mapping.size(), file);
    const std::uint64_t record_count =
        CheckedMarketDataBinaryRecordCount(file, file_size, header, feed);
    return FileState{
        .path = file,
        .record_count = record_count,
        .payload_offset = header.header_size,
        .mapping = std::move(mapping),
    };
  }

  template <typename Handler>
  void DispatchCurrentRecord(Handler& handler) noexcept {
    switch (feed_) {
      case config::DataReaderFeed::kBookTicker:
        DispatchBookTicker(handler);
        return;
      case config::DataReaderFeed::kTrade:
        DispatchTrade(handler);
        return;
    }
  }

  template <typename Handler>
  void DispatchBookTicker(Handler& handler) noexcept {
    BookTicker book_ticker;
    std::memcpy(&book_ticker, current_cursor_, sizeof(BookTicker));
    current_cursor_ += sizeof(BookTicker);
    if constexpr (Diagnostics::kEnabled) {
      diagnostics_.RecordBookTicker(book_ticker);
    }
    handler.OnBookTicker(book_ticker);
  }

  template <typename Handler>
  void DispatchTrade(Handler& handler) noexcept {
    Trade trade;
    std::memcpy(&trade, current_cursor_, sizeof(Trade));
    current_cursor_ += sizeof(Trade);
    if constexpr (Diagnostics::kEnabled) {
      diagnostics_.RecordTrade(trade);
    }
    handler.OnTrade(trade);
  }

  void PrepareCurrentFile() noexcept {
    current_records_remaining_ = 0;
    current_cursor_ = nullptr;
    SkipEmptyFiles();
    if (current_file_index_ >= files_.size()) {
      return;
    }

    current_records_remaining_ = files_[current_file_index_].record_count;
    current_cursor_ = files_[current_file_index_].mapping.data() +
                      files_[current_file_index_].payload_offset;
  }

  void CompleteCurrentFile() noexcept {
    current_cursor_ = nullptr;
    current_records_remaining_ = 0;
    ++current_file_index_;
    if constexpr (Diagnostics::kEnabled) {
      diagnostics_.RecordFileCompleted();
    }
    PrepareCurrentFile();
  }

  void SkipEmptyFiles() noexcept {
    while (current_file_index_ < files_.size() &&
           files_[current_file_index_].record_count == 0) {
      ++current_file_index_;
      if constexpr (Diagnostics::kEnabled) {
        diagnostics_.RecordFileCompleted();
      }
    }
  }

  std::vector<FileState> files_;
  config::DataReaderFeed feed_{config::DataReaderFeed::kBookTicker};
  std::size_t current_file_index_{0};
  std::uint64_t current_records_remaining_{0};
  const char* current_cursor_{nullptr};
  [[no_unique_address]] Diagnostics diagnostics_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_HISTORICAL_DATA_READER_H_
