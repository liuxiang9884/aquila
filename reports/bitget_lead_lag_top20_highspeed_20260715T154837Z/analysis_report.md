# Bitget LeadLag 20-symbol high-speed 10 小时实盘分析报告

## 结论摘要

本轮 `bitget_lead_lag_top20_highspeed_20260715T154837Z` 于 2026-07-15
15:55:45–2026-07-16 01:55:46 UTC 完成 10 小时真实订单运行。Strategy 正常退出，
gateway、feedback、两路 fusion 与 watchdog 均已停止；guard 返回
`normal_exit_flat`，quiescence 通过，最终 REST 证明无 open order、无 position。

本轮使用 Bitget high-speed public SBE endpoint、`3ms / 200ms` lead/lag freshness、
20 个 symbol 和各合约启动时计算的最小开仓量。共观察到 644 个 signal、211 个真实
订单、21 个成交开仓和 21 个完整平仓。所有成交仓位最终闭合，无 unknown、
continuity lost、reconcile 或人工介入。

按 Bitget `/api/v3/trade/fills` 返回的 `execPnl` 与实际 `feeDetail` 计算，毛 PnL 为
`+0.00455200 USDT`，手续费为 `0.03991720 USDT`，净 PnL 为
`-0.03536520 USDT`。费前胜率为 `52.38%`（11 胜、9 负、1 平），排除持平为
`55.00%`；费后胜率为 `47.62%`（10 胜、11 负）。

最重要的结论不是绝对亏损金额，而是费用覆盖不足：全部成交均为 taker，账户实际费率
约为每侧 `2bps`，而配置仍为每侧 `1.5bps`。本轮费前 edge 只有约
`+0.456bps/entry notional`，不足以覆盖约 `4bps` round-trip fee，费后变成约
`-3.544bps/entry notional`。

| 结论项 | 结果 | 证据边界 |
| --- | --- | --- |
| 10 小时 high-speed public live run | PASS | Strategy `exit_code=0`，实际运行 36000 秒 |
| Signal-conditioned 开平仓闭环 | PASS | 21 个 entry fill、21 个 matched exit fill |
| Partial fill 处理 | PASS | 4 个 partial entry 全部按实际成交量平仓 |
| Close retry | PASS with tail | 6/21 仓位需要重试，最多 3 次，最终全部 flat |
| Fresh-run isolation | PASS | manifest v2、run-specific gateway/feedback SHM、fanout=1 |
| Quiescence 与 final flat | PASS | guard `normal_exit_flat`、quiescence `ok=true` |
| Unknown/reconcile/continuity | PASS | 全部为 0 |
| Entry any-fill rate | 11.67% | 21 / 180；只覆盖本轮 IOC/slippage/时段 |
| Fee-adjusted PnL | NEGATIVE | `-0.03536520 USDT`，21 个 closed positions |
| High-speed 相对 normal 的收益 | INCONCLUSIVE | 本轮没有同时间、同信号的 normal endpoint 对照 |
| Account limiter 压力 | NOT STRESSED | 10 小时仅 211 次 submit，不能验证高请求量场景 |

## 运行范围与实际配置

- Branch / launch HEAD：`feature/bitget-lead-lag-live-parity` / `eade122`
- Strategy duration：`36000s`
- Lead / lag：Binance / Bitget
- Bitget public SBE：`wss://vip-ws-uta-pub-a.bitget.com/v3/ws/public/sbe`
- Bitget private order/feedback：`wss://vip-ws-uta.bitget.com/v3/ws/private`
- Bitget REST：`https://api.bitget.com`
- Freshness：lead `3ms`、lag `200ms`
- Order backend：Bitget gateway，`route_count=1`，`fanout=1`
- 每组 `parallel=1`
- 配置 `lag_taker_fee=0.00015`，实际 fill fee 约为 `0.0002`
- Strategy PID：`4162543`
- Guard PID：`4162539`
- Gateway PID：`4162204`
- Feedback PID：`4162203`
- Binance fusion PID：`4161687`
- Bitget fusion PID：`4161688`
- Watchdog PID：`4164926`
- Strategy 收到的 market-data events：`49,909,825`

20 个目标 symbol 为：BTC、SOL、DOGE、XRP、HYPE、TAC、ZEC、ORDI、WLD、SLX、
UB、VELVET、BTW、RAVE、SUI、AVAX、ENA、BAS、H、LINK。

