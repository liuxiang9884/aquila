#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <istream>
#include <memory>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <CLI/CLI.hpp>
#include <fmt/core.h>

#include "core/config/instrument_catalog.h"
#include "tools/tardis/book_ticker_binary_converter.h"

namespace {

using aquila::Exchange;
namespace tardis = aquila::tools::tardis;

struct CliOptions {
  std::filesystem::path data_root = DefaultDataRoot();
  std::filesystem::path instrument_catalog{
      "config/instruments/usdt_futures.csv"};
  std::string symbol{"ORDI_USDT"};
  std::string start_date;
  std::string end_date;
  std::filesystem::path output_dir;

  static std::filesystem::path DefaultDataRoot() {
    const char* const home = std::getenv("HOME");
    if (home == nullptr || std::string_view{home}.empty()) {
      return std::filesystem::path{"tardis"};
    }
    return std::filesystem::path{home} / "tardis";
  }
};

std::string ShellQuote(std::string_view text) {
  std::string quoted{"'"};
  for (const char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(c);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

class PipeInputStreamBuffer : public std::streambuf {
 public:
  explicit PipeInputStreamBuffer(FILE* file) : file_(file) {
    setg(buffer_.data(), buffer_.data() + buffer_.size(),
         buffer_.data() + buffer_.size());
  }

 protected:
  int_type underflow() override {
    if (gptr() < egptr()) {
      return traits_type::to_int_type(*gptr());
    }
    const std::size_t bytes =
        std::fread(buffer_.data(), 1, buffer_.size(), file_);
    if (bytes == 0) {
      return traits_type::eof();
    }
    setg(buffer_.data(), buffer_.data(), buffer_.data() + bytes);
    return traits_type::to_int_type(*gptr());
  }

 private:
  FILE* file_{};
  std::array<char, 64 * 1024> buffer_{};
};

class GzipInputStream {
 public:
  explicit GzipInputStream(const std::filesystem::path& path)
      : pipe_(OpenPipe(path)), buffer_(pipe_), stream_(&buffer_) {}

  GzipInputStream(const GzipInputStream&) = delete;
  GzipInputStream& operator=(const GzipInputStream&) = delete;

  ~GzipInputStream() {
    if (pipe_ != nullptr) {
      static_cast<void>(::pclose(pipe_));
    }
  }

  std::istream& stream() {
    return stream_;
  }

  void CloseOrThrow() {
    if (pipe_ == nullptr) {
      return;
    }
    const int status = ::pclose(pipe_);
    pipe_ = nullptr;
    if (status != 0) {
      throw std::runtime_error(
          fmt::format("gzip command failed with status {}", status));
    }
  }

 private:
  static FILE* OpenPipe(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
      throw std::runtime_error(
          fmt::format("input file does not exist: {}", path.string()));
    }
    const std::string command =
        fmt::format("gzip -cd -- {}", ShellQuote(path.string()));
    FILE* const pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
      throw std::runtime_error(
          fmt::format("failed to run gzip: {}", std::strerror(errno)));
    }
    return pipe;
  }

  FILE* pipe_{};
  PipeInputStreamBuffer buffer_;
  std::istream stream_;
};

class OwnedInputStream {
 public:
  explicit OwnedInputStream(const std::filesystem::path& path) {
    if (path.extension() == ".gz") {
      gzip_ = std::make_unique<GzipInputStream>(path);
      stream_ = &gzip_->stream();
      return;
    }

    file_.open(path, std::ios::binary);
    if (!file_.is_open()) {
      throw std::runtime_error(
          fmt::format("failed to open input file '{}'", path.string()));
    }
    stream_ = &file_;
  }

  std::istream& stream() {
    return *stream_;
  }

  void CloseOrThrow() {
    if (gzip_ != nullptr) {
      gzip_->CloseOrThrow();
    }
  }

