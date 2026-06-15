#ifndef AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_H_
#define AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <type_traits>

namespace aquila::market_data {

struct FusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t book_ticker_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

static_assert(std::is_standard_layout_v<FusionMetadataRecord>);
static_assert(std::is_trivially_copyable_v<FusionMetadataRecord>);

class FusionMetadataWriter {
 public:
  explicit FusionMetadataWriter(std::filesystem::path output_path)
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

  FusionMetadataWriter(const FusionMetadataWriter&) = delete;
  FusionMetadataWriter& operator=(const FusionMetadataWriter&) = delete;

  [[nodiscard]] bool Write(const FusionMetadataRecord& record) noexcept {
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

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_BOOK_TICKER_FUSION_METADATA_H_
