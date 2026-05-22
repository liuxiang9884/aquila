# LeadLag 策略模块说明

`strategy/lead_lag` 是 Aquila 中 LeadLag 策略的 C++ 实现。当前实现以 Binance 为 lead、Gate 为 lag 的 ORDI_USDT 回放为主要验证对象，但模块本身按 instrument catalog 中的 `symbol_id`、`exchange` 和 pair 配置组织。

## 目录职责

| 文件 | 职责 |
| --- | --- |
| `config.h` / `config.cpp` | 解析 LeadLag TOML 配置，绑定 instrument catalog，生成 pair、trigger、execute、window capacity 和 lag 下单 metadata。 |
| `types.h` | 策略内部基础类型，例如 `PairRole` 和 `QuoteSnapshot`。 |
| `raw_market_state.h` | 按 `symbol_id` 和交易所区分 lead / lag quote，过滤非目标行情，并记录 latest / previous BBO。 |
| `alignment.h` | 计算 lead / lag 的价格 drift，完成 Bootstrap / Aligning / Active 阶段切换，并在进入 Active 时生成 seed quote。 |
| `window_stats.h` | 基于时间窗口的 mean / stddev 统计，底层使用预分配 `RingQueue`。 |
| `recorders.h` | BBO 极值窗口、noise、spread、move quantile 和 recorder 诊断统计。 |
| `threshold.h` | 根据 move quantile、drift std、lead / lag noise 和手续费更新开平仓阈值。 |
| `cost_model.h` | 计算开仓成本、手续费、spread buffer 和目标收益要求。 |
| `signal.h` | 信号判断核心，输出 open / close / stoploss 的 `SignalDecision` 和 `OrderIntent`。 |
| `execution_state.h` | 跟踪策略 position 生命周期：open order、hold position、close order，以及 feedback continuity lost 后暂停新开仓。 |
| `strategy.h` | 策略入口，串接行情状态、alignment、recorder、threshold、signal 和 execution state。 |

## 主流程

`Strategy::OnBookTicker()` 是策略主入口，处理流程如下：

1. `RawMarketState` 根据 `BookTicker.symbol_id` 和 `BookTicker.exchange` 路由到目标 pair 的 lead 或 lag。
2. 只有 BBO 价格变化时才进入主要计算；相同价格 quote 默认不重复触发。
3. 当 lead / lag 都有 quote 后，`AlignmentState` 维护 drift rolling mean / std，并在 warmup 和样本数满足后进入 Active。
4. 进入 Active 时用当前 market state 生成 seed，并对 lead quote 做 drift 调整。
5. Active 阶段按 tick role 分流：
   - lead tick：更新 drifted lead、recorder、threshold，并检查 close / open 信号。
   - lag tick：更新 lag recorder，并检查 stoploss / close 信号。
6. `SignalEngine` 输出 `SignalDecision`，replay 模式下用 synthetic accounting 直接推进 position；生产模式下通过订单与 feedback 推进 `ExecutionState`。

策略使用的全局时间是 `BookTickerEventTimeNs()`：

- 优先使用 `BookTicker.exchange_ns`。
- 如果 `exchange_ns == 0`，回退到 `BookTicker.local_ns`。

## DataReader 顺序边界

LeadLag 对 lead / lag tick 的处理顺序敏感，因为每个 `OnBookTicker()` 都可能推进 alignment、recorder、threshold
和 signal state。实时 `RealtimeDataReader` 的多 source round-robin 只解决公平调度问题：它避免某个高频 SHM
source 长期优先，但不按 `exchange_ns` 或 `local_ns` 做全局排序，也不等价于 merge。

因此：

- live 模式可以把 round-robin 作为低成本默认调度。
- 如果策略研发要求严格事件时间顺序，应让上游 data session / producer 产出已 merge 的统一 source。
- replay / 对账应继续使用预处理后的单一 binary source；多交易所或多 feed 历史输入需要在离线阶段先合成目标顺序。

## 配置入口

常用配置文件：

