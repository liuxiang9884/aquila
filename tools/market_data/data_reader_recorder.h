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
#include <system_error>
#include <type_traits>
#include <utility>

#include <fmt/core.h>

#include "core/common/types.h"
#include "core/config/data_reader_config.h"
#include "core/market_data/market_data_binary_format.h"
#include "core/market_data/types.h"

namespace aquila::tools::market_data {

static_assert(std::is_trivially_copyable_v<BookTicker>);
static_assert(std::is_standard_layout_v<BookTicker>);
static_assert(std::is_trivially_copyable_v<Trade>);
static_assert(std::is_standard_layout_v<Trade>);

enum class RecorderWriteMode : std::uint8_t {
  kTruncate,
  kAppend,
};

[[nodiscard]] inline std::string_view RecorderFeedName(
    config::DataReaderFeed feed) noexcept {
  switch (feed) {
    case config::DataReaderFeed::kBookTicker:
      return "book_ticker";
    case config::DataReaderFeed::kTrade:
      return "trade";
  }
  return "unknown";
}

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

struct BookTickerRecorderTraits {
  static constexpr std::string_view kFeedName{"book_ticker"};
  static constexpr config::DataReaderFeed kFeed{
      config::DataReaderFeed::kBookTicker};

  [[nodiscard]] static Exchange ExchangeOf(const BookTicker& record) noexcept {
    return record.exchange;
  }

  [[nodiscard]] static std::int64_t ExchangeNsOf(
      const BookTicker& record) noexcept {
    return record.exchange_ns;
  }

  [[nodiscard]] static std::int64_t LocalNsOf(
      const BookTicker& record) noexcept {
    return record.local_ns;
  }
};

struct TradeRecorderTraits {
  static constexpr std::string_view kFeedName{"trade"};
  static constexpr config::DataReaderFeed kFeed{config::DataReaderFeed::kTrade};

  [[nodiscard]] static Exchange ExchangeOf(const Trade& record) noexcept {
    return record.exchange;
  }

  [[nodiscard]] static std::int64_t ExchangeNsOf(const Trade& record) noexcept {
    return record.exchange_ns;
  }

  [[nodiscard]] static std::int64_t LocalNsOf(const Trade& record) noexcept {
    return record.local_ns;
  }
};

template <typename RecordT, typename Traits>
struct TypedRecorderStats {
  std::uint64_t total_records{0};
  std::array<std::uint64_t, kRecorderTrackedExchanges.size()>
      records_by_exchange{};
  std::optional<std::int64_t> first_exchange_ns;
  std::optional<std::int64_t> first_local_ns;
  std::optional<std::int64_t> last_exchange_ns;
  std::optional<std::int64_t> last_local_ns;

