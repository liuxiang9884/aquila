#ifndef AQUILA_CORE_CONFIG_STRATEGY_CONFIG_H_
#define AQUILA_CORE_CONFIG_STRATEGY_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <toml++/toml.hpp>

#include "core/common/result.h"

namespace aquila::config {

enum class StrategyMode : std::uint8_t {
  kDryRun,
  kLive,
};

enum class StrategyLoopIdlePolicy : std::uint8_t {
  kSpin,
  kYield,
};

struct StrategyLoopConfig {
  StrategyLoopIdlePolicy idle_policy{StrategyLoopIdlePolicy::kSpin};
  std::int32_t bind_cpu_id{-1};
  std::uint64_t max_loop_seconds{0};
};

struct StrategyDataReaderConfig {
  std::filesystem::path config_path;
};

struct StrategyOrderSessionConfig {
  std::filesystem::path config_path;
};

struct StrategyOrderGatewayConfig {
  std::filesystem::path config_path;
};

struct StrategyFeedbackConfig {
  bool enabled{true};
  std::string shm_name;
  std::string channel_name;
  std::uint32_t poll_budget{32};
  bool force_claim{false};
};

struct StrategyConfig {
  std::string name;
  std::uint8_t strategy_id{0};
  StrategyMode mode{StrategyMode::kDryRun};
  std::size_t order_capacity{0};
  std::filesystem::path user_config_path;
  StrategyLoopConfig loop;
  StrategyDataReaderConfig data_reader;
  StrategyOrderSessionConfig order_session;
  StrategyOrderGatewayConfig order_gateway;
  StrategyFeedbackConfig feedback;
};

using StrategyConfigResult = Result<StrategyConfig>;

[[nodiscard]] StrategyConfigResult ParseStrategyConfig(const toml::table& node);

[[nodiscard]] StrategyConfigResult ParseStrategyConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] StrategyConfigResult LoadStrategyConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_STRATEGY_CONFIG_H_
