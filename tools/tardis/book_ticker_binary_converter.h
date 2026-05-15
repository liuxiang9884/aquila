#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <fast_float/fast_float.h>
#include <fmt/core.h>

#include "core/common/types.h"
#include "core/market_data/types.h"

namespace aquila::tools::tardis {

static_assert(std::is_trivially_copyable_v<BookTicker>);
static_assert(std::is_standard_layout_v<BookTicker>);

struct BookTickerCsvSource {
  Exchange exchange{Exchange::kBinance};
  std::string csv_exchange;
  std::string csv_symbol;
  std::int32_t symbol_id{-1};
};

struct ParsedBookTickerRecord {
  BookTicker ticker{};
  std::uint64_t input_sequence{};
};

struct BookTickerCsvInput {
  std::istream* input{};
  BookTickerCsvSource source;
};

struct BookTickerBinaryWriteStats {
  std::uint64_t records_written{};
  std::optional<std::int64_t> first_exchange_ns;
  std::optional<std::int64_t> last_exchange_ns;
  std::vector<std::uint64_t> records_by_input;
};

namespace detail {

inline constexpr std::string_view kBookTickerCsvHeader =
    "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,bid_price,"
    "bid_amount";
inline constexpr std::size_t kBookTickerCsvFieldCount = 8;

struct BookTickerCsvFields {
  std::array<std::string_view, kBookTickerCsvFieldCount> values{};
};

inline std::string_view TrimLineEnd(std::string_view line) noexcept {
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.remove_suffix(1);
  }
  return line;
}

inline BookTickerCsvFields SplitBookTickerCsvLine(std::string_view line,
                                                  std::uint64_t line_number) {
  line = TrimLineEnd(line);
  BookTickerCsvFields fields;
  std::size_t field_index = 0;
  std::size_t field_start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i != line.size() && line[i] != ',') {
      continue;
    }
    if (field_index == fields.values.size()) {
      throw std::runtime_error(
          fmt::format("too many CSV fields at line {}", line_number));
    }
    fields.values[field_index] = line.substr(field_start, i - field_start);
    ++field_index;
    field_start = i + 1;
  }
  if (field_index != fields.values.size()) {
    throw std::runtime_error(
        fmt::format("expected {} CSV fields at line {}, got {}",
                    fields.values.size(), line_number, field_index));
  }
  return fields;
}

template <typename T>
inline T ParseIntegralField(std::string_view text, std::string_view field_name,
                            std::uint64_t line_number) {
  static_assert(std::is_integral_v<T>);
  if (text.empty()) {
    throw std::runtime_error(
        fmt::format("empty {} at line {}", field_name, line_number));
  }
  T value{};
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw std::runtime_error(
        fmt::format("invalid {} at line {}", field_name, line_number));
  }
  return value;
}

inline double ParseDoubleField(std::string_view text,
                               std::string_view field_name,
                               std::uint64_t line_number) {
  if (text.empty()) {
    throw std::runtime_error(
        fmt::format("empty {} at line {}", field_name, line_number));
  }
  double value{};
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const auto result = fast_float::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw std::runtime_error(
        fmt::format("invalid {} at line {}", field_name, line_number));
  }
  return value;
}

inline std::int64_t TimestampUsToNs(std::int64_t timestamp_us,
                                    std::string_view field_name,
                                    std::uint64_t line_number) {
  if (timestamp_us < 0) {
    throw std::runtime_error(
        fmt::format("negative {} at line {}", field_name, line_number));
  }
  constexpr std::int64_t kScale = 1000;
  if (timestamp_us > std::numeric_limits<std::int64_t>::max() / kScale) {
    throw std::runtime_error(fmt::format("{} overflows nanoseconds at line {}",
                                         field_name, line_number));
  }
  return timestamp_us * kScale;
}

inline int ExchangeOrder(Exchange exchange) noexcept {
  return static_cast<int>(exchange);
}

inline bool RecordLess(const ParsedBookTickerRecord& lhs,
                       const ParsedBookTickerRecord& rhs) noexcept {
  if (lhs.ticker.exchange_ns != rhs.ticker.exchange_ns) {
    return lhs.ticker.exchange_ns < rhs.ticker.exchange_ns;
  }
  if (lhs.ticker.exchange != rhs.ticker.exchange) {
    return ExchangeOrder(lhs.ticker.exchange) <
           ExchangeOrder(rhs.ticker.exchange);
  }
  return lhs.input_sequence < rhs.input_sequence;
}

