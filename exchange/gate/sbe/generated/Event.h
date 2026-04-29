#ifndef AQUILA_EXCHANGE_GATE_SBE_GENERATED_EVENT_H_
#define AQUILA_EXCHANGE_GATE_SBE_GENERATED_EVENT_H_

#include <cstdint>

namespace gate::sbe {

enum class Event : std::int8_t {
  Subscribe = 0,
  Unsubscribe = 1,
  Update = 2,
  All = 3,
  Api = 4,
};

}  // namespace gate::sbe

#endif  // AQUILA_EXCHANGE_GATE_SBE_GENERATED_EVENT_H_
