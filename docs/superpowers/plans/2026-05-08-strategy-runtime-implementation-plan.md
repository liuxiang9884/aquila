# Strategy Runtime Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 core 中落地通用 strategy runtime 配置、上下文和模板骨架，让用户策略只关心业务事件和参数。

**Architecture:** `StrategyRuntime<UserStrategyT, OrderSessionT>` 放在 `core/strategy/`，负责 `DataReader`、`OrderManager`、feedback reader 和 user strategy 的生命周期与事件分发。Gate 具体 order session 构造不进入 core，后续通过 factory/adapter 在 exchange/tool 层接入。

**Tech Stack:** C++20、CMake、toml++、GoogleTest、现有 `Result<T>`、`DataReader`、`OrderFeedbackShmReader`、`OrderManager`。

---

## Scope

本计划只实现通用 runtime 基础能力，不做 Gate live strategy tool，也不把 LeadLag 业务参数完整建模。

必须保持：

- `core` 不 include `exchange/gate/...`。
- 热路径不做 `std::optional::has_value()` 检查。
- runtime 事件分发不使用 virtual。
- 新代码按 TDD：先写失败测试，再实现。
- 每个实现任务完成后提交。

## File Structure

- Create: `core/strategy/order_types.h`
  - 从旧 `strategy/order_types.h` 迁入订单类型。
- Create: `core/strategy/order_manager.h`
  - 从旧 `strategy/order_manager.h` 迁入 `OrderManager<OrderSessionT>`。
- Keep: `strategy/order_types.h`
  - 兼容 forwarding header。
- Keep: `strategy/order_manager.h`
  - 兼容 forwarding header。
- Create: `core/config/strategy_config.h`
  - `[strategy]` TOML 对应配置结构和 parser 声明。
- Create: `core/config/strategy_config.cpp`
  - `[strategy]` TOML parser / loader。
- Modify: `core/config/CMakeLists.txt`
  - 将 strategy config 加入 `aquila_config`。
- Create: `test/config/strategy_config_test.cpp`
  - strategy config parser 测试。
- Modify: `test/config/CMakeLists.txt`
  - 增加 `strategy_config_test`。
- Create: `core/strategy/strategy_context.h`
  - user strategy 下单、撤单、查询订单状态的窄接口。
- Create: `core/strategy/strategy_runtime.h`
  - 通用 runtime 模板和事件分发骨架。
- Create: `test/core/strategy/strategy_context_test.cpp`
  - `StrategyContext` 测试。
- Create: `test/core/strategy/strategy_runtime_test.cpp`
  - `StrategyRuntime` fake session / fake strategy 测试。
- Create: `test/core/strategy/CMakeLists.txt`
  - core strategy 测试 target。
- Modify: `test/CMakeLists.txt`
  - 增加 `test/core/strategy`。

## Task 1: Move Order Framework Headers Into Core

**Files:**

- Create: `core/strategy/order_types.h`
- Create: `core/strategy/order_manager.h`
- Modify: `strategy/order_types.h`
- Modify: `strategy/order_manager.h`
- Modify: `test/strategy/strategy_test.cpp`
- Modify: `test/strategy/strategy_order_feedback_shm_integration_test.cpp`
- Modify: `benchmark/strategy/order_gateway_benchmark.cpp`
- Optional modify: docs that mention the current include path if needed.

- [ ] **Step 1: Write failing include migration test**

Add a small compile target or update existing tests to include:

```cpp
#include "core/strategy/order_manager.h"
#include "core/strategy/order_types.h"
```

Expected failure before implementation: headers do not exist.

- [ ] **Step 2: Run test/build to verify RED**

Run:

```bash
cmake --build build/debug --target strategy_test -j8
```

Expected: FAIL with missing `core/strategy/order_manager.h` or `core/strategy/order_types.h`.

- [ ] **Step 3: Move implementation into core headers**