本轮按启动时 ticker 和 `1.05` buffer 计算固定 `open_notional` 与各合约最小 quantity。
这能限制单笔 notional，但不能保证价格变化后始终产生合法最小数量，也不是账户级累计
请求 limiter。

## Signal 到订单漏斗

```text
644 signals
  ├─ 31 exit signals -> 31 submitted exit orders
  └─ 613 entry signals
       ├─ 174 stale_lag_quote
       ├─   1 stale_lead_quote
       └─ 438 freshness pass
            ├─ 241 parallel_limit
            ├─  17 zero_quantity
            └─ 180 submitted entry orders
                 ├─ 159 zero fill
                 ├─   4 partial fill
                 └─  17 full fill
```

| 阶段 | 数量 | 比例 |
| --- | ---: | ---: |
| 全部 signal | 644 | 100.00% |
| Entry signal | 613 | 95.19% |
| Freshness reject | 175 | Entry 的 28.55% |
| Freshness pass | 438 | Entry 的 71.45% |
| Parallel reject | 241 | Entry 的 39.31% |
| Zero-quantity reject | 17 | Entry 的 2.77% |
| Submitted entry | 180 | Entry 的 29.36% |
| Any-fill entry | 21 | Submitted entry 的 11.67% |
| Full-fill entry | 17 | Submitted entry 的 9.44% |
| Partial-fill entry | 4 | Submitted entry 的 2.22% |
| Closed position | 21 | Any-fill entry 的 100.00% |

`parallel_limit=241` 是 `parallel=1` 下重叠 signal 的预期抑制，不是交易所 reject。
`zero_quantity=17` 全部来自 ORDIUSDT（12）和 VELVETUSDT（5），说明启动时按最小数量
加 `1.05` buffer 生成的固定 notional 在 10 小时价格变化后仍可能低于可下单数量。

## Entry fillability

| Symbol | Entry orders | Any fill | Full fill | Any-fill rate |
| --- | ---: | ---: | ---: | ---: |
| BASUSDT | 6 | 0 | 0 | 0.00% |
| BTWUSDT | 6 | 1 | 1 | 16.67% |
| ENAUSDT | 1 | 0 | 0 | 0.00% |
| HUSDT | 20 | 6 | 3 | 30.00% |
| ORDIUSDT | 1 | 0 | 0 | 0.00% |
| RAVEUSDT | 38 | 7 | 7 | 18.42% |
| TACUSDT | 33 | 3 | 3 | 9.09% |
| UBUSDT | 15 | 0 | 0 | 0.00% |
| VELVETUSDT | 54 | 3 | 2 | 5.56% |
| WLDUSDT | 1 | 1 | 1 | 100.00% |
| ZECUSDT | 5 | 0 | 0 | 0.00% |
| **Total** | **180** | **21** | **17** | **11.67%** |

只有 11/20 symbols 产生 signal，只有 6/20 symbols 产生 entry fill。WLDUSDT 的 100%
只有一个样本，不能当作稳定 fillability。VELVET、TAC、RAVE 三者贡献 431/644 signal，
signal 与成交样本都高度集中，不能把总成交率外推到整个 20-symbol universe。

## 实际 PnL 与手续费

实际证据为 `evidence/bitget_fills.json` 中 42 条 run-scoped fills。全部 fill 的
`tradeScope=taker`、`feeCoin=USDT`；21 个 entry 与 21 个 exit 按 strategy 日志中的
`entry_local_order_id` 对账。

| Symbol | Positions | Gross PnL | Actual fee | Net PnL |
| --- | ---: | ---: | ---: | ---: |
| BTWUSDT | 1 | -0.00510400 | 0.00210424 | -0.00720824 |
| HUSDT | 6 | +0.03629000 | 0.00955744 | +0.02673256 |
| RAVEUSDT | 7 | -0.02956000 | 0.01438350 | -0.04394350 |
| TACUSDT | 3 | +0.02082600 | 0.00630288 | +0.01452312 |
| VELVETUSDT | 3 | -0.01790000 | 0.00545014 | -0.02335014 |
| WLDUSDT | 1 | 0.00000000 | 0.00211900 | -0.00211900 |
| **Total** | **21** | **+0.00455200** | **0.03991720** | **-0.03536520** |

| 指标 | 数值 |
| --- | ---: |
| Fee 前盈利 / 亏损 / 持平 | 11 / 9 / 1 |
| Fee 前胜率（持平计入总样本） | 52.38% |
| Fee 前胜率（排除持平） | 55.00% |
| Fee 后盈利 / 亏损 | 10 / 11 |
| Fee 后胜率 | 47.62% |
| Total turnover | 199.5852 USDT |
| Gross PnL / entry notional | +0.456bps |
| Net PnL / entry notional | -3.544bps |
| Avg gross / position | +0.00021676 USDT |
| Avg net / position | -0.00168406 USDT |

