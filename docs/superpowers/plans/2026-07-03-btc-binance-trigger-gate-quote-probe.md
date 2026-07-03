# BTC Binance-Trigger Gate-Quote Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在现有 Gate BTC fill probe 基础上实现一个 BTC_USDT cross-exchange probe：用 Binance fusion BTC_USDT BBO 触发，用触发时本机已可见的最新 Gate fusion BTC_USDT BBO 对手价在 Gate 下单，并输出可归因的 fillability / latency CSV。

**Architecture:** 基于 `feature/gate-btc-fill-probe` 分支扩展 `fill_probe_strategy`，不改正式 LeadLag 策略。进程形态仍是 fill probe strategy + 专用 `gate_order_gateway` + 专用 `gate_order_feedback_session`；strategy 同时 attach Binance / Gate 两个 canonical fusion SHM，维护 BTC_USDT 最新缓存，Binance 新 tick 作为 node 触发，Gate 最新 tick 作为下单 quote。

**Tech Stack:** C++20, CMake, CLI11, fmt header-only, toml++, GTest, `BookTickerShmReader`, `core::OrderGatewayClient`, Gate order gateway SHM V2, Gate order feedback SHM.

---

## Locked Decisions

- 实现基线分支：`feature/gate-btc-fill-probe`，该分支已有 `tools/gate/fill_probe/*`、`fill_probe_strategy`、两 route order gateway 配置和 CSV 输出。
- 交易账户：沿用 BTC fill probe 的 `TEST_KEY` / `TEST_SECRET` 专用配置。
- Symbol：只做 `BTC_USDT`，`symbol_id = 93`。
- Trigger：Binance fusion BTC_USDT BBO，要求 `binance_freshness_ns < 2_000_000`。
- Quote：trigger 时本机已可见的最新 Gate fusion BTC_USDT BBO，要求 `gate_freshness_ns < 50_000_000`。
- Entry：严格使用 Gate BBO 对手价；buy 用 Gate ask，sell 用 Gate bid；entry 不加 slippage。
- Node：每个 node 仍提交两笔 opening entry，GTC route 0 + IOC route 1；平仓、重试、REST 前后检查、notional 上限和停止条件沿用 Gate BTC fill probe。
- 主指标：Gate 对手价 entry fillability。
- 解释变量：trigger/send/ack/response/fill latency、Gate quote age、Binance/Gate exchange/local delta。
- 运行安全：entry notional 必须 `<= 10 USDT`；运行中不做周期 REST；unresolved node 停住并等待人工检查。

## File Structure

- Modify: `tools/gate/fill_probe/types.h`
  - 增加 trigger mode、Binance/Gate 双 market data 配置、freshness limit 字段、cross-exchange node CSV 字段。
- Modify: `tools/gate/fill_probe/config.h`
  - 保持 `LoadConfig()` 入口不变。
- Modify: `tools/gate/fill_probe/config.cpp`
  - 解析 `[fill_probe] trigger_mode`、`max_binance_freshness_ns`、`max_gate_freshness_ns`、`[market_data.gate]`、`[market_data.binance]`。
- Create: `tools/gate/fill_probe/bbo_cache.h`
  - 纯逻辑缓存指定 `symbol_id` 的最新 sane BBO，避免 30-symbol fusion 下 `TryReadLatest()` 只看全局最新导致节点间隔过长。
- Create: `tools/gate/fill_probe/trigger_quote.h`
  - 纯逻辑评估 Binance trigger + Gate quote freshness，计算 cross-exchange deltas 和 skip reason。
- Modify: `tools/gate/fill_probe/csv_writer.h`
  - 扩展 `NodeCsvRow`，加入 Binance trigger / Gate quote / delta / trigger_to_send 字段。
- Modify: `tools/gate/fill_probe/csv_writer.cpp`
  - 更新 `node.csv` header 和写入顺序。
- Modify: `tools/gate/fill_probe/main.cpp`
  - attach 双 SHM reader，drain unread events 到 BTC cache；Binance 新 tick 触发 node，Gate 最新 quote 下单；写完整字段。
- Modify: `test/tools/gate/fill_probe_test.cpp`
  - 增加配置解析、BBO cache、trigger quote、CSV header / row 回归测试。
- Modify: `config/fill_probe/gate_btc_fill_probe_20260703.toml`
  - 更新为显式 `trigger_mode = "gate_direct"` 与 `[market_data.gate]`，确保旧 Gate-direct probe 仍可运行。
- Create: `config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml`
  - 新实验配置，指定 Binance / Gate fusion SHM、2ms / 50ms freshness。
- Modify: `docs/gate_btc_fill_probe.md`
  - 增加 `binance_trigger_gate_quote` 模式运行顺序、字段说明和分析口径。
- Modify: `docs/diagnostic_fields.md`
  - 登记新增 CSV 字段和 log key。

## Task 0: Worktree And Baseline Verification

**Files:**
- Read: `docs/superpowers/plans/2026-07-03-gate-btc-fill-probe.md`
- Read: `tools/gate/fill_probe/main.cpp`
- Read: `tools/gate/fill_probe/types.h`
- Read: `tools/gate/fill_probe/config.cpp`
- Read: `test/tools/gate/fill_probe_test.cpp`

- [ ] **Step 1: Create an isolated worktree from the fill probe branch**

Run:

```bash
cd /home/liuxiang/dev/aquila
git worktree add /home/liuxiang/dev/aquila/.worktrees/btc-binance-trigger-gate-quote-probe feature/gate-btc-fill-probe
cd /home/liuxiang/dev/aquila/.worktrees/btc-binance-trigger-gate-quote-probe
git status --short --branch
```

Expected:

```text
## feature/gate-btc-fill-probe
```