Move current contents of `strategy/order_types.h` to `core/strategy/order_types.h`, preserving namespace `aquila::strategy`.

Move current contents of `strategy/order_manager.h` to `core/strategy/order_manager.h`, and update its include from:

```cpp
#include "strategy/order_types.h"
```

to:

```cpp
#include "core/strategy/order_types.h"
```

Keep old headers as forwarding compatibility headers:

```cpp
#ifndef AQUILA_STRATEGY_ORDER_TYPES_H_
#define AQUILA_STRATEGY_ORDER_TYPES_H_

#include "core/strategy/order_types.h"

#endif  // AQUILA_STRATEGY_ORDER_TYPES_H_
```

```cpp
#ifndef AQUILA_STRATEGY_ORDER_MANAGER_H_
#define AQUILA_STRATEGY_ORDER_MANAGER_H_

#include "core/strategy/order_manager.h"

#endif  // AQUILA_STRATEGY_ORDER_MANAGER_H_
```

- [ ] **Step 4: Update first-party includes**

Prefer new core path in tests, benchmark and tools that are touched by this task:

```cpp
#include "core/strategy/order_manager.h"
#include "core/strategy/order_types.h"
```

- [ ] **Step 5: Verify GREEN**

Run:

```bash
cmake --build build/debug --target strategy_test strategy_order_feedback_shm_integration_test strategy_order_gateway_benchmark -j8
./build/debug/test/strategy/strategy_test
./build/debug/test/strategy/strategy_order_feedback_shm_integration_test
git diff --check
```

- [ ] **Step 6: Commit**

Commit message:

```text
Move strategy order framework into core
```

## Task 2: Add Strategy Config Parser

**Files:**

- Create: `core/config/strategy_config.h`
- Create: `core/config/strategy_config.cpp`
- Create: `test/config/strategy_config_test.cpp`
- Modify: `core/config/CMakeLists.txt`
- Modify: `test/config/CMakeLists.txt`

- [ ] **Step 1: Write failing parser tests**

Create tests for:

- `LoadStrategyConfigFile("config/strategies/lead_lag_btc_strategy.toml")` succeeds.
- `strategy.name == "lead_lag_btc"`.
- `strategy_id == 4`.
- `mode == StrategyMode::kDryRun`.
- `order_capacity == 8`.
- `user_config_path` resolves to `config/strategies/lead_lag_btc.toml`.
- data reader and order session config paths resolve.
- feedback shm fields parse.
- reject `strategy_id = 8`.
- reject `order_capacity = 0`.
- reject missing `strategy.config`.
- reject unknown `mode`.
- reject `feedback.enabled = true` without `shm_name`.

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
cmake --build build/debug --target strategy_config_test -j8
```

Expected: FAIL because target / files do not exist.

- [ ] **Step 3: Implement config types**

Use these public types in `aquila::config`:

```cpp
enum class StrategyMode : std::uint8_t {
  kDryRun,
  kLive,
};

enum class StrategyLoopIdlePolicy : std::uint8_t {
  kSpin,
  kYield,
};

struct StrategyLoopConfig {
  StrategyLoopIdlePolicy idle_policy{StrategyLoopIdlePolicy::kSpin};
  std::int32_t bind_cpu_id{-1};
  std::uint64_t max_loop_seconds{0};
};

struct StrategyDataReaderConfig {
  std::filesystem::path config_path;
};

struct StrategyOrderSessionConfig {
  std::filesystem::path config_path;
};

struct StrategyFeedbackConfig {
  bool enabled{true};
  std::string shm_name;
  std::string channel_name;
  std::uint32_t poll_budget{32};
  bool force_claim{false};
};

struct StrategyConfig {
  std::string name;
  std::uint8_t strategy_id{0};
  StrategyMode mode{StrategyMode::kDryRun};
  std::size_t order_capacity{0};
  std::filesystem::path user_config_path;
  StrategyLoopConfig loop;
  StrategyDataReaderConfig data_reader;
  StrategyOrderSessionConfig order_session;
  StrategyFeedbackConfig feedback;
};
```

Expose:

```cpp
using StrategyConfigResult = Result<StrategyConfig>;

