# LeadLag 长时间实盘运行与测试 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 支持 LeadLag 先做 signal-only 长时间实盘观察，再逐步完成恢复链路验证、小额真实下单和端到端性能测试。

**Architecture:** 第一阶段新增独立 LeadLag live runner，复用现有 `TradingRuntime`、`RealtimeDataReader`、Gate `OrderSessionRuntimeAdapter` 和 feedback SHM，默认只跑信号与诊断，不提交订单。第二阶段已在 strategy 层补齐 LeadLag 生产订单意图、`OnOrderResponse()` / `OnOrderFeedback()` 执行状态闭环；真实 `--execute` runner 仍保持禁用，等待 REST reconcile、feedback WS 断线恢复、account / position 校验、小额真实订单 smoke guardrails 和端到端 benchmark。

**Tech Stack:** C++20、CMake、`core/trading/*`、`core/market_data/*`、`exchange/gate/trading/*`、`strategy/lead_lag/*`、Gate REST 辅助脚本。

---

## 当前判断

- LeadLag replay / signal 主链路已落地，`leadlag::Strategy::OnBookTicker()` 已串起 raw market、alignment、recorder、threshold、signal 和 synthetic position accounting。
- LeadLag live runner 已落地为 `tools/lead_lag/live_strategy.cpp`：默认 validate-only；`--connect-data` 进入 signal-only；`--execute` 只有在 `strategy.mode=live` 时解析为 `RunMode::kLiveOrders`。
- 生产订单闭环已在 strategy 层完成并通过测试：`SignalDecision::intent` 会转换为 IOC limit `core::OrderCreateRequest`，open / close / stoploss 订单接入 execution state，`OnOrderResponse()` 处理 rejected / cancel-rejected，`OnOrderFeedback()` 处理 terminal feedback、cancelled / partially-cancelled 和 rejected，`price_text` 使用固定 storage。
- 真实 `RunLiveOrders()` 仍是禁用 stub；即使 `--execute` 解析到 `RunMode::kLiveOrders`，runner 也返回禁用错误，不打开真实订单路径。
- 小额真实下单前仍缺 REST reconcile、feedback WS 断线未知订单恢复、account / position 事实源和 live smoke guardrails；长时间真实下单前还需要 unfilled-cancel / failure smoke 和端到端 benchmark。

## 文件结构

### 第一阶段：Signal-Only Live Runner

- Create: `tools/lead_lag/live_strategy.cpp`
  - LeadLag live 入口，加载 `config/strategies/lead_lag_btc_strategy.toml`。
  - 默认 dry-run / signal-only，不调用真实下单。
  - 显式 `--execute` 后才允许解析到 `RunMode::kLiveOrders`；当前真实订单运行路径仍禁用。
  - 支持运行时长、低频 summary、signals 输出路径和 diagnostics 输出。
- Modify: `tools/CMakeLists.txt`
  - 增加 `lead_lag_strategy` executable。
- Modify: `strategy/lead_lag/strategy.h`
  - 保持现有 synthetic replay 行为；为 live runner 暴露足够的信号与 degraded 状态查询。
- Test: `test/tools/lead_lag/live_strategy_test.cpp`
  - 覆盖 CLI gating、dry-run 默认行为和 signal-only 不下单。

### 第二阶段：Production Order Wiring

- Modify: `strategy/lead_lag/strategy.h`
  - 在非 synthetic accounting 模式下把 `SignalDecision::intent` 转成 `core::OrderCreateRequest`。
  - open 信号下单成功后调用 `ExecutionState::StartOpenOrder()`。
  - close / stoploss 信号下单成功后调用 `ExecutionState::StartCloseOrder()`。
  - `OnOrderResponse()` 处理 rejected / cancel-rejected。
  - `OnOrderFeedback()` 在 terminal feedback 后通过 `context.FindOrder()` 调用 `ExecutionState::ApplyTerminalOrder()`。
