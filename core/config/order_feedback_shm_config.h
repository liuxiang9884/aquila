#ifndef AQUILA_CORE_CONFIG_ORDER_FEEDBACK_SHM_CONFIG_H_
#define AQUILA_CORE_CONFIG_ORDER_FEEDBACK_SHM_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"

namespace aquila::config {

inline constexpr std::uint32_t kOrderFeedbackShmMaxStrategyCount = 8;
inline constexpr std::uint32_t kOrderFeedbackShmQueueCapacity = 65536;

struct OrderFeedbackShmRuntimeConfig {
  std::string shm_name;
  std::string channel_name;
  std::uint32_t max_strategy_count{kOrderFeedbackShmMaxStrategyCount};
  std::uint32_t queue_capacity{kOrderFeedbackShmQueueCapacity};
  bool create{true};
  bool remove_existing{false};
};

using OrderFeedbackShmConfigResult = Result<OrderFeedbackShmRuntimeConfig>;

[[nodiscard]] OrderFeedbackShmConfigResult ParseOrderFeedbackShmConfig(
    const toml::table& node);

[[nodiscard]] OrderFeedbackShmConfigResult LoadOrderFeedbackShmConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_ORDER_FEEDBACK_SHM_CONFIG_H_