inline ParsedBookTickerRecord ParseBookTickerCsvLine(
    std::string_view line, const BookTickerCsvSource& source,
    std::uint64_t input_sequence, std::uint64_t line_number) {
  const BookTickerCsvFields fields = SplitBookTickerCsvLine(line, line_number);
  const std::string_view csv_exchange = fields.values[0];
  const std::string_view csv_symbol = fields.values[1];
  if (csv_exchange != source.csv_exchange) {
    throw std::runtime_error(
        fmt::format("unexpected exchange '{}' at line {}, expected '{}'",
                    csv_exchange, line_number, source.csv_exchange));
  }
  if (csv_symbol != source.csv_symbol) {
    throw std::runtime_error(
        fmt::format("unexpected symbol '{}' at line {}, expected '{}'",
                    csv_symbol, line_number, source.csv_symbol));
  }

  const std::int64_t timestamp_us = ParseIntegralField<std::int64_t>(
      fields.values[2], "timestamp", line_number);
  const std::int64_t local_timestamp_us = ParseIntegralField<std::int64_t>(
      fields.values[3], "local_timestamp", line_number);
  const double ask_amount =
      ParseDoubleField(fields.values[4], "ask_amount", line_number);
  const double ask_price =
      ParseDoubleField(fields.values[5], "ask_price", line_number);
  const double bid_price =
      ParseDoubleField(fields.values[6], "bid_price", line_number);
  const double bid_amount =
      ParseDoubleField(fields.values[7], "bid_amount", line_number);

  return ParsedBookTickerRecord{
      .ticker =
          BookTicker{
              .id = static_cast<std::int64_t>(input_sequence),
              .symbol_id = source.symbol_id,
              .exchange = source.exchange,
              .exchange_ns =
                  TimestampUsToNs(timestamp_us, "timestamp", line_number),
              .local_ns = TimestampUsToNs(local_timestamp_us, "local_timestamp",
                                          line_number),
              .bid_price = bid_price,
              .bid_volume = bid_amount,
              .ask_price = ask_price,
              .ask_volume = ask_amount,
          },
      .input_sequence = input_sequence,
  };
}

inline void WriteOneBookTicker(std::ostream& output, const BookTicker& ticker) {
  output.write(reinterpret_cast<const char*>(&ticker), sizeof(ticker));
  if (!output.good()) {
    throw std::runtime_error("failed to write book ticker record");
  }
}

}  // namespace detail

class BookTickerCsvReader {
 public:
  BookTickerCsvReader(std::istream& input, BookTickerCsvSource source)
      : input_(&input), source_(std::move(source)) {
    ReadHeader();
  }

  bool Next(ParsedBookTickerRecord* out) {
    if (out == nullptr) {
      throw std::invalid_argument("out must not be null");
    }

    std::string line;
    while (std::getline(*input_, line)) {
      ++line_number_;
      const std::string_view trimmed = detail::TrimLineEnd(line);
      if (trimmed.empty()) {
        continue;
      }
      ParsedBookTickerRecord record = detail::ParseBookTickerCsvLine(
          trimmed, source_, input_sequence_, line_number_);
      if (last_exchange_ns_.has_value() &&
          record.ticker.exchange_ns < *last_exchange_ns_) {
        throw std::runtime_error(fmt::format(
            "CSV source is not sorted by timestamp at line {}", line_number_));
      }
      last_exchange_ns_ = record.ticker.exchange_ns;
      ++input_sequence_;
      *out = record;
      return true;
    }
    if (!input_->eof()) {
      throw std::runtime_error("failed to read CSV input stream");
    }
    return false;
  }

 private:
  void ReadHeader() {
    std::string header;
    if (!std::getline(*input_, header)) {
      throw std::runtime_error("empty book ticker CSV input");
    }
    ++line_number_;
    if (detail::TrimLineEnd(header) != detail::kBookTickerCsvHeader) {
      throw std::runtime_error("unexpected book ticker CSV header");
    }
  }

  std::istream* input_{};
  BookTickerCsvSource source_;
  std::uint64_t line_number_{0};
  std::uint64_t input_sequence_{0};
  std::optional<std::int64_t> last_exchange_ns_;
};

inline std::vector<ParsedBookTickerRecord> ReadBookTickerCsv(
    std::istream& input, const BookTickerCsvSource& source) {
  BookTickerCsvReader reader(input, source);
  std::vector<ParsedBookTickerRecord> records;
  ParsedBookTickerRecord record;
  while (reader.Next(&record)) {
    records.push_back(record);
  }
  return records;
}