- Modify: `strategy/lead_lag/execution_state.h`
  - 仅在现有接口无法表达真实订单闭环时做局部补充。
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`
  - 覆盖 open 下单、close 下单、submit rejected、terminal filled、partial 非 terminal 不切 stage、continuity lost 暂停新开仓。

### 第三阶段：REST Reconcile、Feedback 恢复和 Account / Position 校验

- Create: `doc/lead_lag_reconcile_design.md`
  - 定义 feedback continuity lost 后的恢复状态机、暂停 / 恢复新开仓条件、未知订单处理和人工介入边界。
- Modify: `scripts/gate/query_gate_account.py`
  - 保持 read-only 查询职责，输出 live smoke 后需要复核的 open orders、position、pending orders 和 margin / balance 摘要。
- Create: `scripts/gate/reconcile_futures_orders.py`
  - 从本地 `local_order_id` / exchange order id / text 字段映射到 Gate open orders 和 historical order facts。
- Test: `test/tools/gate/reconcile_futures_orders_test.cpp`
  - 覆盖 open order 恢复、已成交恢复、已撤单恢复、无法匹配进入 manual intervention。

### 第四阶段：Live Smoke Ladder

- Document: `doc/lead_lag_live_runtime_plan.md`
  - 维护以下 smoke 顺序和证据要求。
- Runbook inputs:
  - `config/strategies/lead_lag_btc_strategy.toml`
  - `config/strategy/lead_lag.toml`
  - `config/data_readers/strategy_data_reader.toml`
  - `config/order_feedback/gate_order_feedback_session.toml`
  - Gate / Binance data session configs
- Evidence outputs:
  - live runner summary
  - feedback session summary
  - REST open orders / position / pending orders 复核
  - signal CSV 或低频 signal summary

### 第五阶段：End-to-End Benchmark

- Create: `benchmark/strategy/lead_lag_runtime_benchmark.cpp`
  - 覆盖 `RealtimeDataReader -> LeadLag OnBookTicker -> order intent -> OrderManager -> Gate adapter` 的本地路径。
- Create: `benchmark/strategy/lead_lag_feedback_runtime_benchmark.cpp`
  - 覆盖 `OrderFeedbackSession parser -> SHM -> TradingRuntime -> OrderManager -> LeadLag OnOrderFeedback()`。
- Modify: `benchmark/strategy/CMakeLists.txt`
- Verify:
  - benchmark 只作为本地链路证据，不把没有 live 证据的结果写成生产收益结论。

## 任务计划

### Task 1: 新增 LeadLag Signal-Only Live Runner

**Files:**
- Create: `tools/lead_lag/live_strategy.cpp`
- Modify: `tools/CMakeLists.txt`
- Test: `test/tools/lead_lag/live_strategy_test.cpp`

- [x] **Step 1: 写 CLI gating 测试**

验证默认模式不会真实下单，只有显式 `--execute` 才允许真实订单模式。

Run:

```bash
ctest --test-dir build/debug -R lead_lag_live_strategy --output-on-failure
```

Expected before implementation: test target or cases fail.

- [x] **Step 2: 实现 live runner 冷路径**

加载 strategy config、data reader config、order session config 和 feedback config；复用现有 `TradingRuntime` create pattern。默认 dry-run 时构造 signal-only wrapper，不调用 `StrategyContext::PlaceLimitOrder()`。

- [x] **Step 3: 接入低频 diagnostics**

输出 runtime loop counters、data reader poll counters、feedback counters、signal counters、degraded / needs_reconcile 和最后行情时间。低频输出放在 runner 层，避免在 LeadLag 热路径加日志。

- [x] **Step 4: 验证 dry-run runner**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R '(lead_lag|trading_runtime|order_feedback)' --output-on-failure
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --duration-sec 1
```

Expected: build succeeds, tests pass, runner exits normally without submitting orders.

2026-05-21 result:

- `./build.sh debug` passed.
- `ctest --test-dir build/debug -R '(lead_lag|trading_runtime|order_feedback)' --output-on-failure` passed 18/18.
- `./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --duration-sec 1` passed in validate-only mode. In the sandbox this command needed elevated filesystem permission because the configured log sink writes under `/home/liuxiang/log`.