  void Record(const RecordT& record) noexcept {
    if (!first_exchange_ns.has_value()) {
      first_exchange_ns = Traits::ExchangeNsOf(record);
      first_local_ns = Traits::LocalNsOf(record);
    }
    last_exchange_ns = Traits::ExchangeNsOf(record);
    last_local_ns = Traits::LocalNsOf(record);
    ++total_records;

    const std::optional<std::size_t> index =
        RecorderExchangeIndex(Traits::ExchangeOf(record));
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

using BookTickerRecorderStats =
    TypedRecorderStats<BookTicker, BookTickerRecorderTraits>;
using TradeRecorderStats = TypedRecorderStats<Trade, TradeRecorderTraits>;
using RecorderStats = BookTickerRecorderStats;

struct BinaryRecorderOutputPaths {
  std::filesystem::path book_ticker_output_path;
  std::filesystem::path trade_output_path;
};

[[nodiscard]] inline std::filesystem::path RecorderComparablePath(
    const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path canonical_path =
      std::filesystem::weakly_canonical(path, error);
  if (!error) {
    return canonical_path.lexically_normal();
  }
  const std::filesystem::path absolute_path =
      std::filesystem::absolute(path, error);
  if (!error) {
    return absolute_path.lexically_normal();
  }
  return path.lexically_normal();
}

[[nodiscard]] inline bool RecorderEquivalentExistingPath(
    const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
  std::error_code error;
  const bool equivalent = std::filesystem::equivalent(lhs, rhs, error);
  if (!error) {
    return equivalent;
  }
  return false;
}

[[nodiscard]] inline bool RecorderSamePath(const std::filesystem::path& lhs,
                                           const std::filesystem::path& rhs) {
  if (RecorderEquivalentExistingPath(lhs, rhs)) {
    return true;
  }
  return RecorderComparablePath(lhs) == RecorderComparablePath(rhs);
}

[[nodiscard]] inline std::filesystem::file_status RecorderSymlinkStatus(
    const std::filesystem::path& path, std::string_view label) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error && status.type() != std::filesystem::file_type::not_found) {
    throw std::runtime_error(
        fmt::format("failed to inspect {} path '{}'", label, path.string()));
  }
  return status;
}

[[nodiscard]] inline bool RecorderPathIsTerminalSymlink(
    const std::filesystem::path& path, std::string_view label) {
  return std::filesystem::is_symlink(RecorderSymlinkStatus(path, label));
}

inline void EnsureRecorderArtifactPathIsUsable(
    const std::filesystem::path& path, std::string_view label) {
  const std::filesystem::file_status status =
      RecorderSymlinkStatus(path, label);
  if (std::filesystem::is_symlink(status)) {
    throw std::runtime_error(fmt::format("{} path '{}' must not be a symlink",
                                         label, path.string()));
  }
  if (!std::filesystem::exists(status)) {
    return;
  }
  if (!std::filesystem::is_regular_file(status)) {
    throw std::runtime_error(fmt::format("{} path '{}' must be a regular file",
                                         label, path.string()));
  }
}

inline void EnsureRecorderParentDirectory(const std::filesystem::path& path) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
}

inline void PreflightSingleOutputPath(const std::filesystem::path& path,
                                      std::string_view label) {
  if (path.empty()) {
    throw std::invalid_argument(
        fmt::format("{} path must not be empty", label));
  }
  EnsureRecorderParentDirectory(path);
  EnsureRecorderArtifactPathIsUsable(path, fmt::format("{} output", label));
}

[[nodiscard]] inline bool RecorderArtifactPathUnavailable(
    const std::filesystem::path& path, std::string_view label) {
  const std::filesystem::file_status status =
      RecorderSymlinkStatus(path, label);
  return std::filesystem::exists(status) || std::filesystem::is_symlink(status);
}

[[nodiscard]] inline BinaryRecorderOutputPaths PreflightBinaryRecorderOutputs(
    std::filesystem::path book_ticker_output_path,
    std::filesystem::path trade_output_path) {
  if (RecorderSamePath(book_ticker_output_path, trade_output_path)) {
    throw std::invalid_argument(
        "book_ticker and trade output paths must be different");
  }
  PreflightSingleOutputPath(book_ticker_output_path, "book_ticker");
  PreflightSingleOutputPath(trade_output_path, "trade");
  return BinaryRecorderOutputPaths{
      .book_ticker_output_path = std::move(book_ticker_output_path),
      .trade_output_path = std::move(trade_output_path),
  };
}

template <typename RecordT, typename Traits>
class TypedBinaryRecorder {
 public:
  TypedBinaryRecorder(std::filesystem::path output_path,
                      RecorderWriteMode write_mode)
      : output_path_(std::move(output_path)) {
    PreflightSingleOutputPath(output_path_, Traits::kFeedName);
    OpenOutput(write_mode);
  }

  TypedBinaryRecorder(const TypedBinaryRecorder&) = delete;
  TypedBinaryRecorder& operator=(const TypedBinaryRecorder&) = delete;

  void OnRecord(const RecordT& record) noexcept {
    if (write_error_) {
      return;
    }

    output_.write(reinterpret_cast<const char*>(&record), sizeof(record));
    if (!output_.good()) {
      write_error_ = true;
      return;
    }
    stats_.Record(record);
  }

