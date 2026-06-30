#include "core/config/strategy_config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "core/config/order_feedback_shm_config.h"
#include "nova/utils/log.h"

namespace aquila::config {
namespace {

static_assert(kOrderFeedbackShmMaxStrategyCount > 0);
static_assert(
    kOrderFeedbackShmMaxStrategyCount <=
    static_cast<std::uint32_t>(std::numeric_limits<std::uint8_t>::max()) + 1U);

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] StrategyConfigResult Failure(std::string error) {
  MaybeLogError(error);
  StrategyConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] StrategyConfigResult Success(StrategyConfig config) {
  StrategyConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

[[nodiscard]] bool ParseMode(std::string_view text, StrategyMode* mode) {
  if (text == "dry_run") {
    *mode = StrategyMode::kDryRun;
    return true;
  }
  if (text == "live") {
    *mode = StrategyMode::kLive;
    return true;
  }
  return false;
}

[[nodiscard]] bool ParseIdlePolicy(std::string_view text,
                                   StrategyLoopIdlePolicy* policy) {
  if (text == "spin") {
    *policy = StrategyLoopIdlePolicy::kSpin;
    return true;
  }
  if (text == "yield") {
    *policy = StrategyLoopIdlePolicy::kYield;
    return true;
  }
  return false;
}

class StrategyConfigParser {
 public:
  explicit StrategyConfigParser(const toml::table& node) : node_(node) {}
  StrategyConfigParser(const toml::table& node,
                       std::filesystem::path config_file_path)
      : node_(node), config_file_path_(std::move(config_file_path)) {}

  [[nodiscard]] StrategyConfigResult Parse() {
    const toml::table* strategy = node_["strategy"].as_table();
    if (strategy == nullptr) {
      return Failure("strategy section is required");
    }

    ParseStrategy(*strategy);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseLoop((*strategy)["loop"]);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseDataReader((*strategy)["data_reader"]);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseOrderExecutionConfig(*strategy);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseFeedback((*strategy)["feedback"]);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    return Success(std::move(config_));
  }

 private:
  [[nodiscard]] std::string StringOr(
      toml::node_view<const toml::node> value_node,
      const std::string& fallback) const {
    const std::optional<std::string> value = value_node.value<std::string>();
    return value.value_or(fallback);
  }

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback) const {
    const std::optional<bool> value = value_node.value<bool>();
    return value.value_or(fallback);
  }

  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  [[nodiscard]] std::int64_t RequiredInteger(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      Fail(name, " is required");
      return 0;
    }
    return *value;
  }

