#include "tools/bitget/order_session_rtt_probe/connection_plan.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <arpa/inet.h>
#include <fmt/format.h>

#include <csv.hpp>

namespace aquila::tools::bitget_order_session_rtt_probe {
namespace {

[[nodiscard]] ProbeConnectionsCsvResult Failure(std::string error) {
  ProbeConnectionsCsvResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] ProbeConnectionsCsvResult Success(
    std::vector<ProbeConnectionConfig> connections) {
  ProbeConnectionsCsvResult result;
  result.ok = true;
  result.connections = std::move(connections);
  return result;
}

[[nodiscard]] std::string ReadString(csv::CSVRow& row,
                                     std::string_view column) {
  return std::string{row[std::string{column}].get<std::string_view>()};
}

[[nodiscard]] bool ParseBool(std::string_view text, bool* output) noexcept {
  if (text == "true") {
    *output = true;
    return true;
  }
  if (text == "false") {
    *output = false;
    return true;
  }
  return false;
}

[[nodiscard]] bool IsNumericIp(std::string_view text) {
  if (text.empty()) {
    return true;
  }
  std::string value{text};
  unsigned char buffer[sizeof(in6_addr)]{};
  return ::inet_pton(AF_INET, value.c_str(), buffer) == 1 ||
         ::inet_pton(AF_INET6, value.c_str(), buffer) == 1;
}

[[nodiscard]] bool ParseCpu(std::string_view text,
                            std::int32_t* output) noexcept {
  std::int64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || value < -1 ||
      value > std::numeric_limits<std::int32_t>::max()) {
    return false;
  }
  *output = static_cast<std::int32_t>(value);
  return true;
}

}  // namespace

ProbeConnectionsCsvResult LoadProbeConnectionsCsvFile(
    const std::filesystem::path& path) {
  try {
    csv::CSVReader reader(path.string());
    std::vector<ProbeConnectionConfig> connections;
    absl::flat_hash_set<std::string> names;
    std::uint64_t row_index = 1;
    for (csv::CSVRow& row : reader) {
      ++row_index;
      ProbeConnectionConfig connection{
          .name = ReadString(row, "name"),
          .group = ReadString(row, "group"),
          .host = ReadString(row, "host"),
          .connect_ip = ReadString(row, "connect_ip"),
          .port = ReadString(row, "port"),
      };
      if (connection.name.empty() || connection.group.empty() ||
          connection.host.empty() || connection.port.empty()) {
        return Failure(fmt::format(
            "connections CSV row {} has empty required field", row_index));
      }
      if (!names.insert(connection.name).second) {
        return Failure(
            fmt::format("connections CSV row {} duplicates name '{}'",
                        row_index, connection.name));
      }
      if (!IsNumericIp(connection.connect_ip)) {
        return Failure(
            fmt::format("connections CSV row {} has invalid connect_ip '{}'",
                        row_index, connection.connect_ip));
      }
      if (!ParseBool(ReadString(row, "enable_tls"), &connection.enable_tls)) {
        return Failure(fmt::format(
            "connections CSV row {} enable_tls must be true or false",
            row_index));
      }
      if (!ParseCpu(ReadString(row, "worker_cpu_id"),
                    &connection.worker_cpu_id)) {
        return Failure(fmt::format(
            "connections CSV row {} worker_cpu_id must be -1 or int32",
            row_index));
      }
      connections.push_back(std::move(connection));
    }
    if (connections.empty()) {
      return Failure("connections CSV is empty");
    }
    return Success(std::move(connections));
  } catch (const std::exception& exc) {
    return Failure(fmt::format("failed to load connections CSV '{}': {}",
                               path.string(), exc.what()));
  }
}

}  // namespace aquila::tools::bitget_order_session_rtt_probe