  void MarkWriteError() noexcept {
    write_error_ = true;
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

  [[nodiscard]] const TypedRecorderStats<RecordT, Traits>& stats()
      const noexcept {
    return stats_;
  }

  [[nodiscard]] bool write_error() const noexcept {
    return write_error_;
  }

  [[nodiscard]] const std::filesystem::path& output_path() const noexcept {
    return output_path_;
  }

 private:
  void OpenOutput(RecorderWriteMode write_mode) {
    switch (write_mode) {
      case RecorderWriteMode::kTruncate:
        output_.open(output_path_, std::ios::binary | std::ios::trunc);
        if (!output_.is_open()) {
          throw std::runtime_error(fmt::format("failed to open output file '{}'",
                                               output_path_.string()));
        }
        if (!aquila::market_data::WriteMarketDataBinaryHeader(output_,
                                                              Traits::kFeed)) {
          throw std::runtime_error(fmt::format(
              "failed to write market data header to '{}'",
              output_path_.string()));
        }
        return;

      case RecorderWriteMode::kAppend:
        PrepareAppendTarget();
        output_.open(output_path_, std::ios::binary | std::ios::app);
        if (!output_.is_open()) {
          throw std::runtime_error(fmt::format("failed to open output file '{}'",
                                               output_path_.string()));
        }
        return;
    }
  }

  void PrepareAppendTarget() {
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(output_path_, exists_error);
    if (exists_error) {
      throw std::runtime_error(fmt::format(
          "failed to inspect output file '{}': {}", output_path_.string(),
          exists_error.message()));
    }
    if (!exists) {
      std::ofstream create(output_path_, std::ios::binary | std::ios::trunc);
      if (!create.is_open() ||
          !aquila::market_data::WriteMarketDataBinaryHeader(create,
                                                            Traits::kFeed)) {
        throw std::runtime_error(fmt::format(
            "failed to create typed market data file '{}'",
            output_path_.string()));
      }
      return;
    }

    std::error_code size_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(output_path_, size_error);
    if (size_error) {
      throw std::runtime_error(fmt::format(
          "failed to inspect output file '{}': {}", output_path_.string(),
          size_error.message()));
    }
    if (file_size == 0) {
      std::ofstream create(output_path_, std::ios::binary | std::ios::trunc);
      if (!create.is_open() ||
          !aquila::market_data::WriteMarketDataBinaryHeader(create,
                                                            Traits::kFeed)) {
        throw std::runtime_error(fmt::format(
            "failed to initialize typed market data file '{}'",
            output_path_.string()));
      }
      return;
    }

    std::ifstream input(output_path_, std::ios::binary);
    if (!input.is_open()) {
      throw std::runtime_error(fmt::format("failed to open output file '{}'",
                                           output_path_.string()));
    }
    const aquila::market_data::MarketDataBinaryHeader header =
        aquila::market_data::ReadMarketDataBinaryHeader(input, output_path_);
    (void)aquila::market_data::CheckedMarketDataBinaryRecordCount(
        output_path_, file_size, header, Traits::kFeed);
  }

  std::filesystem::path output_path_;
  std::ofstream output_;
  TypedRecorderStats<RecordT, Traits> stats_;
  bool write_error_{false};
};

class BookTickerBinaryRecorder
    : public TypedBinaryRecorder<BookTicker, BookTickerRecorderTraits> {
 public:
  using Base = TypedBinaryRecorder<BookTicker, BookTickerRecorderTraits>;
  using Base::Base;

  void OnBookTicker(const BookTicker& book_ticker) noexcept {
    OnRecord(book_ticker);
  }

  void OnTrade(const Trade&) noexcept {
    MarkWriteError();
  }
};

class TradeBinaryRecorder
    : public TypedBinaryRecorder<Trade, TradeRecorderTraits> {
 public:
  using Base = TypedBinaryRecorder<Trade, TradeRecorderTraits>;
  using Base::Base;

  void OnBookTicker(const BookTicker&) noexcept {
    MarkWriteError();
  }

  void OnTrade(const Trade& trade) noexcept {
    OnRecord(trade);
  }
};

class BinaryRecorder {
 public:
  BinaryRecorder(std::filesystem::path book_ticker_output_path,
                 std::filesystem::path trade_output_path,
                 RecorderWriteMode write_mode)
      : BinaryRecorder(
            PreflightBinaryRecorderOutputs(std::move(book_ticker_output_path),
                                           std::move(trade_output_path)),
            write_mode) {}

  BinaryRecorder(const BinaryRecorder&) = delete;
  BinaryRecorder& operator=(const BinaryRecorder&) = delete;

