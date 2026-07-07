#ifndef AQUILA_CORE_MARKET_DATA_FUSION_BOOK_TICKER_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_BOOK_TICKER_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core/common/fusion_metadata_mode.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/fastest_route.h"
#include "core/market_data/fusion/metadata.h"
#include "core/market_data/types.h"
#include "core/websocket/runtime_clock.h"

namespace aquila::market_data {

using BookTickerFusionDecision = FastestRouteFusionDecision;

struct BookTickerFusionTraits {
  using Record = BookTicker;

  [[nodiscard]] static std::int32_t SymbolId(
      const BookTicker& ticker) noexcept {
    return ticker.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(
      const BookTicker& ticker) noexcept {
    return ticker.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(const BookTicker& ticker) noexcept {
    return ticker.local_ns;
  }

  [[nodiscard]] static std::int64_t EventNs(const BookTicker& ticker) noexcept {
    return ticker.exchange_ns;
  }
};

class BookTickerFusionCore {
 public:
  explicit BookTickerFusionCore(std::size_t max_symbol_id)
      : core_(max_symbol_id + 1) {}

  [[nodiscard]] BookTickerFusionDecision OnBookTicker(
      std::int32_t source_id, const BookTicker& ticker,
      std::int64_t fusion_publish_ns) noexcept {
    const FastestRouteFusionDecision decision =
        core_.OnRecord(source_id, ticker, fusion_publish_ns);
    if (!decision.publish) {
      return {};
    }
    return BookTickerFusionDecision{
        .publish = true,
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .record_id = decision.record_id,
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
  }

 private:
  BasicFastestRouteFusionCore<BookTickerFusionTraits> core_;
};

struct BookTickerFusionMetadataTraits {
  using Config = BookTickerFusionConfig;
  using Record = BookTicker;

  [[nodiscard]] static std::int64_t EventNs(const BookTicker& ticker) noexcept {
    return BookTickerFusionTraits::EventNs(ticker);
  }
};

using FileBookTickerFusionMetadataPolicy =
    FileFusionMetadataPolicy<BookTickerFusionMetadataTraits>;
using NoopBookTickerFusionMetadataPolicy =
    NoopFusionMetadataPolicy<BookTickerFusionConfig>;
using DefaultBookTickerFusionMetadataPolicy =
    std::conditional_t<aquila::kFusionMetadataEnabled,
                       FileBookTickerFusionMetadataPolicy,
                       NoopBookTickerFusionMetadataPolicy>;

struct BookTickerFusionRunnerTraits {
  using Config = BookTickerFusionConfig;
  using Core = BookTickerFusionCore;
  using Decision = BookTickerFusionDecision;
  using OutputConfig = BookTickerFusionOutputConfig;
  using Publisher = DataShmPublisher;
  using Record = BookTicker;
  using Reader = BookTickerShmReader;
  using ShmConfig = BookTickerShmConfig;
  using SourceConfig = BookTickerFusionSourceConfig;

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

  [[nodiscard]] static std::int64_t NowNs() noexcept {
    return static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
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

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_BOOK_TICKER_H_
