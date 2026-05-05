#ifndef AQUILA_CORE_COMMON_RESULT_H_
#define AQUILA_CORE_COMMON_RESULT_H_

#include <string>

namespace aquila {

template <typename T>
struct Result {
  T value{};
  std::string error;
  bool ok{false};
};

}  // namespace aquila

#endif  // AQUILA_CORE_COMMON_RESULT_H_
