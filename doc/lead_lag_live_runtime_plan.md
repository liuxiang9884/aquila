# LeadLag 长时间实盘运行与测试实现计划

> 给 agentic workers：按任务逐项实现本计划；需要使用 subagent 时，遵守 `AGENTS.md` 的 subagent 推理强度约定。步骤使用 checkbox (`- [ ]`) 跟踪状态。

**目标：** 支持 LeadLag 先做 signal-only 长时间实盘观察，再逐步完成恢复链路验证、小额真实下单和端到端性能测试。

**架构：** 独立 LeadLag live runner 复用现有 `TradingRuntime`、`RealtimeDataReader`、Gate `OrderSessionRuntimeAdapter` 和 feedback SHM；默认 validate-only / signal-only 不提交订单。Strategy 层生产订单意图、`OnOrderResponse()` / `OnOrderFeedback()` 闭环已完成；`--execute` 已接到真实 live-orders runtime，并在 `ContinuityLost` 后停止自动交易、返回应急 handoff exit code。外围 guard wrapper 负责 preflight、runner 退出监控、final REST check 和 stop-and-flat handoff。filled open / close 与 unfilled-cancel 小额真实订单 smoke 已完成；submit rejected 安全 live 探测只收到 `Ack`、未收到最终 `kRejected`，不计入已完成 smoke。当前 V1 不新增独立 account / position realtime feedback session；长期运行前优先补端到端 benchmark 和更长时间真实订单 guardrails。

**技术栈：** C++20、CMake、`core/trading/*`、`core/market_data/*`、`exchange/gate/trading/*`、`strategy/lead_lag/*`、Gate REST 辅助脚本。

---

## 当前判断

- LeadLag replay / signal 主链路已落地，`leadlag::Strategy::OnBookTicker()` 已串起 raw market、alignment、recorder、threshold、signal 和 synthetic position accounting。
- LeadLag live runner 已落地为 `tools/lead_lag/live_strategy.cpp`：默认 validate-only；`--connect-data` 进入 signal-only；`--execute` 只有在 `strategy.mode=live` 时解析为 `RunMode::kLiveOrders`。
- 生产订单闭环已在 strategy 层完成并通过测试：`SignalDecision::intent` 会转换为 IOC limit `core::OrderCreateRequest`，open / close / stoploss 订单接入 execution state，`OnOrderResponse()` 处理 rejected / cancel-rejected，`OnOrderFeedback()` 处理 terminal feedback、cancelled / partially-cancelled 和 rejected，`price_text` 使用固定 storage。
- 真实 `RunLiveOrders()` 已打开：显式 `--execute`、`strategy.mode=live`、API 凭据、feedback SHM 和 data reader 都满足后，会构造 Gate order session runtime。缺凭据时返回 exit code `2`，不会进入 runtime create。
- V1 flat-account、tiny-position emergency smoke 和隔离 `ContinuityLost` stop-and-flat smoke 已完成；`scripts/lead_lag/run_live_with_guard.py` 已提供外围 preflight / final-check / abnormal-exit flatten guard。
- 小额真实下单已完成 filled open / close 与 unfilled-cancel smoke；`lead_lag_strategy --smoke-submit-reject` 已有单元测试和诊断入口，但 ZEC_USDT 安全 live 探测没有得到最终 rejected，因此不能作为通过证据。长时间真实下单前还需要端到端 benchmark 和更长时间 guarded 运行证据；failure protocol probe 继续前要先确认 Gate 会返回最终 error 的安全请求形态。

## 文件结构

### 第一阶段：Signal-Only Live Runner

- Create: `tools/lead_lag/live_strategy.cpp`
  - LeadLag live 入口，加载 `config/strategies/lead_lag_btc_strategy.toml`。
  - 默认 dry-run / signal-only，不调用真实下单。
  - 显式 `--execute` 且 `strategy.mode=live` 后才允许进入 `RunMode::kLiveOrders`；默认 validate-only / signal-only 不提交订单。
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
- Create: `scripts/lead_lag/run_live_with_guard.py`
  - 外围运行 guard：启动前 REST preflight，正常退出后 final REST check，异常退出或 final 非 flat 时调用 emergency flatten。
- Test: `test/tools/gate/reconcile_futures_orders_test.cpp`
  - 覆盖 open order 恢复、已成交恢复、已撤单恢复、无法匹配进入 manual intervention。
- Test: `scripts/lead_lag/run_live_with_guard_test.py`
  - 覆盖 preflight 拒绝启动、正常退出 final-check、异常退出 flatten、final-check 非 flat flatten 和 flatten failure 映射。

### 第四阶段：Live Smoke Ladder

- Document: `doc/lead_lag_live_runtime_plan.md`
  - 维护以下 smoke 顺序和证据要求。
