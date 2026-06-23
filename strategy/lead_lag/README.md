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
| `config/instruments/usdt_futures.csv` | instrument catalog，提供 `symbol_id`、交易所 symbol、tick size、数量精度等元信息。 |

实盘运行 runbook 见 `docs/lead_lag_live_runtime_plan.md`；live / replay 信号一致性测试见 `docs/lead_lag_live_replay_testing.md`；REST reconcile / feedback 恢复设计见 `docs/lead_lag_reconcile_design.md`。IOC partial-fill / decimal filled close 当前不再作为 active blocker；后续如果 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再按具体问题复查。

## 启动前参数生成

`scripts/lead_lag/generate_preflight_config_params.py` 用启动前或历史 `BookTicker` binary 生成 fixed candidate 参数：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/generate_preflight_config_params.py \
  --input /path/to/book_ticker.bin.zst \
  --symbol-id 92 \
  --lead-exchange binance \
  --lag-exchange gate \
  --json-output /home/liuxiang/tmp/lead_lag_preflight_params.json \
  --toml-output /home/liuxiang/tmp/lead_lag_preflight_params.toml
```

输出 TOML patch 包含 `execute.taker_buffer.entry_fixed_pct`、`execute.taker_buffer.normal_close_fixed_pct` 和 `execute.freshness_shadow.{lead,lag}_threshold_ms`。freshness threshold 使用非负 `local_ns - exchange_ns` 样本的 `ceil(mean + 3 * std)`，最小输出 `1ms`，保证生成配置能通过正阈值校验；当前 taker buffer 先用 lag BBO spread percentile 作为 proxy，后续接入稳定 Depth L2 输入后可替换生成方法。实时策略只读取这些固定参数，不在 live 热路径中保存 warmup sample、滚动更新 mean/std 或动态调整 buffer。

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

启用 `execute.taker_buffer` 或 `execute.freshness_shadow` 时，真实订单模式会在 signal triggered 后、订单 intent 前输出 `lead_lag_signal_decision`；默认 legacy 配置不新增该日志。该日志记录当前真实下单路径准备提交的 `current_order_price`、用启动前生成的 `execute.taker_buffer` 计算出的 `reference_order_price`，以及 `execute.freshness_shadow` 在 shadow 口径下是否会阻断本次开仓；shadow 结果只用于对比，不改变现有下单、拒单和风控路径。

真实订单模式不接 `SignalCsvWriter`，不写 per-signal CSV。信号触发与下单关联通过策略日志追踪：`lead_lag_signal_triggered` 输出 `trigger_exchange`、`trigger_symbol_id`、`lead_exchange_ns`、`lead_local_ns`、`signal_lead_id`、`lead_freshness_ns`、`lag_exchange_ns`、`lag_local_ns`、`signal_lag_id`、`lag_freshness_ns` 和 `raw_price`；freshness 定义为 `signal_decision_ns - *_exchange_ns`。当前开仓 freshness guard 只作用于 `kOpenLong` / `kOpenShort`；每个 `[[lead_lag.pairs]]` 必须直接配置整数毫秒字段 `max_lead_freshness_ms` / `max_lag_freshness_ms`，当前 checked-in 配置通常为 lead `5ms`、lag `20ms`；close / stoploss 不受该 guard 影响。`lead_lag_order_intent` / `lead_lag_order_intent_rejected` / `lead_lag_order_submitted` 输出同一组 signal timing、`signal_lead_id` / `signal_lag_id` 与 freshness 字段，用于把信号和订单意图或拒绝原因对齐；成功提交后 `lead_lag_order_submitted` 输出 `local_order_id`、最终 `position_id`、`position_event`、`position_direction`、`entry_local_order_id`、`signal_role`、`order_role`、`quantity_text` 和 `price_text`，作为订单分析的主事实源。`lead_lag_order_response`、`lead_lag_order_feedback` 和 `lead_lag_order_finished` 也输出处理该 Ack、回报或终态时策略已看到的 `lead_exchange_ns` / `lag_exchange_ns`；其中 response / feedback 根据事件状态额外输出 `<stage>_lead_id` / `<stage>_lag_id`，例如 `ack_lead_id`、`cancelled_lag_id`、`filled_lead_id` 和 `rejected_lag_id`，用于将 Ack / cancel / fill / reject 时点对齐到 recorder `BookTicker` bin。feedback 额外输出 `exchange_update_ns`、`local_receive_ns` 和 `fill_price`，用于成交 / 取消 / 拒单定位。`lead_lag_order_finished` 同步输出 `position_id`、`position_direction`、`order_role`、`entry_local_order_id` 和 `order_finished_local_ns`，便于后续生成 `position.csv`。可用 `scripts/lead_lag/analyze_order_detail.py` 从 live log 生成 `order_detail.csv`，传入 `--positions-output` 时同时生成 `position.csv`，传入 `--latency-output` 时同时生成 `latency.csv`。

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

`OpenSignalSubmitPath` 等 runtime benchmark 的 tail 解释和当前本机 benchmark 环境限制见
`docs/lead_lag_benchmark_environment_tail_analysis.md`。

性能结论应以 benchmark 或 replay 实测为准，不从代码结构直接推断。

## 当前边界

- replay 可以使用 `PositionAccountingMode::kSyntheticSignals`，不依赖真实订单 session。
- default production accounting 可以把信号转换为 IOC limit order intent，并通过 `StrategyContext` / `OrderManager` 提交；订单生命周期依赖 `OrderManager` 和 private feedback 更新。
- `[lead_lag.risk]` 提供 strategy 全局持仓硬限制：`max_gross_notional` 统计所有持仓和 pending open reservation 的绝对 notional；`max_holding_position` 可选，配置后统计绝对数量，支持 decimal-size 合约。触达限制后只拒绝新开仓，reduce-only close 继续允许。
- `tools/lead_lag/live_strategy.cpp` 的 live runner 默认 validate-only / signal-only；显式 `--execute` 且 `strategy.mode=live` 时进入真实 live-orders runtime，缺凭据会在 runtime create 前返回。
- 当前 V1 应急策略是：真实交易中遇到 feedback `ContinuityLost` 后停止自动交易并返回 handoff exit code `10`，再通过 Python REST helper 撤销 in-scope open orders、提交 reduce-only 市价平仓、用 REST 复核 flat；不在同一轮运行中自动恢复交易。2026-05-22 已完成 BTC_USDT flat-account、tiny-position 和隔离 `ContinuityLost` stop-and-flat smoke。更复杂的 read-only reconcile / resume 作为后续 V2 设计保留。
- `scripts/lead_lag/run_live_with_guard.py` 是当前推荐的真实 runner 外围入口：启动前做 REST preflight，runner 退出后做 final REST check，异常退出或 final 非 flat 时调用 emergency flatten。
- 该目录只包含策略层实现，不包含交易所接入、SHM data reader、binary reader 或 replay 工具本身。
- 当前实现关注确定性和低延迟热路径：初始化阶段绑定 catalog 和 pair，热路径避免字符串 lookup，窗口结构启动期预分配，扩容只记录统计，不作为策略计算指标落地。
