# Gate BTC Fill Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现一个独立 Gate BTC_USDT fillability probe：复用当前 fusion canonical SHM 读 BBO，通过专用 order-gateway SHM 同一 BBO 同时发送最小量 GTC/IOC，并输出可复盘的 fillability CSV。

**Architecture:** 新增 `fill_probe_strategy` 工具进程，策略进程只读 Gate fusion `BookTicker` SHM，使用 `core::OrderGatewayClient` 写专用两 route order-gateway SHM，并从专用 order feedback SHM 读取回报。运行形态沿用 LeadLag 的 strategy / order-gateway / feedback 三进程结构，但不复用 20260701 live 的 order-gateway 或 feedback SHM；只复用当前 Gate fusion canonical SHM。

**Tech Stack:** C++20, CMake, CLI11, fmt header-only, toml++, `aquila_core`, `aquila_config`, `aquila_gate`, Gate order gateway SHM V2, Gate order feedback SHM, `BookTickerShmReader`, GTest。

---

## 已锁定需求

- 账户：`TEST_KEY` / `TEST_SECRET`，BTC_USDT only。
- 行情源：复用当前 Gate fusion canonical SHM，启动前配置显式指定；当前现场名为 `aquila_gfusion_20260701_102201_30s_ogw24h`，channel `book_ticker_channel`。
- 订单路径：专用 `gate_order_gateway`，`route0=GTC`，`route1=IOC`；专用 `gate_order_feedback_session`，probe 独占一个 feedback lane。
- entry：每个节点取同一条最新 BTC_USDT BBO snapshot，buy 节点价格用 ask，sell 节点价格用 bid；方向逐节点交替；entry 不加滑点。
- 数量：Gate BTC_USDT `min_quantity`，按 instrument catalog 计算；单笔 entry notional 必须 `<= 10 USDT`，否则 preflight fail 并报告。
- freshness：节点提交前要求 local freshness `<= 1ms` 且 exchange freshness `<= 2ms`；不满足则 skip node，不发单。
- 节点生命周期：GTC 与 IOC lifecycle 独立处理；任意一条成交不取消另一条。GTC entry 1s 后未终结则 cancel；IOC entry 等 terminal feedback。
- 平仓：当前 Gate encoder 不支持 native market；平仓用 `reduce_only=true`、`tif=ioc`、1% aggressive limit price，最多 3 次，每次用最新 BBO 重算。
- 净仓语义：Gate futures 是账户净仓；节点安全结束以 BTC_USDT 本地净仓归零为准，lifecycle CSV 记录 `closed_by_own_close` / `closed_by_net_flat`。
- REST：只在启动前和结束后做账户 read-only check；运行中不做周期 REST。若本地状态不确定，当前节点最多等待 30s，记录 `node_unresolved`，不启动下一节点，等待外部介入。
- 停止条件：`max_nodes=1000` 或 `duration=30min`，先到即停。
- 输出：三层 CSV：`node.csv`、`lifecycle.csv`、`order_event.csv`。

## 文件结构

- Create: `tools/gate/fill_probe/types.h`
  - Probe 配置、node/lifecycle/order event 数据结构、枚举和 CSV row 类型。
- Create: `tools/gate/fill_probe/config.h`
  - TOML 配置加载入口和配置校验声明。
- Create: `tools/gate/fill_probe/config.cpp`
  - 解析 `[fill_probe]`、`[market_data]`、`[order_gateway]`、`[feedback]`、`[output]`。
- Create: `tools/gate/fill_probe/order_math.h`
  - 价格 rounding、min quantity/notional、entry/close price 计算。
- Create: `tools/gate/fill_probe/state_machine.h`
  - Node / lifecycle 状态机，纯逻辑可单测。
- Create: `tools/gate/fill_probe/state_machine.cpp`
  - 实现 entry terminal、cancel timeout、close retry、node completion、unresolved timeout。
- Create: `tools/gate/fill_probe/csv_writer.h`
  - 三层 CSV writer 声明。
- Create: `tools/gate/fill_probe/csv_writer.cpp`
  - CSV header 和 row 写入实现。
- Create: `tools/gate/fill_probe/main.cpp`
  - CLI、SHM attach、preflight、runtime loop、signal handling。
- Modify: `tools/CMakeLists.txt`
  - 新增 `fill_probe_strategy` target。
- Create: `test/tools/gate/fill_probe_test.cpp`
  - 配置、order math、状态机、CSV 输出单测。
- Modify: `test/tools/gate/CMakeLists.txt`
  - 新增 `gate_fill_probe_test`。
- Create: `config/fill_probe/gate_btc_fill_probe_20260703.toml`
  - probe 进程配置。
- Create: `config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml`
  - 专用两 route gateway 配置。
- Create: `config/order_sessions/gate_order_session_btc_fill_probe_private_plain_20260703.toml`
  - 两 route 复用的 Gate order session 配置。
- Create: `config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml`
  - 专用 feedback session 配置。
- Create: `docs/gate_btc_fill_probe.md`
  - 运行顺序、preflight、stop/final check、CSV 字段说明。
- Modify: `docs/diagnostic_fields.md`
  - 登记新增 log key 和 CSV 字段，满足项目级诊断字段约定。

## Task 1: 配置模型与解析

