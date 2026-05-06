#ifndef AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_

#include <cstdint>
#include <string>

namespace aquila::market_data {

inline constexpr std::uint64_t kBookTickerShmCapacity = 65536;

struct BookTickerShmConfig {
  bool enabled{false};
  std::string shm_name;
  std::string channel_name;
  bool create{true};
  bool remove_existing{false};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_
