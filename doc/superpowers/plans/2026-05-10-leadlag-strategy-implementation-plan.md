# LeadLag Strategy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 fixed Go 语义和 Aquila 低延迟链路实现 `leadlag::Strategy`。

**Architecture:** 策略按 config / raw market state / recorder / alignment / threshold / signal / feedback state 七层拆开。启动期解析 TOML、instrument catalog 和交易所差异，热路径只读 numeric config、`symbol_id`、`Exchange` enum 和预分配 state。

**Tech Stack:** C++20、CMake、toml++、GTest、`core/base` 底层结构、`core/strategy::StrategyRuntime`。

---

## 模块边界

| 模块 | 目标文件 | 责任 |
| --- | --- | --- |
| Config / metadata | `strategy/lead_lag/config.h`、`strategy/lead_lag/config.cpp` | 解析 `[lead_lag]`、pair、trigger、execute、bbo_record、capacity、quantile range/precision，并用 instrument catalog 压缩 lag metadata。 |
| Raw market state | `strategy/lead_lag/types.h`、`strategy/lead_lag/raw_market_state.h` | `symbol_id` slot 路由、lead/lag raw quote、same-price、previous quote 和 Active seed 输入语义。 |
| Recorder / stats | `strategy/lead_lag/window_stats.h`、`strategy/lead_lag/recorders.h` | BBO extrema、noise、spread、move quantile wrapper，复用 `MonotonicDeque` / `RingQueue` / `HistogramQuantile`。 |
| Alignment | `strategy/lead_lag/alignment.h` | drift rolling mean/std、Bootstrap/Aligning/Active、Active transition seed 和 resume lead。 |
| Threshold | `strategy/lead_lag/threshold.h` | fixed Go roll 顺序、histogram move quantile、entry/exit threshold snapshot。 |
| Signal / execution | `strategy/lead_lag/cost_model.h`、`strategy/lead_lag/execution_state.h`、`strategy/lead_lag/signal.h` | open/close/stoploss gate、execution group、IOC limit order intent。 |
| Strategy integration | `strategy/lead_lag/strategy.h` | 串接 runtime hooks：`OnBookTicker()`、`OnOrderResponse()`、`OnOrderFeedback()`、`ShouldStop()`。 |

## Tasks

### Task 1: Config / Metadata

**Files:**
- Create: `strategy/lead_lag/config.h`
- Create: `strategy/lead_lag/config.cpp`
- Modify: `strategy/CMakeLists.txt`
- Modify: `config/strategy/lead_lag.toml`
- Test: `test/strategy/lead_lag_config_test.cpp`

- [x] 写失败测试：checked-in TOML 能解析，catalog metadata 能压缩成 lag runtime metadata。
- [x] 实现 `Config` / `PairConfig` / `TriggerConfig` / `ExecuteConfig` / `BboRecordConfig` / `CapacityConfig`。
- [x] 实现 `ParseConfig()` / `LoadConfigFile()`。
- [x] 校验 `symbol` / `symbol_id` 与 lead / lag 两边 catalog 一致，拒绝重复 `symbol_id`。
- [x] 校验 `parallel > 0`、`drift_min_samples > 0`、`stats_window >= window`、lead / lag exchange 不相同。
- [x] 解析 quantile `up_min` / `up_max` / `down_min` / `down_max` / `precision` 并计算 bins。

### Task 2: Raw Market State

**Files:**
- Create: `strategy/lead_lag/types.h`
- Create: `strategy/lead_lag/raw_market_state.h`
- Test: `test/strategy/lead_lag_raw_market_state_test.cpp`

- [ ] 写测试覆盖 lead / lag role 路由、超出 `symbol_id`、slot 未初始化、非 pair exchange 直接返回。
- [ ] 实现 `QuoteSnapshot`、`MarketSideState`、`PairMarketState` 和 `MarketSideUpdate`。
- [ ] 覆盖 same-price tick 不替换 latest / previous quote，但返回 `price_changed=false`。
- [ ] 覆盖变价 tick 更新 `previous_quote` 和 `latest_quote`。
- [ ] 暴露 Active seed 所需 previous/latest quote 选择 helper。