- [x] **Step 5: Commit**

```bash
git add tools/lead_lag/live_strategy.cpp tools/CMakeLists.txt test/tools/lead_lag/live_strategy_test.cpp
git commit -m "Add lead lag live signal runner"
```

### Task 2: Signal-Only 长时间实盘观察

**Files:**
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Runtime evidence: log files under `/home/liuxiang/log/`

- [x] **Step 1: 启动前检查**

确认 `lead_lag_btc_strategy.toml` 仍是 `mode = "dry_run"`，确认 Gate / Binance data session 和 Gate feedback session configs 指向预期 SHM。

- [x] **Step 2: 30 分钟 signal-only run**

Run:

```bash
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 1800
```

Expected: strategy exits by duration, no real orders submitted, diagnostics shows data reader progress and no unexpected degraded state.

2026-05-21 result: passed. See “运行记录 / 2026-05-21 30 分钟 signal-only live 观察”。

- [ ] **Step 3: 2 到 4 小时 signal-only run**

Run:

```bash
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 14400
```

Expected: no SHM overrun requiring manual reset, signal counters and last event timestamps remain plausible, process exits cleanly.

2026-05-21 status: 4 小时 run 已启动，当前仍在运行。最终 summary 等运行自然结束后补齐；见“运行记录 / 2026-05-21 4 小时 signal-only live 观察（进行中）”。

- [ ] **Step 4: 记录证据**

把运行命令、起止时间、signal summary、runtime diagnostics summary 和异常记录补到本文件的“运行记录”。

### Task 3: 接入 LeadLag 生产订单闭环

**Files:**
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `strategy/lead_lag/execution_state.h` only if existing state interface is insufficient
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [x] **Step 1: 写状态机测试**

覆盖 open 下单成功、close 下单成功、submit rejected、terminal filled、partial non-terminal、continuity lost 后停止新开仓。

Run:

```bash
ctest --test-dir build/debug -R lead_lag_strategy_interface --output-on-failure
```

Completed: production order cases were added to `test/strategy/lead_lag_strategy_interface_test.cpp` and verified.

- [x] **Step 2: 实现订单请求转换**

把 `SignalDecision::intent` 转成 `core::OrderCreateRequest`，使用 lag side metadata 进行 symbol、side、quantity、price、time-in-force、reduce-only 设置。数量来自 `open_notional` 和 lag quote，价格必须符合 instrument tick 约束。

- [x] **Step 3: 实现 response / feedback hook**

`OnOrderResponse()` 只处理 submit rejected / cancel rejected 等上行响应；position 和 stage 只由 terminal private feedback 推进。`OnOrderFeedback()` 在 runtime 先更新 `OrderManager` 之后读取 `context.FindOrder(event.local_order_id)`，再调用 `ExecutionState::ApplyTerminalOrder()`。

