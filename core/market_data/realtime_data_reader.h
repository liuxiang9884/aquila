#ifndef AQUILA_CORE_MARKET_DATA_REALTIME_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_REALTIME_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_shm.h"

namespace aquila::market_data {

struct RealtimeDataReaderSourceStats {
  std::uint64_t book_ticker_count{0};
  std::uint64_t skipped{0};
  std::uint64_t overruns{0};
  std::int64_t last_book_ticker_id{0};
};

struct RealtimeDataReaderStats {
  std::uint64_t total_count{0};
  std::vector<RealtimeDataReaderSourceStats> sources;
};

struct NoopRealtimeDataReaderDiagnostics {
  static constexpr bool kEnabled = false;

  explicit NoopRealtimeDataReaderDiagnostics(
      std::size_t source_count) noexcept {
    (void)source_count;
  }
  void RecordBookTicker(std::size_t, const BookTicker&) noexcept {}
  void RecordSkipped(std::size_t, std::uint64_t) noexcept {}
  void RecordOverrun(std::size_t, std::uint64_t) noexcept {}
};

class RealtimeDataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  explicit RealtimeDataReaderDiagnostics(std::size_t source_count)
      : stats_{.sources =
                   std::vector<RealtimeDataReaderSourceStats>(source_count)} {}

  void RecordBookTicker(std::size_t source_index,
                        const BookTicker& book_ticker) noexcept {
    ++stats_.total_count;
    ++stats_.sources[source_index].book_ticker_count;
    stats_.sources[source_index].last_book_ticker_id = book_ticker.id;
  }

  void RecordSkipped(std::size_t source_index, std::uint64_t skipped) noexcept {
    stats_.sources[source_index].skipped += skipped;
  }

  void RecordOverrun(std::size_t source_index,
                     std::uint64_t overrun_delta) noexcept {
    stats_.sources[source_index].overruns += overrun_delta;
  }

  [[nodiscard]] const RealtimeDataReaderStats& stats() const noexcept {
    return stats_;
  }

 private:
  RealtimeDataReaderStats stats_;
};

template <typename Diagnostics = NoopRealtimeDataReaderDiagnostics>
class RealtimeDataReader {
 public:
  explicit RealtimeDataReader(config::DataReaderConfig data_reader_config)
      : diagnostics_(data_reader_config.sources.size()) {
    if (data_reader_config.sources.empty()) {
      throw std::invalid_argument(
          "realtime data reader requires at least one source");
    }

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

      const config::DataReaderStartPosition start_position =
          source_config.start_position;
      const config::DataReaderReadMode read_mode = source_config.read_mode;
      auto source = std::make_unique<Source>(read_mode, shm_config);
      if (start_position == config::DataReaderStartPosition::kLatest) {
        source->reader.SeekLatest();
      } else {
        source->reader.SeekEarliestVisible();
      }
      sources_.push_back(std::move(source));
    }
    if (sources_.size() > 1) {
      scan_sources_.reserve(sources_.size() * 2);
      scan_source_indices_.reserve(sources_.size() * 2);
      for (std::size_t repeat = 0; repeat < 2; ++repeat) {
        for (std::size_t i = 0; i < sources_.size(); ++i) {
          scan_sources_.push_back(sources_[i].get());
          scan_source_indices_.push_back(i);
        }
      }
    }
  }

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept {
    if (sources_.empty()) {
      return 0;
    }

    const std::size_t source_count = sources_.size();
    if (source_count == 1) {
      Source& source = *sources_[0];
      switch (source.read_mode) {
        case config::DataReaderReadMode::kLatest:
          return PollLatestSource(0, source, handler);
        case config::DataReaderReadMode::kDrain:
          return PollDrainSource(0, source, handler);
      }
      return 0;
    }

    const std::size_t scan_end = next_source_index_ + source_count;
    for (std::size_t scan_position = next_source_index_;
         scan_position < scan_end; ++scan_position) {
      Source& source = *scan_sources_[scan_position];
      const std::size_t source_index = scan_source_indices_[scan_position];

      std::uint64_t handled = 0;
      switch (source.read_mode) {
        case config::DataReaderReadMode::kLatest:
          handled = PollLatestSource(source_index, source, handler);
          break;
        case config::DataReaderReadMode::kDrain:
          handled = PollDrainSource(source_index, source, handler);
          break;
      }
      if (handled != 0) {
        next_source_index_ = scan_source_indices_[scan_position + 1];
        return handled;
      }
    }

    return 0;
  }

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    std::uint64_t count = 0;
    while (count < max_events) {
      const std::uint64_t handled = Poll(handler);
      if (handled == 0) {
        break;
      }
      count += handled;
    }
    return count;
  }

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  struct Source {
    Source(config::DataReaderReadMode read_mode_in,
           const BookTickerShmConfig& shm_config)
        : read_mode(read_mode_in), reader(shm_config) {}

    config::DataReaderReadMode read_mode{config::DataReaderReadMode::kLatest};
    BookTickerShmReader reader;
    std::uint64_t last_overrun_count{0};
  };

  template <typename Handler>
  std::uint64_t PollLatestSource(std::size_t source_index, Source& source,
                                 Handler& handler) noexcept {
    BookTicker book_ticker;
    std::uint64_t skipped{0};
    if (!source.reader.TryReadLatest(&book_ticker, &skipped)) {
      return 0;
    }
    if constexpr (Diagnostics::kEnabled) {
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
    BookTicker book_ticker;
    if (!source.reader.TryReadOne(&book_ticker)) {
      return 0;
    }
    if constexpr (Diagnostics::kEnabled) {
      diagnostics_.RecordBookTicker(source_index, book_ticker);
      const std::uint64_t overrun = source.reader.overrun_count();
      diagnostics_.RecordOverrun(source_index,
                                 overrun - source.last_overrun_count);
      source.last_overrun_count = overrun;
    }
    handler.OnBookTicker(book_ticker);
    return 1;
  }

  std::size_t next_source_index_{0};
  std::vector<std::unique_ptr<Source>> sources_;
  std::vector<Source*> scan_sources_;
  std::vector<std::size_t> scan_source_indices_;
  [[no_unique_address]] Diagnostics diagnostics_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_REALTIME_DATA_READER_H_
