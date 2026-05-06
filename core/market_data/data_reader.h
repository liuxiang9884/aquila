#ifndef AQUILA_CORE_MARKET_DATA_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_shm.h"

namespace aquila::market_data {

struct DataReaderSourceStats {
  std::uint64_t book_tickers{0};
  std::uint64_t skipped{0};
  std::uint64_t overruns{0};
  std::int64_t last_book_ticker_id{0};
};

struct DataReaderStats {
  std::uint64_t poll_calls{0};
  std::uint64_t empty_polls{0};
  std::uint64_t book_tickers{0};
  std::vector<DataReaderSourceStats> sources;
};

struct NoopDataReaderDiagnostics {
  explicit NoopDataReaderDiagnostics(std::size_t source_count) noexcept {
    (void)source_count;
  }
  void RecordPoll() noexcept {}
  void RecordEmptyPoll() noexcept {}
  void RecordBookTicker(std::size_t, const BookTicker&) noexcept {}
  void RecordSkipped(std::size_t, std::uint64_t) noexcept {}
  void RecordOverrun(std::size_t, std::uint64_t) noexcept {}
};

class DataReaderDiagnostics {
 public:
  explicit DataReaderDiagnostics(std::size_t source_count)
      : stats_{.sources = std::vector<DataReaderSourceStats>(source_count)} {}

  void RecordPoll() noexcept {
    ++stats_.poll_calls;
  }

  void RecordEmptyPoll() noexcept {
    ++stats_.empty_polls;
  }

  void RecordBookTicker(std::size_t source_index,
                        const BookTicker& book_ticker) noexcept {
    ++stats_.book_tickers;
    ++stats_.sources[source_index].book_tickers;
    stats_.sources[source_index].last_book_ticker_id = book_ticker.id;
  }

  void RecordSkipped(std::size_t source_index, std::uint64_t skipped) noexcept {
    stats_.sources[source_index].skipped += skipped;
  }

  void RecordOverrun(std::size_t source_index,
                     std::uint64_t overrun_delta) noexcept {
    stats_.sources[source_index].overruns += overrun_delta;
  }

  [[nodiscard]] const DataReaderStats& stats() const noexcept {
    return stats_;
  }

 private:
  DataReaderStats stats_;
};

template <typename Diagnostics = NoopDataReaderDiagnostics>
class DataReader {
 public:
  explicit DataReader(config::DataReaderConfig data_reader_config)
      : max_events_per_source_(data_reader_config.max_events_per_source),
        diagnostics_(data_reader_config.sources.size()) {
    sources_.reserve(data_reader_config.sources.size());
    for (config::DataReaderSourceConfig& source_config :
         data_reader_config.sources) {
      BookTickerShmConfig shm_config{
          .enabled = true,
          .shm_name = source_config.shm_name,
          .channel_name = source_config.channel_name,
          .create = false,
          .remove_existing = false,
      };

      auto source =
          std::make_unique<Source>(std::move(source_config), shm_config);
      if (source->config.start_position ==
          config::DataReaderStartPosition::kLatest) {
        source->reader.SeekLatest();
      } else {
        source->reader.SeekEarliestVisible();
      }
      sources_.push_back(std::move(source));
    }
  }

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept {
    if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
      diagnostics_.RecordPoll();
    }
    if (sources_.empty()) {
      if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
        diagnostics_.RecordEmptyPoll();
      }
      return 0;
    }

    std::uint64_t handled = 0;
    const std::size_t source_count = sources_.size();
    for (std::size_t checked = 0; checked < source_count; ++checked) {
      const std::size_t index = (next_source_index_ + checked) % source_count;
      Source& source = *sources_[index];

      switch (source.config.read_mode) {
        case config::DataReaderReadMode::kLatest:
          handled += PollLatestSource(index, source, handler);
          break;
        case config::DataReaderReadMode::kDrain:
          handled += PollDrainSource(index, source, handler);
          break;
      }
    }

    next_source_index_ = (next_source_index_ + 1) % source_count;
    if (handled == 0) {
      if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
        diagnostics_.RecordEmptyPoll();
      }
    }
    return handled;
  }

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  struct Source {
    Source(config::DataReaderSourceConfig source_config,
           const BookTickerShmConfig& shm_config)
        : config(std::move(source_config)), reader(shm_config) {}

    config::DataReaderSourceConfig config;
    BookTickerShmReader reader;
    std::uint64_t last_overrun_count{0};
  };

  template <typename Handler>
  std::uint64_t PollLatestSource(std::size_t source_index, Source& source,
                                 Handler& handler) noexcept {
    BookTicker book_ticker{};
    std::uint64_t skipped{0};
    if (!source.reader.TryReadLatest(&book_ticker, &skipped)) {
      return 0;
    }
    if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
      diagnostics_.RecordBookTicker(source_index, book_ticker);
      diagnostics_.RecordSkipped(source_index, skipped);
      const std::uint64_t overrun = source.reader.overrun_count();
      diagnostics_.RecordOverrun(source_index,
                                 overrun - source.last_overrun_count);
      source.last_overrun_count = overrun;
    }
    handler.OnBookTicker(book_ticker);
    return 1;
  }

  template <typename Handler>
  std::uint64_t PollDrainSource(std::size_t source_index, Source& source,
                                Handler& handler) noexcept {
    std::uint64_t handled = 0;
    while (handled < max_events_per_source_) {
      BookTicker book_ticker{};
      if (!source.reader.TryReadOne(&book_ticker)) {
        break;
      }
      if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
        diagnostics_.RecordBookTicker(source_index, book_ticker);
        const std::uint64_t overrun = source.reader.overrun_count();
        diagnostics_.RecordOverrun(source_index,
                                   overrun - source.last_overrun_count);
        source.last_overrun_count = overrun;
      }
      handler.OnBookTicker(book_ticker);
      ++handled;
    }
    return handled;
  }

  std::uint32_t max_events_per_source_{64};
  std::size_t next_source_index_{0};
  std::vector<std::unique_ptr<Source>> sources_;
  [[no_unique_address]] Diagnostics diagnostics_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_READER_H_