- Runbook inputs:
  - `config/strategies/lead_lag_btc_strategy.toml`
  - `config/strategies/lead_lag.toml`
  - `config/strategies/lead_lag_first5_strategy_20260521.toml`
  - `config/strategies/lead_lag_first5_20260521.toml`
  - `config/strategies/lead_lag_requested_strategy_20260521.toml`
  - `config/strategies/lead_lag_requested_20260521.toml`
  - `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml`
  - `config/strategies/lead_lag_requested_11symbols_20260522.toml`
  - `config/data_readers/strategy_data_reader.toml`
  - `config/data_readers/strategy_data_reader_first5_20260521.toml`
  - `config/data_readers/strategy_data_reader_requested_20260521.toml`
  - `config/order_feedback/gate_order_feedback_session.toml`
  - Gate / Binance data session configs
  - 2026-05-21 first5 配置对应 `PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`。
  - 2026-05-21 requested data session 配置订阅 `PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`、`DASH_USDT`、`RIVER_USDT`、`SUI_USDT`、`INJ_USDT`、`ENA_USDT`、`BRETT_USDT`。
  - 2026-05-22 requested 11-symbol LeadLag pair 配置包含上述全部 symbol。`RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 是 Gate decimal-size 合约；当前版本仍只按整数 `size` 下单，catalog 中这些 Gate 行使用 `quantity_step=1.0`、`quantity_decimal_places=0` 的 runtime 约束。
  - decimal-size 完整支持留到下一版本：不要只把下单接口改成 string 或 double，应先做定点数量类型，并覆盖 core order、feedback SHM、OrderManager、Gate encoder / parser、REST reconcile、emergency flatten 和 LeadLag sizing。
- Evidence outputs:
  - live runner summary
  - feedback session summary
  - REST open orders / position / pending orders 复核
  - signal CSV 或低频 signal summary

### 第五阶段：End-to-End Benchmark

- Create: `benchmark/strategy/lead_lag_runtime_benchmark.cpp`
  - 覆盖本地 book ticker 进入 `TradingRuntime -> LeadLag OnBookTicker -> OrderManager -> fake order session` 的 submit 路径。
- Create: `benchmark/strategy/lead_lag_feedback_runtime_benchmark.cpp`
  - 覆盖 Gate `futures.orders` parser -> in-memory SHM -> `TradingRuntime -> OrderManager -> LeadLag OnOrderFeedback()` 的回报路径。
- Modify: `benchmark/strategy/CMakeLists.txt`
- Verify:
  - benchmark 只作为本地链路证据，不把没有 live 证据的结果写成生产收益结论。

## 任务计划

### Task 1: 新增 LeadLag Signal-Only Live Runner

**文件：**
- Create: `tools/lead_lag/live_strategy.cpp`
- Modify: `tools/CMakeLists.txt`
- Test: `test/tools/lead_lag/live_strategy_test.cpp`

- [x] **Step 1: 写 CLI gating 测试**

验证默认模式不会真实下单，只有显式 `--execute` 才允许真实订单模式。

运行：

```bash
ctest --test-dir build/debug -R lead_lag_live_strategy --output-on-failure
```

实现前期望：test target 或对应 case 失败。

- [x] **Step 2: 实现 live runner 冷路径**

加载 strategy config、data reader config、order session config 和 feedback config；复用现有 `TradingRuntime` create pattern。默认 dry-run 时构造 signal-only wrapper，不调用 `StrategyContext::PlaceLimitOrder()`。

- [x] **Step 3: 接入低频 diagnostics**

输出 runtime loop counters、data reader poll counters、feedback counters、signal counters、degraded / needs_reconcile 和最后行情时间。低频输出放在 runner 层，避免在 LeadLag 热路径加日志。

- [x] **Step 4: 验证 dry-run runner**

运行：

```bash
./build.sh debug
ctest --test-dir build/debug -R '(lead_lag|trading_runtime|order_feedback)' --output-on-failure
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --duration-sec 1
```

期望：build 成功，测试通过，runner 正常退出且不提交订单。

2026-05-21 结果：

- `./build.sh debug` passed.
- `ctest --test-dir build/debug -R '(lead_lag|trading_runtime|order_feedback)' --output-on-failure` passed 18/18.
- `./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --duration-sec 1` passed in validate-only mode. In the sandbox this command needed elevated filesystem permission because the configured log sink writes under `/home/liuxiang/log`.

- [x] **Step 5: Commit**

```bash
git add tools/lead_lag/live_strategy.cpp tools/CMakeLists.txt test/tools/lead_lag/live_strategy_test.cpp
git commit -m "Add lead lag live signal runner"
```

### Task 2: Signal-Only 长时间实盘观察

**文件：**
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Runtime evidence: log files under `/home/liuxiang/log/`

- [x] **Step 1: 启动前检查**

确认 `lead_lag_btc_strategy.toml` 仍是 `mode = "dry_run"`，确认 Gate / Binance data session 和 Gate feedback session configs 指向预期 SHM。

- [x] **Step 2: 30 分钟 signal-only run**

运行：

```bash
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 1800
```

期望：strategy 按 duration 退出，不提交真实订单，diagnostics 显示 data reader 有进展且没有非预期 degraded 状态。

2026-05-21 result: passed. See “运行记录 / 2026-05-21 30 分钟 signal-only live 观察”。

- [ ] **Step 3: 2 到 4 小时 signal-only run**

运行：

```bash
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 14400
```

期望：没有需要人工 reset 的 SHM overrun，signal counters 和最后事件时间戳合理，进程干净退出。

2026-05-21 result: interrupted. 该轮运行在约 3 小时 6 分后收到 SIGTERM，未自然达到 14400 秒，不能作为 4 小时通过证据；见“运行记录 / 2026-05-21 4 小时 signal-only live 观察（中断）”。

- [x] **Step 4: 记录证据**

把运行命令、起止时间、signal summary、runtime diagnostics summary 和异常记录补到本文件的“运行记录”。2026-05-21 中断轮已记录；后续仍需重新跑一次自然完成的 2 到 4 小时 signal-only 观察。

### Task 3: 接入 LeadLag 生产订单闭环

**文件：**
- Modify: `strategy/lead_lag/strategy.h`
- Modify: `strategy/lead_lag/execution_state.h` only if existing state interface is insufficient
- Test: `test/strategy/lead_lag_strategy_interface_test.cpp`

- [x] **Step 1: 写状态机测试**

覆盖 open 下单成功、close 下单成功、submit rejected、terminal filled、partial non-terminal、continuity lost 后停止新开仓。

运行：

```bash
ctest --test-dir build/debug -R lead_lag_strategy_interface --output-on-failure
```

已完成：production order cases 已加入 `test/strategy/lead_lag_strategy_interface_test.cpp` 并完成验证。

- [x] **Step 2: 实现订单请求转换**

把 `SignalDecision::intent` 转成 `core::OrderCreateRequest`，使用 lag side metadata 进行 symbol、side、quantity、price、time-in-force、reduce-only 设置。数量来自 `open_notional` 和 lag quote，价格必须符合 instrument tick 约束。

- [x] **Step 3: 实现 response / feedback hook**

`OnOrderResponse()` 只处理 submit rejected / cancel rejected 等上行响应；position 和 stage 只由 terminal private feedback 推进。`OnOrderFeedback()` 在 runtime 先更新 `OrderManager` 之后读取 `context.FindOrder(event.local_order_id)`，再调用 `ExecutionState::ApplyTerminalOrder()`。

- [x] **Step 4: 验证状态机**

运行：

```bash
./build.sh debug
ctest --test-dir build/debug -R lead_lag --output-on-failure
```

2026-05-21 状态：strategy-layer order wiring 已完成，并由 `lead_lag_strategy_interface` tests 覆盖。后续提交也覆盖了 IOC terminal handling、cancelled / partially-cancelled、rejected feedback edge cases 和 fixed-buffer `price_text` 边界。

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

**文件：**
- Modify: `test/tools/lead_lag/live_strategy_test.cpp`
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Modify: `strategy/lead_lag/README.md`
- Optional summary: `doc/project_onboarding_guide.md`

- [x] **Step 1: 覆盖 execute=false / live / connect-data 边界**

`execute=false` 时即使 `strategy.mode=live` 且 `connect_data=true`，`ResolveRunMode()` 仍必须返回 `RunMode::kSignalOnly`，不进入 live orders。

- [x] **Step 2: 覆盖 execute=true 优先级**

`execute=true`、`strategy.mode=live`、`connect_data=true` 时解析为 `RunMode::kLiveOrders`。该任务只覆盖当时的 gating 选择；真实 live-orders runtime 后续在 Task4 接入。

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

Commit boundary: this historical task only added gating/smoke tests and documentation;真实 live order path 后续在 Task4 单独打开。

### Task 3.6: Review Findings 后续处理

**文件：**
- Create: `test/tools/lead_lag/lead_lag_live_orders_strategy.toml`
- Create: `test/tools/lead_lag/lead_lag_live_orders_missing_credentials_smoke.cmake`
- Modify: `test/tools/lead_lag/CMakeLists.txt`
- Modify: `tools/lead_lag/live_strategy.cpp`
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Modify: `doc/project_onboarding_guide.md`

- [x] **Step 1: 覆盖当时实际 runner 禁用路径**

历史状态：新增 CTest wrapper 实际运行 `lead_lag_strategy --execute --duration-sec 1`，断言 exit code 为 3，输出包含禁用错误，并避免进入 signal-only / runtime create / credentials 错误路径。Task4 打开 live-orders runtime 后，该 smoke 已替换为 missing-credentials 冷路径检查。

- [x] **Step 2: 修正禁用文案**

`RunLiveOrders()` 文案说明当前剩余 blocker 是 REST reconcile、feedback recovery 和 live smoke guardrails，不再写 strategy 层 production order wiring。

- [x] **Step 3: 修正后续任务顺序**

REST reconcile / feedback 断线恢复必须排在小额真实下单 smoke 前；Live Smoke 顺序同步按该边界排列。

### Task 3.7: REST Reconcile / Feedback Recovery Design Handoff

**文件：**
- Create: `doc/lead_lag_reconcile_design.md`
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Modify: `doc/project_onboarding_guide.md`
- Modify: `strategy/lead_lag/README.md`

- [x] **Step 1: 写 read-only reconcile / recovery 设计文档**

设计文档见 `doc/lead_lag_reconcile_design.md`。该文档固定第一版边界：先做 read-only
reconcile，不自动补单、不自动平仓，恢复成功前暂停新开仓，事实无法确认时进入
`ManualIntervention`。

- [x] **Step 2: 同步计划索引**

Task4 Step1 指向新设计文档；后续实现步骤仍保持未完成。小额真实下单 smoke 继续排在
REST reconcile / feedback recovery 之后。

### Task D: Runner Recovery Diagnostics

**文件：**
- Modify: `tools/lead_lag/live_strategy.h`
- Modify: `tools/lead_lag/live_strategy.cpp`
- Modify: `test/tools/lead_lag/live_strategy_test.cpp`
- Modify: `test/tools/lead_lag/lead_lag_live_orders_missing_credentials_smoke.cmake`

- [x] **Step 1: 保持当时 live orders 禁用边界**

历史状态：`lead_lag_strategy --execute` 解析到 `RunMode::kLiveOrders` 后立即走 `RunLiveOrders()` 禁用 stub，返回 exit 3。Task4 打开 live-orders runtime 后，该检查改为缺凭据时 exit code `2` 且不进入 runtime create。

- [x] **Step 2: 接入 signal-only recovery diagnostics summary**

signal-only runner 的最终 `lead_lag_strategy_signal_only_summary` 增加稳定字段：
`recovery_state`、`needs_reconcile`、`manual_intervention`、`new_entries_paused`。这些字段来自
`SignalOnlyStrategy` 内部 `leadlag::Strategy` 的 recovery API 快照；默认 signal-only 状态为
`normal/false/false/false`。runner 只在最终 summary 输出，不在行情热路径新增日志。

- [x] **Step 3: 验证**

2026-05-21 result:

```bash
cmake --build build/debug --target lead_lag_strategy lead_lag_live_strategy_test -j8
ctest --test-dir build/debug -R lead_lag_live --output-on-failure
ctest --test-dir build/debug -R '(lead_lag_live|lead_lag_strategy_interface)' --output-on-failure
git diff --check
```

Build passed；focused ctest 分别通过 2/2 和 3/3；`git diff --check` passed。

### Task 4: ContinuityLost Stop-and-Flat 应急链路

**文件：**
- Modify: `doc/lead_lag_reconcile_design.md`
- Create: `scripts/gate/emergency_flatten_futures.py`
- Modify: `scripts/gate/query_gate_account.py`
- Modify: `tools/lead_lag/live_strategy.cpp`
- Test: `scripts/gate/emergency_flatten_futures_test.py`
- Test: `test/tools/lead_lag/live_strategy_test.cpp`

边界：`lead_lag_strategy --execute` 已在 Task4 中打开真实 live-orders runtime，但仍必须显式 opt-in；未完成 flat-account / tiny-position / continuity-lost smoke 前，不把它视为可长期无人值守运行。

- [x] **Step 1: 写 stop-and-flat 应急设计**

已完成：`doc/lead_lag_reconcile_design.md` 已把当前 V1 固定为：收到 `ContinuityLost` 后停止自动交易，用 Python REST API 查询 in-scope position，先撤 open orders，再用 reduce-only market close 平掉非零仓位，最后用 REST 复核 positions flat 且 open orders 为空。V2 read-only reconcile / resume 只作为后续优化保留。

- [x] **Step 2: 实现 Python emergency flatten helper**

实现 `scripts/gate/emergency_flatten_futures.py`，支持 dedicated-account scope、contract allowlist、dry-run、撤单、reduce-only market close、REST 轮询复核和结构化 summary。

- [x] **Step 3: 接入 live runner stop handoff**

真实 live orders 模式下，一旦 strategy 收到 `ContinuityLost`，runner 停止自动交易并返回专用 exit code `10`；signal-only 模式只记录 diagnostics，不提交订单。2026-05-22 已完成代码接入、missing-credentials smoke 和隔离 continuity-lost stop-and-flat smoke。

- [x] **Step 4: 验证 flat account emergency smoke**

期望：无仓位时 emergency helper 不提交不必要订单；REST 复核 open orders 为空、position size 为 0。

- [x] **Step 5: 验证 tiny position emergency smoke**

期望：有最小受控仓位时，helper 提交 reduce-only market close；REST 复核 open orders 为空、position size 为 0。

### Task 5: 小额真实下单 Smoke

**文件：**
- Modify: `doc/lead_lag_live_runtime_plan.md`
- Config: `config/order_feedback/gate_order_feedback_session_live_smoke_20260522.toml`
- Runtime evidence: Gate REST query output and live runner output

前置条件：Task4 的 emergency helper、`ContinuityLost` stop handoff 和 smoke 完成后，才允许开始小额真实订单 smoke。

2026-05-22 ZEC_USDT `$10` guarded live smoke 记录：第一次启动时，strategy 读到旧 feedback SHM lane 中残留的 global `ContinuityLost`，立即返回 handoff exit code `10`，外围 guard 执行 emergency flatten，REST 复核 ZEC flat 且 open orders 为空。随后新增 live-smoke 专用 feedback session 配置，使用 `remove_existing=true` 在启动前重建 feedback SHM；第二次运行 600 秒正常退出，`book_tickers=124058`、`order_responses=0`、`order_feedbacks=0`、`recovery_state=normal`、`needs_reconcile=false`，最终 REST check flat。该记录只说明 runner / guard / fresh feedback SHM 可以无残留运行；因为窗口内没有产生订单，不计入 filled open / close smoke 完成项。

- [x] **Step 1: Filled open / close smoke**

用极小 size 或最小合约张数跑一轮 open / close，要求真实执行前显式传入 `--execute`。

期望：feedback 至少包含 open terminal 和 close terminal；REST 复核 open orders 为空、position size 为 0。

2026-05-22 已完成：新增 `lead_lag_strategy --smoke-open-close` 显式模式，只在 `--execute` 且 `strategy.mode=live` 时进入，仍复用 `TradingRuntime`、Gate `OrderSessionRuntimeAdapter`、`OrderManager`、feedback SHM 和外围 `run_live_with_guard.py`。本轮使用 ZEC-only Gate data session / data reader、fresh feedback SHM 和 `ZEC_USDT` 目标 notional `$10`；受整数张数约束，实际 open quantity 为 1 张，估算 notional `6.5678`。运行结果：`completed=true`、`open_quantity=1`、`close_quantity=1`、`order_responses=2`、`order_feedbacks=2`；feedback session 发布 open / close 两个 `kFilled` taker event，fill price 分别为 `656.78` 和 `656.77`。最终 REST 复核 open orders 为空、position `size=0`、`pending_orders=0`。

- [x] **Step 2: Unfilled cancel smoke**

使用不可立即成交价格触发挂单，然后走 cancel 分支。

期望：feedback terminal cancelled；REST 复核无残留 open orders。

2026-05-22 已完成：新增 `lead_lag_strategy --smoke-unfilled-cancel` 显式模式，只在 `--execute` 且 `strategy.mode=live` 时进入，仍复用 `TradingRuntime`、Gate `OrderSessionRuntimeAdapter`、`OrderManager`、feedback SHM 和外围 `run_live_with_guard.py`。本轮使用 ZEC-only Gate data session / data reader、fresh feedback SHM 和 `ZEC_USDT` 目标 notional `$10`；passive buy 价格使用 Gate best bid 下方 `200 bps`，GTC limit，非 reduce-only。运行结果：`completed=true`、`state=done`、`open_quantity=1`、`order_responses=1`、`order_feedbacks=2`、`cancel_requested=true`、`estimated_open_notional=6.3854`；feedback session 发布 `kAccepted` 和 `kCancelled`，cancelled event 为 `cumulative_filled_quantity=0`、`left_quantity=1`、`cancelled_quantity=1`。最终 REST 复核 open orders 为空、position `size=0`、`pending_orders=0`。

- [ ] **Step 3: Rejected / cancel-rejected protocol probe**

用受控非法价格、数量或过期订单构造 rejected 场景。

期望：LeadLag execution group 不残留 pending order；strategy 不继续盲目开仓。

2026-05-22 状态：新增 `lead_lag_strategy --smoke-submit-reject` 诊断模式并通过单元测试，覆盖 `Ack` 不完成、`kRejected` 才完成、unexpected accepted / cancel response / private feedback 失败和 `ContinuityLost` handoff。随后做了两次 ZEC_USDT 安全 live 探测：

- LeadLag guarded smoke：`buy limit IOC`、`price=0.01`、`quantity=1`、`reduce_only=true`，120 秒内只收到 1 个 `kAck`，未收到最终 `kRejected`；外围 guard 执行 final check / flatten，REST 复核 open orders 为空、position `size=0`。
- `gate_strategy_order` 单变量协议探测：同样 `buy limit IOC`、`price=0.01`、`quantity=1`，但 `reduce_only=false`，25 秒内仍只收到 1 个 `kAck`，未收到最终 response；REST 复核 open orders 为空、ZEC_USDT position `size=0` / `pending_orders=0`。

结论：这个安全 invalid-price IOC 场景在 Gate WS 下表现为请求 `Ack` 后无最终 rejected response，不适合作为 LeadLag submit-rejected live smoke。不要继续用随机非法价格或数量硬凑 smoke；若需要 rejected / cancel-rejected 实盘证据，应单独做 `OrderSession` protocol probe，明确协议字段、风险边界和 REST 复核。`cancel-rejected` 不应强塞进 LeadLag runtime：LeadLag 只取消已知订单，未知订单、重复取消或 terminal 订单取消在当前系统里属于本地拒绝，不是交易所 cancel-rejected。

2026-05-22 后续状态：新增独立 `gate_order_session_failure_probe`，只直接使用 Gate `OrderSession`，不经过 `OrderManager`、`TradingRuntime` 或 LeadLag。它支持 `--probe cancel-rejected` 和 `--probe submit-rejected`，默认 dry-run，只有显式 `--execute` 才连接 WebSocket；`Ack` 不算成功，必须收到匹配请求的最终 `kRejected` / `kCancelRejected`。

已做的安全 live 探测仍未拿到最终 failure response：

- `cancel-rejected`，`order_id=9000000000000000000`：cancel request send `kOk`，20 秒内 `responses=0`，timeout；REST 复核 BTC_USDT open orders 为空、position `size=0`。
- `cancel-rejected`，fallback text `order_id=t-999999`：cancel request send `kOk`，20 秒内 `responses=0`，timeout；REST 复核 BTC_USDT open orders 为空、position `size=0`。
- `submit-rejected`，`BTC_USDT buy limit IOC size=0 price=0.01`：只收到 `kAck`，20 秒内无最终 `kRejected`，timeout；REST 复核 BTC_USDT open orders 为空、position `size=0`。

结论：当前可以保留独立 probe 作为后续协议诊断入口，但 rejected / cancel-rejected live 证据仍未完成。后续优先级应转向端到端 benchmark 和真实订单长跑 guardrails；如继续 failure response，需要先基于 Gate 官方协议或更低风险 sandbox / dedicated account 明确可返回最终 error 的请求形态。

V1 不新增独立 `AccountPositionFeedbackSession`：运行中策略持仓由订单回报推导，停机、异常或 `ContinuityLost` 后由外围 guard 做 REST final check / emergency flatten；account / position realtime feedback 作为 V2 风控状态能力保留。

- [x] **Step 4: Commit smoke evidence**

只提交整理后的文档证据，不提交密钥、原始敏感日志或本地临时配置。

```bash
git add doc/lead_lag_live_runtime_plan.md
git commit -m "Document lead lag live smoke evidence"
```

### Task 6: 长时间真实订单运行

**文件：**
- Modify: `doc/lead_lag_live_runtime_plan.md`

- [ ] **Step 1: 30 分钟真实订单 run**

期望：所有订单生命周期都有 feedback；结束后 REST 复核 open orders 为空或符合预期持仓。

- [ ] **Step 2: 2 到 4 小时真实订单 run**

期望：无未知 pending order、无未解释 position drift、无 feedback continuity lost 未恢复状态。

- [ ] **Step 3: 更长时间 run**

开始前固定最大仓位、最大订单频率、最大连续错误次数和 kill switch 操作方式。结束后保存 summary、REST 复核和异常清单。

### Task 7: 端到端 Benchmark

**文件：**
- Create: `benchmark/strategy/lead_lag_runtime_benchmark.cpp`
- Create: `benchmark/strategy/lead_lag_feedback_runtime_benchmark.cpp`
- Modify: `benchmark/strategy/CMakeLists.txt`

- [x] **Step 1: 写本地 runtime benchmark**

覆盖本地行情触发进入 `TradingRuntime -> LeadLag OnBookTicker -> OrderManager -> fake order session` 的 submit 路径；不访问外网。benchmark 使用确定性的三条 book ticker 触发 open-long IOC limit order，计时范围只包含触发 ticker 进入 runtime 到 fake session 收到订单。

- [x] **Step 2: 写 feedback benchmark**

覆盖固定 terminal feedback event 进入 in-memory feedback SHM、reader poll、`TradingRuntime::HandleOrderFeedbackForTest()`、`OrderManager::OnOrderFeedback()` 和 LeadLag `OnOrderFeedback()`；不访问外网。fixture setup 使用 `PauseTiming()`，release 结果只解释本地路径，不解释真实网络或交易所延迟。

- [x] **Step 3: 运行 release benchmark**

运行：

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target lead_lag_runtime_benchmark lead_lag_feedback_runtime_benchmark -- -j8
./build/release/benchmark/strategy/lead_lag_runtime_benchmark --benchmark_min_time=0.01s
./build/release/benchmark/strategy/lead_lag_feedback_runtime_benchmark --benchmark_min_time=0.01s
```

