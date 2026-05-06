#ifndef AQUILA_CORE_MARKET_DATA_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_shm.h"

namespace aquila::market_data {

struct NoopDataReaderDiagnostics {};

template <typename Diagnostics = NoopDataReaderDiagnostics>
class DataReader {
 public:
  explicit DataReader(config::DataReaderConfig data_reader_config)
      : max_events_per_source_(data_reader_config.max_events_per_source) {
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

    std::uint64_t handled = 0;
    const std::size_t source_count = sources_.size();
    for (std::size_t checked = 0; checked < source_count; ++checked) {
      const std::size_t index = (next_source_index_ + checked) % source_count;
      Source& source = *sources_[index];

      switch (source.config.read_mode) {
        case config::DataReaderReadMode::kLatest:
          handled += PollLatestSource(source, handler);
          break;
        case config::DataReaderReadMode::kDrain:
          handled += PollDrainSource(source, handler);
          break;
      }
    }

    next_source_index_ = (next_source_index_ + 1) % source_count;
    return handled;
  }

 private:
  struct Source {
    Source(config::DataReaderSourceConfig source_config,
           const BookTickerShmConfig& shm_config)
        : config(std::move(source_config)), reader(shm_config) {}

    config::DataReaderSourceConfig config;
    BookTickerShmReader reader;
  };

  template <typename Handler>
  std::uint64_t PollLatestSource(Source& source, Handler& handler) noexcept {
    BookTicker book_ticker{};
    std::uint64_t skipped{0};
    if (!source.reader.TryReadLatest(&book_ticker, &skipped)) {
      return 0;
    }
    handler.OnBookTicker(book_ticker);
    return 1;
  }

  template <typename Handler>
  std::uint64_t PollDrainSource(Source& source, Handler& handler) noexcept {
    std::uint64_t handled = 0;
    while (handled < max_events_per_source_) {
      BookTicker book_ticker{};
      if (!source.reader.TryReadOne(&book_ticker)) {
        break;
      }
      handler.OnBookTicker(book_ticker);
      ++handled;
    }
    return handled;
  }

  std::uint32_t max_events_per_source_{64};
  std::size_t next_source_index_{0};
  std::vector<std::unique_ptr<Source>> sources_;
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_READER_H_
