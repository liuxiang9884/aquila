#ifndef AQUILA_STRATEGY_LEAD_LAG_RAW_MARKET_STATE_H_
#define AQUILA_STRATEGY_LEAD_LAG_RAW_MARKET_STATE_H_

#include <cstdint>
#include <vector>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/types.h"

namespace aquila::strategy::leadlag {

[[nodiscard]] inline std::int64_t BookTickerEventTimeNs(
    const BookTicker& ticker) noexcept {
  if (ticker.event_ns != 0) {
    return ticker.event_ns;
  }
  return ticker.exchange_ns != 0 ? ticker.exchange_ns : ticker.local_ns;
}

struct MarketUpdate {
  std::int32_t symbol_id{-1};
  PairRole role{PairRole::kNone};
  bool tracked{false};
  bool price_changed{false};
  bool both_sides_valid{false};
};

struct ActiveSeed {
  QuoteSnapshot lead;
  QuoteSnapshot lag;
  bool valid{false};
  bool resume_lead_tick{false};
};

struct MarketSideState {
  QuoteSnapshot latest_quote;
  QuoteSnapshot previous_quote;
  bool has_quote{false};
  bool has_previous_quote{false};

  [[nodiscard]] bool Update(const BookTicker& ticker) noexcept {
    const QuoteSnapshot next{
        .id = ticker.id,
        .event_ns = BookTickerEventTimeNs(ticker),
        .exchange_ns = ticker.exchange_ns,
        .local_ns = ticker.local_ns,
        .bid_price = ticker.bid_price,
        .ask_price = ticker.ask_price,
    };
    if (!has_quote) {
      latest_quote = next;
      has_quote = true;
      return true;
    }
    if (ticker.bid_price == latest_quote.bid_price &&
        ticker.ask_price == latest_quote.ask_price) {
      latest_quote.event_ns = next.event_ns;
      latest_quote.exchange_ns = next.exchange_ns;
      latest_quote.local_ns = next.local_ns;
      latest_quote.id = next.id;
      return false;
    }
    previous_quote = latest_quote;
    has_previous_quote = true;
    latest_quote = next;
    return true;
  }
};

struct PairMarketState {
  MarketSideState lead;
  MarketSideState lag;
  std::int64_t last_event_ns{0};

  [[nodiscard]] bool BothSidesValid() const noexcept {
    return lead.has_quote && lag.has_quote;
  }

  [[nodiscard]] MarketUpdate Update(PairRole role,
                                    const BookTicker& ticker) noexcept {
    MarketUpdate update{
        .symbol_id = ticker.symbol_id,
        .role = role,
        .tracked = true,
        .price_changed = false,
        .both_sides_valid = false,
    };
    if (role == PairRole::kLead) {
      update.price_changed = lead.Update(ticker);
    } else if (role == PairRole::kLag) {
      update.price_changed = lag.Update(ticker);
    }
    last_event_ns = BookTickerEventTimeNs(ticker);
    update.both_sides_valid = BothSidesValid();
    return update;
  }

  [[nodiscard]] ActiveSeed SelectActiveSeed(PairRole trigger_role,
                                            bool price_changed) const noexcept {
    ActiveSeed seed;
    if (!BothSidesValid()) {
      return seed;
    }
    seed.valid = true;
    seed.lead = lead.latest_quote;
    seed.lag = lag.latest_quote;

    if (lead.has_previous_quote &&
        (trigger_role == PairRole::kLag || price_changed)) {
      seed.lead = lead.previous_quote;
    }
    if (trigger_role == PairRole::kLag && price_changed &&
        lag.has_previous_quote) {
      seed.lag = lag.previous_quote;
    }
    seed.resume_lead_tick = trigger_role == PairRole::kLag;
    return seed;
  }
};

struct PairMarketSlot {
  bool initialized{false};
  Exchange lead_exchange{Exchange::kBinance};
  Exchange lag_exchange{Exchange::kGate};
  PairMarketState market;
};

class RawMarketState {
 public:
  void Reset(const Config& config) {
    std::int32_t max_symbol_id = -1;
    for (const PairConfig& pair : config.pairs) {
      if (pair.symbol_id > max_symbol_id) {
        max_symbol_id = pair.symbol_id;
      }
    }
    pairs_by_symbol_id_.assign(static_cast<std::size_t>(max_symbol_id + 1),
                               PairMarketSlot{});
    for (const PairConfig& pair : config.pairs) {
      PairMarketSlot& slot =
          pairs_by_symbol_id_[static_cast<std::size_t>(pair.symbol_id)];
      slot.initialized = true;
      slot.lead_exchange = pair.lead_exchange;
      slot.lag_exchange = pair.lag_exchange;
      slot.market = PairMarketState{};
    }
  }

  [[nodiscard]] MarketUpdate OnBookTicker(const BookTicker& ticker) noexcept {
    PairMarketSlot* slot = MutableSlot(ticker.symbol_id);
    if (slot == nullptr || !slot->initialized) {
      return MarketUpdate{.symbol_id = ticker.symbol_id};
    }
    if (ticker.exchange == slot->lead_exchange) {
      return slot->market.Update(PairRole::kLead, ticker);
    }
    if (ticker.exchange == slot->lag_exchange) {
      return slot->market.Update(PairRole::kLag, ticker);
    }
    return MarketUpdate{.symbol_id = ticker.symbol_id};
  }

  [[nodiscard]] const PairMarketState* FindPair(
      std::int32_t symbol_id) const noexcept {
    const PairMarketSlot* slot = Slot(symbol_id);
    if (slot == nullptr || !slot->initialized) {
      return nullptr;
    }
    return &slot->market;
  }

  [[nodiscard]] PairMarketState* MutablePair(std::int32_t symbol_id) noexcept {
    PairMarketSlot* slot = MutableSlot(symbol_id);
    if (slot == nullptr || !slot->initialized) {
      return nullptr;
    }
    return &slot->market;
  }

 private:
  [[nodiscard]] PairMarketSlot* MutableSlot(std::int32_t symbol_id) noexcept {
    if (symbol_id < 0 ||
        static_cast<std::size_t>(symbol_id) >= pairs_by_symbol_id_.size()) {
      return nullptr;
    }
    return &pairs_by_symbol_id_[static_cast<std::size_t>(symbol_id)];
  }

  [[nodiscard]] const PairMarketSlot* Slot(
      std::int32_t symbol_id) const noexcept {
    if (symbol_id < 0 ||
        static_cast<std::size_t>(symbol_id) >= pairs_by_symbol_id_.size()) {
      return nullptr;
    }
    return &pairs_by_symbol_id_[static_cast<std::size_t>(symbol_id)];
  }

  std::vector<PairMarketSlot> pairs_by_symbol_id_;
};

}  // namespace aquila::strategy::leadlag

#endif  // AQUILA_STRATEGY_LEAD_LAG_RAW_MARKET_STATE_H_