[[nodiscard]] StrategyConfigResult ParseStrategyConfig(
    const toml::table& node);

[[nodiscard]] StrategyConfigResult ParseStrategyConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] StrategyConfigResult LoadStrategyConfigFile(
    const std::filesystem::path& path);
```

- [ ] **Step 4: Implement parser**

Parser rules:

- `[strategy]` section is required.
- `name` is required and non-empty.
- `strategy_id` is required, integer, and must be `< 8`.
- `mode` defaults to `dry_run`; allowed values: `dry_run`, `live`.
- `order_capacity` is required and positive.
- `config` is required and resolves to `user_config_path`.
- `[strategy.loop]` defaults to `spin`, `bind_cpu_id=-1`, `max_loop_seconds=0`.
- `idle_policy` allowed values: `spin`, `yield`.
- `[strategy.data_reader].config` is required.
- `[strategy.order_session].config` is required.
- `[strategy.feedback].enabled` defaults to `true`.
- When feedback is enabled: `shm_name`, `channel_name` required; `poll_budget > 0`.
- When feedback is disabled: `shm_name` and `channel_name` may be empty.
- `force_claim` defaults to `false`.
- File paths resolve relative to the strategy config file path by walking up parent dirs, matching existing data reader behavior.

- [ ] **Step 5: Verify GREEN**

Run:

```bash
cmake --build build/debug --target strategy_config_test -j8
./build/debug/test/config/strategy_config_test
git diff --check
```

- [ ] **Step 6: Commit**

Commit message:

```text
Add strategy config parser
```

## Task 3: Add Strategy Context

**Files:**

- Create: `core/strategy/strategy_context.h`
- Create: `test/core/strategy/strategy_context_test.cpp`
- Create: `test/core/strategy/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`

- [ ] **Step 1: Write failing context test**

Use a fake order session with `PlaceOrder()` and `CancelOrder()` methods. Verify:

- `StrategyContext::PlaceLimitOrder()` delegates through `OrderManager`.
- `StrategyContext::CancelOrder()` delegates through `OrderManager`.
- `StrategyContext::FindOrder()` returns the managed order pointer.

Expected failure before implementation: `core/strategy/strategy_context.h` does not exist.

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
cmake --build build/debug --target core_strategy_context_test -j8
```

Expected: FAIL because target / header does not exist.

- [ ] **Step 3: Implement context**

Create:

```cpp
template <typename OrderSessionT>
class StrategyContext {
 public:
  using OrderManagerT = OrderManager<OrderSessionT>;

  explicit StrategyContext(OrderManagerT& order_manager) noexcept
      : order_manager_(order_manager) {}

  OrderPlaceResult PlaceLimitOrder(OrderCreateRequest request) noexcept {
    return order_manager_.PlaceLimitOrder(std::move(request));
  }

  OrderCancelResult CancelOrder(std::uint64_t local_order_id) noexcept {
    return order_manager_.CancelOrder(local_order_id);
  }

  [[nodiscard]] const StrategyOrder* FindOrder(
      std::uint64_t local_order_id) const noexcept {
    return order_manager_.FindOrder(local_order_id);
  }

 private:
  OrderManagerT& order_manager_;
};
```

- [ ] **Step 4: Verify GREEN**

Run:

```bash
cmake --build build/debug --target core_strategy_context_test strategy_test -j8
./build/debug/test/core/strategy/core_strategy_context_test
./build/debug/test/strategy/strategy_test
git diff --check
```

- [ ] **Step 5: Commit**

Commit message:

```text
Add strategy context
```

## Task 4: Add Strategy Runtime Skeleton

**Files:**