  void OnBookTicker(const BookTicker& book_ticker) noexcept {
    book_ticker_.OnBookTicker(book_ticker);
  }

  void OnTrade(const Trade& trade) noexcept {
    trade_.OnTrade(trade);
  }

  [[nodiscard]] bool Flush() noexcept {
    const bool book_ok = book_ticker_.Flush();
    const bool trade_ok = trade_.Flush();
    return book_ok && trade_ok;
  }

  [[nodiscard]] bool write_error() const noexcept {
    return book_ticker_.write_error() || trade_.write_error();
  }

  [[nodiscard]] const BookTickerRecorderStats& book_ticker_stats()
      const noexcept {
    return book_ticker_.stats();
  }

  [[nodiscard]] const TradeRecorderStats& trade_stats() const noexcept {
    return trade_.stats();
  }

  [[nodiscard]] const std::filesystem::path& book_ticker_output_path()
      const noexcept {
    return book_ticker_.output_path();
  }

  [[nodiscard]] const std::filesystem::path& trade_output_path()
      const noexcept {
    return trade_.output_path();
  }

  [[nodiscard]] std::uint64_t book_ticker_segments_completed() const noexcept {
    return 0;
  }

  [[nodiscard]] std::uint64_t trade_segments_completed() const noexcept {
    return 0;
  }

 private:
  BinaryRecorder(BinaryRecorderOutputPaths paths, RecorderWriteMode write_mode)
      : book_ticker_(std::move(paths.book_ticker_output_path), write_mode),
        trade_(std::move(paths.trade_output_path), write_mode) {}

