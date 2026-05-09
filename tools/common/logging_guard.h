#pragma once

#include <toml++/toml.hpp>

#include "nova/utils/log.h"

namespace aquila::tools {

class LoggingGuard {
 public:
  explicit LoggingGuard(const toml::table& toml) {
    nova::LogConfig log_config;
    log_config.FromToml(toml["log"]);
    nova::InitializeLogging(log_config);
  }

  ~LoggingGuard() noexcept {
    nova::StopLogging();
  }

  LoggingGuard(const LoggingGuard&) = delete;
  LoggingGuard& operator=(const LoggingGuard&) = delete;
  LoggingGuard(LoggingGuard&&) = delete;
  LoggingGuard& operator=(LoggingGuard&&) = delete;
};

}  // namespace aquila::tools