- [x] **Step 4: 验证状态机**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R lead_lag --output-on-failure
```

2026-05-21 status: strategy-layer order wiring is complete and covered by `lead_lag_strategy_interface` tests. Follow-up commits also covered IOC terminal handling, cancelled / partially-cancelled and rejected feedback edge cases, and fixed-buffer `price_text` boundaries.

- [x] **Step 5: Commit**

```bash
git add strategy/lead_lag/strategy.h strategy/lead_lag/execution_state.h test/strategy/lead_lag_strategy_interface_test.cpp
git commit -m "Wire lead lag production order feedback"
```

Commit boundary:

- `ae66ee0 Wire lead lag external orders`
- `4c992d6 Fix lead lag order feedback edge cases`
- `37a3cc3 Fix lead lag IOC terminal handling`
- `0ae3a7e Add lead lag price text boundary tests`

### Task 3.5: Live Runner Execute Gating Smoke Boundaries

**Files:**
- Modify: `test/tools/lead_lag/live_strategy_test.cpp`
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Modify: `strategy/lead_lag/README.md`
- Optional summary: `doc/project_onboarding_guide.md`

- [x] **Step 1: 覆盖 execute=false / live / connect-data 边界**

`execute=false` 时即使 `strategy.mode=live` 且 `connect_data=true`，`ResolveRunMode()` 仍必须返回 `RunMode::kSignalOnly`，不进入 live orders。

- [x] **Step 2: 覆盖 execute=true 优先级**

`execute=true`、`strategy.mode=live`、`connect_data=true` 时解析为 `RunMode::kLiveOrders`。这只验证 gating 选择；真实 `RunLiveOrders()` 仍是禁用 stub。

- [x] **Step 3: 覆盖 summary 文本稳定性**

`RunModeName()` 覆盖 `validate_only`、`signal_only`、`live_orders`，避免 runner summary 文本无意漂移。

Verification:

```bash
cmake --build build/debug --target lead_lag_live_strategy_test -j8
ctest --test-dir build/debug -R lead_lag_live_strategy --output-on-failure
./build.sh debug
ctest --test-dir build/debug -R '(lead_lag_live_strategy|lead_lag_strategy_interface|core_trading_strategy_context|gate_order)' --output-on-failure
git diff --check
```

2026-05-21 result: target build passed; target `lead_lag_live_strategy` ctest passed; full debug build passed; focused ctest passed 9/9; `git diff --check` passed.

Commit boundary: this task only adds gating/smoke tests and documentation. It must not enable `RunLiveOrders()` or open the real live order path.

### Task 3.6: Review Findings Follow-up

**Files:**
- Create: `test/tools/lead_lag/lead_lag_live_orders_disabled_strategy.toml`
- Create: `test/tools/lead_lag/lead_lag_live_orders_disabled_smoke.cmake`
- Modify: `test/tools/lead_lag/CMakeLists.txt`
- Modify: `tools/lead_lag/live_strategy.cpp`
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Modify: `doc/project_onboarding_guide.md`

- [x] **Step 1: 覆盖实际 runner 禁用路径**

新增 CTest wrapper 实际运行 `lead_lag_strategy --execute --duration-sec 1`，断言 exit code 为 3，输出包含禁用错误，并避免进入 signal-only / runtime create / credentials 错误路径。

- [x] **Step 2: 修正禁用文案**

`RunLiveOrders()` 文案说明当前剩余 blocker 是 REST reconcile、feedback recovery 和 live smoke guardrails，不再写 strategy 层 production order wiring。

- [x] **Step 3: 修正后续任务顺序**

REST reconcile / feedback 断线恢复必须排在小额真实下单 smoke 前；Live Smoke 顺序同步按该边界排列。

### Task 4: REST Reconcile 和恢复链路

**Files:**
- Create: `doc/lead_lag_reconcile_design.md`
- Create: `scripts/gate/reconcile_futures_orders.py`
- Modify: `scripts/gate/query_gate_account.py`
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `strategy/lead_lag/execution_state.h`
- Test: `test/tools/gate/reconcile_futures_orders_test.cpp`
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`

Precondition: Task4 完成前，`lead_lag_strategy --execute` 仍应作为禁用边界处理；真实订单路径不可打开。

- [ ] **Step 1: 写 reconcile 设计**

定义 continuity lost、WS reconnect、进程重启后的本地订单、open orders、position 和 execution group 恢复规则。

- [ ] **Step 2: 实现 read-only reconcile helper**

先用 read-only REST 查询恢复事实，不在第一版自动补单。无法匹配的订单进入 manual intervention 状态。

- [ ] **Step 3: 接入 LeadLag 恢复状态**

恢复成功后清除 `needs_reconcile`，恢复失败则保持新开仓暂停，只允许人工确认后继续。

- [ ] **Step 4: 验证 feedback 断线 / reconnect 恢复 smoke**

Expected: feedback continuity lost 后策略暂停新开仓；WS reconnect 后不会把未知订单伪造成 terminal fact。

- [ ] **Step 5: 验证 REST reconcile smoke**

