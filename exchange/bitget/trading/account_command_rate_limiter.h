#ifndef AQUILA_EXCHANGE_BITGET_TRADING_ACCOUNT_COMMAND_RATE_LIMITER_H_
#define AQUILA_EXCHANGE_BITGET_TRADING_ACCOUNT_COMMAND_RATE_LIMITER_H_

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"

namespace aquila::bitget {

struct AccountCommandRateLimitConfig {
  bool enabled{false};
  std::uint32_t max_commands_per_second{0};
  std::uint32_t reserved_exit_commands{0};
};

using AccountCommandRateLimitConfigResult =
    Result<AccountCommandRateLimitConfig>;

[[nodiscard]] AccountCommandRateLimitConfigResult
ParseAccountCommandRateLimitConfig(const toml::table& node);

class AccountCommandRateLimiter {
 public:
  explicit AccountCommandRateLimiter(AccountCommandRateLimitConfig config);

  AccountCommandRateLimiter(const AccountCommandRateLimiter&) = delete;
  AccountCommandRateLimiter& operator=(const AccountCommandRateLimiter&) =
      delete;

  [[nodiscard]] bool TryAcquire(bool exit_priority,
                                std::uint64_t now_ns) noexcept;

  [[nodiscard]] const AccountCommandRateLimitConfig& config() const noexcept {
    return config_;
  }

 private:
  void EvictExpired(std::uint64_t now_ns) noexcept;

  static constexpr std::uint64_t kWindowNs = 1'000'000'000ULL;

  AccountCommandRateLimitConfig config_;
  std::vector<std::uint64_t> accepted_timestamps_ns_;
  std::size_t head_{0};
  std::size_t size_{0};
  std::mutex mutex_;
};

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_TRADING_ACCOUNT_COMMAND_RATE_LIMITER_H_