 private:
  std::ifstream file_;
  std::unique_ptr<GzipInputStream> gzip_;
  std::istream* stream_{};
};

int ParseFixedInt(std::string_view text, std::string_view field_name) {
  int value{};
  const char* const begin = text.data();
  const char* const end = text.data() + text.size();
  const auto result = std::from_chars(begin, end, value);
  if (result.ec != std::errc{} || result.ptr != end) {
    throw std::invalid_argument(fmt::format("invalid {}", field_name));
  }
  return value;
}

std::chrono::sys_days ParseDate(std::string_view text) {
  if (text.size() != 8) {
    throw std::invalid_argument("date must use YYYYMMDD format");
  }
  const int year = ParseFixedInt(text.substr(0, 4), "year");
  const unsigned month =
      static_cast<unsigned>(ParseFixedInt(text.substr(4, 2), "month"));
  const unsigned day =
      static_cast<unsigned>(ParseFixedInt(text.substr(6, 2), "day"));
  const std::chrono::year_month_day ymd{std::chrono::year{year},
                                        std::chrono::month{month},
                                        std::chrono::day{day}};
  if (!ymd.ok()) {
    throw std::invalid_argument("invalid date");
  }
  return std::chrono::sys_days{ymd};
}

std::string FormatDate(std::chrono::sys_days date) {
  const std::chrono::year_month_day ymd{date};
  return fmt::format("{:04}{:02}{:02}", static_cast<int>(ymd.year()),
                     static_cast<unsigned>(ymd.month()),
                     static_cast<unsigned>(ymd.day()));
}

std::string TardisExchangeDir(Exchange exchange) {
  switch (exchange) {
    case Exchange::kBinance:
      return "binance-futures";
    case Exchange::kGate:
      return "gate-io-futures";
    default:
      throw std::invalid_argument("unsupported exchange for Tardis input");
  }
}

tardis::BookTickerCsvSource MakeSource(
    const aquila::config::InstrumentCatalog& catalog, Exchange exchange,
    std::string_view symbol) {
  const aquila::config::InstrumentInfo* const instrument =
      catalog.Find(exchange, symbol);
  if (instrument == nullptr) {
    throw std::runtime_error(fmt::format(
        "missing instrument catalog row for symbol '{}' exchange {}", symbol,
        static_cast<int>(exchange)));
  }

  return tardis::BookTickerCsvSource{
      .exchange = exchange,
      .csv_exchange = TardisExchangeDir(exchange),
      .csv_symbol = instrument->exchange_symbol,
      .symbol_id = instrument->symbol_id,
  };
}

std::filesystem::path BuildInputPath(const std::filesystem::path& data_root,
                                     const tardis::BookTickerCsvSource& source,
                                     std::string_view date) {
  return data_root / source.csv_exchange / "book_ticker" / std::string{date} /
         fmt::format("{}-book_ticker-{}.csv.gz", source.csv_symbol, date);
}

std::filesystem::path ResolveOutputDir(const CliOptions& options) {
  if (!options.output_dir.empty()) {
    return options.output_dir;
  }
  return options.data_root / "merged_book_ticker" / options.symbol;
}

std::uintmax_t ExpectedOutputSize(
    const tardis::BookTickerBinaryWriteStats& stats) {
  return static_cast<std::uintmax_t>(stats.records_written) *
         static_cast<std::uintmax_t>(sizeof(aquila::BookTicker));
}

std::string TemporaryOutputSuffix(std::string_view date) {
  return fmt::format(".tmp.{}.{}", date, static_cast<long>(::getpid()));
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  CLI::App app{
      "Convert Tardis book_ticker CSV gzip files to Aquila BookTicker binary"};
  app.add_option("--data-root", options.data_root,
                 "Root of the local Tardis mirror")
      ->capture_default_str();
  app.add_option("--instrument-catalog", options.instrument_catalog,
                 "Instrument catalog CSV path")
      ->capture_default_str();
  app.add_option("--symbol", options.symbol, "Canonical symbol")
      ->capture_default_str();
  app.add_option("--start-date", options.start_date, "Start date YYYYMMDD")
      ->required();
  app.add_option("--end-date", options.end_date, "End date YYYYMMDD")
      ->required();
  app.add_option("--output-dir", options.output_dir,
                 "Output directory; defaults to data-root/merged_book_ticker/"
                 "<symbol>");
  CLI11_PARSE(app, argc, argv);

  try {
    const auto catalog_result = aquila::config::LoadInstrumentCatalogFromCsv(
        options.instrument_catalog);
    if (!catalog_result.ok) {
      throw std::runtime_error(catalog_result.error);
    }
    const aquila::config::InstrumentCatalog& catalog = catalog_result.value;
    const tardis::BookTickerCsvSource binance_source =
        MakeSource(catalog, Exchange::kBinance, options.symbol);
    const tardis::BookTickerCsvSource gate_source =
        MakeSource(catalog, Exchange::kGate, options.symbol);

    const std::chrono::sys_days start = ParseDate(options.start_date);
    const std::chrono::sys_days end = ParseDate(options.end_date);
    if (end < start) {
      throw std::invalid_argument("end-date must be >= start-date");
    }

    const std::filesystem::path output_dir = ResolveOutputDir(options);
    for (std::chrono::sys_days day = start; day <= end;
         day += std::chrono::days{1}) {
      const std::string date = FormatDate(day);
      const std::filesystem::path binance_path =
          BuildInputPath(options.data_root, binance_source, date);
      const std::filesystem::path gate_path =
          BuildInputPath(options.data_root, gate_source, date);
      const std::filesystem::path output_path = output_dir / (date + ".bin");
      tardis::ScopedOutputFileReplacement scoped_output(
          output_path, TemporaryOutputSuffix(date));

      OwnedInputStream binance_input(binance_path);
      OwnedInputStream gate_input(gate_path);
      const std::vector<tardis::BookTickerCsvInput> inputs{
          tardis::BookTickerCsvInput{.input = &binance_input.stream(),
                                     .source = binance_source},
          tardis::BookTickerCsvInput{.input = &gate_input.stream(),
                                     .source = gate_source},
      };

      const tardis::BookTickerBinaryWriteStats stats =
          tardis::WriteMergedBookTickerCsvStreams(scoped_output.temp_path(),
                                                  inputs);
      binance_input.CloseOrThrow();
      gate_input.CloseOrThrow();

      const std::uintmax_t file_size =
          std::filesystem::file_size(scoped_output.temp_path());
      const std::uintmax_t expected_size = ExpectedOutputSize(stats);
      if (file_size != expected_size) {
        throw std::runtime_error(
            fmt::format("output size mismatch for {}: expected {}, got {}",
                        date, expected_size, file_size));
      }
      scoped_output.Commit();

      fmt::print(
          "converted date={} output={} records={} binance_records={} "
          "gate_records={} first_exchange_ns={} last_exchange_ns={} "
          "file_size_bytes={}\n",
          date, output_path.string(), stats.records_written,
          stats.records_by_input.at(0), stats.records_by_input.at(1),
          stats.first_exchange_ns.value_or(0),
          stats.last_exchange_ns.value_or(0), file_size);
    }
  } catch (const std::exception& ex) {
    fmt::print(stderr, "[FAIL] {}\n", ex.what());
    return 1;
  }

  return 0;
}
