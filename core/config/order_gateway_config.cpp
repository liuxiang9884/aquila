#include "core/config/order_gateway_config.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "nova/utils/log.h"

namespace aquila::config {
namespace {

void MaybeLogError(std::string_view message) {
  if (::nova::kLogManager.logger() != nullptr) {
    NOVA_ERROR("{}", message);
  }
}

[[nodiscard]] OrderGatewayConfigResult Failure(std::string error) {
  MaybeLogError(error);
  OrderGatewayConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] OrderGatewayConfigResult Success(OrderGatewayConfig config) {
  OrderGatewayConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}

class OrderGatewayConfigParser {
 public:
  explicit OrderGatewayConfigParser(const toml::table& node) : node_(node) {}
  OrderGatewayConfigParser(const toml::table& node,
                           std::filesystem::path config_file_path)
      : node_(node), config_file_path_(std::move(config_file_path)) {}

  [[nodiscard]] OrderGatewayConfigResult Parse() {
    const toml::table* order_gateway = node_["order_gateway"].as_table();
    if (order_gateway == nullptr) {
      return Failure("order_gateway section is required");
    }

    ParseGateway(*order_gateway);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    ParseRoutes(*order_gateway);
    if (!ok_) {
      return Failure(std::move(error_));
    }

    if (config_.routes.size() != config_.route_count) {
      return Failure("order_gateway.routes.size must match route_count");
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

  [[nodiscard]] std::string RequiredString(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::string> value = value_node.value<std::string>();
    if (!value || value->empty()) {
      Fail(name, " is required");
      return {};
    }
    return *value;
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

  [[nodiscard]] std::int32_t RequiredInt32(
      toml::node_view<const toml::node> value_node, std::string_view name) {
    const std::optional<std::int64_t> value = value_node.value<std::int64_t>();
    if (!value) {
      Fail(name, " is required");
      return 0;
    }
    if (*value < std::numeric_limits<std::int32_t>::min() ||
        *value > std::numeric_limits<std::int32_t>::max()) {
      Fail(name, " exceeds int32 range");
      return 0;
    }
    return static_cast<std::int32_t>(*value);
  }

  void ParseGateway(const toml::table& order_gateway) {
    config_.name = RequiredString(order_gateway["name"], "order_gateway.name");
    if (!ok_) {
      return;
    }
    config_.shm_name = StringOr(order_gateway["shm_name"], config_.name);
    if (config_.shm_name.empty()) {
      Fail("order_gateway.shm_name", " must be non-empty");
      return;
    }

    const std::uint32_t route_count =
        UInt32Or(order_gateway["route_count"], config_.route_count,
                 "order_gateway.route_count");
    if (!ok_) {
      return;
    }
    if (route_count == 0 || route_count > core::kMaxOrderGatewayRoutes) {
      Fail("order_gateway.route_count", " must be in [1, 16]");
      return;
    }
    config_.route_count = static_cast<std::uint16_t>(route_count);

    config_.command_queue_capacity = UInt32Or(
        order_gateway["command_queue_capacity"], config_.command_queue_capacity,
        "order_gateway.command_queue_capacity");
    if (!ok_) {
      return;
    }
    config_.event_queue_capacity = UInt32Or(
        order_gateway["event_queue_capacity"], config_.event_queue_capacity,
        "order_gateway.event_queue_capacity");
    if (!ok_) {
      return;
    }
    config_.startup_ready_timeout_s =
        UInt32Or(order_gateway["startup_ready_timeout_s"],
                 config_.startup_ready_timeout_s,
                 "order_gateway.startup_ready_timeout_s");
  }

  void ParseRoutes(const toml::table& order_gateway) {
    const toml::array* routes = order_gateway["routes"].as_array();
    if (routes == nullptr) {
      Fail("order_gateway.routes", " is required");
      return;
    }

    config_.routes.reserve(routes->size());
    for (std::size_t i = 0; i < routes->size(); ++i) {
      const toml::table* route = (*routes)[i].as_table();
      if (route == nullptr) {
        Fail("order_gateway.routes", " entries must be tables");
        return;
      }
      OrderGatewayRouteConfig route_config;
      route_config.name =
          RequiredString((*route)["name"], "order_gateway.routes.name");
      if (!ok_) {
        return;
      }
      route_config.order_session_config_path = ResolveConfigPath(
          RequiredString((*route)["order_session_config"],
                         "order_gateway.routes.order_session_config"));
      if (!ok_) {
        return;
      }
      route_config.worker_cpu_id = RequiredInt32(
          (*route)["worker_cpu_id"], "order_gateway.routes.worker_cpu_id");
      if (!ok_) {
        return;
      }
      config_.routes.push_back(std::move(route_config));
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
  OrderGatewayConfig config_;
  std::string error_;
  bool ok_{true};
};

}  // namespace

OrderGatewayConfigResult ParseOrderGatewayConfig(const toml::table& node) {
  return OrderGatewayConfigParser{node}.Parse();
}

OrderGatewayConfigResult ParseOrderGatewayConfig(
    const toml::table& node, const std::filesystem::path& config_file_path) {
  return OrderGatewayConfigParser{node, config_file_path}.Parse();
}

OrderGatewayConfigResult LoadOrderGatewayConfigFile(
    const std::filesystem::path& path) {
  try {
    const toml::parse_result parsed = toml::parse_file(path.string());
    return ParseOrderGatewayConfig(parsed, path);
  } catch (const std::exception& exc) {
    return Failure(std::string{"failed to load order gateway config: "} +
                   exc.what());
  }
}

}  // namespace aquila::config
