# OrderFeedback SHM Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现订单 feedback SHM transport，让共享 `OrderFeedbackSession` producer 可以按 `strategy_id` 把固定 event 写入 8 条 Nova SPSC lane，Strategy reader 可以 claim 自己的 lane 并按预算 drain event。

**Architecture:** Task1 只做通用 transport。`local_order_id` 高 8 bit 直接路由到 fixed lane；同一个 SHM object 内预分配 8 个 SPSC queue；正常 publish / poll 热路径不做动态分配、不阻塞、不做 per-event atomic stats。Gate SBE parser、Strategy 状态机、REST reconcile 留给 Task2 和后续任务。

**Tech Stack:** C++20、CMake、GoogleTest、Google Benchmark、Abseil、Nova `ShmAllocator`、Nova `static_impl::SPSCQueue`。

---

## 当前实现状态

截至 2026-05-08，Task1 已实现。当前工作区入口：

```text
core/trading/order_feedback_event.h
core/trading/order_feedback_shm.h
core/config/order_feedback_shm_config.h
core/config/order_feedback_shm_config.cpp
config/order_feedback/gate_order_feedback_shm.toml
test/core/trading/order_feedback_event_test.cpp
test/core/trading/order_feedback_shm_test.cpp
test/config/order_feedback_shm_config_test.cpp
benchmark/core/trading/order_feedback_shm_benchmark.cpp
```

实现相对初版计划做了关键简化：

- `OrderFeedbackEvent` 宽结构 ABI 已实现，并增加 `OrderFeedbackKind::kGap` control event。
- gap 不再通过 `global_gap_epoch` / `lane_gap_epoch` atomic 进入 Strategy；reader 只通过 `Poll()` 消费 event。
- `PublishGlobalGap()` 对 8 lane fanout `kGap`；lane queue full 时 publisher 保留 pending gap，本地重试。
- queue full 不阻塞、不覆盖、不影响其他 lane；只更新当前 lane `queue_full_count` / `dropped_count` 异常计数，并产生 pending lane `kGap`。
- 第一版不做 producer / reader heartbeat，不做 stale owner 自动判断或 pid alive probe。
- reader ownership 只以 `consumer_run_id` 为 token，`consumer_pid` 仅诊断；`force_claim=true` 是显式恢复动作。
- 不做 shared successful `published_count` / `consumed_count`；成功计数是 publisher / reader 对象本地字段。
- config 字段只有 `shm_name`、`channel_name`、`max_strategy_count`、`queue_capacity`、`create`、`remove_existing`。

下面的任务清单保留为历史执行记录；新 worker 不应把它当作下一步功能清单重复执行，下一步应进入 Task2。

## Reference Docs

- `docs/superpowers/specs/2026-05-08-order-feedback-shm-transport-design.md`
- `docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md`
- `core/market_data/data_shm.h`
- `/home/liuxiang/dev/nova/include/nova/interprocess/shm_allocator.h`
- `/home/liuxiang/dev/nova/include/nova/concurrency/spsc_queue.h`

## Task 1: Event ABI

- [ ] **Step 1: 新增 event 类型测试**

Create `test/core/trading/order_feedback_event_test.cpp`。

覆盖：

- `OrderFeedbackEvent` 是 trivial；
- `OrderFeedbackEvent` 是 standard-layout；
- `OrderFeedbackEvent` 支持零初始化；
- `LocalOrderIdCodec` 解出的 `strategy_id` 可用于 feedback routing；
- `sizeof(OrderFeedbackEvent)` 固定在测试断言中。

运行：

```bash
cmake --build build/debug --target core_order_feedback_event_test -j8
./build/debug/test/core/trading/core_order_feedback_event_test
```

Expected: target 初始编译失败，因为类型尚未实现。

- [ ] **Step 2: 实现 event 类型**

Create `core/trading/order_feedback_event.h`。

实现：

- `OrderFeedbackKind`
- `OrderRole`
- `OrderFinishReason`
- `OrderRejectReason`
- `OrderFeedbackEvent`
- ABI `static_assert`

注意：

- 不新增动态成员；
- 不放入 `std::string`、`std::string_view` 或非平凡类型；
- `exchange_update_ns` 表示交易所更新时间；
- `local_receive_ns` 表示 feedback producer 本地收到并发布 event 前的时间。

- [ ] **Step 3: 接入 CMake**

Modify `test/core/trading/CMakeLists.txt` 增加 `core_order_feedback_event_test`。

运行：

```bash
cmake --build build/debug --target core_order_feedback_event_test -j8
./build/debug/test/core/trading/core_order_feedback_event_test
```

Expected: event ABI tests pass。

## Task 2: SHM Layout And Manager

- [ ] **Step 1: 新增 SHM layout 测试**

