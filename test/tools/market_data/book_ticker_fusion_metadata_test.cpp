#include "tools/market_data/book_ticker_fusion_metadata.h"

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace {

std::filesystem::path UniqueOutputPath() {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("aquila_fusion_metadata_test_{}.bin", ::getpid());
}

std::vector<aquila::tools::market_data::FusionMetadataRecord> ReadRecords(
    const std::filesystem::path& path) {
  const std::uintmax_t size = std::filesystem::file_size(path);
  EXPECT_EQ(size % sizeof(aquila::tools::market_data::FusionMetadataRecord),
            0U);
  std::vector<aquila::tools::market_data::FusionMetadataRecord> records(
      size / sizeof(aquila::tools::market_data::FusionMetadataRecord));
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(
                   records.size() *
                   sizeof(aquila::tools::market_data::FusionMetadataRecord)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

void ExpectRecordEq(
    const aquila::tools::market_data::FusionMetadataRecord& actual,
    const aquila::tools::market_data::FusionMetadataRecord& expected) {
  EXPECT_EQ(actual.source_id, expected.source_id);
  EXPECT_EQ(actual.symbol_id, expected.symbol_id);
  EXPECT_EQ(actual.book_ticker_id, expected.book_ticker_id);
  EXPECT_EQ(actual.exchange_ns, expected.exchange_ns);
  EXPECT_EQ(actual.source_local_ns, expected.source_local_ns);
  EXPECT_EQ(actual.fusion_publish_ns, expected.fusion_publish_ns);
}

TEST(BookTickerFusionMetadataWriterTest, WritesFixedSizeMetadataRecords) {
  const std::filesystem::path output_path = UniqueOutputPath();
  std::filesystem::remove(output_path);

  const aquila::tools::market_data::FusionMetadataRecord first{
      .source_id = 0,
      .symbol_id = 42,
      .book_ticker_id = 100,
      .exchange_ns = 1'780'000'000'000'000'100,
      .source_local_ns = 1'780'000'000'000'100'100,
      .fusion_publish_ns = 1'780'000'000'000'100'200,
  };
  const aquila::tools::market_data::FusionMetadataRecord second{
      .source_id = 3,
      .symbol_id = 43,
      .book_ticker_id = 101,
      .exchange_ns = 1'780'000'000'000'000'101,
      .source_local_ns = 1'780'000'000'000'100'101,
      .fusion_publish_ns = 1'780'000'000'000'100'201,
  };

  {
    aquila::tools::market_data::FusionMetadataWriter writer(output_path);
    ASSERT_TRUE(writer.Write(first));
    ASSERT_TRUE(writer.Write(second));
    ASSERT_TRUE(writer.Flush());
  }

  const std::vector<aquila::tools::market_data::FusionMetadataRecord> records =
      ReadRecords(output_path);
  ASSERT_EQ(records.size(), 2U);
  ExpectRecordEq(records[0], first);
  ExpectRecordEq(records[1], second);

  std::filesystem::remove(output_path);
}

}  // namespace
