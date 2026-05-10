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

## User Strategy 接口

`leadlag::Strategy` 第一版对齐 `StrategyRuntime` 的 user strategy hook 形状，构造函数只接收 `leadlag::Config`。runtime 层已经消费 `StrategyConfig`、`DataReaderConfig`、order capacity、strategy id、feedback SHM 和 loop policy，LeadLag 策略运行期不保存这些冷路径配置。

固定接口：

```cpp
explicit Strategy(leadlag::Config config);

template <typename ContextT>
void OnBookTicker(const BookTicker& ticker, ContextT& context) noexcept;

template <typename ContextT>
void OnOrderResponse(const strategy::OrderResponseEvent& event,
                     ContextT& context) noexcept;

template <typename ContextT>
void OnOrderFeedback(const OrderFeedbackEvent& event, ContextT& context) noexcept;

[[nodiscard]] bool ShouldStop() const noexcept;
```

第一版 `OnBookTicker()` 是主链路入口；`OnOrderFeedback()` 处理 gap、terminal feedback、position / stage 和 order retire；`OnOrderResponse()` 只处理 submit rejected 这类 fast failure。`OnStart()` / `OnStop()` / `OnLoop()` / `OnIdle()` 不作为固定必需接口；如需诊断或测试退出，可后续加空实现或显式状态。

## 计算边界

| 部分 | 主要计算 |
| --- | --- |
| 1. Config / metadata | TOML 数值解析，duration 转 ns，quantile bins = `ceil((max - min) / precision)`，catalog 一致性校验，`max_entry_spread < 0` 时 fallback 到 `trailing_stop`。 |
| 2. Raw market state | `symbol_id` vector lookup，`BookTicker.exchange` role 判断，bid/ask 是否变价比较，latest / previous quote 搬迁，Active seed 选择和 `resume_lead_tick` 标记。 |
| 3. Recorder / stats | rolling bid/ask min/max，mid rolling mean/std，normalized std rolling mean，lag spread rolling mean，move sample 计算和 histogram quantile window。 |
| 4. Drift / alignment | `drift = (lag_ask + lag_bid) / (lead_ask + lead_bid)`，drift rolling mean/std，drift std rolling mean，warmup / sample count readiness，drifted lead quote。 |
| 5. Threshold | `up_move = lead_bid / bid_min - 1`，`down_move = lead_ask / ask_max - 1`，up/down quantile 查询，`profitBuffer = fee*2 + LeadNoise + LagNoise`，entry/exit threshold 更新。 |
| 6. Signal / execution | open/close/stoploss gate、lead / lag price diff、move space / lag_part、required edge、lag spread gate、trailing high/low 和 open/close quantity/price 归整。 |
| 7. Feedback state | terminal feedback 的 cumulative filled quantity 应用，signed position 更新，Open/Hold/Close stage 转移，trailing 初始化，finished order retire。 |

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

- [x] 写测试覆盖 lead / lag role 路由、超出 `symbol_id`、slot 未初始化、非 pair exchange 直接返回。
- [x] 实现 `QuoteSnapshot`、`MarketSideState`、`PairMarketState` 和 `MarketSideUpdate`。
- [x] 覆盖 same-price tick 不替换 latest / previous quote，但返回 `price_changed=false`。
- [x] 覆盖变价 tick 更新 `previous_quote` 和 `latest_quote`。
- [x] 暴露 Active seed 所需 previous/latest quote 选择 helper。

### Task 3: Recorder Wrappers

**Files:**
- Create: `strategy/lead_lag/window_stats.h`
- Create: `strategy/lead_lag/recorders.h`
- Test: `test/strategy/lead_lag_recorders_test.cpp`

- [x] 写 `MeanWindow` / `MeanStdWindow` 测试，覆盖时间淘汰、mean/std、capacity grow 后结果不变。
- [x] 实现 `BboExtremaWindow`，输入 drifted lead / raw lag quote，输出 rolling bid/ask min/max。
- [x] 实现 `NoiseState` 和 `SpreadState`。
- [x] 实现 `MoveQuantileWindow` wrapper，使用两个 `HistogramQuantile<double>`。

### Task 4: Drift / Alignment

**Files:**
- Create: `strategy/lead_lag/alignment.h`
- Test: `test/strategy/lead_lag_alignment_test.cpp`

- [x] 写 paired drift、first timestamp、warmup、phase transition 和 seed 测试。
- [x] 实现 `AlignmentState`、`AlignmentConfig`、`AlignmentSnapshot`、`ActiveTransition`。
- [x] 覆盖 `drift_warmup=0` fallback 到 `stats_window`。
- [x] 覆盖 lag tick 触发 Active 后下一笔 lead same-price resume。

### Task 5: Threshold Engine

**Files:**
- Create: `strategy/lead_lag/threshold.h`
- Test: `test/strategy/lead_lag_threshold_test.cpp`

- [x] 写 roll 边界测试：`now > roll_at` 才 roll，当前 lead tick move 进入新窗口。
- [x] 实现 `ThresholdState` 和 `ThresholdSnapshot`。
- [x] 覆盖 up 使用 `quantile.move` + upper edge，down 使用 `1 - quantile.move` + lower edge。
- [x] 覆盖 `fee*2 + LeadNoise + LagNoise` profit buffer。

### Task 6: Signal / Execution State

**Files:**
- Create: `strategy/lead_lag/cost_model.h`
- Create: `strategy/lead_lag/execution_state.h`
- Create: `strategy/lead_lag/signal.h`
- Test: `test/strategy/lead_lag_signal_test.cpp`

- [x] 写 open long / open short 每个 reject gate 测试。
- [x] 实现 `EntryCostBreakdown`，确保 `RequiredEdge()` 不重复加入 spread / lag spread buffer。
- [x] 实现 `ExecutionGroup` / `ExecutionState`。
- [x] 实现 lead close 先于 open、lag stoploss 先于 signal close。

### Task 7: Feedback State / Order Retire

**Files:**
- Modify: `core/strategy/order_manager.h`
- Create/modify: `strategy/lead_lag/execution_state.h`
- Test: `test/strategy/lead_lag_feedback_state_test.cpp`

- [x] 给 `OrderManager` 增加 `RetireFinishedOrder(local_order_id)`。
- [x] 覆盖 open terminal feedback 到 Hold / delete cache。
- [x] 覆盖 close terminal feedback 到 Hold / delete cache。
- [x] 覆盖 submit rejected 和 feedback gap 后暂停新开仓。

### Task 8: Strategy Integration

**Files:**
- Create: `strategy/lead_lag/strategy.h`
- Modify/create: Gate LeadLag runtime tool after dry-run boundary明确
- Test: `test/strategy/lead_lag_strategy_test.cpp`

- [x] 增加 `leadlag::Strategy` user strategy hook skeleton，构造函数接收 `leadlag::Config`。
- [x] 增加 strategy interface 测试，验证 `StrategyRuntime` 可以构造并 dispatch hooks。
- [ ] 串接 `OnBookTicker()` 完整主链路。
- [ ] 串接 `OnOrderResponse()` / `OnOrderFeedback()` 完整 execution state。
- [ ] 增加 dry-run config load test。
- [ ] 增加 fixed Go replay 对账入口计划。
