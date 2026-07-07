#include "core/market_data/fusion_metadata_policy.h"

#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "core/market_data/fusion_config.h"

namespace {

namespace md = aquila::market_data;

struct TestRecord {
  std::int64_t exchange_ns{0};
  std::int64_t event_ns{0};
};

struct TestConfig {
  md::FusionOutputConfig output;
};

struct TestMetadataTraits {
  using Config = TestConfig;
  using Record = TestRecord;

  [[nodiscard]] static std::int64_t EventNs(const TestRecord& record) noexcept {
    return record.event_ns;
  }
};

std::filesystem::path UniquePath() {
  return std::filesystem::path{"/home/liuxiang/tmp"} /
         fmt::format("aquila_fusion_metadata_policy_test_{}.bin", ::getpid());
}

std::vector<md::FusionMetadataRecord> ReadRecords(
    const std::filesystem::path& path) {
  const std::uintmax_t size = std::filesystem::file_size(path);
  EXPECT_EQ(size % sizeof(md::FusionMetadataRecord), 0U);
  std::vector<md::FusionMetadataRecord> records(
      size / sizeof(md::FusionMetadataRecord));
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  if (!records.empty()) {
    input.read(reinterpret_cast<char*>(records.data()),
               static_cast<std::streamsize>(records.size() *
                                            sizeof(md::FusionMetadataRecord)));
    EXPECT_TRUE(input.good());
  }
  return records;
}

TEST(FusionMetadataPolicyTest, BuildsUnifiedRecordFromDecisionAndRecordTraits) {
  const std::filesystem::path path = UniquePath();
  std::filesystem::remove(path);
  const TestConfig config{
      .output =
          md::FusionOutputConfig{
              .metadata_bin = path,
          },
  };
  md::FileFusionMetadataPolicy<TestMetadataTraits> policy(config);

  const md::FastestRouteFusionDecision decision{
      .publish = true,
      .source_id = 7,
      .symbol_id = 42,
      .record_id = 123,
      .source_local_ns = 2'000,
      .fusion_publish_ns = 3'000,
  };
  const TestRecord record{
      .exchange_ns = 1'000,
      .event_ns = 1'010,
  };

  ASSERT_TRUE(policy.Write(decision, record));
  ASSERT_TRUE(policy.Flush());

  const std::vector<md::FusionMetadataRecord> records = ReadRecords(path);
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].source_id, decision.source_id);
  EXPECT_EQ(records[0].symbol_id, decision.symbol_id);
  EXPECT_EQ(records[0].record_id, decision.record_id);
  EXPECT_EQ(records[0].exchange_ns, record.exchange_ns);
  EXPECT_EQ(records[0].event_ns, record.event_ns);
  EXPECT_EQ(records[0].source_local_ns, decision.source_local_ns);
  EXPECT_EQ(records[0].fusion_publish_ns, decision.fusion_publish_ns);

  std::filesystem::remove(path);
}

TEST(FusionMetadataPolicyTest, NoopPolicyFlushesWithoutOutputPath) {
  md::NoopFusionMetadataPolicy<TestConfig> policy(TestConfig{});

  EXPECT_TRUE(policy.Flush());
}

}  // namespace
