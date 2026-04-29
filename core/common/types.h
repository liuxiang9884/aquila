#ifndef AQUILA_CORE_COMMON_TYPES_H_
#define AQUILA_CORE_COMMON_TYPES_H_

#include <cstdint>

namespace aquila {

enum class Exchange : std::uint8_t {
  kBinance = 0,
  kOkx = 1,
  kGate = 2,
  kBybit = 3,
  kBitget = 4,
  kCoinbase = 5,
};

}  // namespace aquila

#endif  // AQUILA_CORE_COMMON_TYPES_H_
