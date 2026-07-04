# 交易所撮合与成交率测试记录

本文记录我们围绕 Gate 撮合 / fillability 做过的实盘小实验、与 LeadLag live 结果的对比、当前推断和后续可验证做法。这里不作为实盘启动 runbook；真实下单仍按 `docs/lead_lag_live_operations_pipeline.md` 和对应 probe 文档执行。

## 口径

- `submitted entry order`：已经实际提交给 Gate 的开仓订单。LeadLag report 中按 `order_detail.csv` 的 `source_schema=submitted_v1` 且 `order_role=entry` 统计；不把 `parallel_limit`、freshness guard 或其他本地拒绝信号放入分母。
- `parent group`：同一次策略开仓意图 fanout 出来的多路 child order。30-symbol live 中 BTC 使用 `order_session_fanout=4`，所以一个 parent group 通常有 4 条 entry order。
- `Gate local freshness`：使用本机时钟口径，LeadLag 为 `signal_decision_ns - lag_local_ns`，BTC probe 为 `decision_ns - gate_local_ns`。它表示决策时本机可见 Gate BBO 的年龄。
- `Gate exchange freshness`：LeadLag report 的 `lag_freshness_ns = signal_decision_ns - lag_exchange_ns`。该值跨本地和交易所时钟，适合做同一套 report 内的诊断参考，不应当单独解释为真实网络延迟。
- `exchange_lifecycle_ns`：`latency.csv` 中 Gate 同一交易所时钟域内的订单生命周期差值。IOC 场景下这个值越大，越说明撮合 / 结果返回发生在更晚的交易所时间窗口。
- `row fill rate` 和 `group fill rate` 分开看。前者按 child order 行统计，后者按 parent group 是否任一路成交统计。

## 2026-07-04 BTC Binance-trigger / Gate-quote probe

数据来源：

- run dir：`/home/liuxiang/tmp/20260704_071322_gate_btc_binance_trigger_gate_quote_probe_10m_100nodes`
- scratch config：`/home/liuxiang/tmp/gate_btc_binance_trigger_gate_quote_probe_20260704_071322_10m100.toml`
- 代码 / 配置来源：`docs/gate_btc_fill_probe.md`

实验设置：

- 目标为 `BTC_USDT`。
- Binance fusion BBO 触发，使用触发时本机可见的最新 Gate fusion BBO 作为下单 quote。
- Entry 严格使用 Gate 对手价，不加 entry slippage：buy 用 Gate ask，sell 用 Gate bid。
- 每个 node 同时提交 1 个 GTC entry 和 1 个 IOC entry，分别走两条 order session。
- Entry 数量为 Gate instrument catalog 中 BTC 最小可交易量；preflight notional 约 `6.25 USDT`，低于 `10 USDT` 上限。
- GTC 1 秒未成交撤单；任一 entry 成交后用 reduce-only IOC aggressive limit 平仓。
- 配置要求 `max_nodes=100`、`duration_ms=600000`。实际先达到 100 个 submitted entry nodes，约 `160.72s` 后停止。

结果：

| 指标 | 数值 |
| --- | ---: |
| submitted nodes | 100 |
| entry order rows | 200 |
| filled entry order rows | 198 |
| row fill rate | 99.0% |
| nodes any filled | 99 |
| node any-fill rate | 99.0% |
| completed_closed nodes | 99 |
| completed_no_fill nodes | 1 |
| skipped rows | 1887 |
| `stale_gate_quote` skipped | 1802 |
| `stale_binance_trigger` skipped | 85 |

Gate local freshness，submitted nodes：

| 分位 | 数值 |
| --- | ---: |
| min | `0.042ms` |
| p50 | `7.764ms` |
| p75 | `22.415ms` |
| p90 | `33.006ms` |
| p95 | `39.408ms` |
| p99 | `47.772ms` |
| max | `49.030ms` |
| avg | `12.976ms` |