  [[nodiscard]] std::int32_t Int32Or(
      toml::node_view<const toml::node> value_node, std::int32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()) {
      Fail(name, " exceeds int32 range");
      return fallback;
    }
    return static_cast<std::int32_t>(*value);
  }

  [[nodiscard]] std::uint64_t UInt64Or(
      toml::node_view<const toml::node> value_node, std::uint64_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value < 0) {
      Fail(name, " must be non-negative");
      return fallback;
    }
    return static_cast<std::uint64_t>(*value);
  }

  [[nodiscard]] std::uint32_t UInt32Or(
      toml::node_view<const toml::node> value_node, std::uint32_t fallback,
      std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      return fallback;
    }
    if (*value <= 0) {
      Fail(name, " must be positive");
      return fallback;
    }
    if (*value > std::numeric_limits<std::uint32_t>::max()) {
      Fail(name, " exceeds uint32 range");
      return fallback;
    }
    return static_cast<std::uint32_t>(*value);
  }

  void ParseStrategy(const toml::table& strategy) {
    config_.name = RequiredString(strategy["name"], "strategy.name");
    if (!ok_) {
      return;
    }

    const std::int64_t strategy_id =
        RequiredInteger(strategy["strategy_id"], "strategy.strategy_id");
    if (!ok_) {
      return;
    }
    if (strategy_id < 0 ||
        strategy_id >=
            static_cast<std::int64_t>(kOrderFeedbackShmMaxStrategyCount)) {
      Fail("strategy.strategy_id",
           fmt::format(" must be between 0 and {} (feedback lane count {})",
                       kOrderFeedbackShmMaxStrategyCount - 1,
                       kOrderFeedbackShmMaxStrategyCount));
      return;
    }
    config_.strategy_id = static_cast<std::uint8_t>(strategy_id);

    const std::string mode_text = StringOr(strategy["mode"], "dry_run");
    if (!ParseMode(mode_text, &config_.mode)) {
      Fail("strategy.mode", " must be dry_run or live");
      return;
    }

    const std::int64_t order_capacity =
        RequiredInteger(strategy["order_capacity"], "strategy.order_capacity");
    if (!ok_) {
      return;
    }
    if (order_capacity <= 0) {
      Fail("strategy.order_capacity", " must be positive");
      return;
    }
    if (static_cast<std::uint64_t>(order_capacity) >
        std::numeric_limits<std::size_t>::max()) {
      Fail("strategy.order_capacity", " exceeds size_t range");
      return;
    }
    config_.order_capacity = static_cast<std::size_t>(order_capacity);

    config_.user_config_path = ResolveConfigPath(
        RequiredString(strategy["config"], "strategy.config"));
  }

  void ParseLoop(toml::node_view<const toml::node> loop) {
    const std::string idle_policy_text = StringOr(loop["idle_policy"], "spin");
    if (!ParseIdlePolicy(idle_policy_text, &config_.loop.idle_policy)) {
      Fail("strategy.loop.idle_policy", " must be spin or yield");
      return;
    }

    config_.loop.bind_cpu_id =
        Int32Or(loop["bind_cpu_id"], config_.loop.bind_cpu_id,
                "strategy.loop.bind_cpu_id");
    if (!ok_) {
      return;
    }
    config_.loop.max_loop_seconds =
        UInt64Or(loop["max_loop_seconds"], config_.loop.max_loop_seconds,
                 "strategy.loop.max_loop_seconds");
  }

  void ParseDataReader(toml::node_view<const toml::node> data_reader) {
    config_.data_reader.config_path = ResolveConfigPath(
        RequiredString(data_reader["config"], "strategy.data_reader.config"));
  }

  void ParseOrderExecutionConfig(const toml::table& strategy) {
    const toml::table* order_session = strategy["order_session"].as_table();
    const toml::table* order_gateway = strategy["order_gateway"].as_table();
    if (order_session != nullptr && order_gateway != nullptr) {
      Fail("strategy.order_session / strategy.order_gateway",
           " are mutually exclusive");
      return;
    }
    if (order_session == nullptr && order_gateway == nullptr) {
      Fail("strategy.order_session / strategy.order_gateway",
           " one section is required");
      return;
    }
    if (order_session != nullptr) {
      config_.order_session.config_path = ResolveConfigPath(RequiredString(
          (*order_session)["config"], "strategy.order_session.config"));
      return;
    }
    config_.order_gateway.config_path = ResolveConfigPath(RequiredString(
        (*order_gateway)["config"], "strategy.order_gateway.config"));
  }

  void ParseFeedback(toml::node_view<const toml::node> feedback) {
    config_.feedback.enabled =
        BoolOr(feedback["enabled"], config_.feedback.enabled);
    config_.feedback.poll_budget =
        UInt32Or(feedback["poll_budget"], config_.feedback.poll_budget,
                 "strategy.feedback.poll_budget");
    if (!ok_) {
      return;
    }
    config_.feedback.force_claim =
        BoolOr(feedback["force_claim"], config_.feedback.force_claim);

    config_.feedback.shm_name =
        StringOr(feedback["shm_name"], config_.feedback.shm_name);
    config_.feedback.channel_name =
        StringOr(feedback["channel_name"], config_.feedback.channel_name);

    if (!config_.feedback.enabled) {
      return;
    }
    if (config_.feedback.shm_name.empty()) {
      Fail("strategy.feedback.shm_name", " is required");
      return;
    }
    if (config_.feedback.channel_name.empty()) {
      Fail("strategy.feedback.channel_name", " is required");
    }
  }

  [[nodiscard]] std::filesystem::path ResolveConfigPath(
      const std::filesystem::path& path) const {
    if (path.is_absolute() || config_file_path_.empty()) {
      return path;
    }

    std::filesystem::path base =
        std::filesystem::absolute(config_file_path_).parent_path();
    while (!base.empty()) {
      const std::filesystem::path candidate = (base / path).lexically_normal();
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
      if (base == base.root_path()) {
        break;
      }
      base = base.parent_path();
    }
    return path;
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  std::filesystem::path config_file_path_;
  StrategyConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

StrategyConfigResult ParseStrategyConfig(const toml::table& node) {
  return StrategyConfigParser{node}.Parse();
}

StrategyConfigResult ParseStrategyConfig(
    const toml::table& node, const std::filesystem::path& config_file_path) {
  return StrategyConfigParser{node, config_file_path}.Parse();
}

StrategyConfigResult LoadStrategyConfigFile(const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseStrategyConfig(parsed, path);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load strategy config: "} +
                   exc.what());
  }
}

}  // namespace aquila::config
