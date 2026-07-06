#include "core/market_data/fastest_route_fusion.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

struct TestRecord {
  std::int32_t symbol_id{0};
  std::int64_t id{0};
  std::int64_t local_ns{0};
};

struct TestTraits {
  using Record = TestRecord;

  [[nodiscard]] static std::int32_t SymbolId(
      const TestRecord& record) noexcept {
    return record.symbol_id;
  }

  [[nodiscard]] static std::int64_t RecordId(
      const TestRecord& record) noexcept {
    return record.id;
  }

  [[nodiscard]] static std::int64_t LocalNs(
      const TestRecord& record) noexcept {
    return record.local_ns;
  }
};

using TestFusion = aquila::market_data::BasicFastestRouteFusionCore<TestTraits>;

TEST(FastestRouteFusionCoreTest, PublishesOnlyIncreasingIdsPerSymbol) {
  TestFusion fusion(/*max_symbol_id=*/16);

  const TestRecord first{.symbol_id = 3, .id = 100, .local_ns = 1'000};
  const auto first_decision =
      fusion.OnRecord(/*source_id=*/0, first, /*fusion_publish_ns=*/2'000);
  ASSERT_TRUE(first_decision.publish);
  EXPECT_EQ(first_decision.source_id, 0);
  EXPECT_EQ(first_decision.symbol_id, 3);
  EXPECT_EQ(first_decision.record_id, 100);
  EXPECT_EQ(first_decision.source_local_ns, 1'000);
  EXPECT_EQ(first_decision.fusion_publish_ns, 2'000);

  const TestRecord duplicate{.symbol_id = 3, .id = 100, .local_ns = 1'100};
  EXPECT_FALSE(
      fusion.OnRecord(/*source_id=*/1, duplicate, /*fusion_publish_ns=*/2'100)
          .publish);

  const TestRecord older{.symbol_id = 3, .id = 99, .local_ns = 1'200};
  EXPECT_FALSE(
      fusion.OnRecord(/*source_id=*/2, older, /*fusion_publish_ns=*/2'200)
          .publish);

  const TestRecord next{.symbol_id = 3, .id = 101, .local_ns = 1'300};
  const auto next_decision =
      fusion.OnRecord(/*source_id=*/3, next, /*fusion_publish_ns=*/2'300);
  ASSERT_TRUE(next_decision.publish);
  EXPECT_EQ(next_decision.source_id, 3);
  EXPECT_EQ(next_decision.record_id, 101);
}

TEST(FastestRouteFusionCoreTest, MaintainsIndependentSymbolState) {
  TestFusion fusion(/*max_symbol_id=*/16);

  EXPECT_TRUE(fusion
                  .OnRecord(/*source_id=*/0,
                            TestRecord{.symbol_id = 1,
                                       .id = 10,
                                       .local_ns = 1'000},
                            /*fusion_publish_ns=*/2'000)
                  .publish);
  EXPECT_TRUE(fusion
                  .OnRecord(/*source_id=*/1,
                            TestRecord{.symbol_id = 2,
                                       .id = 5,
                                       .local_ns = 1'100},
                            /*fusion_publish_ns=*/2'100)
                  .publish);
  EXPECT_FALSE(fusion
                   .OnRecord(/*source_id=*/2,
                             TestRecord{.symbol_id = 1,
                                        .id = 9,
                                        .local_ns = 1'200},
                             /*fusion_publish_ns=*/2'200)
                   .publish);
}

TEST(FastestRouteFusionCoreTest, DropsOutOfRangeSymbolsWithUnsetMetadata) {
  TestFusion fusion(/*max_symbol_id=*/4);

  const auto negative = fusion.OnRecord(
      /*source_id=*/0, TestRecord{.symbol_id = -1, .id = 1, .local_ns = 1'000},
      /*fusion_publish_ns=*/2'000);
  EXPECT_FALSE(negative.publish);
  EXPECT_EQ(negative.source_id, -1);
  EXPECT_EQ(negative.symbol_id, -1);
  EXPECT_EQ(negative.record_id, 0);
  EXPECT_EQ(negative.source_local_ns, 0);
  EXPECT_EQ(negative.fusion_publish_ns, 0);

  const auto too_large = fusion.OnRecord(
      /*source_id=*/1, TestRecord{.symbol_id = 4, .id = 1, .local_ns = 1'100},
      /*fusion_publish_ns=*/2'100);
  EXPECT_FALSE(too_large.publish);
  EXPECT_EQ(too_large.source_id, -1);
  EXPECT_EQ(too_large.symbol_id, -1);
  EXPECT_EQ(too_large.record_id, 0);
}

}  // namespace