**Files:**
- Create: `tools/gate/fill_probe/types.h`
- Create: `tools/gate/fill_probe/config.h`
- Create: `tools/gate/fill_probe/config.cpp`
- Test: `test/tools/gate/fill_probe_test.cpp`
- Modify: `test/tools/gate/CMakeLists.txt`

- [ ] **Step 1: 写失败测试：最小合法配置能解析**

在 `test/tools/gate/fill_probe_test.cpp` 增加：

```cpp
#include "tools/gate/fill_probe/config.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace {

TEST(GateFillProbeConfigTest, LoadsMinimalConfig) {
  const std::filesystem::path path =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_minimal_config.toml";
  std::ofstream out(path);
  out << R"toml(
[fill_probe]
name = "gate_btc_fill_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
max_nodes = 1000
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_fill_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_fill_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_fill_probe_test"
)toml";
  out.close();

  const auto result = aquila::tools::gate::fill_probe::LoadConfig(path);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.value.probe.symbol, "BTC_USDT");
  EXPECT_EQ(result.value.probe.symbol_id, 93);
  EXPECT_EQ(result.value.order_gateway.gtc_route_id, 0);
  EXPECT_EQ(result.value.order_gateway.ioc_route_id, 1);
  EXPECT_EQ(result.value.probe.max_close_retries, 3);
}

}  // namespace
```

- [ ] **Step 2: 运行失败测试**

Run:

```bash
cmake --build build/release --target gate_fill_probe_test -j
```

Expected: 编译失败，提示 `tools/gate/fill_probe/config.h` 不存在或 target 不存在。

- [ ] **Step 3: 新增配置类型**

在 `tools/gate/fill_probe/types.h` 写入：

```cpp
#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_TYPES_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_TYPES_H_

#include <cstdint>
#include <filesystem>
#include <string>

namespace aquila::tools::gate::fill_probe {

struct ProbeConfig {
  std::string name;
  std::string symbol;
  std::string exchange_symbol;
  std::int32_t symbol_id{0};
  std::uint8_t strategy_id{0};
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
};

struct MarketDataConfig {
  std::string shm_name;
  std::string channel_name{"book_ticker_channel"};
};

struct ProbeOrderGatewayConfig {
  std::string shm_name;
  std::uint16_t route_count{2};
  std::uint32_t command_queue_capacity{4096};
  std::uint32_t event_queue_capacity{8192};
  std::uint32_t startup_ready_timeout_s{30};
  std::uint16_t gtc_route_id{0};
  std::uint16_t ioc_route_id{1};
};

struct FeedbackConfig {
  std::string shm_name;
  std::string channel_name{"orders"};
  bool force_claim{true};
};

struct OutputConfig {
  std::filesystem::path run_dir;
};

struct FillProbeConfig {
  ProbeConfig probe;
  std::filesystem::path instrument_catalog_file;
  MarketDataConfig market_data;
  ProbeOrderGatewayConfig order_gateway;
  FeedbackConfig feedback;
  OutputConfig output;
};

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_TYPES_H_
```

- [ ] **Step 4: 新增配置加载入口**

在 `tools/gate/fill_probe/config.h` 写入：

```cpp
#ifndef AQUILA_TOOLS_GATE_FILL_PROBE_CONFIG_H_
#define AQUILA_TOOLS_GATE_FILL_PROBE_CONFIG_H_

#include <filesystem>

#include "core/common/result.h"
#include "tools/gate/fill_probe/types.h"

namespace aquila::tools::gate::fill_probe {

using FillProbeConfigResult = Result<FillProbeConfig>;

[[nodiscard]] FillProbeConfigResult LoadConfig(
    const std::filesystem::path& path);

}  // namespace aquila::tools::gate::fill_probe

#endif  // AQUILA_TOOLS_GATE_FILL_PROBE_CONFIG_H_
```

- [ ] **Step 5: 实现 TOML 解析和强校验**

在 `tools/gate/fill_probe/config.cpp` 实现 `LoadConfig()`。校验规则必须包括：

```cpp
// Required validation results:
// - symbol, exchange_symbol, market_data.shm_name, order_gateway.shm_name,
//   feedback.shm_name, output.run_dir are non-empty.
// - route_count == 2.
// - gtc_route_id == 0 and ioc_route_id == 1.
// - gtc_route_id != ioc_route_id.
// - max_nodes > 0, duration_ms > 0.
// - gtc_cancel_after_ms == 1000 for this experiment.
// - node_pause_ms == 1000 for this experiment.
// - max_close_retries == 3.
// - close_slippage_bps == 100.
// - max_entry_notional_usdt > 0 and <= 10.
// - freshness thresholds are positive.
```

Use `toml++` APIs and return `Result` errors with exact field names, for example `"fill_probe.symbol is required"` and `"order_gateway.route_count must be 2"`.

- [ ] **Step 6: 接入测试 target**

在 `test/tools/gate/CMakeLists.txt` 增加：

```cmake
add_executable(gate_fill_probe_test
    ../../..//tools/gate/fill_probe/config.cpp
    fill_probe_test.cpp
)

target_link_libraries(gate_fill_probe_test
    PRIVATE
        aquila_core
        aquila_config
        GTest::gtest_main
        PkgConfig::tomlplusplus
)

add_test(NAME gate_fill_probe_test
         COMMAND gate_fill_probe_test)
```

修正路径为仓库实际相对路径 `../../../tools/gate/fill_probe/config.cpp`，不要保留双斜杠。

