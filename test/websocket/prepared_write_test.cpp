#include "core/websocket/prepared_write.h"

#include <cstddef>

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

  arena.Release(first);
  PreparedWrite* reacquired = arena.TryAcquire();
  if (reacquired == nullptr) {
    return 1;
  }
  if (reacquired->slot_index != first_slot ||
      reacquired->storage.data() != first_storage) {
    return 1;
  }

  return 0;
}
