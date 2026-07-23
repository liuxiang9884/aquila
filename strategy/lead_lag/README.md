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
| `signal_csv_writer.*` | 触发信号 CSV 诊断输出。 |
| `market_calc_diagnostics.h` / `market_calc_csv_writer.*` | compile-time market calculation CSV 诊断 row 和 quill / nova writer。 |

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
| `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml` | requested 12-symbol 实盘订单 runtime 配置；文件名保留历史 `11symbols`，当前内容已追加 `ETH_USDT`，必须显式 `--execute` 才会提交真实订单。 |
| `config/strategies/lead_lag_ordi_replay.toml` | 回放 runtime 配置。 |
| `config/strategies/lead_lag_ordi.toml` | ORDI_USDT LeadLag 策略参数。 |
| `config/data_readers/lead_lag_ordi_binary_replay.toml` | Tardis binary replay 输入配置。 |
| `config/instruments/usdt_future_universe.csv` | instrument catalog，提供 `symbol_id`、交易所 symbol、tick size、数量精度等元信息。 |

实盘运行 runbook 见 `docs/lead_lag_live_operations.md`；live/replay 信号一致性测试见
`docs/lead_lag_live_replay_testing.md`；REST reconcile/feedback 恢复设计见 `docs/lead_lag_reconcile_design.md`；
latency 证据与复现方法见 `docs/lead_lag_latency_analysis.md`。

## 启动前参数生成

`lead_lag_freshness_preflight` 用 live data reader 读取 lead / lag fusion canonical
`BookTicker`，运行指定秒数后生成固定 freshness threshold：

```bash
./build/release/tools/lead_lag_freshness_preflight \
  --data-reader-config config/data_readers/strategy_data_reader_31symbols_no_ton_fusion_20260616.toml \
  --duration-sec 60 \
  --summary-json /home/liuxiang/tmp/lead_lag_freshness_preflight.json \
  --lead-lag-config-in config/strategies/lead_lag_30symbols_fusion_2bps_5bps_20260627.toml \
  --lead-lag-config-out /home/liuxiang/tmp/lead_lag_live_generated.toml
```

统计规则与 Go reference 的 freshness auto 对齐：对每条 BBO 计算
`local_ns - exchange_ns`，过滤负数样本，按 exchange / symbol 分组计算
`ceil(mean + 3 * population_std)`，并写入输出 config 的
`max_lead_freshness_ms` / `max_lag_freshness_ms`。工具会强制把 data reader source
按 `start_position=latest`、`read_mode=drain` 采样；如果读取的是 fusion canonical，
`BookTicker.local_ns` 是 `fusion_publish_ns`，因此生成值表示策略实际看到的
`fusion_publish_ns - exchange_ns`，包含 source 接收延迟和 fusion hop。实时策略只读取
生成后的固定配置，不在 live 热路径中做 warmup、滚动统计或动态改阈值。

`scripts/lead_lag/generate_preflight_config_params.py` 用启动前或历史 `BookTicker` binary 生成 fixed candidate 参数：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/generate_preflight_config_params.py \
  --input /path/to/book_ticker.bin.zst \
  --symbol-id 92 \
  --lead-exchange binance \
  --lag-exchange gate \
  --buffer-percentile 95 \
  --lag-price-tick 0.0001 \
  --json-output /home/liuxiang/tmp/lead_lag_preflight_params.json \
  --toml-output /home/liuxiang/tmp/lead_lag_preflight_params.toml
```

`--buffer-percentile` 必须显式传入，避免无意中用 p100 这类过激上限参数进入配置。`*_fixed_pct` 是 ratio，配置层只要求字段可解析为数值；JSON 输出会保留 p50/p95/p99/p100 spread 分布、p95/p99 候选值和 freshness 统计审计信息。传入 `--lag-price-tick` 后，脚本会按同一窗口内有效 lag BBO 的最大 bid / ask 价格，把 pct buffer 转成保守的固定 tick 数：`ceil(max_price * buffer_pct / price_tick)`，并输出 `open_slippage_ticks` / `close_slippage_ticks`。taker buffer preflight 只覆盖 open 和 normal close 的固定 ticks，不覆盖 `stoploss_slippage_ticks`、`close_retry_times` 或 `close_retry_slippage_step_ticks`。

多 pair 实盘启动配置使用 `scripts/lead_lag/apply_taker_buffer_slippage.py` 把每个 symbol 的 JSON 结果写回策略 TOML，并输出审计 CSV：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/apply_taker_buffer_slippage.py \
  --params-json /home/liuxiang/tmp/preflight_PROVE_USDT.json \
  --params-json /home/liuxiang/tmp/preflight_SKYAI_USDT.json \
  --config-in config/strategies/lead_lag_30symbols_fusion_2bps_5bps_20260627.toml \
  --config-out /home/liuxiang/tmp/lead_lag_live_slippage_generated.toml \
  --csv-out /home/liuxiang/tmp/lead_lag_live_slippage_audit.csv
```