Probe entry latency：

| 指标 | p50 | p90 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| submit to ack local | `0.619ms` | `0.749ms` | `2.214ms` | `5.350ms` |
| accepted exchange - ack exchange | `0.954ms` | `2.176ms` | `8.422ms` | `14.906ms` |
| feedback exchange - ack exchange | `0.359ms` | `1.249ms` | `4.817ms` | `7.467ms` |

结束检查：

- Probe 正常输出 `fill_probe_stop`。
- final REST dry-run 显示 `BTC_USDT` 无 open order、position size `0`。
- 专用 `gate_order_gateway` / `gate_order_feedback_session` 已关闭。

## 2026-07-01 30-symbol LeadLag live 对比

数据来源：

- A 组 run dir：`/home/liuxiang/tmp/20260701_102201_30symbols_ogw_24h/`
- final stopped report：`/home/liuxiang/tmp/aquila_partial_reports/20260701_102201_30symbols_ogw_24h_stopped_20260702_032345/`
- 主要 CSV：`signal.csv`、`order_detail.csv`、`latency.csv`
- 策略配置：`config/strategies/lead_lag_30symbols_fusion_2bps_2bps_5bps_lag200_order_gateway_20260701.toml`

BTC 配置和行为：

- `lead_exchange=binance`，`lag_exchange=gate`。
- `max_lead_freshness_ms=5`，`max_lag_freshness_ms=200`。
- `open_notional=20.0`。
- `open_slippage_ticks=118`，按 BTC 价格约 `1.95bps`。
- `parallel=1`。
- `order_session_fanout=4`。
- 原始 gateway log 显示 BTC entry 下发为 `tif=kImmediateOrCancel`。

只看 BTC 已下出去的开仓单：

| 口径 | 分母 | 成交 | 成交率 |
| --- | ---: | ---: | ---: |
| submitted entry order rows | 16 | 2 | 12.5% |
| submitted parent groups | 4 | 1 | 25.0% |

BTC submitted entry 细节：

| parent | action | order rows | status | Gate local freshness | order offset |
| --- | --- | ---: | --- | ---: | ---: |
| `191` | `kOpenLong` | 4 | `kCancelled=4` | `4.089ms` | 118 ticks |
| `210` | `kOpenShort` | 4 | `kCancelled=4` | `0.947ms` | 118 ticks |
| `336` | `kOpenShort` | 4 | `kCancelled=4` | `0.340ms` | 118 ticks |
| `420` | `kOpenShort` | 4 | `kFilled=2, kCancelled=2` | `9.536ms` | 118 ticks |

BTC submitted entry latency：

| 指标 | p50 | p75 | p90 | max |
| --- | ---: | ---: | ---: | ---: |
| `ack_rtt_ns` | `28.895ms` | `81.590ms` | `913.494ms` | `1030.249ms` |
| `exchange_lifecycle_ns` | `222.930ms` | `987.089ms` | `1980.749ms` | `2161.633ms` |

BTC 的 Gate local freshness 并不差：

| 口径 | p50 | p90 | max |
| --- | ---: | ---: | ---: |
| BTC submitted entry | `0.947ms` | `9.536ms` | `9.536ms` |
| BTC filled entry | `9.536ms` | `9.536ms` | `9.536ms` |

对比全部 30-symbol submitted entry：

| 口径 | 分母 | 成交 | 成交率 |
| --- | ---: | ---: | ---: |
| submitted entry order rows | 2616 | 25 | 0.96% |
| submitted parent groups | 654 | 10 | 1.53% |

全部 submitted entry 的本地链路不是瓶颈：

| 指标 | p50 | p90 | p99 | max |
| --- | ---: | ---: | ---: | ---: |
| `signal_to_request_send` | `0.024ms` | `0.028ms` | `0.036ms` | `0.045ms` |
| `trigger_to_request_send` | `0.026ms` | `0.030ms` | `0.037ms` | `0.046ms` |

