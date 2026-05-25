#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

struct RecorderTimeSnapshot {
  std::chrono::steady_clock::time_point steady;
  std::chrono::system_clock::time_point wall;
};

[[nodiscard]] inline RecorderTimeSnapshot SystemRecorderTimeSnapshot() {
  return RecorderTimeSnapshot{
      .steady = std::chrono::steady_clock::now(),
      .wall = std::chrono::system_clock::now(),
  };
}

struct RecorderRotationConfig {
  bool enabled{false};
  std::uint32_t rotation_interval_sec{3600};
  std::filesystem::path output_dir;
  std::string file_prefix;
  std::filesystem::path manifest_path;
};

class RotatingBookTickerBinaryRecorder {
 public:
  using Clock = std::function<RecorderTimeSnapshot()>;

  explicit RotatingBookTickerBinaryRecorder(RecorderRotationConfig config,
                                            Clock clock = Clock{})
      : config_(std::move(config)), clock_(std::move(clock)) {
    if (config_.rotation_interval_sec == 0) {
      throw std::invalid_argument("rotation interval must be positive");
    }
    if (config_.output_dir.empty()) {
      throw std::invalid_argument("rotation output_dir must not be empty");
    }
    if (config_.file_prefix.empty()) {
      throw std::invalid_argument("rotation file_prefix must not be empty");
    }
    if (config_.manifest_path.empty()) {
      throw std::invalid_argument("rotation manifest_path must not be empty");
    }
    if (!clock_) {
      clock_ = SystemRecorderTimeSnapshot;
    }
    std::filesystem::create_directories(config_.output_dir);
    if (config_.manifest_path.has_parent_path()) {
      std::filesystem::create_directories(config_.manifest_path.parent_path());
    }
    if (!OpenSegment(clock_())) {
      throw std::runtime_error(fmt::format(
          "failed to open rotation segment '{}'", current_tmp_path_.string()));
    }
  }

  RotatingBookTickerBinaryRecorder(const RotatingBookTickerBinaryRecorder&) =
      delete;
  RotatingBookTickerBinaryRecorder& operator=(
      const RotatingBookTickerBinaryRecorder&) = delete;

  void OnBookTicker(const BookTicker& book_ticker) noexcept {
    if (write_error_) {
      return;
    }

    try {
      OnBookTickerImpl(book_ticker);
    } catch (...) {
      write_error_ = true;
    }
  }

  [[nodiscard]] bool Flush() noexcept {
    if (write_error_) {
      return false;
    }
    try {
      return FinalizeCurrentSegment("flush");
    } catch (...) {
      write_error_ = true;
      return false;
    }
  }

  [[nodiscard]] const RecorderStats& stats() const noexcept {
    return stats_;
  }

  [[nodiscard]] bool write_error() const noexcept {
    return write_error_;
  }

  [[nodiscard]] std::uint64_t segments_completed() const noexcept {
    return segments_completed_;
  }

 private:
  void OnBookTickerImpl(const BookTicker& book_ticker) {
    const RecorderTimeSnapshot now = clock_();
    if (current_stats_.total_records != 0 &&
        now.steady >= next_rotation_deadline_) {
      if (!FinalizeCurrentSegment("rotation_interval")) {
        return;
      }
      if (!OpenSegment(now)) {
        return;
      }
    }

    output_.write(reinterpret_cast<const char*>(&book_ticker),
                  sizeof(book_ticker));
    if (!output_.good()) {
      write_error_ = true;
      return;
    }
    stats_.Record(book_ticker);
    current_stats_.Record(book_ticker);
  }

  [[nodiscard]] static std::string FormatWallTime(
      std::chrono::system_clock::time_point wall) {
    const std::time_t time = std::chrono::system_clock::to_time_t(wall);
    std::tm tm{};
    gmtime_r(&time, &tm);
    return fmt::format("{:04}{:02}{:02}_{:02}{:02}{:02}", tm.tm_year + 1900,
                       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                       tm.tm_sec);
  }