freshness live 参数由 `lead_lag_freshness_preflight` 加 `scripts/lead_lag/apply_freshness_preflight_summary.py` 生成 `max_lag_freshness_ms`，不再通过策略内 `freshness_shadow` 配置对比。当前 taker buffer 先用 lag BBO spread percentile 作为 proxy，后续接入稳定 Depth L2 输入后可替换生成方法。实时策略只读取启动前生成的固定 freshness / slippage 配置，不在 live 热路径中保存 warmup sample、滚动更新 mean/std 或动态调整 buffer。

## Go reference 迁移边界

当前迁移原则是：会影响热路径但可在启动前完成的参数生成，不进入 C++ 实时策略；需要实盘化的 guard 必须有明确执行职责和边界，不在同一策略进程中做 shadow A/B。

### 当前 Go / C++ 对照

事实源：Go reference 为 `reference/leadlag-current-strategy-package.zip`，C++ 当前实现以本文件、`strategy/lead_lag/*`、`tools/lead_lag/*` 和 checked-in 策略 TOML 为准。

| 主题 | Go current | C++ current | 当前结论 |
| --- | --- | --- | --- |
| 策略配置形态 | 单 symbol JSON，`trigger` / `execute` / `cost` / `analysis` 分区。 | 多 pair TOML，绑定 instrument catalog，显式配置 exchange、symbol metadata、capacity 和 per-pair execution 参数。 | 配置载体不同是架构差异；不追求字段逐字同形。 |
| signal 主流程 | lead tick 上先处理已有持仓 close，再评估 open；lag tick 上处理 stoploss / close；open signal 后进入 guard。 | 同样 close 优先、open 后置；lag tick 处理 stoploss / close；open freshness / drift guard 在 signal triggered 后、订单 intent / synthetic accounting 前执行。 | 核心 signal 顺序已基本对齐。 |
| alignment drift | `drift_period` / `drift_min_samples` / `drift_warmup` 负责 drift readiness 和 drifted lead。 | 同样保留这些字段，继续服务 alignment readiness 和 drifted lead；不再作为旧 `drift_limit` 的唯一 guard 配置。 | 对齐。 |
| `drift_guard` | `enabled` 缺省等价启用；字段为 `drift_instant`、`ratio_std`、`ratio_std_window`、`drift_mean`、`drift_mean_window`；post-signal open-only。 | 字段和判断口径对齐，但 TOML 必须显式写 `enabled`；checked-in 配置已启用。窗口容量通过 `capacity.drift_guard_window_capacity` 预分配。 | 行为已进入 live hot path；C++ 的显式 `enabled` 和容量字段是安全 / 性能边界差异。 |
| 旧 `drift_limit` | Go current 没有独立 `drift_limit`。 | 旧 pre-signal `trigger.drift_limit` 已删除并被 parser 拒绝；回退方法见下方历史说明。 | 已按 Go current 收敛，不保留双 guard enforce。 |
| `lag_vol_guard` | `enabled` 缺省等价启用；按 lag mid jump、amplitude 和 cooldown post-signal 阻断 open。 | 不进入 live hot path；只在 `lead_lag_replay --lag-vol-guard-audit-output` 中离线评估 would-block。ORDI_USDT sweep 当前为负向结果。 | 刻意不同。除非更宽 replay / live smoke 给出反证，否则不 live enforce。 |
| freshness | 支持 `mode=auto`，策略内 warmup 后得到 lead / lag freshness threshold；也支持 fixed threshold。 | 不在实时策略内 auto warmup；启动前用 `lead_lag_freshness_preflight` 和 `apply_freshness_preflight_summary.py` 生成固定 `max_*_freshness_ms`，live 只执行固定 open guard。 | 刻意不同。C++ 把 auto 迁到启动前，避免热路径动态阈值。 |
| taker buffer / slippage cost | `cost.taker_buffer` 支持 `fixed` / `auto`，可影响 entry / normal close order price，并可按配置进入成本模型；auto 可基于 lag Depth L2 warmup。 | 不作为实时动态 order price 模型；启动前用 BBO spread pct 转成 `open_slippage_ticks` / `close_slippage_ticks` 写入 TOML。open signal 成本模型使用 fixed ticks 折算出的 entry / normal close slippage pct；`execute.taker_buffer` 仅输出 `lead_lag_signal_decision` 参考价诊断，不直接筛 signal。 | 部分对齐。C++ 用 fixed ticks 对齐“执行摩擦进入 open cost model”，但不迁入 Go-style auto warmup / pct hot path。 |
| Depth L2 | 可读取 lag depth，参与 taker buffer auto warmup、entry opportunity / closeability attribution。 | 当前 LeadLag live 策略主路径只消费 `BookTicker`；taker buffer preflight 暂用 lag BBO spread percentile proxy，未接稳定 Depth L2 输入。 | 仍未对齐；后续若接 Depth，应先替换 preflight 生成方法，不直接把 Depth warmup 放进 live hot path。 |
| normal close retry | `normal_close_retry_aggressive` 为 bool；普通 close 多次失败后把 normal close buffer 提高到 pct floor。 | 旧 bool 已废弃并被 parser 拒绝；当前用 `close_retry_times` 控制 retry 次数，用 `close_retry_slippage_step_ticks` 按 retry 次数递增 ticks。 | 已实现同类目的，但设计不同且更显式。 |
| close / stoploss slippage | Go normal close buffer 与 stoploss 价格模型分离。 | 已拆为 `close_slippage_ticks` 和 `stoploss_slippage_ticks`；normal close retry 不影响 stoploss。 | 对齐到“normal close 与 stoploss 分离”的语义。 |
| attribution / `signal_decision` | 有 `leadlag_attribution.v1`、stable linkage、`signal_decision`、entry opportunity、order lifecycle、Depth / guard snapshot 等完整 attribution 面。 | 有 live log、`order_detail.csv` / `position.csv` / `latency.csv`、stage BBO id、`intent_rejected_v1` 和有限 `lead_lag_signal_decision` 参考价诊断；没有完整 Go stable linkage schema。 | 部分对齐。C++ 当前优先保证订单 / report 可对账，未迁入完整 attribution schema。 |
| order lifecycle price semantics | attribution 中显式区分 `avg_fill_price`、`raw_price`、`limit_price`、`order_price`。 | report / live log 已区分 `raw_price`、`order_price`、`fill_price` / average fill 相关字段；字段语义见 `docs/lead_lag_live_report_csv_schema.md` 和 `docs/diagnostic_fields.md`。 | 大体对齐，但 schema 不同。 |
| A/B / shadow 方式 | Go reference 可在策略内输出丰富 attribution / guard snapshot。 | 项目约定不在既有 live 策略进程内新增 shadow 双路逻辑；候选实现用独立 replay、signal-only 或 live 进程评估。 | 明确不同，这是项目级热路径边界。 |