最佳单笔为 HUSDT long，净 PnL `+0.02381284 USDT`；最差单笔为 RAVEUSDT long，
净 PnL `-0.04049249 USDT`。单个最差仓位的损失大于本轮总净损失，说明 21 个仓位的
结果仍受单笔尾部显著影响；与此同时，RAVEUSDT 聚合净 PnL `-0.04394350 USDT`，
是本轮主要亏损来源。

### Fee 配置偏差

配置费率为每侧 `1.5bps`，但实际 fills 对应约每侧 `2bps`：

- 按实际 turnover 和配置费率估计 fee：`0.02993778 USDT`
- Bitget 实际 fee：`0.03991720 USDT`
- 低估：`0.00997942 USDT`
- 实际费率比配置高约 33.33%；配置估计比实际费用低 25%

标准 `report.md` 的 gross/net PnL 显示为 `0`，是因为通用 report parser 当前按 Gate
catalog key 查找 multiplier，无法关联 Bitget log 的 `BTCUSDT` 等 symbol；该值不是
账户实际 PnL。本分析页以交易所实际 `execPnl` 和 `feeDetail` 为准。

## Close retry 与持仓尾部

21 个成交仓位共提交 31 个 exit order：

- 15 个仓位第一次 close 即成交；
- 2 个仓位需要 2 次 close；
- 4 个仓位需要 3 次 close；
- 6/21（28.57%）至少发生一次 close IOC 未成交；
- 所有重试最终均以实际 entry filled quantity 完整平仓。

需要重试的 symbol 为 RAVEUSDT（3 个仓位）、VELVETUSDT（2 个）和 HUSDT（1 个）。
Holding time 中位数为 `93.716ms`、P95 为 `2.826s`、最大为 `22.993s`。这证明
stop-and-flat/retry 闭环有效，但 23 秒持仓长尾说明“绝大多数很快平仓”不能替代对
最坏持仓时间和 close retry 行为的持续监控。

## Freshness

613 个 entry signal 中，438 个通过 freshness，174 个因 stale lag 被拒，1 个因
stale lead 被拒。

| Scope | Lead median | Lead P95 | Lag median | Lag P95 | Max |
| --- | ---: | ---: | ---: | ---: | ---: |
| Entry signals | 1.094ms | 1.620ms | 2.914ms | 2236.255ms | Lag 15640.136ms |
| Freshness-pass entry signals | - | - | 1.498ms | 104.603ms | Lag 200.034ms |
| Stale-lag rejects | - | - | 785.183ms | 4880.952ms | Lag 15640.136ms |

3ms lead threshold 只拒绝 1 个 13.067ms 的 WLD entry signal，当前 entry 主体没有被 lead
freshness 限制。200ms lag threshold 拒绝 174 个 entry signal，且 stale lag 存在秒级
长尾。high-speed endpoint 并未消除稀疏 symbol 的 lag quote 老化；这些长尾可能来自
symbol 更新频率、fusion 时序或网络，单凭本轮不能归因。

31 个 exit signal 全部允许提交，其中部分 exit 的 lead freshness 超过 3ms。这符合
平仓不应被 entry freshness gate 阻断的安全语义，不能把 exit 样本混入 entry freshness
合格率。

## Ack 与 terminal latency

标准 CSV 按 finished snapshot 统计 211 个 order：

| 指标 | 标准 CSV | late-Ack 重建后 |
| --- | ---: | ---: |
| Samples | 211 | 211 |
| Min | 0ms | 1.989ms |
| Median | 2.321ms | 2.326ms |
| Average | 2.503ms | 3.640ms |
| P95 | 2.952ms | 4.861ms |
| P99 | 7.259ms | 50.458ms |
| Max | 16.402ms | 79.827ms |

5 个订单的 terminal feedback 先于 Ack 到达，finished snapshot 把 `ack_rtt_ns` 留为 0。
通过同一 strategy 日志的 `request_send_local_ns` 与后到 Ack `local_receive_ns` 重建后，
这 5 个 RTT 为 `13.736ms`、`79.827ms`、`50.458ms`、`43.329ms`、`52.506ms`。
其中后 4 个来自同一个 RAVE position 的 entry 与多次 close 尝试，是本轮 Ack 尾部的
集中事件。

