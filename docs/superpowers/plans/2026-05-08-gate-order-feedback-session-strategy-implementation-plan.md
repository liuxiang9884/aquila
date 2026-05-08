# Gate OrderFeedbackSession And Strategy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Task1 的 SHM transport 上实现 Gate private `futures.orders` feedback session，并让 Strategy 通过 `OrderFeedbackEvent` 推进订单状态。

**Architecture:** `OrderFeedbackSession` 是下行协议适配层，只负责 login / subscribe / parse / event publish；Strategy 按自己的 `strategy_id` drain SHM lane 并更新本地 `OrderPool` 中的订单。`OrderFeedbackSession` 不访问 Strategy order，不维护策略状态。第一版只使用 `futures.orders`，不接 `futures.usertrades`。

**Tech Stack:** C++20、CMake、GoogleTest、Google Benchmark、simdjson、Gate SBE generated headers、Nova SHM SPSC、existing `aquila::gate::OrderSession`。

---

## Dependencies

先完成并提交：

- `docs/superpowers/plans/2026-05-08-order-feedback-shm-transport-implementation-plan.md`

Task2 不应与 Task1 放在同一个实现提交中。

## Reference Docs

- `docs/superpowers/specs/2026-05-08-gate-order-feedback-session-strategy-design.md`
- `docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md`
- `docs/superpowers/specs/2026-05-08-order-feedback-shm-transport-design.md`
- `doc/agent-handoff-gate-trade-architecture.md`
- `third_party/sirius/exchange/gate/trade/trade_engine.cpp`
- `third_party/sirius/exchange/gate/parser/trade_parser.cpp`
- `exchange/gate/sbe/schema/gate_fex_ws_latest.xml`
- `exchange/gate/sbe/generated/`

## Task 1: Gate Orders Parser

- [ ] **Step 1: 新增 parser tests**

Create `test/exchange/gate/trading/order_feedback_parser_test.cpp`。

覆盖：

- `_new` -> `OrderFeedbackKind::kAccepted`；
- `_update` with left > 0 -> `kPartialFilled`；
- `filled` with left == 0 -> `kFilled`；
- `cancelled` -> `kCancelled` with `kManualCancelled`；
- `ioc` / `reduce_only` / `reduce_out` / `stp` / `liquidated` / `auto_deleveraging` / `position_close` 映射；
- `text=t-<local_order_id>` parse；
- invalid text drops event and increments diagnostics；
- `sizeExponent != 0` unsupported；
- `filled` with left != 0 unsupported；
- exchange update time converts to ns；
- `role` maps maker / taker / none。

运行：

```bash
cmake --build build/debug --target gate_order_feedback_parser_test -j8
./build/debug/test/exchange/gate/trading/gate_order_feedback_parser_test
```

Expected: target 初始编译失败，因为 parser 尚未实现。

- [ ] **Step 2: 实现 parser 类型**

Create:

- `exchange/gate/trading/order_feedback_parser.h`
- optionally `exchange/gate/trading/order_feedback_types.h` if Gate parser diagnostics need a separate type file。

实现：

- `OrderFeedbackParseStatus`
- `OrderFeedbackParserStats`
- `ParseOrdersFeedback(...) -> optional<OrderFeedbackEvent>` 或返回轻量 result struct；
- finish reason mapping；
- role mapping；
- SBE decimal price to `double`；
- `sizeExponent == 0` contract quantity path。

约束：

- 不访问 Strategy order；
- 不分配动态内存；
- 不把 Gate string 字段保存到 event；
- malformed 单条 update 计数并丢弃；
- session 级不可恢复错误由 session 决定是否 `MarkGlobalGap()`。

- [ ] **Step 3: 接入 CMake 并验证**

Modify `test/exchange/gate/trading/CMakeLists.txt`。

运行：

```bash
cmake --build build/debug --target gate_order_feedback_parser_test -j8
./build/debug/test/exchange/gate/trading/gate_order_feedback_parser_test
```

Expected: parser tests pass。

## Task 2: OrderFeedbackSession

- [ ] **Step 1: 新增 session tests**

Create `test/exchange/gate/trading/order_feedback_session_test.cpp`。

覆盖 fake WebSocket / fake publisher：

- active 后发送 login；
- login success 后 subscribe `futures.orders`；
- subscription success 后进入 ready；
- binary SBE orders payload publish event；
- malformed payload increments diagnostics；
- disconnect / reconnect path marks global gap；
- `Publish()` lane full failure increments session diagnostics but does not block。

运行：

```bash
cmake --build build/debug --target gate_order_feedback_session_test -j8
./build/debug/test/exchange/gate/trading/gate_order_feedback_session_test
```