已完成：

- `freshness_auto` 不在策略内 warmup 或实时滚动更新。启动前用 `lead_lag_freshness_preflight` 采样 fusion / data reader BBO，再用 `apply_freshness_preflight_summary.py` 把 lag p50 bucket 生成的 `max_lag_freshness_ms` 写入策略配置；策略只按固定配置执行 open freshness guard。
- `freshness_shadow` 已从策略内删除。需要做 freshness 对照实验时，应另起 signal-only strategy / replay 进程运行候选配置，不在同一实盘策略进程里保留 shadow 逻辑。
- `taker_buffer` 不作为实时策略的动态 order price 模型。启动前用 `generate_preflight_config_params.py --lag-price-tick` 把 BBO spread pct 转成 `open_slippage_ticks` / `close_slippage_ticks`，再用 `apply_taker_buffer_slippage.py` 写入策略配置。open signal 的 `required_edge` 会把 fixed `open_slippage_ticks` / `close_slippage_ticks` 按 `ticks * lag_price_tick / trigger_price` 折算为基础 entry / normal close 执行成本；`execute.taker_buffer` 仅保留为 `lead_lag_signal_decision` 的参考价诊断，不改变真实下单路径，也不直接参与 signal 筛选。
- normal close 与 stoploss slippage 已拆分为 `close_slippage_ticks` 和 `stoploss_slippage_ticks`；旧 `close_slippage` 不再解析。normal close 失败后的额外 retry 由 `close_retry_times` 控制，`0` 表示不做 normal close retry；第 `n` 次 retry 使用 `close_slippage_ticks + n * close_retry_slippage_step_ticks`。stoploss 与 stoploss retry 始终使用 `stoploss_slippage_ticks`，不受 normal close retry step 影响。旧 `normal_close_retry_aggressive` bool 已废弃，parser 会直接拒绝。
- `lag_vol_guard` 第一版只落地 replay-only audit：`lead_lag_replay --lag-vol-guard-audit-output` 在同一份回放行情中维护独立 Go-like guard 状态，只对 open signal 写 `lag_vol_guard_audit.csv` 的 `would_block` / snapshot，不改变 replay signal、synthetic accounting 或 live hot path。2026-06-25 ORDI_USDT 三天 Tardis replay sweep 显示 `cooldown=3m/5m/10m/15m` 均降低 signal-only PnL，当前不推进 live shadow 或 enforce。
- `drift_guard` 已按 Go reference 口径替代旧 `trigger.drift_limit`，定位为 open-only emergency sanity guard。配置为 `[lead_lag.pairs.trigger.drift_guard]`，字段是 `enabled`、`drift_instant`、`ratio_std`、`ratio_std_window`、`drift_mean` 和 `drift_mean_window`；parser 会直接拒绝旧 `drift_limit` 和旧 `drift_guard.mode`。guard 使用 `lag_mid / lead_mid` 的 instant ratio、ratio std 和 drift mean 窗口，在 signal 触发后、真实订单 intent / synthetic accounting 前执行；只拦截 `kOpenLong` / `kOpenShort`，close / stoploss 不受影响。命中时输出 `lead_lag_order_intent_rejected reason=drift_guard`，live report 将其作为 `source_schema=intent_rejected_v1` 的拒绝意图行，而不是 `missing_order`。两个 drift guard 窗口的预分配容量由 `capacity.drift_guard_window_capacity` 控制，默认和 checked-in 策略配置为 `131072`，用于降低 1 分钟 live 窗口复用 spread 容量带来的 hot path 扩容风险；若要声明某次 live 不扩容，需要基于 tick-rate 实测或继续上调该容量。
- `lead_lag_replay --lag-vol-guard-audit-output` 同时记录生产 `DriftGuardState` 的 replay audit 字段：`drift_instant` 为原始 `lag_mid / lead_mid` ratio，`ratio_std` 为窗口标准差，`drift_mean` 为窗口均值，`drift_guard_outcome` 为 `disabled`、`not_ready`、`pass` 或 `blocked:*`。replay 通过 signal observer 在 post-signal guard 执行前写出 triggered signal，因此被 live drift guard 阻断的 open signal 也会出现在 `signals.csv` 和 audit CSV 中。

