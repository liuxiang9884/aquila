#ifndef AQUILA_CORE_MARKET_DATA_REALTIME_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_REALTIME_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
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
    if (sources_.empty()) {
      return 0;
    }

    const std::size_t source_count = sources_.size();
    for (std::size_t checked = 0; checked < source_count; ++checked) {
      const std::size_t index = (next_source_index_ + checked) % source_count;
      Source& source = *sources_[index];

      std::uint64_t handled = 0;
      switch (source.config.read_mode) {
        case config::DataReaderReadMode::kLatest:
          handled = PollLatestSource(index, source, handler);
          break;
        case config::DataReaderReadMode::kDrain:
          handled = PollDrainSource(index, source, handler);
          break;
      }
      if (handled != 0) {
        next_source_index_ = (index + 1) % source_count;
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
    BookTicker book_ticker{};
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
  [[no_unique_address]] Diagnostics diagnostics_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_REALTIME_DATA_READER_H_