  BookTickerBinaryRecorder book_ticker_;
  TradeBinaryRecorder trade_;
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

using RecorderClock = std::function<RecorderTimeSnapshot()>;

struct RecorderRotationConfig {
  bool enabled{false};
  std::uint32_t rotation_interval_sec{3600};
  std::filesystem::path output_dir;
  std::string file_prefix;
  std::filesystem::path manifest_path;
};

[[nodiscard]] inline std::string FormatRecorderWallTime(
    std::chrono::system_clock::time_point wall) {
  const std::time_t time = std::chrono::system_clock::to_time_t(wall);
  std::tm tm{};
  gmtime_r(&time, &tm);
  return fmt::format("{:04}{:02}{:02}_{:02}{:02}{:02}", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                     tm.tm_sec);
}

struct RecorderSegmentPaths {
  std::filesystem::path tmp_path;
  std::filesystem::path final_path;
};

[[nodiscard]] inline RecorderSegmentPaths BuildRecorderSegmentPaths(
    const RecorderRotationConfig& config, const RecorderTimeSnapshot& now,
    std::uint64_t sequence) {
  const std::string file_name =
      fmt::format("{}_{}_{:06}.bin", config.file_prefix,
                  FormatRecorderWallTime(now.wall), sequence);
  std::filesystem::path final_path = config.output_dir / file_name;
  std::filesystem::path tmp_path = final_path;
  tmp_path += ".tmp";
  return RecorderSegmentPaths{
      .tmp_path = std::move(tmp_path),
      .final_path = std::move(final_path),
  };
}

inline void PreflightRotationConfig(const RecorderRotationConfig& config,
                                    std::string_view label) {
  if (config.rotation_interval_sec == 0) {
    throw std::invalid_argument(
        fmt::format("{} rotation interval must be positive", label));
  }
  if (config.output_dir.empty()) {
    throw std::invalid_argument(
        fmt::format("{} rotation output_dir must not be empty", label));
  }
  if (config.file_prefix.empty()) {
    throw std::invalid_argument(
        fmt::format("{} rotation file_prefix must not be empty", label));
  }
  if (config.manifest_path.empty()) {
    throw std::invalid_argument(
        fmt::format("{} rotation manifest_path must not be empty", label));
  }
}

inline void PrepareRotationDirectories(const RecorderRotationConfig& config) {
  std::filesystem::create_directories(config.output_dir);
  if (config.manifest_path.has_parent_path()) {
    std::filesystem::create_directories(config.manifest_path.parent_path());
  }
}

inline void PreflightRotationManifestPath(const RecorderRotationConfig& config,
                                          std::string_view label) {
  EnsureRecorderArtifactPathIsUsable(
      config.manifest_path, fmt::format("{} rotation manifest", label));
}

inline void TruncateRotationManifest(const RecorderRotationConfig& config,
                                     std::string_view label) {
  std::ofstream manifest(config.manifest_path,
                         std::ios::binary | std::ios::trunc);
  if (!manifest.is_open()) {
    throw std::runtime_error(
        fmt::format("failed to initialize {} rotation manifest '{}'", label,
                    config.manifest_path.string()));
  }
}

inline void PreflightInitialRotationSegment(
    const RecorderRotationConfig& config, const RecorderTimeSnapshot& now,
    std::string_view label) {
  const RecorderSegmentPaths paths = BuildRecorderSegmentPaths(config, now, 1);
  if (RecorderArtifactPathUnavailable(paths.tmp_path, label)) {
    throw std::runtime_error(
        fmt::format("{} rotation tmp segment '{}' is not available", label,
                    paths.tmp_path.string()));
  }
  if (RecorderArtifactPathUnavailable(paths.final_path, label)) {
    throw std::runtime_error(
        fmt::format("{} rotation final segment '{}' is not available", label,
                    paths.final_path.string()));
  }
}

struct RotatingBinaryRecorderInputs {
  RecorderRotationConfig book_ticker_config;
  RecorderRotationConfig trade_config;
  RecorderClock clock;
  RecorderTimeSnapshot initial_now;
};

[[nodiscard]] inline RotatingBinaryRecorderInputs
PreflightRotatingBinaryRecorderInputs(RecorderRotationConfig book_ticker_config,
                                      RecorderRotationConfig trade_config,
                                      RecorderClock clock) {
  if (!clock) {
    clock = SystemRecorderTimeSnapshot;
  }
  if (RecorderSamePath(book_ticker_config.manifest_path,
                       trade_config.manifest_path)) {
    throw std::invalid_argument(
        "book_ticker and trade rotation manifest paths must be different");
  }
  if (RecorderSamePath(
          book_ticker_config.output_dir / book_ticker_config.file_prefix,
          trade_config.output_dir / trade_config.file_prefix)) {
    throw std::invalid_argument(
        "book_ticker and trade rotation segment namespaces must be different");
  }

  PreflightRotationConfig(book_ticker_config, "book_ticker");
  PreflightRotationConfig(trade_config, "trade");
  PrepareRotationDirectories(book_ticker_config);
  PrepareRotationDirectories(trade_config);
  PreflightRotationManifestPath(book_ticker_config, "book_ticker");
  PreflightRotationManifestPath(trade_config, "trade");

  const RecorderTimeSnapshot initial_now = clock();
  PreflightInitialRotationSegment(book_ticker_config, initial_now,
                                  "book_ticker");
  PreflightInitialRotationSegment(trade_config, initial_now, "trade");
  TruncateRotationManifest(book_ticker_config, "book_ticker");
  TruncateRotationManifest(trade_config, "trade");
  return RotatingBinaryRecorderInputs{
      .book_ticker_config = std::move(book_ticker_config),
      .trade_config = std::move(trade_config),
      .clock = std::move(clock),
      .initial_now = initial_now,
  };
}

template <typename RecordT, typename Traits>
class TypedRotatingBinaryRecorder {
 public:
  using Clock = RecorderClock;

  explicit TypedRotatingBinaryRecorder(RecorderRotationConfig config,
                                       Clock clock = Clock{})
      : config_(std::move(config)), clock_(std::move(clock)) {
    PreflightRotationConfig(config_, Traits::kFeedName);
    if (!clock_) {
      clock_ = SystemRecorderTimeSnapshot;
    }
    PrepareRotationDirectories(config_);
    PreflightRotationManifestPath(config_, Traits::kFeedName);
    if (!OpenSegment(clock_())) {
      throw std::runtime_error(fmt::format(
          "failed to open rotation segment '{}'", current_tmp_path_.string()));
    }
  }

