#include "core/common/book_ticker_fusion_metadata_mode.h"

#include <gtest/gtest.h>

TEST(BookTickerFusionMetadataModeTest, ExposesCompileTimeMode) {
  EXPECT_TRUE(aquila::kBookTickerFusionMetadataEnabled ||
              !aquila::kBookTickerFusionMetadataEnabled);
  EXPECT_GE(AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED, 0);
  EXPECT_LE(AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED, 1);
}