Expected: target 初始编译失败，因为 session 尚未实现。

- [ ] **Step 2: 实现 session**

Create `exchange/gate/trading/order_feedback_session.h`。

实现 template session，风格参考 `exchange/gate/trading/order_session.h` 和 market data `DataSession`：

```cpp
template <typename PublisherT, typename WebSocketPolicy, typename DiagnosticsPolicy>
class OrderFeedbackSession {
 public:
  bool Start();
  void Stop();
  void OnWebSocketActive();
  void OnTextMessage(websocket::MessageView message);
  void OnBinaryMessage(websocket::MessageView message);
};
```

实现职责：

- login request；
- subscribe request；
- JSON control response parse；
- SBE binary dispatch to orders parser；
- successful parse -> `publisher.Publish(event)`；
- disconnect / reconnect unknown gap -> `publisher.MarkGlobalGap()`。

约束：

- 不访问 Strategy；
- 不访问 `OrderPool`；
- 不维护 exchange id map；
- 不做 REST query。

- [ ] **Step 3: 验证 session tests**

运行：

```bash
cmake --build build/debug --target gate_order_feedback_session_test -j8
./build/debug/test/exchange/gate/trading/gate_order_feedback_session_test
```

Expected: session tests pass。

## Task 3: Config And Tool

- [ ] **Step 1: 新增 config 示例**

Create `config/order_feedback/gate_order_feedback_session.toml`。

建议结构：

```toml
[log]
name = "gate_order_feedback_session"
console = true
file = true
file_dir = "/home/liuxiang/log"

[order_feedback_session]
name = "gate_order_feedback_session"
settle = "usdt"

[order_feedback_session.credentials]
api_key_env = "TEST_KEY"
api_secret_env = "TEST_SECRET"

[order_feedback_session.websocket]
host = "fx-ws.gateio.ws"
service = "443"
target = "/v4/ws/usdt"
enable_tls = true
```

SHM sink 使用 Task1 config，或在同文件中引用：

```toml
[order_feedback_session.shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
```

- [ ] **Step 2: 新增 config parser tests**

Create `test/config/order_feedback_session_config_test.cpp`。

覆盖：

- 示例配置 load 成功；
- `settle` 生成或校验 `/v4/ws/<settle>` target；
- credentials env name 读取；
- websocket config 复用现有 `ParseWebSocketConfig()` 风格；
- shm config 与 Task1 transport config 对齐。

- [ ] **Step 3: 实现 config parser**

Create:

- `exchange/gate/trading/order_feedback_session_config.h`
- `exchange/gate/trading/order_feedback_session_config.cpp`

保持和 `order_session_config` 同风格，配置 parse 在 cold path 完成。

- [ ] **Step 4: 新增 live probe tool**

Create `tools/gate/order_feedback_session.cpp`。

行为：

- 默认 dry-run 打印 config；
- `--connect` 后 login + subscribe；
- `--duration-sec` 控制运行时长；
- 低频打印 event count / gap epoch / parser stats；
- 不做下单。

运行：

```bash
cmake --build build/debug --target gate_order_feedback_session -j8
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml
```

Expected: dry-run 打印配置并退出。

## Task 4: Strategy Feedback Apply

- [ ] **Step 1: 新增 Strategy feedback tests**

Extend `test/strategy/strategy_test.cpp` or create `test/strategy/strategy_feedback_test.cpp`。

覆盖：

- accepted event moves sent order to `kAccepted` and stores `exchange_order_id`；
- accepted event notifies session cache update；
- partial filled updates cumulative quantity and average fill price；
- repeated partial with same cumulative quantity is ignored；
- filled event finalizes order and is idempotent；
- cancelled with zero fill -> `kCancelled`；
- cancelled with nonzero fill -> `kPartiallyCancelled`；
- terminal event notifies session cache forget；
- rejected event moves order to `kRejected`；
- unknown local order id increments diagnostics；
- lane/global gap epoch change sets `feedback_gap_detected`。

运行：

```bash
cmake --build build/debug --target strategy_test -j8
./build/debug/test/strategy/strategy_test
```

Expected: 初始测试失败，因为 Strategy 尚未消费 feedback event。

- [ ] **Step 2: 扩展 Strategy order 类型**

Modify `strategy/order_types.h`：

- add `exchange_order_id`；
- add `cumulative_filled_quantity`；
- add `cumulative_filled_value` or equivalent average fill state；
- add `last_fill_price` if needed for diagnostics；
- add `exchange_update_ns`；
- add `finish_reason`；
- add `role`；
- add `is_finished` if current status alone cannot express terminal efficiently。