Create `test/core/trading/order_feedback_shm_test.cpp`。

先覆盖：

- create 初始化 header magic / version / abi size；
- `max_strategy_count == 8`；
- `queue_capacity == 65536`；
- lane header `strategy_id` 为 `0..7`；
- attach 校验 header，不接受错误 magic / version / event size。

运行：

```bash
cmake --build build/debug --target core_order_feedback_shm_test -j8
./build/debug/test/core/trading/core_order_feedback_shm_test
```

Expected: target 初始编译失败，因为 SHM 类型尚未实现。

- [ ] **Step 2: 实现 SHM layout**

Create `core/trading/order_feedback_shm.h`。

实现：

- `kMaxOrderFeedbackStrategies = 8`
- `kOrderFeedbackQueueCapacity = 65536`
- `kOrderFeedbackShmMagic`
- `kOrderFeedbackShmVersion = 1`
- `OrderFeedbackShmConfig`
- `OrderFeedbackShmHeader`
- `OrderFeedbackLaneHeader`
- `OrderFeedbackLane`
- `OrderFeedbackShmChannel`

复用 market data SHM 的实现风格，使用 Nova `ShmAllocator` 管理 named shared memory。

- [ ] **Step 3: 实现 manager create / attach**

在 `core/trading/order_feedback_shm.h` 中实现：

- `OrderFeedbackShmManager::Create(config)`
- `OrderFeedbackShmManager::Open(config)`
- header 初始化；
- header 校验；
- lane 初始化；
- `channel()` accessor。

运行：

```bash
cmake --build build/debug --target core_order_feedback_shm_test -j8
./build/debug/test/core/trading/core_order_feedback_shm_test
```

Expected: layout and manager tests pass。

## Task 3: Producer Publish

- [ ] **Step 1: 新增 publish route 测试**

Extend `test/core/trading/order_feedback_shm_test.cpp`。

覆盖：

- `Publish(event)` 根据 `LocalOrderIdCodec::StrategyId()` 写入对应 lane；
- lane 0 event 不出现在 lane 1；
- `strategy_id >= 8` publish 返回失败并增加 invalid route diagnostics；
- queue full 时返回失败，只更新当前 lane `queue_full_count` / `dropped_count`，并保留 pending lane `kGap` event。

- [ ] **Step 2: 实现 publisher**

在 `core/trading/order_feedback_shm.h` 中实现 `OrderFeedbackShmPublisher`。

接口建议：

```cpp
class OrderFeedbackShmPublisher {
 public:
  explicit OrderFeedbackShmPublisher(OrderFeedbackShmChannel& channel) noexcept;

  [[nodiscard]] bool Publish(const OrderFeedbackEvent& event) noexcept;
  [[nodiscard]] bool PublishGlobalGap(OrderFeedbackGapReason reason,
                                      std::int64_t local_receive_ns) noexcept;
  [[nodiscard]] std::size_t FlushPendingGapEvents() noexcept;
  [[nodiscard]] std::uint64_t published_count() const noexcept;
};
```

实现约束：

- 正常路径只做 route、`TryPush()`；
- `published_count` 是 publisher 对象本地字段，不写 shared header；
- full path 使用 `fetch_add` 更新异常计数；
- lane / global gap 以 `OrderFeedbackKind::kGap` event 投递到 lane；
- 不阻塞等待 queue 空位。

- [ ] **Step 3: 验证 publish tests**

运行：

```bash
cmake --build build/debug --target core_order_feedback_shm_test -j8
./build/debug/test/core/trading/core_order_feedback_shm_test
```

Expected: publish route and full-path diagnostics tests pass。

## Task 4: Consumer Claim And Poll

- [ ] **Step 1: 新增 consumer claim 测试**

Extend `test/core/trading/order_feedback_shm_test.cpp`。

覆盖：

- `strategy_id >= 8` reader attach 失败；
- empty lane claim 成功；
- duplicate claim 失败；
- `force_claim=true` 可显式恢复已 claim lane；
- `consumer_run_id == 0` claim 失败；
- `Release()` 只有 run id 匹配才清 `consumer_pid`。

- [ ] **Step 2: 新增 poll 测试**

覆盖：

- `Poll(max_events)` 按 FIFO 调用 handler；
- `Poll(0)` 不消费；
- `Poll(max_events)` 遵守预算；
- reader 通过 `Poll()` 收到 lane / global `kGap` event；
- `consumed_count` 是 reader 对象本地字段。

- [ ] **Step 3: 实现 reader**

在 `core/trading/order_feedback_shm.h` 中实现 `OrderFeedbackShmReader`。

接口建议：

