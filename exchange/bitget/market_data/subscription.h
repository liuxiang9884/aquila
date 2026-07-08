#ifndef AQUILA_EXCHANGE_BITGET_MARKET_DATA_SUBSCRIPTION_H_
#define AQUILA_EXCHANGE_BITGET_MARKET_DATA_SUBSCRIPTION_H_

#include <iterator>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace aquila::bitget {

namespace detail {

inline std::string BuildTopicSubscriptionRequest(
    std::string_view op, std::string_view inst_type, std::string_view topic,
    std::span<const std::string_view> symbols) {
  fmt::memory_buffer buffer;
  fmt::format_to(std::back_inserter(buffer), R"({{"op":"{}","args":[)", op);
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (i != 0) {
      fmt::format_to(std::back_inserter(buffer), ",");
    }
    fmt::format_to(std::back_inserter(buffer),
                   R"({{"instType":"{}","topic":"{}","symbol":"{}"}})",
                   inst_type, topic, symbols[i]);
  }
  fmt::format_to(std::back_inserter(buffer), "]}}");
  return fmt::to_string(buffer);
}

}  // namespace detail

inline std::string BuildBooks1SubscribeRequest(
    std::string_view inst_type, std::span<const std::string_view> symbols) {
  return detail::BuildTopicSubscriptionRequest("subscribe", inst_type, "books1",
                                               symbols);
}

inline std::string BuildBooks1UnsubscribeRequest(
    std::string_view inst_type, std::span<const std::string_view> symbols) {
  return detail::BuildTopicSubscriptionRequest("unsubscribe", inst_type,
                                               "books1", symbols);
}

inline std::string BuildPublicTradeSubscribeRequest(
    std::string_view inst_type, std::span<const std::string_view> symbols) {
  return detail::BuildTopicSubscriptionRequest("subscribe", inst_type,
                                               "publicTrade", symbols);
}

inline std::string BuildPublicTradeUnsubscribeRequest(
    std::string_view inst_type, std::span<const std::string_view> symbols) {
  return detail::BuildTopicSubscriptionRequest("unsubscribe", inst_type,
                                               "publicTrade", symbols);
}

}  // namespace aquila::bitget

#endif  // AQUILA_EXCHANGE_BITGET_MARKET_DATA_SUBSCRIPTION_H_
