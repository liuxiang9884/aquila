#include <cstdint>

#include <gtest/gtest.h>

#include "core/common/types.h"
#include "core/market_data/types.h"
#include "strategy/lead_lag/config.h"
#include "strategy/lead_lag/raw_market_state.h"

namespace {

namespace leadlag = aquila::strategy::leadlag;

leadlag::Config OnePairConfig() {
  leadlag::Config config;
  config.name = "lead_lag";
  config.version = "1.0";
  config.pairs.push_back(leadlag::PairConfig{
      .symbol = "BTC_USDT",
      .symbol_id = 3,
      .lead_exchange = aquila::Exchange::kBinance,
      .lag_exchange = aquila::Exchange::kGate,
  });
  return config;
}

aquila::BookTicker Ticker(std::int32_t symbol_id, aquila::Exchange exchange,
                          std::int64_t local_ns, double bid_price,
                          double ask_price) {
  return aquila::BookTicker{
      .id = local_ns,
      .symbol_id = symbol_id,
      .exchange = exchange,
      .exchange_ns = local_ns - 10,
      .local_ns = local_ns,
      .bid_price = bid_price,
      .bid_volume = 1.0,
      .ask_price = ask_price,
      .ask_volume = 1.0,
  };
}

TEST(LeadLagRawMarketStateTest, RoutesBySymbolIdAndExchange) {
  leadlag::RawMarketState state;
  state.Reset(OnePairConfig());

  const leadlag::MarketUpdate outside = state.OnBookTicker(
      Ticker(4, aquila::Exchange::kBinance, 100, 100.0, 101.0));
  EXPECT_FALSE(outside.tracked);

  const leadlag::MarketUpdate uninitialized = state.OnBookTicker(
      Ticker(2, aquila::Exchange::kBinance, 101, 100.0, 101.0));
  EXPECT_FALSE(uninitialized.tracked);

  const leadlag::MarketUpdate wrong_exchange =
      state.OnBookTicker(Ticker(3, aquila::Exchange::kOkx, 102, 100.0, 101.0));
  EXPECT_FALSE(wrong_exchange.tracked);

  const leadlag::MarketUpdate lead = state.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 103, 100.0, 101.0));
  EXPECT_TRUE(lead.tracked);
  EXPECT_EQ(lead.role, leadlag::PairRole::kLead);
  EXPECT_TRUE(lead.price_changed);
  EXPECT_FALSE(lead.both_sides_valid);

  const leadlag::MarketUpdate lag =
      state.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 104, 99.5, 100.5));
  EXPECT_TRUE(lag.tracked);
  EXPECT_EQ(lag.role, leadlag::PairRole::kLag);
  EXPECT_TRUE(lag.price_changed);
  EXPECT_TRUE(lag.both_sides_valid);

  const leadlag::PairMarketState* pair = state.FindPair(3);
  ASSERT_NE(pair, nullptr);
  EXPECT_TRUE(pair->lead.has_quote);
  EXPECT_TRUE(pair->lag.has_quote);
  EXPECT_EQ(pair->last_event_ns, 104);
}

TEST(LeadLagRawMarketStateTest, SamePriceTickDoesNotReplaceLatestQuote) {
  leadlag::RawMarketState state;
  state.Reset(OnePairConfig());

  [[maybe_unused]] const leadlag::MarketUpdate first_lead = state.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0));
  [[maybe_unused]] const leadlag::MarketUpdate first_lag =
      state.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5));
  const leadlag::MarketUpdate same_price = state.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 120, 100.0, 101.0));

  EXPECT_TRUE(same_price.tracked);
  EXPECT_EQ(same_price.role, leadlag::PairRole::kLead);
  EXPECT_FALSE(same_price.price_changed);
  EXPECT_TRUE(same_price.both_sides_valid);

  const leadlag::PairMarketState* pair = state.FindPair(3);
  ASSERT_NE(pair, nullptr);
  EXPECT_EQ(pair->lead.latest_quote.local_ns, 100);
  EXPECT_DOUBLE_EQ(pair->lead.latest_quote.bid_price, 100.0);
  EXPECT_DOUBLE_EQ(pair->lead.latest_quote.ask_price, 101.0);
  EXPECT_FALSE(pair->lead.has_previous_quote);
  EXPECT_EQ(pair->last_event_ns, 120);
}

TEST(LeadLagRawMarketStateTest, PriceChangePromotesPreviousQuote) {
  leadlag::RawMarketState state;
  state.Reset(OnePairConfig());

  [[maybe_unused]] const leadlag::MarketUpdate first_lead = state.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0));
  const leadlag::MarketUpdate changed = state.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 130, 100.5, 101.5));

  EXPECT_TRUE(changed.tracked);
  EXPECT_TRUE(changed.price_changed);

  const leadlag::PairMarketState* pair = state.FindPair(3);
  ASSERT_NE(pair, nullptr);
  EXPECT_TRUE(pair->lead.has_previous_quote);
  EXPECT_EQ(pair->lead.previous_quote.local_ns, 100);
  EXPECT_DOUBLE_EQ(pair->lead.previous_quote.bid_price, 100.0);
  EXPECT_DOUBLE_EQ(pair->lead.previous_quote.ask_price, 101.0);
  EXPECT_EQ(pair->lead.latest_quote.local_ns, 130);
  EXPECT_DOUBLE_EQ(pair->lead.latest_quote.bid_price, 100.5);
  EXPECT_DOUBLE_EQ(pair->lead.latest_quote.ask_price, 101.5);
}

TEST(LeadLagRawMarketStateTest, ActiveSeedUsesPreviousQuoteAndResumeFlag) {
  leadlag::RawMarketState state;
  state.Reset(OnePairConfig());

  [[maybe_unused]] const leadlag::MarketUpdate first_lead = state.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 100, 100.0, 101.0));
  [[maybe_unused]] const leadlag::MarketUpdate first_lag =
      state.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 110, 99.5, 100.5));
  [[maybe_unused]] const leadlag::MarketUpdate second_lead = state.OnBookTicker(
      Ticker(3, aquila::Exchange::kBinance, 120, 100.2, 101.2));
  [[maybe_unused]] const leadlag::MarketUpdate second_lag =
      state.OnBookTicker(Ticker(3, aquila::Exchange::kGate, 130, 99.7, 100.7));

  const leadlag::PairMarketState* pair = state.FindPair(3);
  ASSERT_NE(pair, nullptr);

  const leadlag::ActiveSeed lag_trigger_seed =
      pair->SelectActiveSeed(leadlag::PairRole::kLag, true);
  EXPECT_TRUE(lag_trigger_seed.valid);
  EXPECT_TRUE(lag_trigger_seed.resume_lead_tick);
  EXPECT_EQ(lag_trigger_seed.lead.local_ns, 100);
  EXPECT_EQ(lag_trigger_seed.lag.local_ns, 110);

  const leadlag::ActiveSeed lead_trigger_seed =
      pair->SelectActiveSeed(leadlag::PairRole::kLead, true);
  EXPECT_TRUE(lead_trigger_seed.valid);
  EXPECT_FALSE(lead_trigger_seed.resume_lead_tick);
  EXPECT_EQ(lead_trigger_seed.lead.local_ns, 100);
  EXPECT_EQ(lead_trigger_seed.lag.local_ns, 130);
}

}  // namespace
