#ifndef AQUILA_CORE_MARKET_DATA_BINARY_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_BINARY_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "core/config/data_reader_config.h"
#include "core/market_data/types.h"

namespace aquila::market_data {

static_assert(std::is_trivially_copyable_v<BookTicker>);
static_assert(std::is_standard_layout_v<BookTicker>);

struct BinaryDataReaderStats {
  std::uint64_t poll_calls{0};
  std::uint64_t empty_polls{0};
  std::uint64_t book_tickers{0};
  std::uint64_t files_completed{0};
};

struct NoopBinaryDataReaderDiagnostics {
  explicit NoopBinaryDataReaderDiagnostics(std::size_t file_count) noexcept {
    (void)file_count;
  }
  void RecordPoll() noexcept {}
  void RecordEmptyPoll() noexcept {}
  void RecordBookTicker(const BookTicker&) noexcept {}
  void RecordFileCompleted() noexcept {}
};

class BinaryDataReaderDiagnostics {
 public:
  explicit BinaryDataReaderDiagnostics(std::size_t file_count) noexcept {
    (void)file_count;
  }

  void RecordPoll() noexcept {
    ++stats_.poll_calls;
  }

  void RecordEmptyPoll() noexcept {
    ++stats_.empty_polls;
  }

  void RecordBookTicker(const BookTicker&) noexcept {
    ++stats_.book_tickers;
  }

  void RecordFileCompleted() noexcept {
    ++stats_.files_completed;
  }

  [[nodiscard]] const BinaryDataReaderStats& stats() const noexcept {
    return stats_;
  }

 private:
  BinaryDataReaderStats stats_;
};

template <typename Diagnostics = NoopBinaryDataReaderDiagnostics>
class BinaryDataReader {
 public:
  explicit BinaryDataReader(config::DataReaderConfig data_reader_config)
      : max_events_per_poll_(data_reader_config.max_events_per_source),
        diagnostics_(CountFiles(data_reader_config)) {
    if (max_events_per_poll_ == 0) {
      throw std::invalid_argument(
          "data_reader.max_events_per_source must be positive");
    }

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
  }

  BinaryDataReader(const BinaryDataReader&) = delete;
  BinaryDataReader& operator=(const BinaryDataReader&) = delete;
  BinaryDataReader(BinaryDataReader&&) = default;
  BinaryDataReader& operator=(BinaryDataReader&&) = default;

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) {
    if constexpr (!std::is_same_v<Diagnostics,
                                  NoopBinaryDataReaderDiagnostics>) {
      diagnostics_.RecordPoll();
    }

    std::uint64_t handled = 0;
    while (handled < max_events_per_poll_ && EnsureReadableFile()) {
      BookTicker book_ticker{};
      current_input_.read(reinterpret_cast<char*>(&book_ticker),
                          static_cast<std::streamsize>(sizeof(BookTicker)));
      if (current_input_.gcount() !=
          static_cast<std::streamsize>(sizeof(BookTicker))) {
        throw std::runtime_error(
            fmt::format("failed to read binary data file '{}'",
                        files_[current_file_index_].path.string()));
      }

      --current_records_remaining_;
      if constexpr (!std::is_same_v<Diagnostics,
                                    NoopBinaryDataReaderDiagnostics>) {
        diagnostics_.RecordBookTicker(book_ticker);
      }
      handler.OnBookTicker(book_ticker);
      ++handled;

      if (current_records_remaining_ == 0) {
        CompleteCurrentFile();
      }
    }

    if (handled == 0) {
      if constexpr (!std::is_same_v<Diagnostics,
                                    NoopBinaryDataReaderDiagnostics>) {
        diagnostics_.RecordEmptyPoll();
      }
    }
    return handled;
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
          "binary data reader source '{}' must have type binary_file",
          source.name));
    }
    if (source.feed != config::DataReaderFeed::kBookTicker) {
      throw std::invalid_argument(fmt::format(
          "binary data reader source '{}' must use book_ticker feed",
          source.name));
    }
    if (source.files.empty()) {
      throw std::invalid_argument(fmt::format(
          "binary data reader source '{}' requires at least one file",
          source.name));
    }
    if (source.start_position == config::DataReaderStartPosition::kLatest) {
      throw std::invalid_argument(
          fmt::format("binary data reader source '{}' does not support latest "
                      "start_position",
                      source.name));
    }
    if (source.read_mode != config::DataReaderReadMode::kDrain) {
      throw std::invalid_argument(
          fmt::format("binary data reader source '{}' must use drain read_mode",
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

    std::ifstream input(file, std::ios::binary);
    if (!input.is_open()) {
      throw std::runtime_error(
          fmt::format("failed to open binary data file '{}'", file.string()));
    }

    return static_cast<std::uint64_t>(record_count);
  }

  void OpenCurrentFile() {
    current_input_.close();
    current_input_.clear();
    current_input_.open(files_[current_file_index_].path, std::ios::binary);
    if (!current_input_.is_open()) {
      throw std::runtime_error(
          fmt::format("failed to open binary data file '{}'",
                      files_[current_file_index_].path.string()));
    }
    current_records_remaining_ = files_[current_file_index_].record_count;
  }

  [[nodiscard]] bool EnsureReadableFile() {
    while (current_file_index_ < files_.size()) {
      if (!current_input_.is_open()) {
        OpenCurrentFile();
      }
      if (current_records_remaining_ > 0) {
        return true;
      }
      CompleteCurrentFile();
    }
    return false;
  }

  void CompleteCurrentFile() {
    current_input_.close();
    current_input_.clear();
    ++current_file_index_;
    if constexpr (!std::is_same_v<Diagnostics,
                                  NoopBinaryDataReaderDiagnostics>) {
      diagnostics_.RecordFileCompleted();
    }
  }

  std::uint32_t max_events_per_poll_{64};
  std::vector<FileState> files_;
  std::size_t current_file_index_{0};
  std::uint64_t current_records_remaining_{0};
  std::ifstream current_input_;
  [[no_unique_address]] Diagnostics diagnostics_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BINARY_DATA_READER_H_