Expected: REST reconcile 后能明确恢复或停留在人工介入状态；恢复成功前不允许重新打开新开仓。

### Task 5: 小额真实下单 Smoke

**Files:**
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Runtime evidence: Gate REST query output and live runner output

Precondition: Task4 的 REST reconcile 和 feedback 断线恢复 smoke 完成后，才允许用单独审查过的提交打开真实 runner 并开始 Task5。

- [ ] **Step 1: Filled open / close smoke**

用极小 size 或最小合约张数跑一轮 open / close，要求真实执行前显式传入 `--execute`。

Expected: feedback 至少包含 open terminal 和 close terminal；REST 复核 open orders 为空、position size 为 0。

- [ ] **Step 2: Unfilled cancel smoke**

使用不可立即成交价格触发挂单，然后走 cancel 分支。

Expected: feedback terminal cancelled；REST 复核无残留 open orders。

- [ ] **Step 3: Rejected / cancel-rejected smoke**

用受控非法价格、数量或过期订单构造 rejected 场景。

Expected: LeadLag execution group 不残留 pending order；strategy 不继续盲目开仓。

- [ ] **Step 4: Commit smoke evidence**

只提交整理后的文档证据，不提交密钥、原始敏感日志或本地临时配置。

```bash
git add doc/lead_lag_live_runtime_plan.md
git commit -m "Document lead lag live smoke evidence"
```

### Task 6: 长时间真实订单运行

**Files:**
- Modify: `doc/lead_lag_live_runtime_plan.md`

- [ ] **Step 1: 30 分钟真实订单 run**

Expected: 所有订单生命周期都有 feedback；结束后 REST 复核 open orders 为空或符合预期持仓。

- [ ] **Step 2: 2 到 4 小时真实订单 run**

Expected: 无未知 pending order、无未解释 position drift、无 feedback continuity lost 未恢复状态。

- [ ] **Step 3: 更长时间 run**

开始前固定最大仓位、最大订单频率、最大连续错误次数和 kill switch 操作方式。结束后保存 summary、REST 复核和异常清单。

### Task 7: 端到端 Benchmark

**Files:**
- Create: `benchmark/strategy/lead_lag_runtime_benchmark.cpp`
- Modify: `benchmark/strategy/CMakeLists.txt`

- [ ] **Step 1: 写本地 runtime benchmark**

覆盖行情事件进入 LeadLag 到 OrderManager / Gate adapter 的本地路径；不访问外网。

- [ ] **Step 2: 写 feedback benchmark**

覆盖 private feedback 固定事件进入 SHM、runtime 消费、OrderManager 更新和 LeadLag feedback hook。

- [ ] **Step 3: 运行 release benchmark**

Run:

```bash
./build.sh release
taskset -c 2 ./build/release/benchmark/strategy/lead_lag_strategy_benchmark
taskset -c 2 ./build/release/benchmark/strategy/lead_lag_runtime_benchmark
```

Expected: benchmark 正常完成；文档只记录实测数据，不写没有证据的性能收益结论。

## Live Smoke 顺序

1. `lead_lag_strategy` dry-run 1 秒启动验证。
2. signal-only 30 分钟观察。
3. signal-only 2 到 4 小时观察。
4. `lead_lag_strategy --execute` gating smoke：允许解析到 `RunMode::kLiveOrders`，但 `RunLiveOrders()` 必须返回禁用边界，不提交真实订单。
5. feedback session 断线 / reconnect 恢复 smoke。
6. REST reconcile smoke。
7. 小额 filled open / close。
8. unfilled-cancel。
9. rejected / cancel-rejected。
10. 30 分钟真实订单 run。
11. 2 到 4 小时真实订单 run。
12. 更长时间真实订单 run。

## 完成标准