- [ ] **Step 7: 运行测试并提交**

Run:

```bash
cmake --build build/release --target gate_fill_probe_test -j
ctest --test-dir build/release --output-on-failure -R gate_fill_probe_test
```

Expected: `gate_fill_probe_test` PASS。

Commit:

```bash
git add tools/gate/fill_probe/types.h tools/gate/fill_probe/config.h tools/gate/fill_probe/config.cpp test/tools/gate/fill_probe_test.cpp test/tools/gate/CMakeLists.txt
git commit -m "Add Gate BTC fill probe config"
```

## Task 2: Instrument sizing 与价格计算

**Files:**
- Create: `tools/gate/fill_probe/order_math.h`
- Modify: `test/tools/gate/fill_probe_test.cpp`

- [ ] **Step 1: 写失败测试：BTC 最小量 notional 与 entry/close 价格**

在 `fill_probe_test.cpp` 增加：

```cpp
#include "core/config/instrument_catalog.h"
#include "tools/gate/fill_probe/order_math.h"

TEST(GateFillProbeOrderMathTest, ComputesBtcMinimumNotional) {
  const auto catalog_result =
      aquila::config::LoadInstrumentCatalogFromCsv(
          "config/instruments/usdt_futures_common_gate_binance_20260701.csv");
  ASSERT_TRUE(catalog_result.ok) << catalog_result.error;
  const auto* instrument =
      catalog_result.value.Find(aquila::Exchange::kGate, "BTC_USDT");
  ASSERT_NE(instrument, nullptr);

  const auto sizing =
      aquila::tools::gate::fill_probe::BuildOrderSizing(*instrument, 61513.1);
  ASSERT_TRUE(sizing.ok) << sizing.error;
  EXPECT_DOUBLE_EQ(sizing.value.quantity, 1.0);
  EXPECT_EQ(sizing.value.quantity_text, "1");
  EXPECT_NEAR(sizing.value.notional_usdt, 6.15131, 0.00001);
}

TEST(GateFillProbeOrderMathTest, ComputesStrictEntryAndAggressiveClosePrices) {
  const aquila::tools::gate::fill_probe::BboSnapshot bbo{
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .price_tick = 0.1,
  };

  const auto buy_entry =
      aquila::tools::gate::fill_probe::EntryPrice(aquila::OrderSide::kBuy, bbo);
  const auto sell_entry =
      aquila::tools::gate::fill_probe::EntryPrice(aquila::OrderSide::kSell, bbo);
  EXPECT_EQ(buy_entry.price_text, "61513.1");
  EXPECT_EQ(sell_entry.price_text, "61513");

  const auto close_sell = aquila::tools::gate::fill_probe::ClosePrice(
      aquila::OrderSide::kSell, bbo, 100);
  const auto close_buy = aquila::tools::gate::fill_probe::ClosePrice(
      aquila::OrderSide::kBuy, bbo, 100);
  EXPECT_EQ(close_sell.price_text, "60897.9");
  EXPECT_EQ(close_buy.price_text, "62128.3");
}
```

- [ ] **Step 2: 运行失败测试**

Run:

```bash
cmake --build build/release --target gate_fill_probe_test -j
```

Expected: 编译失败，提示 `order_math.h` 不存在。

- [ ] **Step 3: 实现 `order_math.h`**

实现 header-only helper：

```cpp
struct BboSnapshot {
  std::uint64_t id{0};
  std::int32_t symbol_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t local_ns{0};
  double bid_price{0.0};
  double bid_volume{0.0};
  double ask_price{0.0};
  double ask_volume{0.0};
  double price_tick{0.0};
};

struct OrderSizing {
  double quantity{0.0};
  std::string quantity_text;
  double notional_usdt{0.0};
};

struct PriceText {
  double price{0.0};
  std::string price_text;
};
```

Rules:

- `BuildOrderSizing()` uses `instrument.min_quantity`.
- notional = `price * min_quantity * instrument.contract_multiplier` where `contract_multiplier` is `InstrumentInfo::notional_multiplier` for current catalog semantics if no separate field exists in `InstrumentInfo`; before implementing, verify the catalog mapping. If `notional_multiplier` is not contract multiplier in code, add a `contract_multiplier` field to `InstrumentInfo` in `core/config/instrument_catalog.h/.cpp` with tests.
- For current BTC row, expected notional around `6.15 USDT`.
- Entry price is strict BBO opposite side, rounded to price tick without adding slippage.
- Close price uses 1% aggressive bound:
  - close sell uses `floor_to_tick(bid * (1 - 0.01))`
  - close buy uses `ceil_to_tick(ask * (1 + 0.01))`
- `price_text` and `quantity_text` use decimal places from instrument config; no `std::to_string`.

- [ ] **Step 4: 运行测试并提交**

Run:

```bash
cmake --build build/release --target gate_fill_probe_test -j
ctest --test-dir build/release --output-on-failure -R gate_fill_probe_test
```

Expected: PASS。

Commit:

```bash
git add tools/gate/fill_probe/order_math.h test/tools/gate/fill_probe_test.cpp core/config/instrument_catalog.h core/config/instrument_catalog.cpp
git commit -m "Add Gate BTC fill probe order math"
```

If `core/config/instrument_catalog.*` did not need modification, omit those paths from `git add`.

## Task 3: Node / lifecycle 状态机