### Task 3: Recorder Wrappers

**Files:**
- Create: `strategy/lead_lag/window_stats.h`
- Create: `strategy/lead_lag/recorders.h`
- Test: `test/strategy/lead_lag_recorders_test.cpp`

- [ ] 写 `MeanWindow` / `MeanStdWindow` 测试，覆盖时间淘汰、mean/std、capacity grow 后结果不变。
- [ ] 实现 `BboExtremaWindow`，输入 drifted lead / raw lag quote，输出 rolling bid/ask min/max。
- [ ] 实现 `NoiseState` 和 `SpreadState`。
- [ ] 实现 `MoveQuantileWindow` wrapper，使用两个 `HistogramQuantile<double>`。

### Task 4: Drift / Alignment

**Files:**
- Create: `strategy/lead_lag/alignment.h`
- Test: `test/strategy/lead_lag_alignment_test.cpp`

- [ ] 写 paired drift、first timestamp、warmup、phase transition 和 seed 测试。
- [ ] 实现 `AlignmentState`、`AlignmentConfig`、`AlignmentSnapshot`、`ActiveTransition`。
- [ ] 覆盖 `drift_warmup=0` fallback 到 `stats_window`。
- [ ] 覆盖 lag tick 触发 Active 后下一笔 lead same-price resume。

### Task 5: Threshold Engine

**Files:**
- Create: `strategy/lead_lag/threshold.h`
- Test: `test/strategy/lead_lag_threshold_test.cpp`

- [ ] 写 roll 边界测试：`now > roll_at` 才 roll，当前 lead tick move 进入新窗口。
- [ ] 实现 `ThresholdState` 和 `ThresholdSnapshot`。
- [ ] 覆盖 up 使用 `quantile.move` + upper edge，down 使用 `1 - quantile.move` + lower edge。
- [ ] 覆盖 `fee*2 + LeadNoise + LagNoise` profit buffer。

### Task 6: Signal / Execution State

**Files:**
- Create: `strategy/lead_lag/cost_model.h`
- Create: `strategy/lead_lag/execution_state.h`
- Create: `strategy/lead_lag/signal.h`
- Test: `test/strategy/lead_lag_signal_test.cpp`

- [ ] 写 open long / open short 每个 reject gate 测试。
- [ ] 实现 `EntryCostBreakdown`，确保 `RequiredEdge()` 不重复加入 spread / lag spread buffer。
- [ ] 实现 `ExecutionGroup` / `ExecutionState`。
- [ ] 实现 lead close 先于 open、lag stoploss 先于 signal close。

### Task 7: Feedback State / Order Retire

**Files:**
- Modify: `core/strategy/order_manager.h`
- Create/modify: `strategy/lead_lag/execution_state.h`
- Test: `test/strategy/lead_lag_feedback_state_test.cpp`

- [ ] 给 `OrderManager` 增加 `RetireFinishedOrder(local_order_id)`。
- [ ] 覆盖 open terminal feedback 到 Hold / delete cache。
- [ ] 覆盖 close terminal feedback 到 Hold / delete cache。
- [ ] 覆盖 submit rejected 和 feedback gap 后暂停新开仓。

### Task 8: Strategy Integration

**Files:**
- Create: `strategy/lead_lag/strategy.h`
- Modify/create: Gate LeadLag runtime tool after dry-run boundary明确
- Test: `test/strategy/lead_lag_strategy_test.cpp`

- [ ] 串接 `OnBookTicker()` 主链路。
- [ ] 串接 `OnOrderResponse()` / `OnOrderFeedback()`。
- [ ] 增加 dry-run config load test。
- [ ] 增加 fixed Go replay 对账入口计划。