- [ ] **Step 2: Build and test the current probe before editing**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target gate_fill_probe_test fill_probe_strategy -j8
./build/release/test/tools/gate/gate_fill_probe_test
```

Expected:

```text
[  PASSED  ] ... tests.
```

- [ ] **Step 3: Commit nothing in this task**

Run:

```bash
git status --short
```

Expected: no output.

## Task 1: Extend Config For Trigger Modes And Dual Market Data

**Files:**
- Modify: `tools/gate/fill_probe/types.h`
- Modify: `tools/gate/fill_probe/config.cpp`
- Modify: `test/tools/gate/fill_probe_test.cpp`

- [ ] **Step 1: Add failing tests for gate-direct and Binance-trigger configs**

Append these tests to `test/tools/gate/fill_probe_test.cpp`:

```cpp
TEST(GateFillProbeConfigTest, LoadsBinanceTriggerGateQuoteConfig) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_binance_trigger_config.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_binance_trigger_gate_quote_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = "binance_trigger_gate_quote"
max_nodes = 300
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[market_data.binance]
shm_name = "aquila_bfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_binance_trigger_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_binance_trigger_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_binance_trigger_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.probe.trigger_mode,
            aquila::tools::gate::fill_probe::TriggerMode::
                kBinanceTriggerGateQuote);
  EXPECT_EQ(result.value.probe.max_binance_freshness_ns, 2000000);
  EXPECT_EQ(result.value.probe.max_gate_freshness_ns, 50000000);
  EXPECT_EQ(result.value.market_data.gate.shm_name,
            "aquila_gfusion_20260701_102201_30s_ogw24h");
  EXPECT_EQ(result.value.market_data.binance.shm_name,
            "aquila_bfusion_20260701_102201_30s_ogw24h");
}

TEST(GateFillProbeConfigTest, RejectsBinanceTriggerWithoutBinanceMarketData) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_missing_binance_config.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_binance_trigger_gate_quote_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = "binance_trigger_gate_quote"
max_nodes = 300
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_binance_trigger_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_binance_trigger_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_binance_trigger_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("market_data.binance.shm_name is required"),
            std::string::npos);
}
```

- [ ] **Step 2: Run the failing test**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target gate_fill_probe_test -j8
```

Expected: compile failure because `TriggerMode`, `market_data.gate`, and `market_data.binance` do not exist.

- [ ] **Step 3: Add config types**

Modify `tools/gate/fill_probe/types.h` so the config structs include:

```cpp
enum class TriggerMode : std::uint8_t {
  kGateDirect,
  kBinanceTriggerGateQuote,
};

struct ExchangeMarketDataConfig {
  std::string shm_name;
  std::string channel_name{"book_ticker_channel"};
};

struct MarketDataConfig {
  ExchangeMarketDataConfig gate;
  ExchangeMarketDataConfig binance;
};

struct ProbeConfig {
  std::string name;
  std::string symbol;
  std::string exchange_symbol;
  std::int32_t symbol_id{0};
  std::uint8_t strategy_id{0};
  TriggerMode trigger_mode{TriggerMode::kGateDirect};
  std::uint64_t max_nodes{1000};
  std::uint64_t duration_ms{1800000};
  std::uint64_t node_pause_ms{1000};
  std::uint64_t gtc_cancel_after_ms{1000};
  std::uint64_t unresolved_timeout_ms{30000};
  double max_entry_notional_usdt{10.0};
  std::uint32_t max_close_retries{3};
  std::uint32_t close_slippage_bps{100};
  std::int64_t max_local_freshness_ns{1000000};
  std::int64_t max_exchange_freshness_ns{2000000};
  std::int64_t max_binance_freshness_ns{2000000};
  std::int64_t max_gate_freshness_ns{50000000};
};
```

- [ ] **Step 4: Add parser helpers**

In `tools/gate/fill_probe/config.cpp`, add:

```cpp
[[nodiscard]] bool AssignTriggerMode(toml::node_view<const toml::node> node,
                                     TriggerMode* out, std::string* error) {
  const std::string value = node.value<std::string>().value_or("gate_direct");
  if (value == "gate_direct") {
    *out = TriggerMode::kGateDirect;
    return true;
  }
  if (value == "binance_trigger_gate_quote") {
    *out = TriggerMode::kBinanceTriggerGateQuote;
    return true;
  }
  *error = "fill_probe.trigger_mode must be gate_direct or "
           "binance_trigger_gate_quote";
  return false;
}

[[nodiscard]] bool AssignMarketDataSection(
    toml::node_view<const toml::node> section, std::string_view prefix,
    ExchangeMarketDataConfig* out, std::string* error) {
  if (!AssignString(section["shm_name"], std::string{prefix} + ".shm_name",
                    &out->shm_name, error)) {
    return false;
  }
  out->channel_name = section["channel_name"].value_or(out->channel_name);
  return true;
}
```

- [ ] **Step 5: Parse new fields**

Update `ParseConfig()` in `tools/gate/fill_probe/config.cpp`:

```cpp
if (!AssignTriggerMode(probe["trigger_mode"], &config.probe.trigger_mode,
                       &error) ||
    !AssignInteger(probe["max_nodes"], "fill_probe.max_nodes",
                   &config.probe.max_nodes, &error) ||
    !AssignInteger(probe["duration_ms"], "fill_probe.duration_ms",
                   &config.probe.duration_ms, &error) ||
    !AssignInteger(probe["node_pause_ms"], "fill_probe.node_pause_ms",
                   &config.probe.node_pause_ms, &error) ||
    !AssignInteger(probe["gtc_cancel_after_ms"],
                   "fill_probe.gtc_cancel_after_ms",
                   &config.probe.gtc_cancel_after_ms, &error) ||
    !AssignInteger(probe["unresolved_timeout_ms"],
                   "fill_probe.unresolved_timeout_ms",
                   &config.probe.unresolved_timeout_ms, &error) ||
    !AssignDouble(probe["max_entry_notional_usdt"],
                  "fill_probe.max_entry_notional_usdt",
                  &config.probe.max_entry_notional_usdt, &error) ||
    !AssignInteger(probe["max_close_retries"], "fill_probe.max_close_retries",
                   &config.probe.max_close_retries, &error) ||
    !AssignInteger(probe["close_slippage_bps"],
                   "fill_probe.close_slippage_bps",
                   &config.probe.close_slippage_bps, &error) ||
    !AssignSignedInteger(probe["max_local_freshness_ns"],
                         "fill_probe.max_local_freshness_ns",
                         &config.probe.max_local_freshness_ns, &error) ||
    !AssignSignedInteger(probe["max_exchange_freshness_ns"],
                         "fill_probe.max_exchange_freshness_ns",
                         &config.probe.max_exchange_freshness_ns, &error) ||
    !AssignSignedInteger(probe["max_binance_freshness_ns"],
                         "fill_probe.max_binance_freshness_ns",
                         &config.probe.max_binance_freshness_ns, &error) ||
    !AssignSignedInteger(probe["max_gate_freshness_ns"],
                         "fill_probe.max_gate_freshness_ns",
                         &config.probe.max_gate_freshness_ns, &error)) {
  return Failure(std::move(error));
}
```

Replace old `[market_data]` parsing with:

```cpp
const toml::node_view<const toml::node> market_data = root["market_data"];
if (!AssignMarketDataSection(market_data["gate"], "market_data.gate",
                             &config.market_data.gate, &error)) {
  return Failure(std::move(error));
}
if (config.probe.trigger_mode == TriggerMode::kBinanceTriggerGateQuote &&
    !AssignMarketDataSection(market_data["binance"], "market_data.binance",
                             &config.market_data.binance, &error)) {
  return Failure(std::move(error));
}
```

- [ ] **Step 6: Validate freshness and SHM requirements**

Update `ValidateConfig()`:

```cpp
if (config.market_data.gate.shm_name.empty()) {
  *error = "market_data.gate.shm_name is required";
  return false;
}
if (config.probe.trigger_mode == TriggerMode::kBinanceTriggerGateQuote &&
    config.market_data.binance.shm_name.empty()) {
  *error = "market_data.binance.shm_name is required";
  return false;
}
if (config.probe.max_binance_freshness_ns <= 0) {
  *error = "fill_probe.max_binance_freshness_ns must be positive";
  return false;
}
if (config.probe.max_gate_freshness_ns <= 0) {
  *error = "fill_probe.max_gate_freshness_ns must be positive";
  return false;
}
```