## 当前对比结论

1. **Probe 的高成交率不能直接外推到 LeadLag live。**
   Probe 测到的是“普通 Binance tick 到来时，Gate 当前 touch 在低负载 BTC-only 条件下能否被最小量订单打到”。LeadLag live 的 open signal 是在 Binance lead move 条件满足时触发，属于更强 adverse-selection 场景。

2. **BTC live 低成交率不是由 Gate quote freshness 差直接解释。**
   BTC live submitted entry 的 Gate local freshness p50 `0.947ms`、max `9.536ms`，明显比 probe 的 p50 `7.764ms`、max `49.030ms` 更新；但成交率反而低很多。

3. **策略从 signal 到发单不是主要瓶颈。**
   30-symbol live submitted entry 的 `signal_to_request_send` p50 约 `24us`，BTC 也是同一量级。也就是说，策略决策到 SHM/gateway 发单链路没有解释 99% vs 12.5% 的差异。

4. **更可疑的是 signal-conditioned 场景下的 Gate 撮合窗口和订单生命周期。**
   BTC live submitted entry 的 `exchange_lifecycle_ns` p50 `222.930ms`、max `2161.633ms`，远大于 probe entry 的 `accepted exchange - ack exchange` p50 `0.954ms`、max `14.906ms`。IOC 在几十毫秒到秒级窗口里，原先用于定价的 touch 很可能已经撤掉或追价。

5. **Live 的订单形态更重。**
   Probe 每 node 是 `1` 张 GTC + `1` 张 IOC；BTC live 是 4 路 IOC fanout，每路 quantity `3`，同一 parent 目标流动性更大，也会同时竞争同一层或附近流动性。

6. **BTC live 的 118 tick slippage 大约只有 2bps。**
   对普通 touch 打单，这个偏移可能足够；但在 `lead=0.0025` 的 Binance lead move 条件下，Gate 可能已经快速追价。2bps aggressive limit 不一定覆盖 signal-conditioned 的价格移动。

## 当前推断

- **较高置信度：** 普通时刻 BTC Gate touch 的最小量 fillability 很高。证据是 2026-07-04 probe 中 IOC 和 GTC entry 均为 99/100 filled。
- **较高置信度：** LeadLag signal-conditioned open 的 fillability 远低于普通 touch fillability。证据是 30-symbol live 中全部 submitted entry order row fill rate 约 0.96%，BTC submitted entry row fill rate 12.5%。
- **较高置信度：** Freshness guard 只能解释“是否用旧 quote 下单”，不能单独解释 signal-conditioned fillability。BTC live 的 quote freshness 更好但成交率更低。
- **中等置信度：** Gate 订单生命周期尾延迟和交易所侧处理窗口是 live fillability 的重要变量。证据是 BTC live `exchange_lifecycle_ns` 远高于 probe，但当前还没有同一时段、同一 signal 条件下的 A/B。
- **中等置信度：** 4 路 IOC fanout 和更大的 order quantity 会降低相对 probe 的 fillability。需要同一触发条件下比较 `1 route` vs `4 route`、`min qty` vs live qty。
- **低到中等置信度：** 单纯提高 open slippage 可以提高成交率，但可能恶化 adverse selection 下的 PnL。需要用 signal-conditioned sweep 验证，不能仅按 fill rate 决策。

## 讨论后的可验证做法

### 1. 做 signal-conditioned BTC probe

目标是把普通 touch fillability 和 LeadLag signal-conditioned fillability 分开。

建议新增一个只读 Binance/Gate fusion、真实 Gate 下单的 BTC probe 模式：

- 复用 LeadLag BTC trigger 条件：`lead=0.0025`、`max_lead_freshness_ms=5`、`max_lag_freshness_ms=200`，并保留 drift / spread 条件是否启用的配置开关。
- 触发时用本机最新 Gate BBO 计算下单价。
- 输出和当前 `fill_probe_strategy` 类似的 `node.csv`、`lifecycle.csv`、`order_event.csv`。
- 同时记录 signal 条件字段：lead move、Gate price move、spread、Gate local freshness、Gate exchange freshness、signal_to_send。