### `drift_limit` 历史实现与回退说明

旧 `drift_limit` 和当前 `drift_guard` 都是 lead / lag 价格比例异常保护，阈值单位都是 ratio 偏离度，例如 `0.02` 表示约 `2%`。二者都只影响新开仓，normal close 和 stoploss 不受影响；并且二者都依赖 paired raw BBO 样本，而不是订单回报或成交数据。

关键差异如下：

| 维度 | 旧 `trigger.drift_limit` | 当前 `[trigger.drift_guard]` |
| --- | --- | --- |
| 配置字段 | `trigger.drift_limit`，配合既有 `drift_period` / `drift_min_samples` / `drift_warmup` 的 alignment readiness。 | `trigger.drift_guard.enabled`、`drift_instant`、`ratio_std`、`ratio_std_window`、`drift_mean`、`drift_mean_window`；无独立 `cooldown`、`min_samples` 或 `warmup`。 |
| 度量来源 | `AlignmentState` 的 rolling mean：`lag_sum / lead_sum`，等价于 `lag_mid / lead_mid`，再计算 `abs(mean - 1.0)` 得到 `AlignmentSnapshot::drift_deviation`。 | 独立 `DriftGuardState`：当前 instant ratio、ratio rolling std 和 ratio rolling mean，判断 `abs(instant - 1.0)`、`std`、`abs(mean - 1.0)`。 |
| 窗口 | 单一 `drift_period` 窗口，同时服务 lead drift adjustment 和旧 guard；readiness 来自 alignment active 条件。 | `ratio_std_window` 和 `drift_mean_window` 两个窗口；窗口容量由 `capacity.drift_guard_window_capacity` 预分配。 |
| 执行点 | `SignalEngine::OnLeadTick()` 中 close 检查之后、open long / open short 计算之前；`alignment.drift_ready && alignment.drift_deviation > drift_limit` 时直接返回 `kDriftLimit`。 | signal 已经 triggered 后，在 `Strategy::FinalizeActiveSignal()` 中先记录 triggered signal，再于真实订单 intent / synthetic accounting 前执行 `RejectOpenForDriftGuard()`。 |
| 诊断表面 | 因为 open signal 尚未 triggered，通常不会输出 `lead_lag_signal_triggered` 或 `lead_lag_order_intent_rejected`；只体现在内存里的 `SignalDecision.reject_reason=kDriftLimit` 和相关单测。 | 输出 `lead_lag_signal_triggered`，命中时再输出 `lead_lag_order_intent_rejected reason=drift_guard`；report 生成 `source_schema=intent_rejected_v1` 行，避免误判为 `missing_order`。 |
| Go reference 对齐 | 这是 C++ legacy 保护，不是 Go current 的 drift guard。 | 与 Go current 的 post-signal drift guard 字段和判断口径对齐。 |

如果要回到旧 `drift_limit`，建议按“替代当前 `drift_guard`”实现，不要让两套 guard 同时 enforce，否则 open signal 前后会有两次 ratio gate，且诊断语义会混淆。可直接按下面清单改：