2026-05-22 结果：

- `lead_lag_runtime_benchmark`：4096 samples，p50 `1.019 us`、p99 `1.181 us`、p999 `12.729 us`、max `16.205 us`。
- `lead_lag_feedback_runtime_benchmark`：4096 samples，p50 `635 ns`、p99 `780 ns`、p999 `1.212 us`、max `6.071 us`。
- 额外验证：`cmake --build build/debug --target lead_lag_runtime_benchmark lead_lag_feedback_runtime_benchmark -- -j8` passed；`ctest --test-dir build/debug -R lead_lag --output-on-failure` passed 10/10；两个 debug benchmark smoke 均通过。

边界：benchmark 正常完成；这些数字只作为本地链路证据，不写成真实交易环境收益结论。

## Live Smoke 顺序

1. `lead_lag_strategy` dry-run 1 秒启动验证。
2. signal-only 30 分钟观察。
3. signal-only 2 到 4 小时观察。
4. `lead_lag_strategy --execute` missing-credentials smoke：允许解析到 `RunMode::kLiveOrders`，但缺凭据时必须在 runtime create 前返回。已完成。
5. Python emergency flatten flat-account smoke。已完成。
6. tiny position emergency flatten smoke。已完成。
7. feedback session 断线 / `ContinuityLost` stop-and-flat smoke。已完成。
8. 小额 filled open / close。已完成。
9. unfilled-cancel。已完成。
10. 端到端本地 benchmark。已完成。
11. rejected / cancel-rejected。
12. 30 分钟真实订单 run。
13. 2 到 4 小时真实订单 run。
14. 更长时间真实订单 run。

