#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "core/market_data/fusion/fastest_route.h"

namespace aquila::market_data {

template <typename Record>
class BasicFusionMetadataWriter {
 public:
  explicit BasicFusionMetadataWriter(std::filesystem::path output_path)
      : output_path_(std::move(output_path)) {
    if (output_path_.empty()) {
      throw std::invalid_argument("metadata output path must not be empty");
    }
    if (output_path_.has_parent_path()) {
      std::filesystem::create_directories(output_path_.parent_path());
    }
    output_.open(output_path_, std::ios::binary | std::ios::trunc);
    if (!output_.is_open()) {
      throw std::runtime_error("failed to open metadata output");
    }
  }

  BasicFusionMetadataWriter(const BasicFusionMetadataWriter&) = delete;
  BasicFusionMetadataWriter& operator=(const BasicFusionMetadataWriter&) =
      delete;

  [[nodiscard]] bool Write(const Record& record) noexcept {
    if (write_error_) {
      return false;
    }
    output_.write(reinterpret_cast<const char*>(&record), sizeof(record));
    if (!output_.good()) {
      write_error_ = true;
      return false;
    }
    ++records_written_;
    return true;
  }

  [[nodiscard]] bool Flush() noexcept {
    if (write_error_) {
      return false;
    }
    output_.flush();
    if (!output_.good()) {
      write_error_ = true;
      return false;
    }
    return true;
  }

  [[nodiscard]] bool write_error() const noexcept {
    return write_error_;
  }

  [[nodiscard]] std::uint64_t records_written() const noexcept {
    return records_written_;
  }

  [[nodiscard]] const std::filesystem::path& output_path() const noexcept {
    return output_path_;
  }

 private:
  std::filesystem::path output_path_;
  std::ofstream output_;
  std::uint64_t records_written_{0};
  bool write_error_{false};
};

struct FusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t record_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t event_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

static_assert(sizeof(FusionMetadataRecord) == 48);
static_assert(std::is_standard_layout_v<FusionMetadataRecord>);
static_assert(std::is_trivially_copyable_v<FusionMetadataRecord>);

using FusionMetadataWriter = BasicFusionMetadataWriter<FusionMetadataRecord>;

using BookTickerFusionMetadataRecord = FusionMetadataRecord;
using BookTickerFusionMetadataWriter = FusionMetadataWriter;
using TradeFusionMetadataRecord = FusionMetadataRecord;
using TradeFusionMetadataWriter = FusionMetadataWriter;

template <typename Traits>
class FileFusionMetadataPolicy {
 public:
  using Config = typename Traits::Config;
  using Record = typename Traits::Record;
  static constexpr bool kEnabled = true;

  explicit FileFusionMetadataPolicy(const Config& config)
      : writer_(config.output.metadata_bin) {}

  [[nodiscard]] bool Write(const FastestRouteFusionDecision& decision,
                           const Record& record) noexcept {
    const FusionMetadataRecord metadata{
        .source_id = decision.source_id,
        .symbol_id = decision.symbol_id,
        .record_id = decision.record_id,
        .exchange_ns = record.exchange_ns,
        .event_ns = Traits::EventNs(record),
        .source_local_ns = decision.source_local_ns,
        .fusion_publish_ns = decision.fusion_publish_ns,
    };
    return writer_.Write(metadata);
  }

  [[nodiscard]] bool Flush() noexcept {
    return writer_.Flush();
  }

 private:
  FusionMetadataWriter writer_;
};

template <typename Config>
class NoopFusionMetadataPolicy {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopFusionMetadataPolicy(const Config& /*config*/) noexcept {}

  [[nodiscard]] bool Flush() noexcept {
    return true;
  }
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