| 文件 | 用途 |
| --- | --- |
| `config/strategies/lead_lag_btc_strategy.toml` | BTC_USDT 实盘 signal-only runtime 配置。 |
| `config/strategies/lead_lag.toml` | BTC_USDT LeadLag 策略参数。 |
| `config/strategies/lead_lag_first5_strategy_20260521.toml` | 2026-05-21 first5 实盘 signal-only runtime 配置。 |
| `config/strategies/lead_lag_first5_20260521.toml` | 2026-05-21 first5 LeadLag 策略参数。 |
| `config/strategies/lead_lag_requested_strategy_20260521.toml` | 2026-05-21 requested symbols 实盘 signal-only runtime 配置。 |
| `config/strategies/lead_lag_requested_20260521.toml` | 2026-05-21 requested symbols 中 Gate lag 数量元数据完整的 LeadLag 策略参数。 |
| `config/strategies/lead_lag_ordi_replay.toml` | 回放 runtime 配置。 |
| `config/strategies/lead_lag_ordi.toml` | ORDI_USDT LeadLag 策略参数。 |
| `config/data_readers/lead_lag_ordi_binary_replay.toml` | Tardis binary replay 输入配置。 |
| `config/instruments/usdt_futures.csv` | instrument catalog，提供 `symbol_id`、交易所 symbol、tick size、数量精度等元信息。 |

实盘长时间运行和测试计划见 `doc/lead_lag_live_runtime_plan.md`；REST reconcile / feedback 恢复设计见 `doc/lead_lag_reconcile_design.md`。当前顺序是 signal-only live runner、长时间观察、生产订单闭环、`ContinuityLost` stop-and-flat 应急链路、flat-account / tiny-position smoke、小额真实订单 smoke 和端到端 benchmark。

## 回放与输出

回放工具：

```bash
./build/debug/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --signals-output signal.csv
```

如果要替换 binary 输入，可以用 `--data-reader-config` 指向另一份 data reader TOML：

```bash
./build/debug/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --data-reader-config /tmp/lead_lag_compare/lead_lag_ordi_hdf_binary_replay.toml \
  --signals-output /tmp/lead_lag_compare/hdf_signal.csv
```

signal CSV 由 `tools/lead_lag/signal_csv_writer.*` 输出，包含触发信号、价格、reduce_only、lead / lag quote、drift、threshold、noise、active group 等诊断字段，主要用于策略研发对账。

## 测试与 benchmark

相关测试主要位于 `test/strategy/`：

```bash
ctest --test-dir build/debug -R lead_lag --output-on-failure
```

相关 benchmark：

```bash
./build/release/benchmark/strategy/lead_lag_strategy_benchmark
```

性能结论应以 benchmark 或 replay 实测为准，不从代码结构直接推断。

## 当前边界

- replay 可以使用 `PositionAccountingMode::kSyntheticSignals`，不依赖真实订单 session。
- default production accounting 可以把信号转换为 IOC limit order intent，并通过 `StrategyContext` / `OrderManager` 提交；订单生命周期依赖 `OrderManager` 和 private feedback 更新。
- `[lead_lag.risk]` 提供 strategy 全局持仓硬限制：`max_gross_notional` 统计所有持仓和 pending open reservation 的绝对 notional，`max_holding_position` 统计绝对张数；触达限制后只拒绝新开仓，reduce-only close 继续允许。
- `tools/lead_lag/live_strategy.cpp` 的 live runner 默认 validate-only / signal-only；显式 `--execute` 且 `strategy.mode=live` 时进入真实 live-orders runtime，缺凭据会在 runtime create 前返回。
- 当前 V1 应急策略是：真实交易中遇到 feedback `ContinuityLost` 后停止自动交易并返回 handoff exit code `10`，再通过 Python REST helper 撤销 in-scope open orders、提交 reduce-only 市价平仓、用 REST 复核 flat；不在同一轮运行中自动恢复交易。2026-05-22 已完成 BTC_USDT flat-account、tiny-position 和隔离 `ContinuityLost` stop-and-flat smoke。更复杂的 read-only reconcile / resume 作为后续 V2 设计保留。
- `scripts/lead_lag/run_live_with_guard.py` 是当前推荐的真实 runner 外围入口：启动前做 REST preflight，runner 退出后做 final REST check，异常退出或 final 非 flat 时调用 emergency flatten。
- 该目录只包含策略层实现，不包含交易所接入、SHM data reader、binary reader 或 replay 工具本身。
- 当前实现关注确定性和低延迟热路径：初始化阶段绑定 catalog 和 pair，热路径避免字符串 lookup，窗口结构启动期预分配，扩容只记录统计，不作为策略计算指标落地。