  [[nodiscard]] static std::string JsonEscape(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char c : text) {
      switch (c) {
        case '"':
          escaped.append("\\\"");
          break;
        case '\\':
          escaped.append("\\\\");
          break;
        case '\n':
          escaped.append("\\n");
          break;
        case '\r':
          escaped.append("\\r");
          break;
        case '\t':
          escaped.append("\\t");
          break;
        default:
          escaped.push_back(c);
          break;
      }
    }
    return escaped;
  }

  [[nodiscard]] bool OpenSegment(const RecorderTimeSnapshot& now) {
    current_stats_ = RecorderStats{};
    current_sequence_ = next_sequence_++;
    const std::string file_name =
        fmt::format("{}_{}_{:06}.bin", config_.file_prefix,
                    FormatWallTime(now.wall), current_sequence_);
    current_final_path_ = config_.output_dir / file_name;
    current_tmp_path_ = current_final_path_;
    current_tmp_path_ += ".tmp";
    next_rotation_deadline_ =
        now.steady + std::chrono::seconds{config_.rotation_interval_sec};

    std::error_code exists_error;
    const bool tmp_exists =
        std::filesystem::exists(current_tmp_path_, exists_error);
    if (exists_error || tmp_exists) {
      write_error_ = true;
      return false;
    }
    const bool final_exists =
        std::filesystem::exists(current_final_path_, exists_error);
    if (exists_error || final_exists) {
      write_error_ = true;
      return false;
    }

    output_.open(current_tmp_path_, std::ios::binary | std::ios::trunc);
    if (!output_.is_open()) {
      write_error_ = true;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool FinalizeCurrentSegment(std::string_view reason) {
    if (!output_.is_open()) {
      return !write_error_;
    }

    output_.flush();
    const bool stream_ok = output_.good();
    output_.close();
    if (!stream_ok) {
      write_error_ = true;
      return false;
    }

    if (current_stats_.total_records == 0) {
      std::error_code remove_error;
      std::filesystem::remove(current_tmp_path_, remove_error);
      return true;
    }

    std::error_code size_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(current_tmp_path_, size_error);
    if (size_error ||
        file_size != current_stats_.total_records * sizeof(BookTicker)) {
      write_error_ = true;
      return false;
    }
    std::error_code exists_error;
    const bool final_exists =
        std::filesystem::exists(current_final_path_, exists_error);
    if (exists_error || final_exists) {
      write_error_ = true;
      return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(current_tmp_path_, current_final_path_,
                            rename_error);
    if (rename_error) {
      write_error_ = true;
      return false;
    }

    if (!AppendManifestLine(reason, file_size)) {
      write_error_ = true;
      return false;
    }
    ++segments_completed_;
    return true;
  }

  [[nodiscard]] bool AppendManifestLine(std::string_view reason,
                                        std::uintmax_t file_size) {
    std::ofstream manifest(config_.manifest_path,
                           std::ios::binary | std::ios::app);
    if (!manifest.is_open()) {
      return false;
    }
    manifest << fmt::format(
        "{{\"sequence\":{},\"file\":\"{}\",\"records\":{},"
        "\"bytes\":{},\"first_exchange_ns\":{},"
        "\"last_exchange_ns\":{},\"first_local_ns\":{},"
        "\"last_local_ns\":{},\"closed_reason\":\"{}\"}}\n",
        current_sequence_, JsonEscape(current_final_path_.string()),
        current_stats_.total_records, file_size,
        current_stats_.first_exchange_ns.value_or(0),
        current_stats_.last_exchange_ns.value_or(0),
        current_stats_.first_local_ns.value_or(0),
        current_stats_.last_local_ns.value_or(0), JsonEscape(reason));
    return manifest.good();
  }

  RecorderRotationConfig config_;
  Clock clock_;
  std::ofstream output_;
  std::filesystem::path current_tmp_path_;
  std::filesystem::path current_final_path_;
  RecorderStats stats_;
  RecorderStats current_stats_;
  std::chrono::steady_clock::time_point next_rotation_deadline_{};
  std::uint64_t next_sequence_{1};
  std::uint64_t current_sequence_{0};
  std::uint64_t segments_completed_{0};
  bool write_error_{false};
};

}  // namespace aquila::tools::market_data