  TypedRotatingBinaryRecorder(RecorderRotationConfig config, Clock clock,
                              RecorderTimeSnapshot initial_now)
      : config_(std::move(config)), clock_(std::move(clock)) {
    PreflightRotationConfig(config_, Traits::kFeedName);
    if (!clock_) {
      clock_ = SystemRecorderTimeSnapshot;
    }
    PrepareRotationDirectories(config_);
    PreflightRotationManifestPath(config_, Traits::kFeedName);
    if (!OpenSegment(initial_now)) {
      throw std::runtime_error(fmt::format(
          "failed to open rotation segment '{}'", current_tmp_path_.string()));
    }
  }

  TypedRotatingBinaryRecorder(const TypedRotatingBinaryRecorder&) = delete;
  TypedRotatingBinaryRecorder& operator=(const TypedRotatingBinaryRecorder&) =
      delete;

  void OnRecord(const RecordT& record) noexcept {
    if (write_error_) {
      return;
    }

    try {
      OnRecordImpl(record);
    } catch (...) {
      write_error_ = true;
    }
  }

  void MarkWriteError() noexcept {
    write_error_ = true;
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

  [[nodiscard]] const TypedRecorderStats<RecordT, Traits>& stats()
      const noexcept {
    return stats_;
  }

  [[nodiscard]] bool write_error() const noexcept {
    return write_error_;
  }

  [[nodiscard]] std::uint64_t segments_completed() const noexcept {
    return segments_completed_;
  }

 private:
  void OnRecordImpl(const RecordT& record) {
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

    output_.write(reinterpret_cast<const char*>(&record), sizeof(record));
    if (!output_.good()) {
      write_error_ = true;
      return;
    }
    stats_.Record(record);
    current_stats_.Record(record);
  }

  [[nodiscard]] static std::string FormatWallTime(
      std::chrono::system_clock::time_point wall) {
    return FormatRecorderWallTime(wall);
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
    current_stats_ = TypedRecorderStats<RecordT, Traits>{};
    current_sequence_ = next_sequence_++;
    const RecorderSegmentPaths paths =
        BuildRecorderSegmentPaths(config_, now, current_sequence_);
    current_final_path_ = paths.final_path;
    current_tmp_path_ = paths.tmp_path;
    next_rotation_deadline_ =
        now.steady + std::chrono::seconds{config_.rotation_interval_sec};

    if (RecorderArtifactPathUnavailable(current_tmp_path_, Traits::kFeedName)) {
      write_error_ = true;
      return false;
    }
    if (RecorderArtifactPathUnavailable(current_final_path_,
                                        Traits::kFeedName)) {
      write_error_ = true;
      return false;
    }

    output_.open(current_tmp_path_, std::ios::binary | std::ios::trunc);
    if (!output_.is_open()) {
      write_error_ = true;
      return false;
    }
    if (!aquila::market_data::WriteMarketDataBinaryHeader(output_,
                                                          Traits::kFeed)) {
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
    const bool flush_ok = output_.good();
    output_.close();
    const bool close_ok = output_.good();
    if (!flush_ok || !close_ok) {
      write_error_ = true;
      return false;
    }

    if (current_stats_.total_records == 0) {
      std::error_code remove_error;
      std::filesystem::remove(current_tmp_path_, remove_error);
      if (remove_error) {
        write_error_ = true;
        return false;
      }
      return true;
    }

    std::error_code size_error;
    const std::uintmax_t file_size =
        std::filesystem::file_size(current_tmp_path_, size_error);
    const std::uintmax_t expected_size =
        sizeof(aquila::market_data::MarketDataBinaryHeader) +
        current_stats_.total_records * sizeof(RecordT);
    if (size_error || file_size != expected_size) {
      write_error_ = true;
      return false;
    }
    if (RecorderArtifactPathUnavailable(current_final_path_,
                                        Traits::kFeedName)) {
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
        "\"bytes\":{},\"format\":\"{}\",\"version\":{},\"feed\":\"{}\","
        "\"header_bytes\":{},\"record_size\":{},"
        "\"first_exchange_ns\":{},"
        "\"last_exchange_ns\":{},\"first_local_ns\":{},"
        "\"last_local_ns\":{},\"closed_reason\":\"{}\"}}\n",
        current_sequence_, JsonEscape(current_final_path_.string()),
        current_stats_.total_records, file_size,
        aquila::market_data::kMarketDataBinaryFormatName,
        aquila::market_data::kMarketDataBinaryVersion, Traits::kFeedName,
        sizeof(aquila::market_data::MarketDataBinaryHeader), sizeof(RecordT),
        current_stats_.first_exchange_ns.value_or(0),
        current_stats_.last_exchange_ns.value_or(0),
        current_stats_.first_local_ns.value_or(0),
        current_stats_.last_local_ns.value_or(0), JsonEscape(reason));
    manifest.flush();
    const bool flush_ok = manifest.good();
    manifest.close();
    const bool close_ok = manifest.good();
    return flush_ok && close_ok;
  }

  RecorderRotationConfig config_;
  Clock clock_;
  std::ofstream output_;
  std::filesystem::path current_tmp_path_;
  std::filesystem::path current_final_path_;
  TypedRecorderStats<RecordT, Traits> stats_;
  TypedRecorderStats<RecordT, Traits> current_stats_;
  std::chrono::steady_clock::time_point next_rotation_deadline_{};
  std::uint64_t next_sequence_{1};
  std::uint64_t current_sequence_{0};
  std::uint64_t segments_completed_{0};
  bool write_error_{false};
};

class RotatingBookTickerBinaryRecorder
    : public TypedRotatingBinaryRecorder<BookTicker, BookTickerRecorderTraits> {
 public:
  using Base =
      TypedRotatingBinaryRecorder<BookTicker, BookTickerRecorderTraits>;
  using Clock = Base::Clock;
  using Base::Base;

  void OnBookTicker(const BookTicker& book_ticker) noexcept {
    OnRecord(book_ticker);
  }

  void OnTrade(const Trade&) noexcept {
    MarkWriteError();
  }
};

class RotatingTradeBinaryRecorder
    : public TypedRotatingBinaryRecorder<Trade, TradeRecorderTraits> {
 public:
  using Base = TypedRotatingBinaryRecorder<Trade, TradeRecorderTraits>;
  using Clock = Base::Clock;
  using Base::Base;

  void OnBookTicker(const BookTicker&) noexcept {
    MarkWriteError();
  }

  void OnTrade(const Trade& trade) noexcept {
    OnRecord(trade);
  }
};

class RotatingBinaryRecorder {
 public:
  using Clock = RecorderClock;

  RotatingBinaryRecorder(RecorderRotationConfig book_ticker_config,
                         RecorderRotationConfig trade_config,
                         Clock clock = Clock{})
      : RotatingBinaryRecorder(PreflightRotatingBinaryRecorderInputs(
            std::move(book_ticker_config), std::move(trade_config),
            std::move(clock))) {}

  RotatingBinaryRecorder(const RotatingBinaryRecorder&) = delete;
  RotatingBinaryRecorder& operator=(const RotatingBinaryRecorder&) = delete;

  void OnBookTicker(const BookTicker& book_ticker) noexcept {
    book_ticker_.OnBookTicker(book_ticker);
  }

  void OnTrade(const Trade& trade) noexcept {
    trade_.OnTrade(trade);
  }

  [[nodiscard]] bool Flush() noexcept {
    const bool book_ok = book_ticker_.Flush();
    const bool trade_ok = trade_.Flush();
    return book_ok && trade_ok;
  }

  [[nodiscard]] bool write_error() const noexcept {
    return book_ticker_.write_error() || trade_.write_error();
  }

  [[nodiscard]] const BookTickerRecorderStats& book_ticker_stats()
      const noexcept {
    return book_ticker_.stats();
  }

  [[nodiscard]] const TradeRecorderStats& trade_stats() const noexcept {
    return trade_.stats();
  }

  [[nodiscard]] std::uint64_t book_ticker_segments_completed() const noexcept {
    return book_ticker_.segments_completed();
  }

  [[nodiscard]] std::uint64_t trade_segments_completed() const noexcept {
    return trade_.segments_completed();
  }

 private:
  explicit RotatingBinaryRecorder(RotatingBinaryRecorderInputs inputs)
      : book_ticker_(std::move(inputs.book_ticker_config), inputs.clock,
                     inputs.initial_now),
        trade_(std::move(inputs.trade_config), std::move(inputs.clock),
               inputs.initial_now) {}

  RotatingBookTickerBinaryRecorder book_ticker_;
  RotatingTradeBinaryRecorder trade_;
};

}  // namespace aquila::tools::market_data
