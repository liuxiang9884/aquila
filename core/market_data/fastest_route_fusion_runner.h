#ifndef AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_RUNNER_H_
#define AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_RUNNER_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "core/market_data/data_shm.h"
#include "core/websocket/runtime_clock.h"

namespace aquila::market_data {

struct FastestRouteFusionPollStats {
  std::uint64_t read_count{0};
  std::uint64_t published_count{0};
  std::uint64_t metadata_write_errors{0};
};

template <typename Traits, typename MetadataPolicy>
class BasicFastestRouteFusionRunner {
 public:
  using Config = typename Traits::Config;
  using Record = typename Traits::Record;
  using SourceConfig = typename Traits::SourceConfig;

  explicit BasicFastestRouteFusionRunner(const Config& config)
      : max_events_per_source_(config.max_events_per_source),
        fusion_(config.max_symbol_id),
        publisher_(Traits::MakeOutputShmConfig(config.output)),
        metadata_policy_(config) {
    sources_.reserve(config.sources.size());
    for (const SourceConfig& source_config : config.sources) {
      sources_.push_back(std::make_unique<Source>(
          source_config.source_id, Traits::MakeSourceShmConfig(source_config)));
    }
  }

  [[nodiscard]] FastestRouteFusionPollStats PollOnce() noexcept {
    FastestRouteFusionPollStats stats;
    for (std::unique_ptr<Source>& source : sources_) {
      for (std::uint32_t i = 0; i < max_events_per_source_; ++i) {
        Record record{};
        if (!source->reader.TryReadOne(&record)) {
          break;
        }
        ++stats.read_count;

        const std::int64_t fusion_publish_ns =
            static_cast<std::int64_t>(websocket::RealtimeClockNowNs());
        const auto decision =
            Traits::OnRecord(fusion_, source->source_id, record,
                             fusion_publish_ns);
        if (!decision.publish) {
          continue;
        }

        Traits::SetLocalNs(&record, fusion_publish_ns);
        Traits::Publish(publisher_, record);
        ++stats.published_count;

        if constexpr (MetadataPolicy::kEnabled) {
          if (!metadata_policy_.Write(decision, record)) {
            ++stats.metadata_write_errors;
          }
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
    return metadata_policy_.Flush();
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
    Source(std::int32_t source_id_in, const typename Traits::ShmConfig& config)
        : source_id(source_id_in), reader(config) {}

    std::int32_t source_id{-1};
    typename Traits::Reader reader;
  };

  std::uint32_t max_events_per_source_{0};
  typename Traits::Core fusion_;
  DataShmPublisher publisher_;
  MetadataPolicy metadata_policy_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::uint64_t total_read_count_{0};
  std::uint64_t total_published_count_{0};
  std::uint64_t total_metadata_write_errors_{0};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FASTEST_ROUTE_FUSION_RUNNER_H_
