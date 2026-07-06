#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_WRITER_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_WRITER_H_

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <utility>

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

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_WRITER_H_
