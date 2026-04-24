#include "core/websocket/prepared_write.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>

using namespace aquila::websocket;

TEST(WebsocketPreparedWriteTest, ManagesArenaSlotsAndResetState) {
  PreparedWriteArena arena(2, 64);

  PreparedWrite* first = arena.TryAcquire();
  PreparedWrite* second = arena.TryAcquire();
  PreparedWrite* third = arena.TryAcquire();

  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(third, nullptr);
  EXPECT_NE(first, second);
  EXPECT_EQ(first->storage.size(), 64U);
  EXPECT_EQ(second->storage.size(), 64U);

  const std::byte* first_storage = first->storage.data();
  const auto first_slot = first->slot_index;
  first->encoded_size = 17;
  first->write_offset = 9;
  first->kind = PayloadKind::kPing;

  arena.Release(first);
  PreparedWrite* reacquired = arena.TryAcquire();
  ASSERT_NE(reacquired, nullptr);
  EXPECT_EQ(reacquired->slot_index, first_slot);
  EXPECT_EQ(reacquired->storage.data(), first_storage);
  EXPECT_EQ(reacquired->encoded_size, 0U);
  EXPECT_EQ(reacquired->write_offset, 0U);
  EXPECT_EQ(reacquired->kind, PayloadKind::kBinary);

  PreparedWriteArena zero_bytes_arena(2, 0);
  EXPECT_EQ(zero_bytes_arena.TryAcquire(), nullptr);

  PreparedWriteArena overflow_arena(2, std::numeric_limits<size_t>::max());
  EXPECT_EQ(overflow_arena.TryAcquire(), nullptr);

  if constexpr (sizeof(size_t) > sizeof(std::uint32_t)) {
    PreparedWriteArena too_many_slots_arena(
        static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()) + 1, 64);
    EXPECT_EQ(too_many_slots_arena.TryAcquire(), nullptr);
  }
}