- signal-only live runner 可以长时间消费 Gate / Binance realtime SHM，并低频输出可解释 diagnostics。
- REST reconcile / feedback recovery 完成前，`lead_lag_strategy --execute` 仍是禁用边界：`ResolveRunMode()` 可以选择 `live_orders`，但 `RunLiveOrders()` 不可提交真实订单。
- LeadLag strategy 层 default production accounting 可以提交 IOC limit order intent；runner 默认 dry-run / signal-only 不提交订单。
- 订单状态只由 `OrderManager` 和 private feedback 推进；submit rejected 只清理 pending 状态，不伪造成交事实。
- feedback continuity lost 后暂停新开仓；REST reconcile 成功前不自动恢复。
- 每次 live smoke 后都有 REST open orders / position / pending orders 复核。
- 性能、稳定性和长期运行结论均来自测试、benchmark、profile 或 live run 证据。

## 运行记录

本节只记录已经发生的长时间运行或 smoke 证据。新增记录应包含日期、commit、命令、运行时长、账户复核摘要和异常摘要。

### 2026-05-21 30 分钟 signal-only live 观察

- Commit: `92ed8a2 Add lead lag live signal runner`
- Window: 2026-05-21 04:02:29 UTC 到 2026-05-21 04:32 左右 UTC；producer 在 04:34:51 UTC 手动 TERM 后正常收尾。
- Mode: `lead_lag_strategy run_mode=signal_only execute=false connect_data=true`; no live orders were enabled.
- Commands:

```bash
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect --duration-sec 1900
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml --connect
./build/debug/tools/binance_data_session --config config/data_sessions/binance_data_session.toml --connect
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 1800
```

- LeadLag summary: `exit_code=0 book_tickers=480259 signals=0 open=0 close=0 stoploss=0 order_responses=0 order_feedbacks=0 loop_iterations=3374031710 idle_iterations=3373551451 data_reader_polls=3374031710 data_reader_empty_polls=3373551451 data_reader_events=480259 signals_output=-`.
- Gate data session summary: `result=ok active=true phase=kClosed error=kNone book_tickers=641739 rx_messages=85156 tx_messages=392`.
- Binance data session summary: `result=ok active=true phase=kClosed error=kNone book_tickers=9547386 rx_messages=719935 tx_messages=401`.
- Gate feedback summary: `start_result=true duration_reached=true ready=false text_messages=2 binary_messages=0 login_sent=1 login_accepted=1 subscribe_sent=1 subscribe_acks=1 events_published=0 global_continuity_lost_events_published=1 shm_published=8`.
- Notes: first attempt at 03:37 UTC was interrupted by SIGTERM around 03:40 UTC and is not counted as the 30-minute run. The feedback session published `kSessionDisconnected` continuity lost when its configured duration ended; this was expected for a private feedback disconnect and did not affect signal-only observation because the runner did not read feedback and did not submit orders.

### 2026-05-21 4 小时 signal-only live 观察（进行中）

- Commit: `39cc962 Document lead lag signal-only smoke`
- Window: started at 2026-05-21 04:37:38 UTC; expected natural LeadLag exit around 2026-05-21 08:37:38 UTC.
- Checkpoint: at 2026-05-21 07:39:40 UTC the four processes were still running:
  - `1231230 ./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect --duration-sec 14500`
  - `1231262 ./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml --connect`
  - `1231285 ./build/debug/tools/binance_data_session --config config/data_sessions/binance_data_session.toml --connect`
  - `1231306 ./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 14400`
- Mode: `lead_lag_strategy run_mode=signal_only execute=false connect_data=true`; no live orders are enabled.
- Commands:

```bash
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect --duration-sec 14500
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml --connect
./build/debug/tools/binance_data_session --config config/data_sessions/binance_data_session.toml --connect
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 14400
```

- Current observation: Gate / Binance data sessions and LeadLag signal-only process remained alive at the 3-hour checkpoint. Gate feedback session published one `kSessionDisconnected` continuity lost at 2026-05-21 06:54:17 UTC. This does not affect signal-only observation because the runner does not read feedback and does not submit orders, but it remains a blocker/risk to handle before real-order smoke.
- Final summary: pending. Fill in LeadLag summary, producer summaries, and final feedback session summary after the run ends.