## 完成标准

- signal-only live runner 可以长时间消费 Gate / Binance realtime SHM，并低频输出可解释 diagnostics。
- `lead_lag_strategy --execute` 是显式 opt-in 真实 live-orders 入口；缺凭据时必须在 runtime create 前失败，收到 `ContinuityLost` 时必须停止并返回应急 handoff exit code。
- LeadLag strategy 层 default production accounting 可以提交 IOC limit order intent；runner 默认 validate-only / signal-only 不提交订单。
- 订单状态只由 `OrderManager` 和 private feedback 推进；submit rejected 只清理 pending 状态，不伪造成交事实。
- feedback continuity lost 后停止自动交易，并通过 Python REST helper 撤销 in-scope open orders、reduce-only 市价平仓、REST 复核 flat；V1 不自动恢复交易。
- 每次 live smoke 后都有 REST open orders / position / pending orders 复核。
- 性能、稳定性和长期运行结论均来自测试、benchmark、profile 或 live run 证据。

## 运行记录

本节只记录已经发生的长时间运行或 smoke 证据。新增记录应包含日期、commit、命令、运行时长、账户复核摘要和异常摘要。

### 2026-05-22 V1 emergency flatten / ContinuityLost smoke

- Commit under test: `45fcf96 Stop lead lag live runner on continuity loss`.
- Scope: `allowlist` / `BTC_USDT`; 未使用 dedicated-account 全账户平仓。
- Flat-account smoke:
  - Initial REST: BTC_USDT open orders 为空，position `size=0` / `pending_orders=0`。
  - Dry-run: `result=dry_run`，`positions_to_close=[]`，无撤单计划。
  - Live helper: `result=verified_flat`，`close_orders_submitted=[]`，final position `size=0` / `pending_orders=0`，final open orders 为空。