**Files:**
- Create: `tools/gate/fill_probe/state_machine.h`
- Create: `tools/gate/fill_probe/state_machine.cpp`
- Modify: `test/tools/gate/fill_probe_test.cpp`
- Modify: `test/tools/gate/CMakeLists.txt`

- [ ] **Step 1: 写失败测试：IOC cancel + GTC cancel 后节点结束**

Add:

```cpp
#include "tools/gate/fill_probe/state_machine.h"

TEST(GateFillProbeStateMachineTest, NodeEndsAfterIocCancelledAndGtcCancelled) {
  using namespace aquila::tools::gate::fill_probe;
  ProbeNode node = ProbeNode::Start(/*node_id=*/1, NodeSide::kBuy,
                                    /*decision_ns=*/1000);
  node.MarkEntrySubmitted(EntryKind::kGtc, /*local_order_id=*/11, 0, 1000);
  node.MarkEntrySubmitted(EntryKind::kIoc, /*local_order_id=*/12, 1, 1000);

  node.OnEntryTerminal(12, EntryResult::kCancelled, /*filled_qty=*/0.0,
                       /*fill_price=*/0.0, /*event_ns=*/1200);
  EXPECT_FALSE(node.Done());

  EXPECT_TRUE(node.GtcCancelDue(/*now_ns=*/1000 + 1000000001LL));
  node.MarkGtcCancelSubmitted(/*event_ns=*/1000 + 1000000001LL);
  node.OnEntryTerminal(11, EntryResult::kCancelled, /*filled_qty=*/0.0,
                       /*fill_price=*/0.0, /*event_ns=*/1000 + 1000100000LL);

  EXPECT_TRUE(node.Done());
  EXPECT_EQ(node.status(), NodeStatus::kCompletedNoFill);
}
```

- [ ] **Step 2: 写失败测试：两条 entry 都 fill，净仓归零后节点结束**

Add:

```cpp
TEST(GateFillProbeStateMachineTest, NodeUsesNetFlatForCompletion) {
  using namespace aquila::tools::gate::fill_probe;
  ProbeNode node = ProbeNode::Start(2, NodeSide::kSell, 2000);
  node.MarkEntrySubmitted(EntryKind::kGtc, 21, 0, 2000);
  node.MarkEntrySubmitted(EntryKind::kIoc, 22, 1, 2000);

  node.OnEntryTerminal(21, EntryResult::kFilled, 1.0, 61513.0, 2100);
  node.OnEntryTerminal(22, EntryResult::kFilled, 1.0, 61513.0, 2200);
  EXPECT_DOUBLE_EQ(node.net_position(), -2.0);
  EXPECT_FALSE(node.Done());

  node.MarkCloseSubmitted(EntryKind::kGtc, 31, 0, 2300);
  node.OnCloseFill(31, 1.0, 62128.3, 2400);
  EXPECT_FALSE(node.Done());

  node.MarkCloseSubmitted(EntryKind::kIoc, 32, 1, 2500);
  node.OnCloseFill(32, 1.0, 62128.3, 2600);
  EXPECT_TRUE(node.Done());
  EXPECT_EQ(node.status(), NodeStatus::kCompletedClosed);
}
```

- [ ] **Step 3: 写失败测试：close retry 3 次后 unresolved**

Add:

```cpp
TEST(GateFillProbeStateMachineTest, CloseRetryLimitLeavesNodeUnresolved) {
  using namespace aquila::tools::gate::fill_probe;
  ProbeNode node = ProbeNode::Start(3, NodeSide::kBuy, 3000);
  node.MarkEntrySubmitted(EntryKind::kIoc, 42, 1, 3000);
  node.OnEntryTerminal(42, EntryResult::kFilled, 1.0, 61513.1, 3100);

  for (int attempt = 0; attempt < 3; ++attempt) {
    ASSERT_TRUE(node.CloseRetryAllowed(EntryKind::kIoc, 3));
    node.MarkCloseSubmitted(EntryKind::kIoc, 50 + attempt, 1, 3200 + attempt);
    node.OnCloseTerminal(50 + attempt, CloseResult::kCancelled, 3300 + attempt);
  }

  EXPECT_FALSE(node.CloseRetryAllowed(EntryKind::kIoc, 3));
  EXPECT_FALSE(node.Done());
  EXPECT_TRUE(node.UnresolvedDue(3000 + 30000000001LL));
  node.MarkUnresolved(3000 + 30000000001LL);
  EXPECT_EQ(node.status(), NodeStatus::kUnresolved);
}
```

- [ ] **Step 4: 实现状态机**

State model:

```cpp
enum class NodeSide : std::uint8_t { kBuy, kSell };
enum class EntryKind : std::uint8_t { kGtc, kIoc };
enum class EntryResult : std::uint8_t { kPending, kFilled, kPartialFilled, kCancelled, kRejected, kUnknown };
enum class CloseResult : std::uint8_t { kPending, kFilled, kPartialFilled, kCancelled, kRejected, kUnknown };
enum class NodeStatus : std::uint8_t {
  kRunning,
  kCompletedNoFill,
  kCompletedClosed,
  kUnresolved,
};
```

Invariants:

