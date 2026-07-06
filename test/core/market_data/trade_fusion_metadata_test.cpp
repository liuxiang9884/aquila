#include "core/market_data/trade_fusion_metadata.h"

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

namespace {

std::filesystem::path UniquePath() {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("aquila_trade_fusion_metadata_test_{}.bin", ::getpid());
}

std::vector<aquila::market_data::TradeFusionMetadataRecord> ReadRecords(
    const std::filesystem::path& path) {
  const std::uintmax_t size = std::filesystem::file_size(path);
  EXPECT_EQ(size % sizeof(aquila::market_data::TradeFusionMetadataRecord), 0U);
  std::vector<aquila::market_data::TradeFusionMetadataRecord> records(
      size / sizeof(aquila::market_data::TradeFusionMetadataRecord));
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(
                   records.size() *
                   sizeof(aquila::market_data::TradeFusionMetadataRecord)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

TEST(TradeFusionMetadataTest, WritesRawRecords) {
  const std::filesystem::path path = UniquePath();
  std::filesystem::remove(path);

  aquila::market_data::TradeFusionMetadataWriter writer(path);
  const aquila::market_data::TradeFusionMetadataRecord first{
      .source_id = 0,
      .symbol_id = 42,
      .trade_id = 100,
      .exchange_ns = 1'000,
      .trade_ns = 1'010,
      .source_local_ns = 2'000,
      .fusion_publish_ns = 3'000,
  };
  const aquila::market_data::TradeFusionMetadataRecord second{
      .source_id = 1,
      .symbol_id = 42,
      .trade_id = 101,
      .exchange_ns = 1'100,
      .trade_ns = 1'110,
      .source_local_ns = 2'100,
      .fusion_publish_ns = 3'100,
  };

  ASSERT_TRUE(writer.Write(first));
  ASSERT_TRUE(writer.Write(second));
  ASSERT_TRUE(writer.Flush());

  const std::vector<aquila::market_data::TradeFusionMetadataRecord> records =
      ReadRecords(path);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].source_id, first.source_id);
  EXPECT_EQ(records[0].trade_id, first.trade_id);
  EXPECT_EQ(records[0].trade_ns, first.trade_ns);
  EXPECT_EQ(records[1].source_id, second.source_id);
  EXPECT_EQ(records[1].fusion_publish_ns, second.fusion_publish_ns);

  std::filesystem::remove(path);
}

}  // namespace
