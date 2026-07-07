#ifndef AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_

#include <cstdint>
#include <string>

namespace aquila::market_data {

inline constexpr std::uint64_t kBookTickerShmCapacity = 65536;
inline constexpr std::uint64_t kTradeShmCapacity = 65536;

struct BookTickerShmConfig {
  bool enabled{false};
  std::string shm_name;
  std::string channel_name;
  bool create{true};
  bool remove_existing{false};
};

struct TradeShmConfig {
  bool enabled{false};
  std::string shm_name;
  std::string channel_name;
  bool create{true};
  bool remove_existing{false};
};

struct DataShmConfig {
  bool enabled{false};
  bool book_ticker_enabled{true};
  bool trade_enabled{true};
  std::string shm_name;
  std::string book_ticker_channel_name{"book_ticker_channel"};
  std::string trade_channel_name{"trade_channel"};
  bool create{true};
  bool remove_existing{false};

  [[nodiscard]] BookTickerShmConfig BookTickerConfig() const {
    return BookTickerShmConfig{.enabled = enabled && book_ticker_enabled,
                               .shm_name = shm_name,
                               .channel_name = book_ticker_channel_name,
                               .create = create,
                               .remove_existing = remove_existing};
  }

  [[nodiscard]] TradeShmConfig TradeConfig() const {
    return TradeShmConfig{.enabled = enabled && trade_enabled,
                          .shm_name = shm_name,
                          .channel_name = trade_channel_name,
                          .create = create,
                          .remove_existing = remove_existing};
  }

  [[nodiscard]] BookTickerShmConfig BookTickerConfigForAttach() const {
    BookTickerShmConfig config = BookTickerConfig();
    config.create = false;
    config.remove_existing = false;
    return config;
  }

  [[nodiscard]] TradeShmConfig TradeConfigForAttach() const {
    TradeShmConfig config = TradeConfig();
    config.create = false;
    config.remove_existing = false;
    return config;
  }
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_
