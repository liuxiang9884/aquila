#ifndef AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CANDIDATE_IP_LIST_H_
#define AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CANDIDATE_IP_LIST_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <arpa/inet.h>

namespace aquila::tools::gate_order_session_rtt_probe {

struct CandidateIpLoadOptions {
  std::size_t max_candidates{0};
};

struct CandidateIpLoadResult {
  bool ok{false};
  std::vector<std::string> ips;
  std::size_t duplicate_count{0};
  std::string error;
};

namespace candidate_ip_list_detail {

[[nodiscard]] inline std::string_view TrimAsciiWhitespace(
    std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\r' || text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\r' || text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return text;
}

[[nodiscard]] inline bool IsNumericIpAddress(std::string_view text) {
  std::string value{text};
  unsigned char buffer[sizeof(in6_addr)]{};
  return ::inet_pton(AF_INET, value.c_str(), buffer) == 1 ||
         ::inet_pton(AF_INET6, value.c_str(), buffer) == 1;
}

}  // namespace candidate_ip_list_detail

[[nodiscard]] inline CandidateIpLoadResult LoadCandidateIpsFromText(
    std::string_view text, CandidateIpLoadOptions options = {}) {
  CandidateIpLoadResult result;
  absl::flat_hash_set<std::string> seen;

  while (!text.empty()) {
    const std::size_t newline = text.find('\n');
    std::string_view line =
        newline == std::string_view::npos ? text : text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{}
                                             : text.substr(newline + 1);
    line = candidate_ip_list_detail::TrimAsciiWhitespace(line);
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (!candidate_ip_list_detail::IsNumericIpAddress(line)) {
      result.error = "invalid candidate ip line";
      return result;
    }

    std::string ip{line};
    if (!seen.insert(ip).second) {
      ++result.duplicate_count;
      continue;
    }
    if (options.max_candidates != 0 &&
        result.ips.size() >= options.max_candidates) {
      continue;
    }
    result.ips.push_back(std::move(ip));
  }

  if (result.ips.empty()) {
    result.error = "candidate ip list is empty";
    return result;
  }
  result.ok = true;
  return result;
}

}  // namespace aquila::tools::gate_order_session_rtt_probe

#endif  // AQUILA_TOOLS_GATE_ORDER_SESSION_RTT_PROBE_CANDIDATE_IP_LIST_H_
