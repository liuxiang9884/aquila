#ifndef AQUILA_CORE_MARKET_DATA_HISTORICAL_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_HISTORICAL_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/types.h"
#include "core/utils/mapped_file.h"

namespace aquila::market_data {

static_assert(std::is_trivially_copyable_v<BookTicker>);
static_assert(std::is_standard_layout_v<BookTicker>);

struct HistoricalDataReaderStats {
  std::uint64_t total_count{0};
  std::uint64_t files_completed{0};
};

struct NoopHistoricalDataReaderDiagnostics {
  static constexpr bool kEnabled = false;

  explicit NoopHistoricalDataReaderDiagnostics(
      std::size_t file_count) noexcept {
    (void)file_count;
  }
  void RecordBookTicker(const BookTicker&) noexcept {}
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
    files_.reserve(CountFiles(data_reader_config));
    for (const config::DataReaderSourceConfig& source :
         data_reader_config.sources) {
      ValidateSource(source);
      for (const std::filesystem::path& file : source.files) {
        files_.push_back(FileState{
            .path = file,
            .record_count = CheckedRecordCount(file),
        });
      }
    }
    PrepareCurrentFile();
  }

  HistoricalDataReader(const HistoricalDataReader&) = delete;
  HistoricalDataReader& operator=(const HistoricalDataReader&) = delete;
  HistoricalDataReader(HistoricalDataReader&&) = default;
  HistoricalDataReader& operator=(HistoricalDataReader&&) = default;

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) {
    if (finished()) {
      return 0;
    }

    BookTicker book_ticker;
    std::memcpy(&book_ticker, current_cursor_, sizeof(BookTicker));
    current_cursor_ += sizeof(BookTicker);

    --current_records_remaining_;
    if constexpr (Diagnostics::kEnabled) {
      diagnostics_.RecordBookTicker(book_ticker);
    }
    handler.OnBookTicker(book_ticker);

    if (current_records_remaining_ == 0) {
      CompleteCurrentFile();
    }
    return 1;
  }

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) {
    std::uint64_t count = 0;
    while (count < max_events && !finished()) {
      const std::uint64_t events_left = max_events - count;
      const std::uint64_t batch_count = current_records_remaining_ < events_left
                                            ? current_records_remaining_
                                            : events_left;
      for (std::uint64_t i = 0; i < batch_count; ++i) {
        BookTicker book_ticker;
        std::memcpy(&book_ticker, current_cursor_, sizeof(BookTicker));
        current_cursor_ += sizeof(BookTicker);
        if constexpr (Diagnostics::kEnabled) {
          diagnostics_.RecordBookTicker(book_ticker);
        }
        handler.OnBookTicker(book_ticker);
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
    if (source.feed != config::DataReaderFeed::kBookTicker) {
      throw std::invalid_argument(fmt::format(
          "historical data reader source '{}' must use book_ticker feed",
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

  [[nodiscard]] static std::uint64_t CheckedRecordCount(
      const std::filesystem::path& file) {
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
    if (file_size % sizeof(BookTicker) != 0) {
      throw std::runtime_error(fmt::format(
          "binary data file '{}' size {} is not a multiple of BookTicker size "
          "{}",
          file.string(), file_size, sizeof(BookTicker)));
    }

    const std::uintmax_t record_count = file_size / sizeof(BookTicker);
    if (record_count > static_cast<std::uintmax_t>(
                           std::numeric_limits<std::uint64_t>::max())) {
      throw std::runtime_error(
          fmt::format("binary data file '{}' is too large", file.string()));
    }

    return static_cast<std::uint64_t>(record_count);
  }

  void OpenCurrentFile() {
    current_mapping_ = MappedFile(files_[current_file_index_].path,
                                  MappedFileAccessPattern::kSequential);
    const std::size_t expected_size =
        static_cast<std::size_t>(files_[current_file_index_].record_count) *
        sizeof(BookTicker);
    if (current_mapping_.size() != expected_size) {
      throw std::runtime_error(
          fmt::format("binary data file '{}' size changed during open",
                      files_[current_file_index_].path.string()));
    }
    current_records_remaining_ = files_[current_file_index_].record_count;
    current_cursor_ = current_mapping_.data();
  }

  void PrepareCurrentFile() {
    SkipEmptyFiles();
    if (current_file_index_ < files_.size()) {
      OpenCurrentFile();
    }
  }

  void CompleteCurrentFile() {
    current_mapping_ = MappedFile();
    current_cursor_ = nullptr;
    ++current_file_index_;
    if constexpr (Diagnostics::kEnabled) {
      diagnostics_.RecordFileCompleted();
    }
    PrepareCurrentFile();
  }

  void SkipEmptyFiles() {
    while (current_file_index_ < files_.size() &&
           files_[current_file_index_].record_count == 0) {
      ++current_file_index_;
      if constexpr (Diagnostics::kEnabled) {
        diagnostics_.RecordFileCompleted();
      }
    }
  }

  std::vector<FileState> files_;
  std::size_t current_file_index_{0};
  std::uint64_t current_records_remaining_{0};
  MappedFile current_mapping_;
  const char* current_cursor_{nullptr};
  [[no_unique_address]] Diagnostics diagnostics_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_HISTORICAL_DATA_READER_H_
