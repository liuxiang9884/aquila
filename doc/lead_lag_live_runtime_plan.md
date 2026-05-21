# LeadLag 长时间实盘运行与测试 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 支持 LeadLag 先做 signal-only 长时间实盘观察，再逐步进入小额真实下单、恢复链路验证和端到端性能测试。

**Architecture:** 第一阶段新增独立 LeadLag live runner，复用现有 `TradingRuntime`、`RealtimeDataReader`、Gate `OrderSessionRuntimeAdapter` 和 feedback SHM，默认只跑信号与诊断，不提交订单。第二阶段补齐 LeadLag 生产订单意图、`OnOrderResponse()` / `OnOrderFeedback()` 执行状态闭环；第三阶段补 REST reconcile、account / position 校验、异常 smoke 和端到端 benchmark。

**Tech Stack:** C++20、CMake、`core/trading/*`、`core/market_data/*`、`exchange/gate/trading/*`、`strategy/lead_lag/*`、Gate REST 辅助脚本。

---

## 当前判断

- LeadLag replay / signal 主链路已落地，`leadlag::Strategy::OnBookTicker()` 已串起 raw market、alignment、recorder、threshold、signal 和 synthetic position accounting。
- 当前没有 LeadLag live runner；现有 live runtime 工具是 `tools/gate/demo_strategy.*`，LeadLag 工具只有 `tools/lead_lag/replay.cpp`。
- `strategy/lead_lag/strategy.h` 中 `OnOrderResponse()` 仍为空，`OnOrderFeedback()` 目前只处理 feedback continuity lost。
- `strategy/lead_lag/execution_state.h` 已有生产闭环所需的 `StartOpenOrder()`、`StartCloseOrder()`、`ApplyTerminalOrder()`、`ApplySubmitRejected()`，但还没有从 strategy hook 接入。
- 长时间真实下单前仍缺 REST reconcile、feedback WS 断线未知订单恢复、account / position 事实源、unfilled-cancel / failure live smoke 和端到端 benchmark。

## 文件结构

### 第一阶段：Signal-Only Live Runner

- Create: `tools/lead_lag/live_strategy.cpp`
  - LeadLag live 入口，加载 `config/strategies/lead_lag_btc_strategy.toml`。
  - 默认 dry-run / signal-only，不调用真实下单。
  - 显式 `--execute` 后才允许进入真实订单模式。
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

### 第三阶段：REST Reconcile 和 Account / Position 校验

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

- [ ] **Step 1: 启动前检查**

确认 `lead_lag_btc_strategy.toml` 仍是 `mode = "dry_run"`，确认 Gate / Binance data session 和 Gate feedback session configs 指向预期 SHM。

- [ ] **Step 2: 30 分钟 signal-only run**

Run:

```bash
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 1800
```

Expected: strategy exits by duration, no real orders submitted, diagnostics shows data reader progress and no unexpected degraded state.

- [ ] **Step 3: 2 到 4 小时 signal-only run**

Run:

```bash
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 14400
```

Expected: no SHM overrun requiring manual reset, signal counters and last event timestamps remain plausible, process exits cleanly.

- [ ] **Step 4: 记录证据**

把运行命令、起止时间、signal summary、runtime diagnostics summary 和异常记录补到本文件的“运行记录”。

### Task 3: 接入 LeadLag 生产订单闭环

**Files:**
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `strategy/lead_lag/execution_state.h` only if existing state interface is insufficient
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [ ] **Step 1: 写状态机测试**

覆盖 open 下单成功、close 下单成功、submit rejected、terminal filled、partial non-terminal、continuity lost 后停止新开仓。

Run:

```bash
ctest --test-dir build/debug -R lead_lag_strategy_interface --output-on-failure
```

Expected before implementation: new production order cases fail.

- [ ] **Step 2: 实现订单请求转换**

把 `SignalDecision::intent` 转成 `core::OrderCreateRequest`，使用 lag side metadata 进行 symbol、side、quantity、price、time-in-force、reduce-only 设置。数量来自 `open_notional` 和 lag quote，价格必须符合 instrument tick 约束。

- [ ] **Step 3: 实现 response / feedback hook**

`OnOrderResponse()` 只处理 submit rejected / cancel rejected 等上行响应；position 和 stage 只由 terminal private feedback 推进。`OnOrderFeedback()` 在 runtime 先更新 `OrderManager` 之后读取 `context.FindOrder(event.local_order_id)`，再调用 `ExecutionState::ApplyTerminalOrder()`。

- [ ] **Step 4: 验证状态机**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R lead_lag --output-on-failure
```

Expected: LeadLag tests pass.

- [ ] **Step 5: Commit**

```bash
git add strategy/lead_lag/strategy.h strategy/lead_lag/execution_state.h test/strategy/lead_lag_strategy_interface_test.cpp
git commit -m "Wire lead lag production order feedback"
```

### Task 4: 小额真实下单 Smoke

**Files:**
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Runtime evidence: Gate REST query output and live runner output

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

### Task 5: REST Reconcile 和恢复链路

**Files:**
- Create: `doc/lead_lag_reconcile_design.md`
- Create: `scripts/gate/reconcile_futures_orders.py`
- Modify: `scripts/gate/query_gate_account.py`
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `strategy/lead_lag/execution_state.h`
- Test: `test/tools/gate/reconcile_futures_orders_test.cpp`
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [ ] **Step 1: 写 reconcile 设计**

定义 continuity lost、WS reconnect、进程重启后的本地订单、open orders、position 和 execution group 恢复规则。

- [ ] **Step 2: 实现 read-only reconcile helper**

先用 read-only REST 查询恢复事实，不在第一版自动补单。无法匹配的订单进入 manual intervention 状态。

- [ ] **Step 3: 接入 LeadLag 恢复状态**

恢复成功后清除 `needs_reconcile`，恢复失败则保持新开仓暂停，只允许人工确认后继续。

- [ ] **Step 4: 验证断线恢复 smoke**

Expected: feedback continuity lost 后策略暂停新开仓；REST reconcile 后能明确恢复或停留在人工介入状态。

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
4. 小额 filled open / close。
5. unfilled-cancel。
6. rejected / cancel-rejected。
7. feedback session 断线 / reconnect。
8. REST reconcile。
9. 30 分钟真实订单 run。
10. 2 到 4 小时真实订单 run。
11. 更长时间真实订单 run。

## 完成标准

- signal-only live runner 可以长时间消费 Gate / Binance realtime SHM，并低频输出可解释 diagnostics。
- LeadLag 真实订单模式只在显式 `--execute` 后启用，默认 dry-run 不提交订单。
- 订单状态只由 `OrderManager` 和 private feedback 推进；submit rejected 只清理 pending 状态，不伪造成交事实。
- feedback continuity lost 后暂停新开仓；REST reconcile 成功前不自动恢复。
- 每次 live smoke 后都有 REST open orders / position / pending orders 复核。
- 性能、稳定性和长期运行结论均来自测试、benchmark、profile 或 live run 证据。

## 运行记录

本节只记录已经发生的长时间运行或 smoke 证据。新增记录应包含日期、commit、命令、运行时长、账户复核摘要和异常摘要。