send-to-finish 不受 late Ack 的零值问题影响：median `4.300ms`、average `5.335ms`、
P95 `6.822ms`、max `65.457ms`。Bitget Ack/terminal exchange timestamp 只有毫秒级，
`exchange_lifecycle` 的 `-1ms/0ms/1ms` 只能说明消息时间戳粒度与排序，不能解释为真实
负耗时。

这里的 Ack RTT 来自 private order endpoint，不是 high-speed public market-data
latency。没有 normal public endpoint 的同窗口对照，因此本轮不支持“high-speed 让 Ack
更快”或“high-speed 让策略更赚钱”的因果结论。

## 运行监控与收尾

- Watchdog 每分钟 fast check、每 10 分钟 full check；最后记录
  `EXIT normal final summary observed`。
- `monitor/alert.json` 未生成。
- Strategy summary：responses `211`、feedbacks `214`、continuity lost `0`、
  unknown local feedback `0`、duplicate/stale feedback `0`。
- 211 个 submitted order 全部对应 211 个 finished order，无 unresolved local 或
  exchange order。
- Strategy `exit_code=0`、`runtime_exit_code=0`、`needs_reconcile=false`、
  `manual_intervention=false`。
- Gateway 在 grace 阶段停止；feedback 收到 guard 的 `SIGTERM` 后停止；quiescence
  `ok=true`。
- Guard final REST：open orders `[]`、positions `[]`，结果 `normal_exit_flat`。
- 两路 fusion 与 watchdog 随 run 正常结束，最终没有残留目标 PID。

本轮用户明确授权忽略尚未实现的 UID/account limiter 阻断。实际 10 小时只有 211 次
submit，平均约 `0.0059/s`，因此没有观察到请求量异常；但这个低负载结果不构成账户级
limiter 在高信号量场景下的验证。

## 值得优先处理的事项

1. 将 Bitget fee 参数从 `0.00015` 校准为当前账户实际 `0.0002`，或采用更保守覆盖；
   否则 signal threshold、预期净 PnL 和离线评估都会偏乐观。
2. 重新评估 fee-adjusted edge。本轮 gross edge 仅 `0.456bps`，显著低于约 `4bps`
   round-trip fee；提高成交率本身不能解决费用覆盖不足。
3. 修复标准 report 的 Bitget symbol/catalog 关联与实际 fee 接入，避免 `report.md` 把真实
   PnL 显示为 0。
4. 修复 terminal-before-Ack 的聚合：订单 terminal 后仍应保存 late Ack timestamp；否则
   Ack P95/P99 会被 0 值明显压低。
5. 动态按最新价格计算最小合法 quantity，或给固定 notional 使用可证明的漂移 buffer；
   本轮已有 17 个 zero-quantity reject。
6. 将 close retry 和 holding tail 作为独立指标。本轮 28.57% 的成交仓位至少重试一次，
   最大持仓约 23 秒。
7. 若要评价 high-speed endpoint，使用相同时间窗口的独立 normal/high-speed signal-only
   或 live A/B；本轮单臂运行只能证明链路可用，不能证明相对收益。

## 归档文件

- `report.md`：标准生成报告，顶部已标明 Bitget PnL/Ack 限制。
- `analysis_report.md`：实际 fee/PnL、胜率、fillability、freshness 与修正 latency。
- `signal.csv`：644 条 signal。
- `order_detail.csv`：644 条 signal/order/reject 明细。
- `position.csv`：21 条 closed position；PnL 列受 Bitget catalog 限制为空。
- `latency.csv`：211 条 submitted-order latency。
- `runtime_configs/`：实际 strategy、gateway、feedback、data/fusion 配置和 manifest。
- `inputs/`：binary SHA-256、min-amount plan、authorization、preflight 与 instrument
  catalog。
- `evidence/bitget_fills.json`：Bitget REST 实际 fills、`execPnl` 与 `feeDetail`。
- `evidence/guard_final_summary.json`：quiescence 与 final-flat summary。
- `logs/`：strategy、gateway、feedback 与 guard 原始日志。
- `monitor/`：10 分钟检查流水、watchdog final state 与退出记录。

## 证据边界

本报告只陈述该 run 的实际证据。21 个 closed positions 不足以证明未来 PnL、稳定胜率、
跨时段 fillability 或 high-speed endpoint 的相对优势；历史 final flat 也不能替代下一轮
真实订单前的新鲜 REST baseline。
