#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_RUNNER_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_RUNNER_H_

#include <cstdint>

#include "core/market_data/book_ticker_fusion.h"
#include "core/market_data/book_ticker_fusion_config.h"
#include "core/market_data/book_ticker_fusion_metadata_policy.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/fastest_route_fusion_runner.h"

namespace aquila::market_data {

struct BookTickerFusionRunnerTraits {
  using Config = BookTickerFusionConfig;
  using Core = BookTickerFusionCore;
  using Decision = BookTickerFusionDecision;
  using Record = BookTicker;
  using Reader = BookTickerShmReader;
  using ShmConfig = BookTickerShmConfig;
  using SourceConfig = BookTickerFusionSourceConfig;
  using OutputConfig = BookTickerFusionOutputConfig;

  [[nodiscard]] static BookTickerShmConfig MakeSourceShmConfig(
      const BookTickerFusionSourceConfig& source) {
    return BookTickerShmConfig{.enabled = true,
                               .shm_name = source.shm_name,
                               .channel_name = source.channel_name,
                               .create = false,
                               .remove_existing = false};
  }

  [[nodiscard]] static BookTickerShmConfig MakeOutputShmConfig(
      const BookTickerFusionOutputConfig& output) {
    return BookTickerShmConfig{.enabled = true,
                               .shm_name = output.shm_name,
                               .channel_name = output.channel_name,
                               .create = true,
                               .remove_existing = output.remove_existing};
  }

  [[nodiscard]] static BookTickerFusionDecision OnRecord(
      BookTickerFusionCore& core, std::int32_t source_id,
      const BookTicker& ticker, std::int64_t fusion_publish_ns) noexcept {
    return core.OnBookTicker(source_id, ticker, fusion_publish_ns);
  }

  static void SetLocalNs(BookTicker* ticker, std::int64_t local_ns) noexcept {
    ticker->local_ns = local_ns;
  }

  static void Publish(DataShmPublisher& publisher,
                      const BookTicker& ticker) noexcept {
    publisher.OnBookTicker(ticker);
  }
};

template <typename MetadataPolicy>
using BasicBookTickerFusionRunner =
    BasicFastestRouteFusionRunner<BookTickerFusionRunnerTraits, MetadataPolicy>;

using BookTickerFusionPollStats = FastestRouteFusionPollStats;
using BookTickerFusionRunner =
    BasicBookTickerFusionRunner<DefaultBookTickerFusionMetadataPolicy>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_RUNNER_H_