- A lifecycle can submit at most one entry order.
- GTC cancel due is based only on GTC entry submit time and non-terminal entry state.
- IOC and GTC lifecycle terminal states are independent.
- Filled entry adds signed net position; close fill subtracts signed net position.
- Node is `kCompletedNoFill` when both entries terminal and total filled quantity is zero.
- Node is `kCompletedClosed` when both lifecycle entry/close states have terminal or net-flat attribution and `abs(net_position) <= 1e-9`.
- Unresolved does not trigger REST or flatten; it only blocks next node.

- [ ] **Step 5: CMake link state machine source**

Modify `test/tools/gate/CMakeLists.txt`:

```cmake
add_executable(gate_fill_probe_test
    ../../../tools/gate/fill_probe/config.cpp
    ../../../tools/gate/fill_probe/state_machine.cpp
    fill_probe_test.cpp
)
```

- [ ] **Step 6: 运行测试并提交**

Run:

```bash
cmake --build build/release --target gate_fill_probe_test -j
ctest --test-dir build/release --output-on-failure -R gate_fill_probe_test
```

Expected: PASS。

Commit:

```bash
git add tools/gate/fill_probe/state_machine.h tools/gate/fill_probe/state_machine.cpp test/tools/gate/fill_probe_test.cpp test/tools/gate/CMakeLists.txt
git commit -m "Add Gate BTC fill probe state machine"
```

## Task 4: 三层 CSV writer

**Files:**
- Create: `tools/gate/fill_probe/csv_writer.h`
- Create: `tools/gate/fill_probe/csv_writer.cpp`
- Modify: `test/tools/gate/fill_probe_test.cpp`
- Modify: `test/tools/gate/CMakeLists.txt`

- [ ] **Step 1: 写失败测试：CSV header 和 row 稳定**

Add:

```cpp
#include "tools/gate/fill_probe/csv_writer.h"

TEST(GateFillProbeCsvWriterTest, WritesStableCsvFiles) {
  namespace fp = aquila::tools::gate::fill_probe;
  const std::filesystem::path dir =
      std::filesystem::path{"/home/liuxiang/tmp"} /
      "gate_fill_probe_csv_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  fp::CsvWriters writers(dir);
  ASSERT_TRUE(writers.Open().ok);
  writers.WriteNode(fp::NodeCsvRow{
      .run_id = "run-a",
      .node_id = 7,
      .side = "buy",
      .bbo_id = 123,
      .bid_price = 61513.0,
      .ask_price = 61513.1,
      .status = "completed_no_fill",
  });
  writers.WriteLifecycle(fp::LifecycleCsvRow{
      .run_id = "run-a",
      .node_id = 7,
      .entry_kind = "ioc",
      .entry_local_order_id = 70,
      .entry_result = "cancelled",
      .close_attribution = "none",
  });
  writers.WriteOrderEvent(fp::OrderEventCsvRow{
      .run_id = "run-a",
      .node_id = 7,
      .local_order_id = 70,
      .route_id = 1,
      .event_kind = "feedback_cancelled",
  });

  const std::string node_csv =
      ReadWholeFileForTest(dir / "node.csv");
  EXPECT_NE(node_csv.find("run_id,node_id,side,bbo_id"), std::string::npos);
  EXPECT_NE(node_csv.find("run-a,7,buy,123"), std::string::npos);
}
```

Add a local test helper `ReadWholeFileForTest()` using `std::ifstream`.

- [ ] **Step 2: 实现 CSV writer**

Fields:

`node.csv`:

```text
run_id,node_id,side,bbo_id,bbo_exchange_ns,bbo_local_ns,decision_ns,submit_ns,finish_ns,local_freshness_ns,exchange_freshness_ns,bid_price,bid_volume,ask_price,ask_volume,entry_quantity,entry_notional_usdt,status,skip_reason,unresolved_reason
```

`lifecycle.csv`:

```text
run_id,node_id,entry_kind,entry_route_id,entry_local_order_id,entry_side,entry_tif,entry_price,entry_quantity,entry_submit_ns,entry_ack_ns,entry_finish_ns,entry_result,entry_filled_qty,entry_avg_fill_price,close_route_id,close_attempts,close_filled_qty,close_avg_fill_price,close_attribution,pnl_usdt,fee_usdt
```

`order_event.csv`:

```text
run_id,node_id,lifecycle_kind,order_role,local_order_id,parent_id,route_id,event_kind,response_kind,feedback_kind,exchange_order_id,exchange_ns,local_ns,price,quantity,cumulative_filled_quantity,left_quantity,finish_reason,reject_reason
```

Use `fmt::print(file, ...)` or `fmt::format_to` with `FILE*`; do not add `std::cout` or `printf`.

- [ ] **Step 3: 运行测试并提交**

Run:

```bash
cmake --build build/release --target gate_fill_probe_test -j
ctest --test-dir build/release --output-on-failure -R gate_fill_probe_test
```

Expected: PASS。

Commit:

```bash
git add tools/gate/fill_probe/csv_writer.h tools/gate/fill_probe/csv_writer.cpp test/tools/gate/fill_probe_test.cpp test/tools/gate/CMakeLists.txt
git commit -m "Add Gate BTC fill probe CSV writers"
```

## Task 5: Runtime loop 与 SHM adapters