- Create: `core/strategy/strategy_runtime.h`
- Create: `test/core/strategy/strategy_runtime_test.cpp`
- Modify: `test/core/strategy/CMakeLists.txt`

- [ ] **Step 1: Write failing runtime tests**

Use fake components only; do not include Gate.

Tests must verify:

- `StrategyRuntime<UserStrategyT, OrderSessionT>` is non-copyable and non-movable.
- A user strategy receiving `OnBookTicker(const BookTicker&, ContextT&)` is called when runtime dispatches book ticker.
- Feedback dispatch applies `OrderManager::OnOrderFeedback()` before calling user strategy.
- Feedback disabled runtime does not require a feedback reader.
- Runtime storage uses `std::optional` for delayed construction but event dispatch does not check `has_value()` on hot path; verify indirectly by using a successfully created runtime and direct dispatch helper.

- [ ] **Step 2: Run test to verify RED**

Run:

```bash
cmake --build build/debug --target core_strategy_runtime_test -j8
```

Expected: FAIL because runtime header / target does not exist.

- [ ] **Step 3: Implement runtime template**

Create `StrategyRuntime<UserStrategyT, OrderSessionT>` in namespace `aquila::strategy`.

First version can expose test-only/direct dispatch methods as public `HandleBookTickerForTest()` / `HandleOrderFeedbackForTest()` only if guarded by clear naming; production loop can be a minimal `Run()` that returns `0` after calling optional `OnStart(context)` if present.

Core members:

```cpp
config::StrategyConfig config_;
std::optional<market_data::DataReader<>> data_reader_;
std::optional<OrderSessionT> order_session_;
std::optional<OrderManagerT> order_manager_;
std::optional<ContextT> context_;
std::optional<OrderFeedbackShmReader> feedback_reader_;
std::optional<UserStrategyT> user_strategy_;
```

Required behavior:

- Class is non-copyable and non-movable.
- `CreateForTest(...)` or equivalent factory constructs order session, order manager, context and user strategy in dependency order without copying a constructed user strategy.
- `OnBookTicker()` calls `user_strategy_->OnBookTicker(ticker, *context_)`.
- `OnOrderFeedback()` calls `order_manager_->OnOrderFeedback(event)` before `user_strategy_->OnOrderFeedback(event, *context_)`.
- No virtual dispatch.
- No Gate dependency.

- [ ] **Step 4: Verify GREEN**

Run:

```bash
cmake --build build/debug --target core_strategy_runtime_test core_strategy_context_test strategy_test -j8
./build/debug/test/core/strategy/core_strategy_runtime_test
./build/debug/test/core/strategy/core_strategy_context_test
./build/debug/test/strategy/strategy_test
git diff --check
```

- [ ] **Step 5: Commit**

Commit message:

```text
Add strategy runtime skeleton
```

## Task 5: Final Integration Review and Verification

**Files:**

- Modify only if final review finds small compile/test integration issues.

- [ ] **Step 1: Build all affected targets**

Run:

```bash
cmake --build build/debug --target strategy_config_test core_strategy_context_test core_strategy_runtime_test strategy_test strategy_order_feedback_shm_integration_test strategy_order_gateway_benchmark -j8
```

- [ ] **Step 2: Run tests**

Run:

```bash
./build/debug/test/config/strategy_config_test
./build/debug/test/core/strategy/core_strategy_context_test
./build/debug/test/core/strategy/core_strategy_runtime_test
./build/debug/test/strategy/strategy_test
./build/debug/test/strategy/strategy_order_feedback_shm_integration_test
```

- [ ] **Step 3: Check boundaries**

Run:

```bash
rg '#include "exchange/' core/strategy core/config
rg '#include "strategy/' core
git diff --check
```

Expected: no `exchange/` include from `core/strategy` or `core/config`; no `strategy/` include from `core`.

- [ ] **Step 4: Commit any final fixes**

Commit message if fixes are needed:

```text
Fix strategy runtime integration
```
