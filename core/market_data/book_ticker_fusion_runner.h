#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_RUNNER_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_RUNNER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/market_data/book_ticker_fusion.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_metadata.h"
#include "core/market_data/data_shm.h"
#include "core/websocket/runtime_clock.h"
#include "core/websocket/runtime_policy.h"

namespace aquila::market_data {

struct BookTickerFusionPollStats {
  std::uint64_t read_count{0};
  std::uint64_t published_count{0};
  std::uint64_t metadata_write_errors{0};
};

class BookTickerFusionRunner {
 public:
  explicit BookTickerFusionRunner(const BookTickerFusionConfig& config)
      : max_events_per_source_(config.max_events_per_source),
        fusion_(config.max_symbol_id),
        publisher_(MakeOutputShmConfig(config.output)),
        metadata_writer_(config.output.metadata_bin) {
    sources_.reserve(config.sources.size());
    for (const BookTickerFusionSourceConfig& source_config : config.sources) {
      sources_.push_back(std::make_unique<Source>(
          source_config.source_id, MakeSourceShmConfig(source_config)));
    }
  }

  [[nodiscard]] BookTickerFusionPollStats PollOnce() noexcept {
    BookTickerFusionPollStats stats;
    for (std::unique_ptr<Source>& source : sources_) {
      for (std::uint32_t i = 0; i < max_events_per_source_; ++i) {
        BookTicker ticker;
        if (!source->reader.TryReadOne(&ticker)) {
          break;
        }
        ++stats.read_count;

        const std::int64_t fusion_publish_ns =
            static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
        const BookTickerFusionDecision decision =
            fusion_.OnBookTicker(source->source_id, ticker, fusion_publish_ns);
        if (!decision.publish) {
          continue;
        }

        ticker.local_ns = fusion_publish_ns;
        publisher_.OnBookTicker(ticker);
        ++stats.published_count;

        const FusionMetadataRecord record{
            .source_id = decision.source_id,
            .symbol_id = decision.symbol_id,
            .book_ticker_id = decision.book_ticker_id,
            .exchange_ns = ticker.exchange_ns,
            .source_local_ns = decision.source_local_ns,
            .fusion_publish_ns = decision.fusion_publish_ns,
        };
        if (!metadata_writer_.Write(record)) {
          ++stats.metadata_write_errors;
        }
      }
    }
    total_read_count_ += stats.read_count;
    total_published_count_ += stats.published_count;
    total_metadata_write_errors_ += stats.metadata_write_errors;
    return stats;
  }

  [[nodiscard]] bool Flush() noexcept {
    publisher_.FlushPublishedCount();
    return metadata_writer_.Flush();
  }

  [[nodiscard]] std::uint64_t total_read_count() const noexcept {
    return total_read_count_;
  }

  [[nodiscard]] std::uint64_t total_published_count() const noexcept {
    return total_published_count_;
  }

  [[nodiscard]] std::uint64_t total_metadata_write_errors() const noexcept {
    return total_metadata_write_errors_;
  }

 private:
  struct Source {
    Source(std::int32_t source_id_in,
           const aquila::market_data::BookTickerShmConfig& shm_config)
        : source_id(source_id_in), reader(shm_config) {}

    std::int32_t source_id{-1};
    aquila::market_data::BookTickerShmReader reader;
  };

  [[nodiscard]] static BookTickerShmConfig MakeSourceShmConfig(
      const BookTickerFusionSourceConfig& source) {
    return BookTickerShmConfig{
        .enabled = true,
        .shm_name = source.shm_name,
        .channel_name = source.channel_name,
        .create = false,
        .remove_existing = false,
    };
  }

  [[nodiscard]] static BookTickerShmConfig MakeOutputShmConfig(
      const BookTickerFusionOutputConfig& output) {
    return BookTickerShmConfig{
        .enabled = true,
        .shm_name = output.shm_name,
        .channel_name = output.channel_name,
        .create = true,
        .remove_existing = output.remove_existing,
    };
  }

  std::uint32_t max_events_per_source_{0};
  BookTickerFusionCore fusion_;
  DataShmPublisher publisher_;
  FusionMetadataWriter metadata_writer_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::uint64_t total_read_count_{0};
  std::uint64_t total_published_count_{0};
  std::uint64_t total_metadata_write_errors_{0};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_RUNNER_H_