1. `strategy/lead_lag/config.h`：在 `TriggerConfig` 恢复 `double drift_limit{0.0};`。如果完全复原旧行为，`DriftGuardConfig`、`CapacityConfig::drift_guard_window_capacity` 和 `PairRuntimeState::drift_guard` 可删除；如果只是短期保留 replay 对照，需要确保 live 配置中 `drift_guard.enabled=false` 且文档写清楚它不 enforce。
2. `strategy/lead_lag/config.cpp`：删除 `ParseTrigger()` 中对 `drift_limit` 的 fail-fast，恢复 `RequiredDouble(table, "drift_limit", prefix + ".drift_limit")`。如果选择严格化配置，可在恢复时新增 finite / positive 校验，但这不是旧行为的逐字复原。
3. `strategy/lead_lag/signal.h`：恢复 `SignalRejectReason::kDriftLimit`，并把 `OnLeadTick()` 的 `AlignmentSnapshot` 参数重新命名为 `alignment`。在 `execution.new_entries_paused()` 检查之后、`TryOpenLong()` 之前加入：

```cpp
if (alignment.drift_ready &&
    alignment.drift_deviation > pair.trigger.drift_limit) {
  return Reject(SignalRejectReason::kDriftLimit);
}
```

4. `strategy/lead_lag/strategy.h`：如果是严格回退，移除 `DriftGuardState` include、runtime member、`runtime.drift_guard.Init()`、`runtime->drift_guard.OnPairedRawBbo()` 和 `RejectOpenForDriftGuard()` 调用。这样 replay synthetic accounting 与 live 都会在 signal 前被旧 `drift_limit` 拦截。
5. `config/strategies/*.toml`：在每个 `[lead_lag.pairs.trigger]` 下恢复 `drift_limit = <threshold>`，通常先用当前迁移前的 `0.02`；删除 `[lead_lag.pairs.trigger.drift_guard]` 和 `capacity.drift_guard_window_capacity`，除非明确保留 disabled audit。
6. 测试需要同步恢复或新增：`lead_lag_config_test` 覆盖 `drift_limit` 必填 / 可解析；`lead_lag_signal_test` 覆盖 alignment ready 且 `drift_deviation > drift_limit` 时阻断 open、但不阻断已有持仓 close；`lead_lag_strategy_interface_test` 覆盖不会生成 `lead_lag_order_intent_rejected reason=drift_guard`；如果删除 drift guard audit 字段，还要更新 `lead_lag_drift_guard_test`、replay guard audit CLI 测试和 Python report fixture。
7. 文档需要同步更新 `docs/project_onboarding_guide.md`、`docs/lead_lag_live_operations.md` 和
   `docs/diagnostic_fields.md`。尤其是 live report：旧 `drift_limit` 默认没有 rejected intent 行；如果希望保留当前 report 的
   rejected-intent 可观测性，应实现 post-signal drift-limit metric guard，而不是恢复旧语义。

仍需评估或修改：

1. `lag_vol_guard`：Go reference 会根据 lag 侧 jump / amplitude / cooldown 阻断 open；C++ 实时配置当前仍只接受 `mode=off`，未实现运行期 guard。当前 ORDI_USDT sweep 结论为负，除非更宽 symbol / 更长区间给出反证，否则不要设计 live shadow 或 enforce。
2. `drift_guard` 阈值复核：当前 checked-in 策略配置已启用 Go-like `drift_guard`，并沿用旧阈值数值 `0.02` 作为 `drift_mean=0.02`；后续如果有更长实盘或 replay 证据，可以只调阈值。若确实要恢复旧 pre-signal `drift_limit`，按上面的回退清单做一次完整替换，不要和当前 `drift_guard` 同时 enforce。

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

signal CSV 由 `strategy/lead_lag/signal_csv_writer.*` 输出，包含触发信号、`raw_price`、reduce_only、lead / lag `exchange_ns`、lead / lag quote、drift、threshold、noise、active group 等诊断字段，主要用于策略研发对账。`lead_lag_replay` 和 live runner 的 signal-only 模式只有在显式传入 `--signals-output` 时才写 per-signal CSV；未配置时只输出 summary。

Go-like `lag_vol_guard` 评估只通过 replay audit 完成，不进入 live 策略热路径。示例：

```bash
./build/debug/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --data-reader-config config/data_readers/lead_lag_ordi_binary_replay.toml \
  --signals-output /home/liuxiang/tmp/lead_lag_guard_audit/signals.csv \
  --lag-vol-guard-audit-output /home/liuxiang/tmp/lead_lag_guard_audit/lag_vol_guard_audit.csv
```