- Tiny-position smoke:
  - Setup: REST IOC market buy BTC_USDT `size=1`，成交价约 `77381.7`，随后 REST position `size=1` / `pending_orders=0`，open orders 为空。
  - Emergency helper: 提交 reduce-only IOC market close，关键字段 `size=-1`、`price=0`、`tif=ioc`、`reduce_only=true`，成交价约 `77412.3`。
  - Final REST: BTC_USDT position `size=0` / `pending_orders=0`，open orders 为空。
- ContinuityLost stop-and-flat smoke:
  - 使用 `/tmp/aquila_v1_continuity_20260522_011533` 隔离 market-data SHM 和 feedback SHM；data sessions 只创建空 SHM，不连接行情 websocket。
  - `gate_order_feedback_session --duration-sec 3 --connect` 发布 `global_continuity_lost_events_published=1`、`shm_published=8`。
  - `lead_lag_strategy --execute --duration-sec 10` 返回 exit code `10`；summary: `runtime_exit_code=0`、`emergency_handoff=true`、`book_tickers=0`、`order_responses=0`、`order_feedbacks=1`、`recovery_state=degraded_needs_reconcile`、`needs_reconcile=true`、`new_entries_paused=true`、`data_reader_events=0`。
  - Handoff 后执行 emergency helper，`result=verified_flat`；REST 复核 BTC_USDT open orders 为空，position `size=0` / `pending_orders=0`。