第一轮 A/B：

| 组别 | 目的 | 订单形态 |
| --- | --- | --- |
| A | 复现当前 BTC probe | 1 GTC + 1 IOC，最小量，entry 对手价 |
| B | 复现 live-like BTC | 4 路 IOC，quantity=3，open slippage=118 ticks |
| C | 隔离 fanout 影响 | 1 路 IOC，quantity=3，open slippage=118 ticks |
| D | 隔离 quantity 影响 | 1 路 IOC，最小量，open slippage=118 ticks |

### 2. 对 open slippage 做小规模 sweep

只在 signal-conditioned probe 中做，不直接改 live 策略。

候选 sweep：

- `0 ticks`：严格对手价。
- `118 ticks`：当前 BTC live 配置，约 2bps。
- `236 ticks`：约 4bps。
- `590 ticks`：约 10bps。

每档输出：

- row fill rate。
- parent group any-fill rate。
- average / p50 / p90 exec slippage ticks。
- filled order 的 net PnL proxy。
- unfilled / cancelled 的 Gate lifecycle 分布。

决策原则：不能只看 fill rate；必须同时看执行滑点和后续 close / PnL proxy。

### 3. 在 report 中加入“发单时 Gate BBO”反事实字段

当前 report 有 signal 触发时的 lead / lag BBO 和 order price，但缺少 request-send 这一刻的最新 Gate BBO 对比。建议后续增加诊断字段或单独 CSV：

- `send_gate_bid`
- `send_gate_ask`
- `send_gate_local_ns`
- `send_gate_freshness_ns`
- `order_crosses_send_touch`
- `order_offset_vs_send_touch_ticks`
- `gate_bbo_move_since_signal_ticks`
- `gate_bbo_move_since_signal_bps`

这些字段可以直接回答：订单进入 gateway 时，limit price 是否仍然 crossing 当前 Gate touch。

### 4. 分开报告 row fill rate 和 parent group fill rate

多路 fanout 下，row fill rate 和策略语义上的 group fill rate含义不同。后续 live report / probe report 应固定输出：

- `submitted_entry_order_rows`
- `filled_entry_order_rows`
- `entry_row_fill_rate`
- `submitted_entry_parent_groups`
- `entry_parent_groups_any_filled`
- `entry_group_fill_rate`
- `filled_quantity / submitted_quantity`

### 5. 单独分析 Gate lifecycle tail

继续使用 `latency.csv` 和 gateway log，把低成交样本按以下字段分桶：

- `ack_rtt_ns`
- `exchange_lifecycle_ns`
- `ack_exchange_process_ns`
- `response_exchange_ns - ack_exchange_ns`
- `finish_exchange_ns - ack_exchange_ns`
- `tcp_info_rtt_us`
- `route_id`
- `order_session_id`

如果 signal-conditioned probe 复现出 live 的高 lifecycle tail，再分析 gateway / session / exchange 侧原因；如果复现不出来，说明 30-symbol live 的负载、symbol mix 或 run 时段市场状态是主要变量。

## 注意事项

- 不要把 2026-07-04 BTC probe 的 99% fill rate 当作 LeadLag live 预期成交率。
- 不要仅凭 Gate freshness 调整策略。BTC live 的 freshness 足够新但成交率仍低。
- 不要只用 filled rows 评估 latency。Cancelled rows 的 lifecycle 分布对 IOC fillability 更关键。
- 调大 slippage 可能提高成交率，但也可能把 adverse selection 显性转化为更差成交价；必须和 PnL proxy 一起看。
- 所有性能或成交率结论必须绑定具体 run dir、配置、时间窗口和 CSV 口径。