**Files:**
- Create: `tools/gate/fill_probe/main.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `test/tools/gate/fill_probe_test.cpp`

- [ ] **Step 1: 写失败测试：CLI 缺少配置返回错误**

Add a CTest shell smoke in `test/tools/gate/CMakeLists.txt` after target creation:

```cmake
add_test(NAME gate_fill_probe_missing_config_reject
         COMMAND bash -c [=[
output="$("$1" 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 2 ]] &&
  [[ "$output" == *"--config is required"* ]]
]=] _ $<TARGET_FILE:fill_probe_strategy>)
```

- [ ] **Step 2: 写失败测试：`--preflight-only` 在缺 SHM 时报告清晰错误**

Add:

```cmake
add_test(NAME gate_fill_probe_missing_market_shm_reject
         COMMAND bash -c [=[
run_dir="/home/liuxiang/tmp/gate_fill_probe_missing_shm_ctest_$$"
mkdir -p "$run_dir"
config="$run_dir/probe.toml"
cat > "$config" <<EOF
[fill_probe]
name = "missing_shm_probe"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
max_nodes = 1
duration_ms = 1000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000
[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"
[market_data]
shm_name = "aquila_missing_gate_fill_probe_market_shm"
channel_name = "book_ticker_channel"
[order_gateway]
shm_name = "aquila_missing_gate_fill_probe_ogw"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1
[feedback]
shm_name = "aquila_missing_gate_fill_probe_ofb"
channel_name = "orders"
force_claim = true
[output]
run_dir = "$run_dir"
EOF
output="$("$1" --config "$config" --preflight-only 2>&1)"
code=$?
printf '%s\n' "$output"
[[ "$code" -eq 1 ]] &&
  [[ "$output" == *"market_data_shm_open_failed"* ]]
]=] _ $<TARGET_FILE:fill_probe_strategy>)
```

- [ ] **Step 3: 实现 `fill_probe_strategy` target**

Modify `tools/CMakeLists.txt`:

```cmake
add_executable(fill_probe_strategy
    gate/fill_probe/config.cpp
    gate/fill_probe/state_machine.cpp
    gate/fill_probe/csv_writer.cpp
    gate/fill_probe/main.cpp
)

target_link_libraries(fill_probe_strategy
    PRIVATE
        aquila_config
        aquila_core
        aquila_gate
        CLI11::CLI11
        fmt::fmt-header-only
        magic_enum::magic_enum
        nova
        PkgConfig::tomlplusplus
)
```

- [ ] **Step 4: 实现 runtime loop**

Runtime loop responsibilities:

- Attach `BookTickerShmReader` with configured market SHM and `SeekLatest()`.
- Attach `OrderGatewayClient` with configured route count/capacity and call `Start()`.
- Attach `OrderFeedbackShmManager::Open()` and `OrderFeedbackShmReader::Claim(strategy_id, run_id, force_claim)`.
- Load Gate BTC instrument from configured catalog.
- Preflight:
  - confirm current BBO for symbol_id 93 arrives within 5s;
  - compute min quantity notional from BBO;
  - fail if single-entry notional `> max_entry_notional_usdt`;
  - `--preflight-only` exits 0 after these checks.
- Main node loop:
  - skip node without submit when freshness gates fail;
  - alternate buy/sell side;
  - create two `core::StrategyOrder` objects with distinct `local_order_id`, same BBO-derived price/quantity, route 0/1, `TimeInForce::kGoodTillCancel` / `kImmediateOrCancel`;
  - call `OrderGatewayClient::PlaceOrder()` for GTC and IOC as close together as current owner thread permits;
  - poll gateway events through a small local runtime object with `OnOrderResponse(const core::OrderResponseEvent&)`;
  - poll feedback reader and update node state from `OrderFeedbackEvent`;
  - cancel GTC after 1s if still pending using `OrderGatewayClient::CancelOrder()`;
  - submit reduce-only IOC close orders on same route as entry route when an entry fill is observed;
  - retry close up to 3 times with latest BBO and 1% aggressive price;
  - when node done, write CSV rows, sleep 1s, continue;
  - if `node_unresolved`, write CSV rows, stop loop and leave process waiting for external handling or exit non-zero with clear message. Use exit code 10 for unresolved local trading state.

- [ ] **Step 5: 运行 CLI smoke tests**

Run:

```bash
cmake --build build/release --target fill_probe_strategy gate_fill_probe_test -j
ctest --test-dir build/release --output-on-failure -R 'gate_fill_probe'
```

Expected: all `gate_fill_probe*` tests PASS.

- [ ] **Step 6: 提交**

```bash
git add tools/gate/fill_probe/main.cpp tools/CMakeLists.txt test/tools/gate/CMakeLists.txt test/tools/gate/fill_probe_test.cpp
git commit -m "Add Gate BTC fill probe runtime"
```

## Task 6: 专用配置文件

**Files:**
- Create: `config/fill_probe/gate_btc_fill_probe_20260703.toml`
- Create: `config/order_sessions/gate_order_session_btc_fill_probe_private_plain_20260703.toml`
- Create: `config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml`
- Create: `config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml`

- [ ] **Step 1: 新增 probe config**

`config/fill_probe/gate_btc_fill_probe_20260703.toml`:

```toml
[fill_probe]
name = "gate_btc_fill_probe_20260703"
symbol = "BTC_USDT"
exchange_symbol = "BTC_USDT"
symbol_id = 93
strategy_id = 3
max_nodes = 1000
duration_ms = 1800000
node_pause_ms = 1000
gtc_cancel_after_ms = 1000
unresolved_timeout_ms = 30000
max_entry_notional_usdt = 10.0
max_close_retries = 3
close_slippage_bps = 100
max_local_freshness_ns = 1000000
max_exchange_freshness_ns = 2000000

[instrument_catalog]
file = "config/instruments/usdt_futures_common_gate_binance_20260701.csv"

[market_data]
shm_name = "aquila_gfusion_20260701_102201_30s_ogw24h"
channel_name = "book_ticker_channel"

[order_gateway]
shm_name = "aquila_ogw_btc_fill_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
gtc_route_id = 0
ioc_route_id = 1

[feedback]
shm_name = "aquila_ofb_btc_fill_probe_20260703"
channel_name = "orders"
force_claim = true

[output]
run_dir = "/home/liuxiang/tmp/gate_btc_fill_probe_20260703"
```

- [ ] **Step 2: 新增 order session config**

Base it on `config/order_sessions/gate_order_session_30symbols_private_plain_20260701.toml`, but write a dedicated log file and name:

```toml
[order_session]
name = "gate_order_session_btc_fill_probe_private_plain_20260703"
settle = "usdt"

[order_session.credentials]
api_key_env = "TEST_KEY"
api_secret_env = "TEST_SECRET"

[order_session.websocket]
target = "/v4/ws/usdt"

[order_session.websocket.endpoint]
host = "fxws-private.gateapi.io"
connect_ip = "10.0.1.154"
port = "80"
enable_tls = false

[order_session.websocket.execution_policy]
bind_cpu_id = 16
idle_policy = "spin"
```

If the source config contains additional required fields, copy them exactly and only change name/log/cpu fields.

- [ ] **Step 3: 新增 two-route order gateway config**

```toml
[log]
log_level = "info"
file_sink_name = "/home/liuxiang/log/gate_order_gateway_btc_fill_probe_20260703.log"
console_sink_name = "gate_order_gateway_btc_fill_probe_20260703_console"
backend_thread_name = "gate_order_gateway_btc_fill_probe_20260703_log"
backend_cpu_affinity = 0

[order_gateway]
name = "gate_order_gateway_btc_fill_probe_20260703"
shm_name = "aquila_ogw_btc_fill_probe_20260703"
route_count = 2
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30

[[order_gateway.routes]]
name = "gtc_route"
order_session_config = "config/order_sessions/gate_order_session_btc_fill_probe_private_plain_20260703.toml"
worker_cpu_id = 16

[[order_gateway.routes]]
name = "ioc_route"
order_session_config = "config/order_sessions/gate_order_session_btc_fill_probe_private_plain_20260703.toml"
worker_cpu_id = 17
```

- [ ] **Step 4: 新增 feedback session config**

```toml
[log]
log_level = "info"
file_sink_name = "/home/liuxiang/log/gate_order_feedback_session_btc_fill_probe_20260703.log"
console_sink_name = "gate_order_feedback_session_btc_fill_probe_20260703_console"
backend_thread_name = "gate_order_feedback_session_btc_fill_probe_20260703_log"
backend_cpu_affinity = 5

[order_feedback_session]
name = "gate_order_feedback_session_btc_fill_probe_20260703"
settle = "usdt"

[order_feedback_session.credentials]
api_key_env = "TEST_KEY"
api_secret_env = "TEST_SECRET"

[order_feedback_session.websocket]
target = "/v4/ws/usdt/sbe?sbe_schema_id=1"

[order_feedback_session.websocket.endpoint]
host = "fxws-private.gateapi.io"
connect_ip = "10.0.1.154"
port = "80"
enable_tls = false

[order_feedback_session.websocket.execution_policy]
bind_cpu_id = 6

[order_feedback_session.shm]
shm_name = "aquila_ofb_btc_fill_probe_20260703"
channel_name = "orders"
max_strategy_count = 8
queue_capacity = 65536
create = true
remove_existing = true
```

- [ ] **Step 5: validate-only 检查并提交**

Run:

```bash
build/release/tools/gate_order_gateway --config config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml --validate-only
build/release/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml --validate-only
build/release/tools/fill_probe_strategy --config config/fill_probe/gate_btc_fill_probe_20260703.toml --validate-config
```

Expected: all exit 0.

Commit:

```bash
git add config/fill_probe/gate_btc_fill_probe_20260703.toml config/order_sessions/gate_order_session_btc_fill_probe_private_plain_20260703.toml config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml
git commit -m "Add Gate BTC fill probe configs"
```

## Task 7: 文档与诊断字段

**Files:**
- Create: `docs/gate_btc_fill_probe.md`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: 写运行文档**

`docs/gate_btc_fill_probe.md` must include:

```markdown
# Gate BTC Fill Probe

## 目的

测量 Gate BTC_USDT 在 fusion BBO 对手价上的最小量 GTC / IOC fillability。

## 安全边界

- 账户使用 `TEST_KEY` / `TEST_SECRET`。
- 只交易 `BTC_USDT`。
- 单笔 entry notional 必须 `<= 10 USDT`。
- 一个节点内 GTC 和 IOC 可能同时成交，因此节点临时合计暴露可接近单笔阈值的 2 倍。
- 运行中不自动 REST，不自动 emergency flatten；异常本地状态阻塞下一节点并输出 `node_unresolved`。

## 启动顺序

1. rebuild release。
2. REST read-only 确认 BTC_USDT flat 且无 open orders。
3. 确认 fusion SHM 存在且 BTC_USDT BBO freshness 达标。
4. 启动 `gate_order_feedback_session`。
5. 启动 `gate_order_gateway`。
6. 运行 `fill_probe_strategy --preflight-only`。
7. 运行真实 probe。
8. 结束后 REST read-only 确认 flat/no open orders。

## CSV

说明 `node.csv`、`lifecycle.csv`、`order_event.csv` 字段和归因规则。
```

- [ ] **Step 2: 更新诊断字段文档**

In `docs/diagnostic_fields.md`, add a Gate BTC Fill Probe section with:

- log keys: `fill_probe_start`, `fill_probe_preflight_ok`, `fill_probe_node_start`, `fill_probe_order_submitted`, `fill_probe_order_event`, `fill_probe_node_done`, `fill_probe_node_unresolved`, `fill_probe_stop`.
- CSV files: `node.csv`, `lifecycle.csv`, `order_event.csv`.
- deletion condition: keep while `fill_probe_strategy` exists.

- [ ] **Step 3: 验证文档并提交**

Run:

```bash
git diff --check
```

Expected: no output.

Commit:

```bash
git add docs/gate_btc_fill_probe.md docs/diagnostic_fields.md
git commit -m "Document Gate BTC fill probe"
```

## Task 8: 真实运行前验证流程

**Files:**
- No source changes unless previous tasks fail.

- [ ] **Step 1: 完整 release build**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh release
```

Expected: release build succeeds.

- [ ] **Step 2: 单测和 smoke**

Run:

```bash
ctest --test-dir build/release --output-on-failure -R 'gate_fill_probe|gate_order_gateway|order_feedback'
```

Expected: all selected tests PASS.

- [ ] **Step 3: 只读现场检查**

Run:

```bash
ps -eo pid,etime,cmd | rg 'gate_data_fusion|gate_order_gateway|gate_order_feedback_session|fill_probe_strategy' | rg -v 'rg '
find /dev/shm -maxdepth 1 -printf '%f %s\n' | rg 'aquila_gfusion_20260701_102201_30s_ogw24h|aquila_ogw_btc_fill_probe_20260703|aquila_ofb_btc_fill_probe_20260703'
timeout 3s build/release/tools/book_ticker_shm_reader --shm-name aquila_gfusion_20260701_102201_30s_ogw24h --channel-name book_ticker_channel --from-latest --max-messages 20000 | rg 'symbol_id=93' | head -n 5
```

Expected:

- `gate_data_fusion` is running.
- No stale probe `gate_order_gateway`, `gate_order_feedback_session`, or `fill_probe_strategy` process is running.
- Gate fusion SHM exists.
- BTC_USDT BBO rows appear and show sane bid/ask.

- [ ] **Step 4: REST read-only flat check**

Use the existing REST status helper from the LeadLag live operations pipeline. Required result before starting:

```text
open_orders = 0
BTC_USDT position size = 0
pending_orders = 0
```

Do not proceed if any of these are non-zero.

- [ ] **Step 5: Start feedback and gateway**

Run from repo root in a run dir under `/home/liuxiang/tmp/gate_btc_fill_probe_20260703`:

```bash
build/release/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml
build/release/tools/gate_order_gateway --config config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml
```

Expected:

- feedback SHM `aquila_ofb_btc_fill_probe_20260703` exists.
- order gateway SHM `aquila_ogw_btc_fill_probe_20260703` exists.
- both route workers report ready.

- [ ] **Step 6: Probe preflight**

Run:

```bash
build/release/tools/fill_probe_strategy --config config/fill_probe/gate_btc_fill_probe_20260703.toml --preflight-only
```

Expected:

- config ok;
- market data ok;
- order gateway routes ready;
- feedback lane claimed;
- BTC min entry notional printed and `<= 10 USDT`.

- [ ] **Step 7: Live probe**

Run:

```bash
build/release/tools/fill_probe_strategy --config config/fill_probe/gate_btc_fill_probe_20260703.toml
```

Expected:

- exits 0 when `max_nodes` or `duration_ms` is reached;
- exits 10 and writes `node_unresolved` if local trading state remains uncertain for 30s;
- writes `node.csv`, `lifecycle.csv`, `order_event.csv` under configured run dir.

- [ ] **Step 8: Final REST read-only flat check**

Run the same REST status helper as startup. Required result:

```text
open_orders = 0
BTC_USDT position size = 0
pending_orders = 0
```

If non-zero, stop all probe processes and perform the project-approved manual/emergency flatten flow; record exact REST output in the run dir.

## Self-Review

- Spec coverage: plan covers SHM reuse, dedicated order gateway/feedback SHM, two sessions/routes, strict BBO entry, independent lifecycle handling, aggressive reduce-only IOC close, close retry count, notional threshold, freshness gates, REST start/end only, unresolved behavior, run bounds, and three CSV outputs.
- Placeholder scan: no task relies on unspecified future work or vague error handling; each branch has concrete behavior and validation commands.
- Type consistency: `FillProbeConfig`, `BboSnapshot`, `ProbeNode`, `CsvWriters`, `NodeCsvRow`, `LifecycleCsvRow`, and `OrderEventCsvRow` are introduced before use in later tasks. Route names and SHM names are consistent across config, gateway, and probe.