```cpp
class OrderFeedbackShmReader {
 public:
  [[nodiscard]] static Result<OrderFeedbackShmReader> Claim(
      OrderFeedbackShmChannel& channel,
      std::uint8_t strategy_id,
      std::uint64_t consumer_run_id,
      bool force_claim = false);

  template <typename Handler>
  std::size_t Poll(std::size_t max_events, Handler&& handler) noexcept;

  [[nodiscard]] std::uint64_t consumed_count() const noexcept;
  void Release() noexcept;
};
```

实现约束：

- reader 只读自己的 lane；
- 不提供 latest 模式；
- handler 在 strategy 线程执行，不在 transport 内做状态解释；
- `kGap` 和普通 order feedback 一样进入 handler；
- claim 失败返回 `Result` error string。

- [ ] **Step 4: 验证 reader tests**

运行：

```bash
cmake --build build/debug --target core_order_feedback_shm_test -j8
./build/debug/test/core/trading/core_order_feedback_shm_test
```

Expected: consumer claim and poll tests pass。

## Task 5: Config

- [ ] **Step 1: 新增示例配置**

Create `config/order_feedback/gate_order_feedback_shm.toml`：

```toml
[order_feedback_shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
max_strategy_count = 8
queue_capacity = 65536
create = true
remove_existing = false
```

- [ ] **Step 2: 新增 config parser 测试**

Create `test/config/order_feedback_shm_config_test.cpp`。

覆盖：

- 示例配置 load 成功；
- `max_strategy_count` 只能是 8；
- `queue_capacity` 只能是 65536；
- `create` / `remove_existing` 被正确解析；
- 示例配置不包含 `heartbeat_interval_ms` / `stale_consumer_timeout_ms`。

- [ ] **Step 3: 实现 config parser**

Create `core/config/order_feedback_shm_config.h` and `.cpp`，或如果当前 build target 更适合，先放在
`core/trading/order_feedback_shm_config.h`。保持与 data session / order session config 风格一致：

- cold path TOML parse；
- required / optional 字段清晰；
- 返回 `Result<OrderFeedbackShmRuntimeConfig>`；
- 不把 TOML 解析依赖引入 hot path 类型。

运行：

```bash
cmake --build build/debug --target order_feedback_shm_config_test -j8
./build/debug/test/config/order_feedback_shm_config_test
```

Expected: config parser tests pass。

## Task 6: Benchmark

- [ ] **Step 1: 新增 benchmark**

Create `benchmark/core/trading/order_feedback_shm_benchmark.cpp`。

Cases:

- `BM_OrderFeedbackShmPublishThenDrain`
- `BM_OrderFeedbackShmPollOneWithRefill`
- `BM_OrderFeedbackShmPublishPollLoop`
- `BM_OrderFeedbackShmPublishGlobalGapThenDrain`

Add target in `benchmark/core/trading/CMakeLists.txt`。

- [ ] **Step 2: 运行 release benchmark smoke**

运行：

```bash
cmake --build build/release --target order_feedback_shm_benchmark -j8
./build/release/benchmark/core/trading/order_feedback_shm_benchmark --benchmark_min_time=0.01s
```

Expected: benchmark runs and prints timing numbers。`PublishThenDrain` / `PollOneWithRefill` 包含 drain / refill
维护，不能写成纯 publish / poll latency；文档中只记录原始数值，不把结果外推到 Gate parser 或 Strategy 总链路。

## Task 7: Documentation And Verification

- [ ] **Step 1: 更新 onboarding 和 Gate handoff**

Modify:

- `doc/project_onboarding_guide.md`
- `doc/agent-handoff-gate-trade-architecture.md`

记录：

- Task1 已实现的文件入口；
- SHM lane / queue / gap / claim 语义；
- 下一步进入 Task2。

- [ ] **Step 2: 完整验证**

运行：

```bash
cmake --build build/debug --target core_order_feedback_event_test core_order_feedback_shm_test order_feedback_shm_config_test -j8
./build/debug/test/core/trading/core_order_feedback_event_test
./build/debug/test/core/trading/core_order_feedback_shm_test
./build/debug/test/config/order_feedback_shm_config_test
cmake --build build/release --target order_feedback_shm_benchmark -j8
./build/release/benchmark/core/trading/order_feedback_shm_benchmark --benchmark_filter='PublishThenDrain|PollOneWithRefill|PublishPollLoop|PublishGlobalGapThenDrain' --benchmark_min_time=0.01s
git diff --check
```

Expected: all tests pass, benchmark runs, diff check clean。

- [x] **Step 3: Commit boundary**

Task1 完成后再开始 Task2，避免把 transport ABI 和 Gate parser / Strategy 状态机混入同一提交。当前 Task7
docs sync worker 不提交；集成 worker 按用户指令决定是否提交 Task1 代码。
