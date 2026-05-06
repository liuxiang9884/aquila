#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "core/market_data/data_shm.h"

namespace {

namespace md = aquila::market_data;

std::atomic<bool> signal_stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void HandleSignal(int /*signal*/) {
  signal_stop_requested.store(true, std::memory_order_relaxed);
}

void PrintBookTicker(const aquila::BookTicker& book_ticker,
                     std::uint64_t overrun_count) {
  fmt::print(
      "id={} symbol_id={} exchange={} exchange_ns={} local_ns={} "
      "bid_price={:.12g} bid_volume={:.12g} ask_price={:.12g} "
      "ask_volume={:.12g} overrun_count={}\n",
      book_ticker.id, book_ticker.symbol_id,
      magic_enum::enum_name(book_ticker.exchange), book_ticker.exchange_ns,
      book_ticker.local_ns, book_ticker.bid_price, book_ticker.bid_volume,
      book_ticker.ask_price, book_ticker.ask_volume, overrun_count);
}

}  // namespace

int main(int argc, char** argv) {
  std::string shm_name;
  std::string channel_name{"book_ticker_channel"};
  bool from_latest{false};
  bool from_earliest{false};
  std::uint64_t max_messages{0};

  CLI::App app{"BookTicker SHM reader"};
  app.add_option("--shm-name", shm_name, "SHM segment name")->required();
  app.add_option("--channel-name", channel_name, "SHM channel name");
  app.add_flag("--from-latest", from_latest,
               "start at current producer position");
  app.add_flag("--from-earliest", from_earliest,
               "start at earliest currently visible position");
  app.add_option("--max-messages", max_messages,
                 "maximum messages to print; 0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  if (from_latest && from_earliest) {
    fmt::print(stderr,
               "--from-latest and --from-earliest are mutually exclusive\n");
    return 2;
  }

  try {
    md::BookTickerShmConfig config{
        .enabled = true,
        .shm_name = shm_name,
        .channel_name = channel_name,
        .create = false,
        .remove_existing = false,
    };
    md::BookTickerShmReader reader(config);
    if (from_earliest) {
      reader.SeekEarliestVisible();
    } else {
      reader.SeekLatest();
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::uint64_t printed{0};
    while (!signal_stop_requested.load(std::memory_order_relaxed)) {
      aquila::BookTicker book_ticker{};
      if (!reader.TryReadOne(&book_ticker)) {
        std::this_thread::yield();
        continue;
      }

      PrintBookTicker(book_ticker, reader.overrun_count());
      ++printed;
      if (max_messages != 0 && printed >= max_messages) {
        break;
      }
    }
  } catch (const std::exception& exc) {
    fmt::print(stderr, "book_ticker_shm_reader_error={}\n", exc.what());
    return 1;
  }

  return 0;
}
