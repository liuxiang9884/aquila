#include "core/websocket/prepared_write.h"

#include <cstddef>
#include <cstdint>
#include <limits>

using namespace aquila::websocket;

int main() {
  PreparedWriteArena arena(2, 64);

  PreparedWrite* first = arena.TryAcquire();
  PreparedWrite* second = arena.TryAcquire();
  PreparedWrite* third = arena.TryAcquire();

  if (first == nullptr || second == nullptr || third != nullptr) {
    return 1;
  }
  if (first == second) {
    return 1;
  }
  if (first->storage.size() != 64 || second->storage.size() != 64) {
    return 1;
  }

  const std::byte* first_storage = first->storage.data();
  const auto first_slot = first->slot_index;
  first->encoded_size = 17;
  first->write_offset = 9;
  first->kind = PayloadKind::kPing;

  arena.Release(first);
  PreparedWrite* reacquired = arena.TryAcquire();
  if (reacquired == nullptr) {
    return 1;
  }
  if (reacquired->slot_index != first_slot ||
      reacquired->storage.data() != first_storage) {
    return 1;
  }
  if (reacquired->encoded_size != 0 || reacquired->write_offset != 0 ||
      reacquired->kind != PayloadKind::kBinary) {
    return 1;
  }

  PreparedWriteArena zero_bytes_arena(2, 0);
  if (zero_bytes_arena.TryAcquire() != nullptr) {
    return 1;
  }

  PreparedWriteArena overflow_arena(2, std::numeric_limits<size_t>::max());
  if (overflow_arena.TryAcquire() != nullptr) {
    return 1;
  }

  if constexpr (sizeof(size_t) > sizeof(std::uint32_t)) {
    PreparedWriteArena too_many_slots_arena(
        static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()) + 1, 64);
    if (too_many_slots_arena.TryAcquire() != nullptr) {
      return 1;
    }
  }

  return 0;
}