inline std::vector<BookTicker> MergeBookTickerRecords(
    std::initializer_list<std::span<const ParsedBookTickerRecord>> sources) {
  std::vector<ParsedBookTickerRecord> parsed_records;
  std::size_t total_records = 0;
  for (const std::span<const ParsedBookTickerRecord> source : sources) {
    total_records += source.size();
  }
  parsed_records.reserve(total_records);
  for (const std::span<const ParsedBookTickerRecord> source : sources) {
    parsed_records.insert(parsed_records.end(), source.begin(), source.end());
  }

  std::stable_sort(parsed_records.begin(), parsed_records.end(),
                   detail::RecordLess);

  std::vector<BookTicker> merged;
  merged.reserve(parsed_records.size());
  for (std::size_t i = 0; i < parsed_records.size(); ++i) {
    BookTicker ticker = parsed_records[i].ticker;
    if (i >
        static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::runtime_error("too many book ticker records");
    }
    ticker.id = static_cast<std::int64_t>(i);
    merged.push_back(ticker);
  }
  return merged;
}

inline void WriteBookTickerBinaryFile(const std::filesystem::path& output_path,
                                      std::span<const BookTicker> records) {
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path());
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error(
        fmt::format("failed to open output file '{}'", output_path.string()));
  }
  if (!records.empty()) {
    output.write(reinterpret_cast<const char*>(records.data()),
                 static_cast<std::streamsize>(records.size_bytes()));
  }
  if (!output.good()) {
    throw std::runtime_error(
        fmt::format("failed to write output file '{}'", output_path.string()));
  }
}

inline std::vector<BookTicker> ReadBookTickerBinaryFile(
    const std::filesystem::path& input_path) {
  const std::uintmax_t file_size = std::filesystem::file_size(input_path);
  if (file_size % sizeof(BookTicker) != 0) {
    throw std::runtime_error("book ticker binary file has trailing bytes");
  }
  const std::uintmax_t record_count = file_size / sizeof(BookTicker);
  if (record_count >
      static_cast<std::uintmax_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error("book ticker binary file is too large");
  }

  std::ifstream input(input_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error(
        fmt::format("failed to open input file '{}'", input_path.string()));
  }

  std::vector<BookTicker> records(static_cast<std::size_t>(record_count));
  if (!records.empty()) {
    input.read(
        reinterpret_cast<char*>(records.data()),
        static_cast<std::streamsize>(records.size() * sizeof(BookTicker)));
    if (!input.good()) {
      throw std::runtime_error("failed to read book ticker binary file");
    }
  }
  return records;
}

inline BookTickerBinaryWriteStats WriteMergedBookTickerCsvStreams(
    const std::filesystem::path& output_path,
    std::span<const BookTickerCsvInput> inputs) {
  if (inputs.empty()) {
    throw std::invalid_argument("at least one input stream is required");
  }
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path());
  }

  struct ReaderState {
    BookTickerCsvReader reader;
    ParsedBookTickerRecord current{};
    bool has_current{false};
  };

  std::vector<ReaderState> states;
  states.reserve(inputs.size());
  BookTickerBinaryWriteStats stats;
  stats.records_by_input.assign(inputs.size(), 0);

  for (const BookTickerCsvInput& input : inputs) {
    if (input.input == nullptr) {
      throw std::invalid_argument("input stream must not be null");
    }
    ReaderState state{
        .reader = BookTickerCsvReader(*input.input, input.source),
    };
    state.has_current = state.reader.Next(&state.current);
    states.push_back(std::move(state));
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error(
        fmt::format("failed to open output file '{}'", output_path.string()));
  }

  while (true) {
    std::optional<std::size_t> best_index;
    for (std::size_t i = 0; i < states.size(); ++i) {
      if (!states[i].has_current) {
        continue;
      }
      if (!best_index.has_value() ||
          detail::RecordLess(states[i].current, states[*best_index].current)) {
        best_index = i;
      }
    }
    if (!best_index.has_value()) {
      break;
    }

    BookTicker ticker = states[*best_index].current.ticker;
    if (stats.records_written >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      throw std::runtime_error("too many book ticker records");
    }
    ticker.id = static_cast<std::int64_t>(stats.records_written);
    if (stats.last_exchange_ns.has_value() &&
        ticker.exchange_ns < *stats.last_exchange_ns) {
      throw std::runtime_error("merged book ticker records are not sorted");
    }
    if (!stats.first_exchange_ns.has_value()) {
      stats.first_exchange_ns = ticker.exchange_ns;
    }
    stats.last_exchange_ns = ticker.exchange_ns;

    detail::WriteOneBookTicker(output, ticker);
    ++stats.records_written;
    ++stats.records_by_input[*best_index];
    states[*best_index].has_current =
        states[*best_index].reader.Next(&states[*best_index].current);
  }

  if (!output.good()) {
    throw std::runtime_error(
        fmt::format("failed to write output file '{}'", output_path.string()));
  }
  return stats;
}

}  // namespace aquila::tools::tardis
