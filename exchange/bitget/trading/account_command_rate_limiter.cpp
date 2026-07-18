#include "exchange/bitget/trading/account_command_rate_limiter.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace aquila::bitget {
namespace {

[[nodiscard]] AccountCommandRateLimitConfigResult Failure(std::string error) {
  AccountCommandRateLimitConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] AccountCommandRateLimitConfigResult Success(
    AccountCommandRateLimitConfig config) {
  AccountCommandRateLimitConfigResult result;
  result.value = config;
  result.ok = true;
  return result;
}

[[nodiscard]] bool ReadBool(const toml::table& table, std::string_view name,
                            bool fallback, bool* value, std::string* error) {
  const toml::node_view<const toml::node> node = table[name];
  if (!node) {
    *value = fallback;
    return true;
  }
  const std::optional<bool> parsed = node.value<bool>();
  if (!parsed) {
    *error = std::string{name} + " must be a boolean";
    return false;
  }
  *value = *parsed;
  return true;
}

[[nodiscard]] bool ReadUInt32(const toml::table& table, std::string_view name,
                              std::uint32_t fallback, std::uint32_t* value,
                              std::string* error) {
  const toml::node_view<const toml::node> node = table[name];
  if (!node) {
    *value = fallback;
    return true;
  }
  const std::optional<std::int64_t> parsed = node.value<std::int64_t>();
  if (!parsed || *parsed < 0 ||
      *parsed > std::numeric_limits<std::uint32_t>::max()) {
    *error = std::string{name} + " must be in uint32 range";
    return false;
  }
  *value = static_cast<std::uint32_t>(*parsed);
  return true;
}

}  // namespace

AccountCommandRateLimitConfigResult ParseAccountCommandRateLimitConfig(
    const toml::table& node) {
  AccountCommandRateLimitConfig config;
  const toml::node_view<const toml::node> section =
      node["bitget_account_command_rate_limit"];
  if (!section) {
    return Success(config);
  }
  const toml::table* table = section.as_table();
  if (table == nullptr) {
    return Failure("bitget_account_command_rate_limit must be a table");
  }

  std::string error;
  if (!(*table)["enabled"]) {
    return Failure("bitget_account_command_rate_limit.enabled is required");
  }
  if (!ReadBool(*table, "enabled", config.enabled, &config.enabled, &error) ||
      !ReadUInt32(*table, "max_commands_per_second",
                  config.max_commands_per_second,
                  &config.max_commands_per_second, &error) ||
      !ReadUInt32(*table, "reserved_exit_commands",
                  config.reserved_exit_commands, &config.reserved_exit_commands,
                  &error)) {
    return Failure("bitget_account_command_rate_limit." + error);
  }
  if (!config.enabled) {
    return Success(config);
  }
  if (config.max_commands_per_second == 0) {
    return Failure(
        "bitget_account_command_rate_limit.max_commands_per_second must be "
        "positive when enabled");
  }
  if (config.reserved_exit_commands >= config.max_commands_per_second) {
    return Failure(
        "bitget_account_command_rate_limit.reserved_exit_commands must be "
        "less than max_commands_per_second");
  }
  return Success(config);
}

AccountCommandRateLimiter::AccountCommandRateLimiter(
    AccountCommandRateLimitConfig config)
    : config_(config),
      accepted_timestamps_ns_(config.enabled ? config.max_commands_per_second
                                             : 0) {}

bool AccountCommandRateLimiter::TryAcquire(bool exit_priority,
                                           std::uint64_t now_ns) noexcept {
  if (!config_.enabled) {
    return true;
  }
  if (config_.max_commands_per_second == 0 ||
      config_.reserved_exit_commands >= config_.max_commands_per_second) {
    return false;
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  EvictExpired(now_ns);
  const std::size_t maximum = config_.max_commands_per_second;
  const std::size_t ordinary_limit = maximum - config_.reserved_exit_commands;
  if (size_ >= maximum || (!exit_priority && size_ >= ordinary_limit)) {
    return false;
  }

  const std::size_t tail = (head_ + size_) % maximum;
  accepted_timestamps_ns_[tail] = now_ns;
  ++size_;
  return true;
}

void AccountCommandRateLimiter::EvictExpired(std::uint64_t now_ns) noexcept {
  const std::size_t capacity = accepted_timestamps_ns_.size();
  while (size_ != 0) {
    const std::uint64_t accepted_ns = accepted_timestamps_ns_[head_];
    if (now_ns < accepted_ns || now_ns - accepted_ns < kWindowNs) {
      return;
    }
    head_ = (head_ + 1) % capacity;
    --size_;
  }
}

}  // namespace aquila::bitget