- [ ] **Step 7: Run focused tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target gate_fill_probe_test -j8
./build/release/test/tools/gate/gate_fill_probe_test --gtest_filter='GateFillProbeConfigTest.*'
```

Expected:

```text
[  PASSED  ] 3 tests.
```

- [ ] **Step 8: Commit config changes**

Run:

```bash
git add tools/gate/fill_probe/types.h tools/gate/fill_probe/config.cpp test/tools/gate/fill_probe_test.cpp
git commit -m "Add fill probe cross-exchange config"
```

## Task 2: Add BTC BBO Cache And Trigger Quote Evaluation

**Files:**
- Create: `tools/gate/fill_probe/bbo_cache.h`
- Create: `tools/gate/fill_probe/trigger_quote.h`
- Modify: `test/tools/gate/fill_probe_test.cpp`

- [ ] **Step 1: Add failing tests for BTC cache**

Append to `test/tools/gate/fill_probe_test.cpp`:

```cpp
TEST(GateFillProbeBboCacheTest, KeepsLatestSaneTargetSymbolOnly) {
  using aquila::tools::gate::fill_probe::BboCache;
  BboCache cache(/*symbol_id=*/93, /*price_tick=*/0.1);

  aquila::BookTicker other{};
  other.id = 1;
  other.symbol_id = 384;
  other.exchange_ns = 1000;
  other.local_ns = 1100;
  other.bid_price = 10.0;
  other.ask_price = 10.1;
  cache.OnBookTicker(other);
  EXPECT_FALSE(cache.latest().has_value());

  aquila::BookTicker bad{};
  bad.id = 2;
  bad.symbol_id = 93;
  bad.exchange_ns = 1200;
  bad.local_ns = 1300;
  bad.bid_price = 10.2;
  bad.ask_price = 10.1;
  cache.OnBookTicker(bad);
  EXPECT_FALSE(cache.latest().has_value());

  aquila::BookTicker btc{};
  btc.id = 3;
  btc.symbol_id = 93;
  btc.exchange_ns = 1400;
  btc.local_ns = 1500;
  btc.bid_price = 61513.0;
  btc.bid_volume = 4.0;
  btc.ask_price = 61513.1;
  btc.ask_volume = 5.0;
  cache.OnBookTicker(btc);

  ASSERT_TRUE(cache.latest().has_value());
  EXPECT_EQ(cache.latest()->id, 3);
  EXPECT_EQ(cache.latest()->symbol_id, 93);
  EXPECT_DOUBLE_EQ(cache.latest()->price_tick, 0.1);
}
```

- [ ] **Step 2: Add failing tests for trigger quote freshness**

Append to `test/tools/gate/fill_probe_test.cpp`:

```cpp
TEST(GateFillProbeTriggerQuoteTest, AcceptsFreshBinanceAndGateSnapshots) {
  using namespace aquila::tools::gate::fill_probe;
  const BboSnapshot binance{
      .id = 10,
      .symbol_id = 93,
      .exchange_ns = 1'000'000,
      .local_ns = 2'000'000,
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .price_tick = 0.1,
  };
  const BboSnapshot gate{
      .id = 20,
      .symbol_id = 93,
      .exchange_ns = 900'000,
      .local_ns = 1'900'000,
      .bid_price = 61512.9,
      .ask_price = 61513.0,
      .price_tick = 0.1,
  };
  const auto result = EvaluateTriggerQuote(
      binance, gate, /*decision_ns=*/3'000'000,
      FreshnessLimits{.max_binance_freshness_ns = 2'000'000,
                      .max_gate_freshness_ns = 50'000'000});
  ASSERT_TRUE(result.accepted);
  EXPECT_EQ(result.binance_freshness_ns, 1'000'000);
  EXPECT_EQ(result.gate_freshness_ns, 1'100'000);
  EXPECT_EQ(result.gate_exchange_delta_ns, -100'000);
  EXPECT_EQ(result.gate_local_delta_ns, -100'000);
  EXPECT_TRUE(result.skip_reason.empty());
}

TEST(GateFillProbeTriggerQuoteTest, RejectsStaleGateQuote) {
  using namespace aquila::tools::gate::fill_probe;
  const BboSnapshot binance{
      .id = 10,
      .symbol_id = 93,
      .exchange_ns = 100'000'000,
      .local_ns = 100'000'000,
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .price_tick = 0.1,
  };
  const BboSnapshot gate{
      .id = 20,
      .symbol_id = 93,
      .exchange_ns = 40'000'000,
      .local_ns = 40'000'000,
      .bid_price = 61512.9,
      .ask_price = 61513.0,
      .price_tick = 0.1,
  };
  const auto result = EvaluateTriggerQuote(
      binance, gate, /*decision_ns=*/100'000'000,
      FreshnessLimits{.max_binance_freshness_ns = 2'000'000,
                      .max_gate_freshness_ns = 50'000'000});
  EXPECT_FALSE(result.accepted);
  EXPECT_EQ(result.skip_reason, "stale_gate_quote");
}
```

- [ ] **Step 3: Run the failing tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target gate_fill_probe_test -j8
```

Expected: compile failure because `BboCache`, `FreshnessLimits`, and `EvaluateTriggerQuote` do not exist.

- [ ] **Step 4: Create `bbo_cache.h`**

Create `tools/gate/fill_probe/bbo_cache.h`:

```cpp
#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_BBO_CACHE_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_BBO_CACHE_H_

#include <cmath>
#include <cstdint>
#include <optional>

#include "core/market_data/types.h"
#include "tools/gate/fill_probe/order_math.h"

namespace aquila::tools::gate::fill_probe {

class BboCache {
 public:
  BboCache(std::int32_t symbol_id, double price_tick)
      : symbol_id_(symbol_id), price_tick_(price_tick) {}

  void OnBookTicker(const aquila::BookTicker& ticker) noexcept {
    if (ticker.symbol_id != symbol_id_ || !Sane(ticker)) {
      return;
    }
    latest_ = BboSnapshot{
        .id = static_cast<std::uint64_t>(ticker.id),
        .symbol_id = ticker.symbol_id,
        .exchange_ns = ticker.exchange_ns,
        .local_ns = ticker.local_ns,
        .bid_price = ticker.bid_price,
        .bid_volume = ticker.bid_volume,
        .ask_price = ticker.ask_price,
        .ask_volume = ticker.ask_volume,
        .price_tick = price_tick_,
    };
  }

  [[nodiscard]] const std::optional<BboSnapshot>& latest() const noexcept {
    return latest_;
  }

 private:
  [[nodiscard]] static bool Sane(const aquila::BookTicker& ticker) noexcept {
    return std::isfinite(ticker.bid_price) && ticker.bid_price > 0.0 &&
           std::isfinite(ticker.ask_price) && ticker.ask_price > 0.0 &&
           ticker.bid_price <= ticker.ask_price;
  }

  std::int32_t symbol_id_{0};
  double price_tick_{0.0};
  std::optional<BboSnapshot> latest_;
};

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_BBO_CACHE_H_
```

- [ ] **Step 5: Create `trigger_quote.h`**

Create `tools/gate/fill_probe/trigger_quote.h`:

```cpp
#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_TRIGGER_QUOTE_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_TRIGGER_QUOTE_H_

#include <cstdint>
#include <string>

#include "tools/gate/fill_probe/order_math.h"

namespace aquila::tools::gate::fill_probe {

struct FreshnessLimits {
  std::int64_t max_binance_freshness_ns{2'000'000};
  std::int64_t max_gate_freshness_ns{50'000'000};
};

struct TriggerQuoteDecision {
  bool accepted{false};
  std::int64_t binance_freshness_ns{0};
  std::int64_t gate_freshness_ns{0};
  std::int64_t gate_exchange_delta_ns{0};
  std::int64_t gate_local_delta_ns{0};
  std::string skip_reason;
};

[[nodiscard]] inline TriggerQuoteDecision EvaluateTriggerQuote(
    const BboSnapshot& binance, const BboSnapshot& gate,
    std::int64_t decision_ns, FreshnessLimits limits) {
  TriggerQuoteDecision decision;
  decision.binance_freshness_ns = decision_ns - binance.local_ns;
  decision.gate_freshness_ns = decision_ns - gate.local_ns;
  decision.gate_exchange_delta_ns = gate.exchange_ns - binance.exchange_ns;
  decision.gate_local_delta_ns = gate.local_ns - binance.local_ns;

  if (decision.binance_freshness_ns < 0 ||
      decision.binance_freshness_ns >= limits.max_binance_freshness_ns) {
    decision.skip_reason = "stale_binance_trigger";
    return decision;
  }
  if (decision.gate_freshness_ns < 0 ||
      decision.gate_freshness_ns >= limits.max_gate_freshness_ns) {
    decision.skip_reason = "stale_gate_quote";
    return decision;
  }
  decision.accepted = true;
  return decision;
}

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_TRIGGER_QUOTE_H_
```

- [ ] **Step 6: Include the new headers in tests**

Add to `test/tools/gate/fill_probe_test.cpp`:

```cpp
#include "tools/gate/fill_probe/bbo_cache.h"
#include "tools/gate/fill_probe/trigger_quote.h"
```

- [ ] **Step 7: Run focused tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target gate_fill_probe_test -j8
./build/release/test/tools/gate/gate_fill_probe_test --gtest_filter='GateFillProbeBboCacheTest.*:GateFillProbeTriggerQuoteTest.*'
```

Expected:

```text
[  PASSED  ] 3 tests.
```

- [ ] **Step 8: Commit cache and trigger quote logic**

Run:

```bash
git add tools/gate/fill_probe/bbo_cache.h tools/gate/fill_probe/trigger_quote.h test/tools/gate/fill_probe_test.cpp
git commit -m "Add fill probe cross-exchange trigger logic"
```

## Task 3: Extend Node CSV Schema

**Files:**
- Modify: `tools/gate/fill_probe/csv_writer.h`
- Modify: `tools/gate/fill_probe/csv_writer.cpp`
- Modify: `test/tools/gate/fill_probe_test.cpp`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: Add failing CSV test expectations**

Update `GateFillProbeCsvWriterTest.WritesStableCsvFiles` to write cross-exchange fields:

```cpp
writers.WriteNode(fp::NodeCsvRow{
    .run_id = "run-a",
    .node_id = 7,
    .side = "buy",
    .trigger_mode = "binance_trigger_gate_quote",
    .binance_bbo_id = 321,
    .binance_exchange_ns = 1000,
    .binance_local_ns = 1100,
    .gate_bbo_id = 123,
    .gate_exchange_ns = 900,
    .gate_local_ns = 1050,
    .decision_ns = 1200,
    .submit_ns = 1300,
    .finish_ns = 1400,
    .binance_freshness_ns = 100,
    .gate_freshness_ns = 150,
    .gate_exchange_delta_ns = -100,
    .gate_local_delta_ns = -50,
    .trigger_to_send_ns = 100,
    .bid_price = 61513.0,
    .ask_price = 61513.1,
    .status = "completed_no_fill",
});
```

Update the assertions:

```cpp
EXPECT_NE(node_csv.find("run_id,node_id,side,trigger_mode,binance_bbo_id"),
          std::string::npos);
EXPECT_NE(node_csv.find("run-a,7,buy,binance_trigger_gate_quote,321"),
          std::string::npos);
```

- [ ] **Step 2: Run failing CSV test**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target gate_fill_probe_test -j8
```

Expected: compile failure because `NodeCsvRow` has no cross-exchange fields.

- [ ] **Step 3: Extend `NodeCsvRow`**

Modify `tools/gate/fill_probe/csv_writer.h`:

```cpp
struct NodeCsvRow {
  std::string run_id;
  std::uint64_t node_id{0};
  std::string side;
  std::string trigger_mode;
  std::uint64_t binance_bbo_id{0};
  std::int64_t binance_exchange_ns{0};
  std::int64_t binance_local_ns{0};
  std::uint64_t gate_bbo_id{0};
  std::int64_t gate_exchange_ns{0};
  std::int64_t gate_local_ns{0};
  std::uint64_t bbo_id{0};
  std::int64_t bbo_exchange_ns{0};
  std::int64_t bbo_local_ns{0};
  std::int64_t decision_ns{0};
  std::int64_t submit_ns{0};
  std::int64_t finish_ns{0};
  std::int64_t local_freshness_ns{0};
  std::int64_t exchange_freshness_ns{0};
  std::int64_t binance_freshness_ns{0};
  std::int64_t gate_freshness_ns{0};
  std::int64_t gate_exchange_delta_ns{0};
  std::int64_t gate_local_delta_ns{0};
  std::int64_t trigger_to_send_ns{0};
  double bid_price{0.0};
  double bid_volume{0.0};
  double ask_price{0.0};
  double ask_volume{0.0};
  double entry_quantity{0.0};
  double entry_notional_usdt{0.0};
  std::string status;
  std::string skip_reason;
  std::string unresolved_reason;
};
```

- [ ] **Step 4: Update node CSV header**

In `CsvWriters::Open()` replace the node header with:

```cpp
fmt::print(node_file_,
           "run_id,node_id,side,trigger_mode,binance_bbo_id,"
           "binance_exchange_ns,binance_local_ns,gate_bbo_id,"
           "gate_exchange_ns,gate_local_ns,bbo_id,bbo_exchange_ns,"
           "bbo_local_ns,decision_ns,submit_ns,finish_ns,"
           "local_freshness_ns,exchange_freshness_ns,"
           "binance_freshness_ns,gate_freshness_ns,"
           "gate_exchange_delta_ns,gate_local_delta_ns,trigger_to_send_ns,"
           "bid_price,bid_volume,ask_price,ask_volume,entry_quantity,"
           "entry_notional_usdt,status,skip_reason,unresolved_reason\n");
```

- [ ] **Step 5: Update node CSV writer**

In `CsvWriters::WriteNode()` replace the `fmt::print` call with:

```cpp
fmt::print(node_file_,
           "{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
           "{},{},{},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},{:.12g},"
           "{},{},{}\n",
           EscapeCsv(row.run_id), row.node_id, EscapeCsv(row.side),
           EscapeCsv(row.trigger_mode), row.binance_bbo_id,
           row.binance_exchange_ns, row.binance_local_ns, row.gate_bbo_id,
           row.gate_exchange_ns, row.gate_local_ns, row.bbo_id,
           row.bbo_exchange_ns, row.bbo_local_ns, row.decision_ns,
           row.submit_ns, row.finish_ns, row.local_freshness_ns,
           row.exchange_freshness_ns, row.binance_freshness_ns,
           row.gate_freshness_ns, row.gate_exchange_delta_ns,
           row.gate_local_delta_ns, row.trigger_to_send_ns, row.bid_price,
           row.bid_volume, row.ask_price, row.ask_volume, row.entry_quantity,
           row.entry_notional_usdt, EscapeCsv(row.status),
           EscapeCsv(row.skip_reason), EscapeCsv(row.unresolved_reason));
```

- [ ] **Step 6: Update diagnostic field registry**

In `docs/diagnostic_fields.md`, add a subsection under fill probe fields:

```markdown
### Gate BTC Fill Probe Cross-Exchange Node CSV

| Field | Source | Stability | Unit / Values | Meaning | Removal condition |
| --- | --- | --- | --- | --- | --- |
| `trigger_mode` | `node.csv` | experiment | `gate_direct` / `binance_trigger_gate_quote` | Node 的触发模式。 | Fill probe CSV schema 删除后同步删除。 |
| `binance_bbo_id` | `node.csv` | experiment | Binance fusion BBO id | 触发 node 的 Binance BTC_USDT BBO id。 | 同上。 |
| `gate_bbo_id` | `node.csv` | experiment | Gate fusion BBO id | 下单 quote 使用的 Gate BTC_USDT BBO id。 | 同上。 |
| `binance_freshness_ns` | `node.csv` | experiment | ns | `decision_ns - binance_local_ns`。 | 同上。 |
| `gate_freshness_ns` | `node.csv` | experiment | ns | `decision_ns - gate_local_ns`。 | 同上。 |
| `gate_exchange_delta_ns` | `node.csv` | experiment | ns | `gate_exchange_ns - binance_exchange_ns`。 | 同上。 |
| `gate_local_delta_ns` | `node.csv` | experiment | ns | `gate_local_ns - binance_local_ns`。 | 同上。 |
| `trigger_to_send_ns` | `node.csv` | experiment | ns | `submit_ns - decision_ns`，用于解释 trigger 到 entry submit 的策略端延迟。 | 同上。 |
```

- [ ] **Step 7: Run focused CSV test**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target gate_fill_probe_test -j8
./build/release/test/tools/gate/gate_fill_probe_test --gtest_filter='GateFillProbeCsvWriterTest.*'
```

Expected:

```text
[  PASSED  ] 1 test.
```

- [ ] **Step 8: Commit CSV schema**

Run:

```bash
git add tools/gate/fill_probe/csv_writer.h tools/gate/fill_probe/csv_writer.cpp test/tools/gate/fill_probe_test.cpp docs/diagnostic_fields.md
git commit -m "Extend fill probe node CSV for cross-exchange triggers"
```

## Task 4: Wire Dual SHM Readers Into Main Loop

**Files:**
- Modify: `tools/gate/fill_probe/main.cpp`
- Modify: `test/tools/gate/fill_probe_test.cpp`

- [ ] **Step 1: Add a small pure helper for trigger mode text**

In `tools/gate/fill_probe/main.cpp`, add:

```cpp
[[nodiscard]] std::string TriggerModeText(fp::TriggerMode mode) {
  return mode == fp::TriggerMode::kBinanceTriggerGateQuote
             ? "binance_trigger_gate_quote"
             : "gate_direct";
}
```

- [ ] **Step 2: Add drain helper for cache updates**

In `tools/gate/fill_probe/main.cpp`, include:

```cpp
#include "tools/gate/fill_probe/bbo_cache.h"
#include "tools/gate/fill_probe/trigger_quote.h"
```

Add:

```cpp
void DrainBboReader(md::BookTickerShmReader& reader, fp::BboCache* cache,
                    std::uint64_t max_events) {
  for (std::uint64_t i = 0; i < max_events; ++i) {
    aquila::BookTicker ticker{};
    if (!reader.TryReadOne(&ticker)) {
      return;
    }
    cache->OnBookTicker(ticker);
  }
}
```

- [ ] **Step 3: Attach Gate reader from the new config shape**

Update the Gate SHM reader construction in `main.cpp`:

```cpp
md::BookTickerShmReader gate_reader(md::BookTickerShmConfig{
    .shm_name = context.config.market_data.gate.shm_name,
    .channel_name = context.config.market_data.gate.channel_name,
    .create = false,
});
```

- [ ] **Step 4: Attach optional Binance reader**

In `main.cpp`, create the Binance reader only for cross-exchange mode:

```cpp
std::optional<md::BookTickerShmReader> binance_reader;
if (context.config.probe.trigger_mode ==
    fp::TriggerMode::kBinanceTriggerGateQuote) {
  binance_reader.emplace(md::BookTickerShmConfig{
      .shm_name = context.config.market_data.binance.shm_name,
      .channel_name = context.config.market_data.binance.channel_name,
      .create = false,
  });
}
```

- [ ] **Step 5: Replace `ReadBboWithin()` node trigger with cached drain**

Inside the main node loop, maintain caches:

```cpp
fp::BboCache gate_cache(context.config.probe.symbol_id,
                        context.instrument->price_tick);
fp::BboCache binance_cache(context.config.probe.symbol_id,
                           context.instrument->price_tick);
std::uint64_t last_binance_trigger_id = 0;
```

At the top of each loop iteration:

```cpp
DrainBboReader(gate_reader, &gate_cache, /*max_events=*/256);
if (binance_reader.has_value()) {
  DrainBboReader(*binance_reader, &binance_cache, /*max_events=*/256);
}
```

For `gate_direct`, use the existing Gate snapshot path:

```cpp
std::optional<fp::BboSnapshot> trigger_bbo;
std::optional<fp::BboSnapshot> quote_bbo;
std::string skip_reason;
std::int64_t decision_ns = SystemNowNs();
fp::TriggerQuoteDecision trigger_quote;

if (context.config.probe.trigger_mode == fp::TriggerMode::kGateDirect) {
  if (!gate_cache.latest().has_value()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    continue;
  }
  trigger_bbo = *gate_cache.latest();
  quote_bbo = *gate_cache.latest();
  std::int64_t local_freshness_ns = 0;
  std::int64_t exchange_freshness_ns = 0;
  if (!FreshEnough(context.config, *quote_bbo, decision_ns,
                   &local_freshness_ns, &exchange_freshness_ns)) {
    skip_reason = "stale_gate_direct_quote";
  }
} else {
  if (!binance_cache.latest().has_value()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    continue;
  }
  if (binance_cache.latest()->id == last_binance_trigger_id) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    continue;
  }
  if (!gate_cache.latest().has_value()) {
    WriteSkippedNode("missing_gate_quote");
    last_binance_trigger_id = binance_cache.latest()->id;
    continue;
  }
  trigger_bbo = *binance_cache.latest();
  quote_bbo = *gate_cache.latest();
  last_binance_trigger_id = trigger_bbo->id;
  trigger_quote = fp::EvaluateTriggerQuote(
      *trigger_bbo, *quote_bbo, decision_ns,
      fp::FreshnessLimits{
          .max_binance_freshness_ns =
              context.config.probe.max_binance_freshness_ns,
          .max_gate_freshness_ns = context.config.probe.max_gate_freshness_ns,
      });
  if (!trigger_quote.accepted) {
    skip_reason = trigger_quote.skip_reason;
  }
}
```

The helper `WriteSkippedNode()` must write `NodeCsvRow` with `status="skipped"` and `skip_reason` set. It must not call `SubmittedNodeBudget::ReserveSubmittedNode()`.

- [ ] **Step 6: Ensure entry price uses Gate quote**

Where entry orders are built, use `quote_bbo` for sizing and price:

```cpp
const fp::OrderSizingResult sizing =
    fp::BuildOrderSizing(*context.instrument, quote_bbo->ask_price);
const aquila::OrderSide entry_side = ToOrderSide(node_side);
const fp::PriceText entry_price = fp::EntryPrice(entry_side, *quote_bbo);
```

- [ ] **Step 7: Write cross-exchange node fields on submit and finish**

When writing `NodeCsvRow`, fill these fields:

```cpp
.trigger_mode = TriggerModeText(context.config.probe.trigger_mode),
.binance_bbo_id = trigger_bbo->id,
.binance_exchange_ns = trigger_bbo->exchange_ns,
.binance_local_ns = trigger_bbo->local_ns,
.gate_bbo_id = quote_bbo->id,
.gate_exchange_ns = quote_bbo->exchange_ns,
.gate_local_ns = quote_bbo->local_ns,
.bbo_id = quote_bbo->id,
.bbo_exchange_ns = quote_bbo->exchange_ns,
.bbo_local_ns = quote_bbo->local_ns,
.binance_freshness_ns = trigger_quote.binance_freshness_ns,
.gate_freshness_ns = trigger_quote.gate_freshness_ns,
.gate_exchange_delta_ns = trigger_quote.gate_exchange_delta_ns,
.gate_local_delta_ns = trigger_quote.gate_local_delta_ns,
.trigger_to_send_ns = submit_ns - decision_ns,
```

For `gate_direct`, set `binance_*` fields to `0`, set `gate_*` fields from Gate BBO, and set `trigger_to_send_ns = submit_ns - decision_ns`.

- [ ] **Step 8: Build the tool**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target fill_probe_strategy gate_fill_probe_test -j8
```

Expected: build succeeds.

- [ ] **Step 9: Run all fill probe tests**

Run:

```bash
./build/release/test/tools/gate/gate_fill_probe_test
```

Expected:

```text
[  PASSED  ] ... tests.
```

- [ ] **Step 10: Commit main loop wiring**

Run:

```bash
git add tools/gate/fill_probe/main.cpp test/tools/gate/fill_probe_test.cpp
git commit -m "Wire fill probe Binance trigger to Gate quote"
```

## Task 5: Add Run Configs

**Files:**
- Modify: `config/fill_probe/gate_btc_fill_probe_20260703.toml`
- Create: `config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml`

- [ ] **Step 1: Update existing Gate-direct config shape**

In `config/fill_probe/gate_btc_fill_probe_20260703.toml`, set:

```toml
[fill_probe]
trigger_mode = "gate_direct"
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"
```

Remove the old flat `[market_data]` table after moving its values under `[market_data.gate]`.

- [ ] **Step 2: Add Binance-trigger config**

Create `config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml`:

```toml
[fill_probe]
name = "gate_btc_binance_trigger_gate_quote_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
trigger_mode = "binance_trigger_gate_quote"
max_nodes = 300
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
max_binance_freshness_ns = 2000000
max_gate_freshness_ns = 50000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data.gate]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[market_data.binance]
shm_name = "aquila_bfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_binance_trigger_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_binance_trigger_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_binance_trigger_gate_quote_probe_20260703"
```

- [ ] **Step 3: Validate both configs**

Run:

```bash
./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_fill_probe_20260703.toml \
  --validate-config
./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml \
  --validate-config
```

Expected: both commands exit `0`.

- [ ] **Step 4: Commit configs**

Run:

```bash
git add config/fill_probe/gate_btc_fill_probe_20260703.toml config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml
git commit -m "Add Binance-trigger Gate quote fill probe config"
```

## Task 6: Documentation And Field Registry

**Files:**
- Modify: `docs/gate_btc_fill_probe.md`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: Document the new mode**

Add to `docs/gate_btc_fill_probe.md`:

```markdown
## Binance Trigger / Gate Quote 模式

`trigger_mode = "binance_trigger_gate_quote"` 用 Binance fusion BTC_USDT BBO 触发 node，并使用触发时本机已可见的最新 Gate fusion BTC_USDT BBO 作为下单 quote。

运行前置条件：

- Binance fusion canonical SHM 可读：`aquila_bfusion_20260701_102201_30s_ogw24h` / `book_ticker_channel`
- Gate fusion canonical SHM 可读：`aquila_gfusion_20260701_102201_30s_ogw24h` / `book_ticker_channel`
- `binance_freshness_ns < 2_000_000`
- `gate_freshness_ns < 50_000_000`
- BTC_USDT min quantity notional `<= 10 USDT`

该模式的主指标是 entry fillability。Latency 字段用于解释 fill / no-fill，不作为本轮实验的主成败指标。
```

- [ ] **Step 2: Document operating commands**

Add to the same doc:

````markdown
### 30 分钟 / 300 node 实验命令

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh release

./build/release/tools/gate_order_feedback_session \
  --config config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml

./build/release/tools/gate_order_gateway \
  --config config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml

./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml \
  --preflight-only

./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml
```
````

- [ ] **Step 3: Document analysis fields**

Add:

```markdown
### Cross-exchange 归因口径

- `binance_freshness_ns = decision_ns - binance_local_ns`
- `gate_freshness_ns = decision_ns - gate_local_ns`
- `gate_exchange_delta_ns = gate_exchange_ns - binance_exchange_ns`
- `gate_local_delta_ns = gate_local_ns - binance_local_ns`
- `trigger_to_send_ns = submit_ns - decision_ns`

`skip_reason = stale_binance_trigger` 表示 Binance trigger 过期；`skip_reason = stale_gate_quote` 表示 Gate quote 过期；这两类 skipped node 不计入 `max_nodes`。
```

- [ ] **Step 4: Run markdown and diff checks**

Run:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 5: Commit docs**

Run:

```bash
git add docs/gate_btc_fill_probe.md docs/diagnostic_fields.md
git commit -m "Document Binance-trigger Gate quote fill probe"
```

## Task 7: Final Verification

**Files:**
- Read: all files modified in Tasks 1-6

- [ ] **Step 1: Build focused targets**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target fill_probe_strategy gate_fill_probe_test gate_order_gateway gate_order_feedback_session -j8
```

Expected: build succeeds.

- [ ] **Step 2: Run focused tests**

Run:

```bash
./build/release/test/tools/gate/gate_fill_probe_test
```

Expected:

```text
[  PASSED  ] ... tests.
```

- [ ] **Step 3: Validate configs**

Run:

```bash
./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_fill_probe_20260703.toml \
  --validate-config
./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml \
  --validate-config
```

Expected: both commands exit `0`.

- [ ] **Step 4: Run preflight only when the referenced SHM is live**

Run:

```bash
./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml \
  --preflight-only
```

Expected:

```text
fill_probe_preflight_ok ...
```

If SHM is not present, the command must fail before live orders and print the missing SHM error. Do not treat missing SHM as implementation failure.

- [ ] **Step 5: Check evaluation boundary**

Run:

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: no output.

- [ ] **Step 6: Check whitespace**

Run:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 7: Commit final verification note if files changed**

Run:

```bash
git status --short
```

Expected after commits: no output. If a verification command changed no files, do not create an empty commit.

## Task 8: Live Experiment Runbook

**Files:**
- Read: `docs/gate_btc_fill_probe.md`
- Read: `docs/lead_lag_live_operations_pipeline.md`
- Read: generated run dir under `/home/liuxiang/tmp`

- [ ] **Step 1: Confirm no stale probe processes**

Run:

```bash
pgrep -af 'fill_probe_strategy|gate_order_gateway|gate_order_feedback_session'
```

Expected: no unrelated probe process using the same SHM names.

- [ ] **Step 2: Start dedicated feedback and gateway**

Run each command in its own managed session:

```bash
./build/release/tools/gate_order_feedback_session \
  --config config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml
```

```bash
./build/release/tools/gate_order_gateway \
  --config config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml
```

Expected: feedback session subscribed, gateway route 0 and route 1 ready.

- [ ] **Step 3: Run REST and notional preflight**

Run:

```bash
./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml \
  --preflight-only
```

Expected:

```text
BTC_USDT entry_notional_usdt <= 10
open_orders = 0
position size = 0
```

- [ ] **Step 4: Run 30m / 300 submitted node experiment**

Run:

```bash
./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml
```

Expected stop condition: `max_nodes = 300` or `duration_ms = 1800000`, whichever arrives first.

- [ ] **Step 5: Post-run REST check**

Run the existing fill probe post-run check path from `docs/gate_btc_fill_probe.md`.

Expected:

```text
BTC_USDT position size = 0
BTC_USDT open orders = 0
```

- [ ] **Step 6: Analyze primary fillability**

Run a local Python one-liner against the run dir:

```bash
python3 - <<'PY'
import csv
from collections import Counter
from pathlib import Path
run_dir = Path("/home/liuxiang/tmp/gate_btc_binance_trigger_gate_quote_probe_20260703")
events = list(csv.DictReader((run_dir / "lifecycle.csv").open()))
entries = [r for r in events if r["entry_kind"] in {"gtc", "ioc"}]
counts = Counter(r["entry_result"] for r in entries)
print("entry_results", dict(counts))
filled = sum(1 for r in entries if float(r["entry_filled_qty"] or 0) > 0)
print("entries", len(entries), "filled", filled, "fill_rate", filled / len(entries) if entries else 0.0)
PY
```

Expected: prints entry count, filled count and fill rate. Do not claim success until REST post-check confirms flat.

## Self-Review

- Spec coverage: trigger source, Gate quote source, freshness thresholds, strict opponent entry, fillability primary metric, latency attribution fields, dual SHM read, skipped-node semantics, run safety and docs are covered.
- Placeholder scan: this plan contains no unresolved placeholder markers or undefined task references.
- Type consistency: `TriggerMode`, `ExchangeMarketDataConfig`, `BboCache`, `FreshnessLimits`, `TriggerQuoteDecision`, and `EvaluateTriggerQuote` are introduced before use.
- Performance risk noted: the main loop must drain unread SHM events into BTC caches and must not use one-shot `TryReadLatest()` as the only symbol filter in 30-symbol fusion.
