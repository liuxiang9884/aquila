#include "core/common/fusion_metadata_mode.h"

#include <gtest/gtest.h>

TEST(FusionMetadataModeTest, ExposesCompileTimeMode) {
  EXPECT_TRUE(aquila::kFusionMetadataEnabled ||
              !aquila::kFusionMetadataEnabled);
  EXPECT_GE(AQUILA_FUSION_METADATA_ENABLED, 0);
  EXPECT_LE(AQUILA_FUSION_METADATA_ENABLED, 1);
}