保持字段是 plain data，不引入动态分配。

- [ ] **Step 3: 实现 Strategy::OnOrderFeedback**

Modify `strategy/strategy.h`：

- `OnOrderFeedback(const OrderFeedbackEvent&)`；
- `OnFeedbackGap(std::uint64_t lane_gap_epoch, std::uint64_t global_gap_epoch)`；
- duplicate / stale event diagnostics；
- terminal idempotency；
- session cache update / forget hooks。

OrderSession cache hooks 需要是同线程调用。若当前 `OrderSession` 缺少公开 cache update API，新增小接口：

```cpp
void CacheExchangeOrderId(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept;
void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept;
```

命名可按现有 `OrderSession` API 调整。

- [ ] **Step 4: 验证 Strategy tests**

运行：

```bash
cmake --build build/debug --target strategy_test -j8
./build/debug/test/strategy/strategy_test
```

Expected: Strategy feedback apply tests pass。

## Task 5: End-To-End Fake Integration

- [ ] **Step 1: 新增 fake integration test**

Create `test/exchange/gate/trading/order_feedback_integration_test.cpp`。

流程：

- create SHM；
- create fake `OrderFeedbackSession` publisher；
- create Strategy with `strategy_id=1`；
- Strategy place fake order；
- feed `_new` SBE payload；
- Strategy reader poll；
- assert order accepted and exchange id cached；
- feed partial / filled / cancelled payloads；
- assert state machine final state。

运行：

```bash
cmake --build build/debug --target gate_order_feedback_integration_test -j8
./build/debug/test/exchange/gate/trading/gate_order_feedback_integration_test
```

Expected: fake end-to-end lifecycle pass。

- [ ] **Step 2: benchmark parse + publish**

Create `benchmark/exchange/gate/trading/order_feedback_session_benchmark.cpp`。

Cases:

- `BM_GateOrdersParseAccepted`
- `BM_GateOrdersParsePublish`
- `BM_StrategyApplyOrderFeedback`

运行：

```bash
cmake --build build/release --target gate_order_feedback_session_benchmark -j8
./build/release/benchmark/exchange/gate/trading/gate_order_feedback_session_benchmark --benchmark_min_time=0.01s
```

Expected: benchmark runs and reports raw timing。

## Task 6: Live Smoke

- [ ] **Step 1: 准备 live smoke 脚本或 tool flow**

第一版 live smoke 使用极小数量：

- Strategy 通过 Gate WS 下 1 手 `BTC_USDT` limit order；
- feedback session 确认 accepted；
- 30s 未成交则 cancel；
- 已成交则使用 REST 或已有工具市价平仓；
- 结束后用 `scripts/gate/query_gate_account.py positions` 和 `orders` 查询确认无残留。

如果 Task2 尚未实现可靠平仓链路，live smoke 只做 accepted / cancel lifecycle，不做性能结论。

- [ ] **Step 2: 执行 live smoke 时保留输出**

运行前确认用户明确允许真实下单。

命令示例按最终 tool 参数填写，并保留：

- place local order id；
- accepted exchange order id；
- final order status；
- REST positions；
- REST open orders。

## Task 7: Documentation And Verification

- [ ] **Step 1: 更新文档**

Modify:

- `doc/project_onboarding_guide.md`
- `doc/agent-handoff-gate-trade-architecture.md`
- relevant specs / plans if implementation偏离本计划。

记录：

- parser / session / Strategy feedback apply 代码入口；
- 验证命令；
- live smoke 结果；
- 下一步 REST reconcile 和 account / position feedback 边界。

- [ ] **Step 2: 完整验证**

运行：

```bash
cmake --build build/debug --target gate_order_feedback_parser_test gate_order_feedback_session_test strategy_test gate_order_feedback_integration_test -j8
./build/debug/test/exchange/gate/trading/gate_order_feedback_parser_test
./build/debug/test/exchange/gate/trading/gate_order_feedback_session_test
./build/debug/test/strategy/strategy_test
./build/debug/test/exchange/gate/trading/gate_order_feedback_integration_test
cmake --build build/release --target gate_order_feedback_session_benchmark -j8
./build/release/benchmark/exchange/gate/trading/gate_order_feedback_session_benchmark --benchmark_min_time=0.01s
git diff --check
```

Expected: all tests pass, benchmark runs, diff check clean。

- [ ] **Step 3: Commit**

Commit message:

```text
Add Gate order feedback session
```

提交后再讨论 REST reconcile、account / position feedback 和多 strategy production supervisor。