- 边界：该 smoke 证明 V1 stop-and-flat handoff 和 emergency helper 可工作；它不是长时间真实订单运行证据。

### 2026-05-21 30 分钟 signal-only live 观察

- Commit: `92ed8a2 Add lead lag live signal runner`
- Window: 2026-05-21 04:02:29 UTC 到 2026-05-21 04:32 左右 UTC；producer 在 04:34:51 UTC 手动 TERM 后正常收尾。
- Mode: `lead_lag_strategy run_mode=signal_only execute=false connect_data=true`; no live orders were enabled.
- 命令：

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

### 2026-05-21 4 小时 signal-only live 观察（中断）

- Commit: `39cc962 Document lead lag signal-only smoke`
- Window: started at 2026-05-21 04:37:38 UTC; expected natural LeadLag exit around 2026-05-21 08:37:38 UTC.
- Checkpoint: at 2026-05-21 07:39:40 UTC the four processes were still running:
  - `1231230 ./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect --duration-sec 14500`
  - `1231262 ./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml --connect`
  - `1231285 ./build/debug/tools/binance_data_session --config config/data_sessions/binance_data_session.toml --connect`
  - `1231306 ./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 14400`
- Mode: `lead_lag_strategy run_mode=signal_only execute=false connect_data=true`; no live orders are enabled.
- 命令：

