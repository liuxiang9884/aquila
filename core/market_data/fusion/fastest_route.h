#ifndef AQUILA_CORE_MARKET_DATA_FUSION_FASTEST_ROUTE_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_FASTEST_ROUTE_H_

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace aquila::market_data {

struct FastestRouteFusionDecision {
  bool publish{false};
  std::int32_t source_id{-1};
  std::int32_t symbol_id{-1};
  std::int64_t record_id{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

template <typename Traits>
class BasicFastestRouteFusionCore {
 public:
  using Record = typename Traits::Record;

  explicit BasicFastestRouteFusionCore(std::size_t symbol_capacity)
      : states_(symbol_capacity) {}

  [[nodiscard]] FastestRouteFusionDecision OnRecord(
      std::int32_t source_id, const Record& record,
      std::int64_t fusion_publish_ns) noexcept {
    const std::int32_t symbol_id = Traits::SymbolId(record);
    if (symbol_id < 0 ||
        static_cast<std::size_t>(symbol_id) >= states_.size()) {
      return {};
    }

    SymbolState& state = states_[static_cast<std::size_t>(symbol_id)];
    const std::int64_t record_id = Traits::RecordId(record);
    if (record_id <= state.last_published_id) {
      return {};
    }

    state.last_published_id = record_id;
    state.last_published_source = source_id;
    return FastestRouteFusionDecision{
        .publish = true,
        .source_id = source_id,
        .symbol_id = symbol_id,
        .record_id = record_id,
        .source_local_ns = Traits::LocalNs(record),
        .fusion_publish_ns = fusion_publish_ns,
    };
  }

 private:
  struct SymbolState {
    std::int64_t last_published_id{std::numeric_limits<std::int64_t>::min()};
    std::int32_t last_published_source{-1};
  };

  std::vector<SymbolState> states_;
};

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

        const std::int64_t fusion_publish_ns = Traits::NowNs();
        const auto decision = Traits::OnRecord(fusion_, source->source_id,
                                               record, fusion_publish_ns);
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
  typename Traits::Publisher publisher_;
  MetadataPolicy metadata_policy_;
  std::vector<std::unique_ptr<Source>> sources_;
  std::uint64_t total_read_count_{0};
  std::uint64_t total_published_count_{0};
  std::uint64_t total_metadata_write_errors_{0};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_FASTEST_ROUTE_H_