可选参数包括 `--lag-vol-guard-jump-threshold`、`--lag-vol-guard-jump-count`、`--lag-vol-guard-jump-window`、`--lag-vol-guard-amplitude-threshold`、`--lag-vol-guard-amplitude-window` 和 `--lag-vol-guard-cooldown`；duration 支持 `ns`、`us`、`ms`、`s`、`m`、`h`，默认值分别对应 Go reference 第一版口径：jump `0.005` / `3` / `5m`，amplitude `0.025` / `1s`，cooldown `15m`。该 CSV 每行对应一个 replay open signal，字段包括 signal id、lead / lag exchange timestamp、`would_block`、`would_block_reason`、jump / amplitude / cooldown snapshot 和当前 guard 参数；`drift_guard` 字段来自策略配置中的生产 `DriftGuardState`，`drift_instant` 是原始 `lag_mid / lead_mid` ratio，`ratio_std` 是窗口标准差，`drift_mean` 是窗口均值，`drift_guard_outcome` 为 `disabled`、`not_ready`、`pass` 或 `blocked:instant` / `blocked:ratio_std` / `blocked:drift_mean`。

与真实订单 report 对齐时，先生成 `order_detail.csv` / `position.csv`，再汇总：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/summarize_guard_audit.py \
  --guard-audit /home/liuxiang/tmp/lead_lag_guard_audit/lag_vol_guard_audit.csv \
  --order-detail reports/<run>/order_detail.csv \
  --position reports/<run>/position.csv \
  --summary-json /home/liuxiang/tmp/lead_lag_guard_audit/guard_audit_summary.json \
  --summary-md /home/liuxiang/tmp/lead_lag_guard_audit/guard_audit_summary.md
```

汇总脚本按 `symbol_id + signal_lag_id + action` 把 audit row 对齐 entry order，再按 `symbol_id + position_id` 汇总 position PnL；未传 `--position` 时仍可输出订单侧 blocked / allowed 对比。

当前 ORDI_USDT 三天 Tardis replay 结论：`cooldown=3m/5m/10m/15m` 都降低 signal-only net PnL；默认 `15m` 过滤 `62/1175` 个 open signal，主要由 cooldown 扩大，且被过滤 trade 在 0 tick 和 5 ticks 滑点口径下整体仍为正贡献。因此 `lag_vol_guard` 当前只作为离线评估工具保留，不进入真实订单 hot path。

market calculation CSV 是 compile-time 策略诊断能力，默认不编译；开启方式：

```bash
cmake -S . -B build/debug_market_calc \
  -DAQUILA_ENABLE_LEAD_LAG_MARKET_CALC_CSV=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug_market_calc --target lead_lag_replay lead_lag_strategy
```

开启后，仍使用原有 runner，只是运行 `market_calc` diagnostic mode。replay 示例：

```bash
./build/debug_market_calc/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --diagnostic-mode market_calc \
  --market-calc-output-dir /home/liuxiang/tmp/lead_lag_market_calc
```

realtime 示例：

```bash
./build/debug_market_calc/tools/lead_lag_strategy \
  --config config/strategies/lead_lag_btc_strategy.toml \
  --connect-data \
  --diagnostic-mode market_calc \
  --market-calc-output-dir /home/liuxiang/tmp/lead_lag_market_calc_live