```bash
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --connect --duration-sec 14500
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml --connect
./build/debug/tools/binance_data_session --config config/data_sessions/binance_data_session.toml --connect
./build/debug/tools/lead_lag_strategy --config config/strategies/lead_lag_btc_strategy.toml --connect-data --duration-sec 14400
```

- 当前观察：Gate / Binance data sessions 和 LeadLag signal-only process 在 3 小时 checkpoint 仍存活。Gate feedback session 在 2026-05-21 06:54:17 UTC 发布了一个 `kSessionDisconnected` continuity lost。由于 runner 不读取 feedback 且不提交订单，这不影响 signal-only 观察；但真实订单 smoke 前仍需处理该 blocker / risk。
- Final result: interrupted at 2026-05-21 07:44:04 UTC by `SIGTERM`, about 3h06m after the LeadLag process started. This run is not counted as a completed 4-hour signal-only observation.
- LeadLag log summary: `/home/liuxiang/log/lead_lag_strategy_20260521_043738.log` contains startup instrument catalog load and `Received signal: Terminated`; no normal duration-reached LeadLag summary was available in the log.
- Gate data session summary: `/home/liuxiang/log/gate_data_session_20260521_043727.log` recorded `result=ok active=true phase=kClosed error=kNone book_tickers=1281236 rx_messages=639501 tx_messages=2242`.
- Binance data session summary: `/home/liuxiang/log/binance_data_session_20260521_043733.log` recorded `result=ok active=true phase=kClosed error=kNone book_tickers=14775719 rx_messages=5228333 tx_messages=2300`.
- Gate feedback summary: `/home/liuxiang/log/gate_order_feedback_session_20260521_043720.log` recorded one `feedback_global_continuity_lost` at 2026-05-21 06:54:17 UTC and then `Received signal: Terminated` at 2026-05-21 07:44:04 UTC. No feedback order events were expected for signal-only mode.
- REST account check at 2026-05-21 09:45 UTC: `orders --contract BTC_USDT --status open` returned `[]`; `positions --contract BTC_USDT` returned `size=0` and `pending_orders=0`; `account --currency USDT --contract BTC_USDT` returned `order_margin=0`.
- 后续：重跑一次干净的 2 到 4 小时 signal-only 观察，并让 runner / launch wrapper 保留 stdout summary 和显式 exit code，这样中断运行不需要只依赖 log sink 记录来判断。
