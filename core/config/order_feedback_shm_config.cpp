#include "core/config/order_feedback_shm_config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/trading/order_feedback_shm.h"
#include "nova/utils/log.h"

namespace aquila::config {

static_assert(kOrderFeedbackShmMaxStrategyCount ==
              aquila::kMaxOrderFeedbackStrategies);
static_assert(kOrderFeedbackShmQueueCapacity ==
              aquila::kOrderFeedbackQueueCapacity);

namespace {

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] OrderFeedbackShmConfigResult Failure(std::string error) {
  MaybeLogError(error);
  OrderFeedbackShmConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] OrderFeedbackShmConfigResult Success(
    OrderFeedbackShmRuntimeConfig config) {
  OrderFeedbackShmConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

class OrderFeedbackShmConfigParser {
 public:
  explicit OrderFeedbackShmConfigParser(const toml::table& node)
      : node_(node) {}

  [[nodiscard]] OrderFeedbackShmConfigResult Parse() {
    const toml::table* order_feedback_shm =
        node_["order_feedback_shm"].as_table();
    if (order_feedback_shm == nullptr) {
      return Failure("order_feedback_shm section is required");
    }

    ParseRequiredStrings(*order_feedback_shm);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    config_.max_strategy_count =
        UInt32Or((*order_feedback_shm)["max_strategy_count"],
                 config_.max_strategy_count,
                 "order_feedback_shm.max_strategy_count");
    if (!ok_) {
      return Failure(std::move(error_));
    }
    if (config_.max_strategy_count != kOrderFeedbackShmMaxStrategyCount) {
      return Failure(
          "order_feedback_shm.max_strategy_count must equal compiled "
          "feedback lane count");
    }

    config_.queue_capacity =
        UInt32Or((*order_feedback_shm)["queue_capacity"],
                 config_.queue_capacity,
                 "order_feedback_shm.queue_capacity");
    if (!ok_) {
      return Failure(std::move(error_));
    }
    if (config_.queue_capacity != kOrderFeedbackShmQueueCapacity) {
      return Failure(
          "order_feedback_shm.queue_capacity must equal compiled queue "
          "capacity");
    }

    config_.create =
        BoolOr((*order_feedback_shm)["create"], config_.create);
    config_.remove_existing = BoolOr((*order_feedback_shm)["remove_existing"],
                                     config_.remove_existing);
    if (config_.remove_existing && !config_.create) {
      return Failure(
          "order_feedback_shm.remove_existing requires create=true");
    }

    return Success(std::move(config_));
  }

 private:
  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
  }

  [[nodiscard]] bool BoolOr(toml::node_view<const toml::node> value_node,
                            bool fallback) const {
    const std::optional<bool> value = value_node.value<bool>();
    return value.value_or(fallback);
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

  void ParseRequiredStrings(const toml::table& order_feedback_shm) {
    config_.shm_name = RequiredString(order_feedback_shm["shm_name"],
                                      "order_feedback_shm.shm_name");
    if (!ok_) {
      return;
    }
    config_.channel_name = RequiredString(order_feedback_shm["channel_name"],
                                          "order_feedback_shm.channel_name");
  }

  void Fail(std::string_view name, std::string_view message) {
    ok_ = false;
    error_.assign(name);
    error_.append(message);
  }

  const toml::table& node_;
  OrderFeedbackShmRuntimeConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

OrderFeedbackShmConfigResult ParseOrderFeedbackShmConfig(
    const toml::table& node) {
  return OrderFeedbackShmConfigParser{node}.Parse();
}

OrderFeedbackShmConfigResult LoadOrderFeedbackShmConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseOrderFeedbackShmConfig(parsed);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load order feedback shm config: "} +
                   exc.what());
  }
}

}  // namespace aquila::config