```

该模式输出 `lead_calc.csv` 和 `lag_calc.csv`，每条已路由 lead / lag 行情处理后各输出一行当前计算快照；不触发 signal，不执行 synthetic position accounting，不提交外部订单。CSV 通过 `strategy/lead_lag/market_calc_csv_writer.*` 使用 quill / nova 异步写入；bool 输出 `true` / `false`，enum 输出名字，尚未初始化或数学无定义的 double 输出 `nan`。

启用 `execute.taker_buffer` 时，真实订单模式会在 signal triggered 后、订单提交或拒绝前输出 `lead_lag_signal_decision`；默认 legacy 配置不新增该日志。该日志记录当前真实下单路径准备提交的 `current_order_price`，以及用启动前生成的 `execute.taker_buffer` 计算出的 `reference_order_price`；结果只用于 taker buffer 参考价对比，不改变现有下单、拒单和风控路径。freshness 对照实验使用独立 signal-only strategy 进程运行候选配置，不在同一策略进程内保留 `freshness_shadow`。

真实订单模式不接 `SignalCsvWriter`，不写 per-signal CSV。信号触发与下单关联通过策略日志追踪：`lead_lag_signal_triggered` 输出 `trigger_exchange`、`trigger_symbol_id`、`lead_exchange_ns`、`lead_local_ns`、`signal_lead_id`、`lead_freshness_ns`、`lag_exchange_ns`、`lag_local_ns`、`signal_lag_id`、`lag_freshness_ns` 和 `raw_price`；freshness 定义为 `signal_decision_ns - *_exchange_ns`。当前开仓 freshness guard 和 `drift_guard` 都只作用于 `kOpenLong` / `kOpenShort`；每个 `[[lead_lag.pairs]]` 必须直接配置整数毫秒字段 `max_lead_freshness_ms` / `max_lag_freshness_ms`，当前 checked-in 配置通常为 lead `5ms`、lag `20ms`；close / stoploss 不受这些 open guard 影响。成功路径不再输出字段重复的 `lead_lag_order_intent`；`lead_lag_signal_triggered`、`lead_lag_order_intent_rejected` 和 `lead_lag_order_submitted` 保留同一组 signal timing、`signal_lead_id` / `signal_lag_id` 与 freshness 字段，用于把信号和成功订单或拒绝原因对齐。`drift_guard` 命中时拒绝原因是 `reason=drift_guard`，report 会把该拒绝作为 `intent_rejected_v1` 伪订单行关联到 signal，避免误报为 `missing_order`。成功提交后 `lead_lag_order_submitted` 输出 `local_order_id`、最终 `position_id`、`position_event`、`position_direction`、`entry_local_order_id`、`signal_role`、`order_role`、数值 `quantity` 和 `order_price`，作为订单分析的主事实源；重复的 `quantity_text` / `price_text` 已删除。Gate 的真实 wire text 可由 `gate_order_send_ok` 追溯；Bitget send log 当前不含这两个 text，分析脚本从数值字段生成规范化 text。`lead_lag_order_response`、`lead_lag_order_feedback` 和 `lead_lag_order_finished` 也输出处理该 Ack、回报或终态时策略已看到的 `lead_exchange_ns` / `lag_exchange_ns`；其中 response / feedback 根据事件状态额外输出 `<stage>_lead_id` / `<stage>_lag_id`，例如 `ack_lead_id`、`cancelled_lag_id`、`filled_lead_id` 和 `rejected_lag_id`，用于将 Ack / cancel / fill / reject 时点对齐到 recorder `BookTicker` bin。feedback 额外输出 `exchange_update_ns`、`local_receive_ns` 和 `fill_price`，用于成交 / 取消 / 拒单定位。`lead_lag_order_finished` 同步输出 `position_id`、`position_direction`、`order_role`、`entry_local_order_id` 和 `order_finished_local_ns`，便于后续生成 `position.csv`。可用 `scripts/lead_lag/analyze_order_detail.py` 从 live log 生成 `order_detail.csv`，传入 `--positions-output` 时同时生成 `position.csv`，传入 `--latency-output` 时同时生成 `latency.csv`。

`position.csv` 按 `run_id + symbol_id + position_id` 关联 entry / exit；每个有成交的 exit 生成一行 closed / partial_closed position slice，未平的 entry 生成一行 open。`gross_pnl` 按 `position_direction`、entry / exit average fill price、matched volume 和 `contract_multiplier` 计算；`net_pnl` 在 `gross_pnl` 基础上扣除 config 估算 fee，entry fee 会按 matched volume 比例分摊。

`latency.csv` 一行对应一个本地订单，保留 `request_send_local_ns`、`ack_local_receive_ns`、`order_finished_local_ns`、`ack_exchange_ns`、`finish_exchange_ns`、`ack_rtt_ns`、`send_to_finish_local_ns`、`ack_to_finish_local_ns`、`ack_exchange_to_local_ns` 和 `exchange_lifecycle_ns` 等字段。`ack_rtt_ns` 是本地发送到本地收到 ack 的 RTT，用于规避本地和交易所时钟不同步的问题；exchange timestamp 字段只用于辅助定位，不直接当作单程网络延迟。

## 测试与 benchmark

相关测试主要位于 `test/strategy/`：

```bash
ctest --test-dir build/debug -R lead_lag --output-on-failure
```

相关 benchmark：

```bash
./build/release/benchmark/strategy/lead_lag_strategy_benchmark
```

`OpenSignalSubmitPath` 等 runtime benchmark 的 tail 解释和当前环境限制见 `docs/lead_lag_latency_analysis.md`。

性能结论应以 benchmark 或 replay 实测为准，不从代码结构直接推断。

## Go/C++ 当前语义对齐

本节只保留仍影响当前实现或验证的差异；历史修改过程不作为事实源。

### `parallel-limit`

C++ 在 signal 形成后、order intent 前执行 `parallel-limit`，命中时保留 signal 并输出
`lead_lag_order_intent_rejected reason=parallel_limit`。一个 multi-route execution group 只占一个 parallel slot，
不能按 child order 数量重复占用。Go 与 C++ 的具体执行结构不同，但当前外部语义统一为“信号存在，订单意图被容量 guard 拒绝”。

`execute.parallel` 的合法范围是 `1..16`。每个 pair 使用固定容量的
`FixedOrderedSlotPool<ExecutionGroup, 16>` 保存 active group；配置解析和 runtime 初始化都会拒绝越界值，不做
静默 clamp。`(symbol_id, group_id)` 是稳定 group identity，`group_index` 只用于进程内精确定位。terminal 回报的
slot/index 与 `group_id` 不匹配时禁止扫描 fallback，pair 进入 `needs_reconcile` 并暂停新开仓，但已有持仓的 close / stoploss
路径继续保留。完整 contract 见 `docs/lead_lag_fixed_ordered_slot_pool_parallel.md`。

### Multi-route fanout 与 entry quantity

`execute.order_session_fanout` 是一个 execution group 最多使用的 ready route 数，不是 `parallel`。Entry fanout 的每张
child 使用相同 quantity，risk reservation 按实际 selected route 数累计；child 分别拥有独立 `local_order_id/route_id`，
已知成交量汇总回同一个 group。

Entry quantity 先按实际 order price、`open_notional` 和 quantity step 计算；结果低于 instrument `min_quantity` 时直接使用
`min_quantity`，高于最小量时保留计算结果，不做“必须恰好等于最小量”的额外拒绝。Normal close、stoploss 和 retry 继续按
group 已知总持仓生成 full-size `reduce-only` child。Bitget 20-symbol 四路配置见
`config/strategies/lead_lag_bitget_top20_highspeed_fanout4_20260716.toml`，每个 pair 使用 lead/lag freshness
`3ms/500ms`、`open_notional=10` 和 `order_session_fanout=4`。

### `lag_vol_guard`

C++ 的 lag-vol guard 保留为 replay-only audit，不进入 live hot path。ORDI 三天 replay 的 3m/5m/10m/15m cooldown
都降低 signal-only net PnL；默认 15m 过滤 62/1175 个 open signal，过滤集合整体仍为正贡献。该证据只覆盖该数据与参数，
不能外推到 live universe。需要候选评估时运行独立 replay/signal-only 进程，不在现有 live process 增加 shadow 双路逻辑。

### `drift_guard` 与 freshness

`drift_guard` 和 freshness 都是 open-only、post-signal、pre-order-intent guard；close/stoploss 不受影响。
命中后输出 `lead_lag_order_intent_rejected`，report 生成 `intent_rejected_v1`，不得误标 `missing_order`。
`needs_reconcile/new_entries_paused` 属于更高等级交易状态保护，仍可在 open signal 评估前返回 degraded。

Freshness 使用每 pair 的整数毫秒 `max_lead_freshness_ms/max_lag_freshness_ms`。阈值应由启动前 preflight 固化，
不在 live hot path 动态学习；候选阈值 A/B 使用独立 signal-only 进程。

### Slippage cost model

C++ 不迁入 Go 的 Depth-L2/taker-buffer auto warmup。当前用真实执行配置的 fixed ticks 进入 open cost：

```text
entry_slippage_buffer = open_slippage_ticks * lag_price_tick / trigger_price
normal_close_slippage_buffer = close_slippage_ticks * lag_price_tick / trigger_price
required_edge = fee + entry_slippage_buffer + normal_close_slippage_buffer
                + lead_noise + lag_noise + target_profit_rate
