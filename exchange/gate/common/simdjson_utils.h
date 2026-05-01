#ifndef AQUILA_EXCHANGE_GATE_COMMON_SIMDJSON_UTILS_H_
#define AQUILA_EXCHANGE_GATE_COMMON_SIMDJSON_UTILS_H_

#include "exchange/common/simdjson_utils.h"

namespace aquila::gate::detail {

using aquila::exchange::detail::FindSimdjsonField;
using aquila::exchange::detail::FindSimdjsonObject;
using aquila::exchange::detail::ReadSimdjsonBool;
using aquila::exchange::detail::ReadSimdjsonString;

}  // namespace aquila::gate::detail

#endif  // AQUILA_EXCHANGE_GATE_COMMON_SIMDJSON_UTILS_H_