```

Stoploss slippage 与 close-retry step 不进入普通 open threshold。`execute.taker_buffer` 在 C++ 只提供参考价诊断/preflight，
不直接筛 signal。目标 universe 上线前需跑 replay/signal-only delta，不能从 ORDI 单 symbol zero-vs-five-tick smoke 外推。

## 当前边界

- replay 可以使用 `PositionAccountingMode::kSyntheticSignals`，不依赖真实订单 session。
- default production accounting 可以把信号转换为 IOC limit order intent，并通过 `StrategyContext` / `OrderManager` 提交；订单生命周期依赖 `OrderManager` 和 private feedback 更新。
- `[lead_lag.risk]` 提供 strategy 全局持仓硬限制：`max_gross_notional` 统计所有持仓和 pending open reservation 的绝对 notional；`max_holding_position` 可选，配置后统计绝对数量，支持 decimal-size 合约。触达限制后只拒绝新开仓，reduce-only close 继续允许。
- `tools/lead_lag/live_strategy.cpp` 的 live runner 默认 validate-only / signal-only；显式 `--execute` 且 `strategy.mode=live` 时进入真实 live-orders runtime，缺凭据会在 runtime create 前返回。
- 当前 V1 应急策略是：真实交易中遇到 feedback `ContinuityLost` 后停止自动交易并返回 handoff exit code `10`，再通过 Python REST helper 撤销 in-scope open orders、提交 reduce-only 市价平仓、用 REST 复核 flat；不在同一轮运行中自动恢复交易。2026-05-22 已完成 BTC_USDT flat-account、tiny-position 和隔离 `ContinuityLost` stop-and-flat smoke。更复杂的 read-only reconcile / resume 作为后续 V2 设计保留。
- `scripts/lead_lag/run_live_with_guard.py` 是当前推荐的真实 runner 外围入口：启动前做 REST preflight，runner 退出后做 final REST check，异常退出或 final 非 flat 时调用 emergency flatten。
- 该目录只包含策略层实现，不包含交易所接入、SHM data reader、binary reader 或 replay 工具本身。
- 当前实现关注确定性和低延迟热路径：初始化阶段绑定 catalog 和 pair，热路径避免字符串 lookup，窗口结构启动期预分配，扩容只记录统计，不作为策略计算指标落地。
