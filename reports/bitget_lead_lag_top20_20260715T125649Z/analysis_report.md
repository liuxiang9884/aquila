# Bitget LeadLag 20-symbol 一小时实盘分析报告

## 结论摘要

本轮 `bitget_lead_lag_top20_20260715T125649Z` 在 2026-07-15 13:01:19–14:01:19 UTC 完成一小时真实订单运行。Strategy 正常退出，gateway、feedback 和两路 fusion 均已停止，guard 返回 `normal_exit_flat`，最终 REST 证明无 open order、无 position。

本轮首次取得 Bitget signal-conditioned LeadLag 的完整真实成交闭环：RAVEUSDT 空单开仓 17 张后，以同量 reduce-only 买单平仓，持仓约 376.949 ms。按 Bitget REST 返回的实际成交价和 `feeDetail` 计算，毛 PnL 为 `+0.02397 USDT`，实际手续费为 `0.00209184 USDT`，净 PnL 为 `+0.02187816 USDT`。

这条证据证明了 signal → entry → independent feedback → reduce-only exit → final flat 的实盘链路，但只有一个闭合 position，不能据此判断策略长期 PnL、跨 symbol fillability 或 high-speed endpoint 收益。

| 结论项 | 结果 | 证据边界 |
| --- | --- | --- |
| Signal-conditioned Bitget LeadLag live 链路 | PASS | 117 signals、41 submitted orders、41 terminal feedbacks |
| 完整开平仓 | PASS | RAVEUSDT short 17，1 entry fill + 1 exit fill |
| Fresh-run isolation | PASS | manifest v2、run-specific gateway/feedback SHM、fanout=1 |
| Quiescence 与 final flat | PASS | strategy/gateway/feedback/fusion 全部退出，REST final flat |
| Unknown/reconcile/continuity | PASS | 全部为 0 |
| 跨 symbol fillability | INCONCLUSIVE | 39 个 entry 中仅 RAVE 1 个成交 |
| 策略收益能力 | INCONCLUSIVE | 仅 1 个 closed position |
| High-speed endpoint | NOT TESTED | 本轮使用 normal/available endpoint |
| Fee 参数一致性 | NEEDS ACTION | 配置 1.5 bps/side，REST 实际 2 bps/side |

## 运行范围与实际配置

- Branch / commit：`feature/bitget-lead-lag-live-parity` / `7e14275`
- Strategy duration：`3600s`
- Order backend：Bitget gateway，`route_count=1`，`fanout=1`
- Strategy PID：`4124499`
- Guard PID：`4124464`
- Gateway PID：`4123646`
- Feedback PID：`4123647`
- Binance fusion PID：`4123365`
- Bitget fusion PID：`4123367`
- Market data events delivered：`13,221,609`
- Lead / lag：Binance / Bitget
- 实际 freshness：lead `5ms`、lag `200ms`
- 开仓/平仓 slippage：按当前 Bitget tick 换算约 `2bps`
- Stop-loss slippage：按当前 Bitget tick 换算约 `5bps`
- 配置 taker fee：`0.00015`，即 1.5 bps/side
- 实际 endpoint 类型：normal/available，不是 high speed

实际 WebSocket / REST 地址：

- Bitget public SBE：`wss://vip-ws-uta.bitget.com/v3/ws/public/sbe`
- Bitget order / feedback：`wss://vip-ws-uta.bitget.com/v3/ws/private`
- REST baseline、fee 与 final flat：`https://api.bitget.com`
- 未使用的 high-speed public endpoint：`wss://vip-ws-uta-pub-a.bitget.com/v3/ws/public/sbe`

20 个目标 symbol 为：BTC、SOL、DOGE、XRP、HYPE、TAC、ZEC、ORDI、WLD、SLX、UB、VELVET、BTW、RAVE、SUI、AVAX、ENA、BAS、H、LINK。Gate rank 9 的 AIGENSYN 在 Bitget 不可用，因此使用 rank 21 的 LINK 补足 20 个。

本轮实际使用 `5ms / 200ms`，不是更早讨论过的 `3ms / 200ms`。在已记录的 117 个 signal 中，lead freshness 最大仅 2.000152 ms，因此这些已触发 signal 即使改为 3ms 也仍会通过；该反事实只覆盖已记录 signal，不能外推到未触发事件。

## Signal 到订单的漏斗

```text
117 signals
  ├─ 36 stale_lag_quote
  └─ 81 freshness pass
       ├─ 39 parallel_limit
       ├─  1 zero_quantity
       └─ 41 submitted orders
            ├─ 39 cancelled
            └─  2 filled = 1 entry + 1 reduce-only exit
```

| 阶段 | 数量 | 占全部 signal |
| --- | ---: | ---: |
| Signal | 117 | 100.00% |
| Freshness pass | 81 | 69.23% |
| Stale lag rejected | 36 | 30.77% |
| Submitted order | 41 | 35.04% |
| Entry order | 39 | 33.33% |
| Exit order | 2 | 1.71% |
| Filled order | 2 | 1.71% |
| Closed position | 1 | 0.85% |

`parallel_limit=39` 来自 `fanout=1` 下同批或重叠 signal 的正常抑制。`zero_quantity=1` 出现在 RAVEUSDT：价格相对 launch 时上涨后，固定 `open_notional` 算出的数量低于当前最小合法数量 17，策略按现有语义拒绝而不是 clamp；该 signal 没有向交易所发单。

标准 `report.md` 中的 `Gate send ok: 0` 是通用 Gate 模板字段，不表示 Bitget 发单失败。Bitget 本轮的有效发送证据是 41 个 direct Ack、41 个 independent terminal feedback 和 41 个 finished order。

## Symbol 分布

| Symbol | Signals | Submitted | Cancelled | Filled | Parallel reject | Stale reject | Zero qty |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| BASUSDT | 15 | 2 | 2 | 0 | 0 | 13 | 0 |
| BTWUSDT | 3 | 1 | 1 | 0 | 0 | 2 | 0 |
| HUSDT | 14 | 3 | 3 | 0 | 11 | 0 | 0 |
| ORDIUSDT | 10 | 4 | 4 | 0 | 6 | 0 | 0 |
| RAVEUSDT | 22 | 12 | 10 | 2 | 9 | 0 | 1 |
| SOLUSDT | 1 | 1 | 1 | 0 | 0 | 0 | 0 |
| TACUSDT | 14 | 3 | 3 | 0 | 1 | 10 | 0 |
| UBUSDT | 14 | 3 | 3 | 0 | 7 | 4 | 0 |
| VELVETUSDT | 19 | 9 | 9 | 0 | 3 | 7 | 0 |
| ZECUSDT | 5 | 3 | 3 | 0 | 2 | 0 | 0 |

BTC、DOGE、XRP、HYPE、WLD、SLX、SUI、AVAX、ENA、LINK 在本小时没有触发 signal。只有 10/20 symbols 进入 signal 表，只有 RAVEUSDT 产生实际成交，因此本轮不支持对其余 symbol 的 fillability 作正面或负面结论。

## 成交与实际 PnL

RAVEUSDT position lifecycle：

1. Short entry：sell 17，limit `0.30777`，实际均价 `0.30833`，exchange order id `1461278351278354442`。
2. 第一次 reduce-only close：buy 17，IOC 未成交并取消。
3. 约 0.36 秒后第二次 reduce-only close：buy 17，limit/实际均价 `0.30692`，exchange order id `1461278352859607041`。
4. Position 关闭，`active_groups=0`；随后的 REST snapshot 与最终 guard snapshot 均为 flat。

| 项目 | 数值 |
| --- | ---: |
| Entry quantity | 17 |
| Entry average price | 0.30833 |
| Exit average price | 0.30692 |
| Holding time | 376.949052 ms |
| Gross PnL | +0.02397000 USDT |
| Entry fee | 0.00104832 USDT |
| Exit fee | 0.00104352 USDT |
| Total actual fee | 0.00209184 USDT |
| Net PnL | **+0.02187816 USDT** |
| Net / entry executed notional | +0.4174% |

PnL 计算：

```text
gross = (0.30833 - 0.30692) * 17 = 0.02397 USDT
net   = 0.02397 - 0.00104832 - 0.00104352
      = 0.02187816 USDT
```

### Fee 配置偏差

Bitget 两张成交订单的 `feeDetail` 对应约 2 bps/side，而本轮 `lag_taker_fee=0.00015` 只计 1.5 bps/side。按 REST `cumExecValue` 估算：

- 配置预计 round-trip fee：`0.00156888 USDT`
- 实际 round-trip fee：`0.00209184 USDT`
- 低估：`0.00052296 USDT`，即每侧少计 0.5 bps

这不会改变本次 position 为正 PnL 的符号，但会使策略门槛和离线净 PnL 偏乐观。下一轮真实订单前，应以当前账户 tier 的实际 taker fee 更新或保守覆盖配置。

标准生成器的 `report.md` 显示 gross/net PnL 为 0，是因为当前 Bitget instrument catalog 没有给 report parser 提供 `contract_multiplier`，order feedback 也没有携带实际 fee。该 0 不是账户真实 PnL；本分析使用归档的 Bitget REST `order-info.feeDetail` 纠正。

## 下单金额与 fillability

39 个 entry order 的 limit notional：

- 最小：`5.09728 USDT`
- 中位数：`5.22404 USDT`
- 最大：`7.87420 USDT`（SOLUSDT 最小数量 0.1）
- Entry 成交率：`1 / 39 = 2.56%`
- 全部 submitted order 成交率：`2 / 41 = 4.88%`，但其中一个 fill 是平仓，不能作为 entry fillability 使用

本轮只证明当前 IOC/slippage 参数在 RAVEUSDT 的一次信号上可完成开平仓。38 个未成交 entry 均获得独立 cancelled terminal，说明订单状态链路完整，但不证明这些 symbol 不可成交；样本受 signal 时刻、BBO、2bps limit、最小数量和一小时窗口共同影响。

## Freshness 分析

| 指标 | Lead freshness | Lag freshness |
| --- | ---: | ---: |
| Min | 0.465578 ms | 0.369968 ms |
| Median | 1.236074 ms | 9.467497 ms |
| P95 | 1.560895 ms | 3431.929203 ms |
| Max | 2.000152 ms | 18672.053576 ms |

Lead 侧所有已触发 signal 均明显低于实际 5ms threshold，lead freshness 在本轮不是主要阻断。Lag 侧是主要 freshness filter：

- Freshness pass 的 lag freshness：median `2.672278ms`、P95 `179.377149ms`、max `189.249408ms`
- Stale lag reject：36 次，median `799.905721ms`、max `18.672054s`
- 200ms threshold 挡掉 30.77% signal；通过样本的 P95 已接近门限

因此不能简单放宽 lag threshold 来增加订单数；stale 样本存在秒级长尾，放宽会直接改变信号含义。若要优化，应先按 symbol 分析 Bitget BBO 更新稀疏、fusion 时序和 signal burst，而不是从本轮数据直接扩大 threshold。

## 延迟分析

标准 CSV 的 41 个 latency row 中，有 2 个 Ack RTT 记录为 0：SOLUSDT `432345564227567618` 和 RAVEUSDT `432345564227567651`。两笔都是 independent cancelled terminal 先到，Ack 后到；strategy 在 terminal 时已完成订单，因此 `ack_local_receive_ns` 没有回填到 finished snapshot。日志仍保留了后到 Ack，可由相同本地时钟重建 RTT。

| 指标 | 标准 CSV（含 2 个零值） | 使用 late Ack 日志修正 |
| --- | ---: | ---: |
| Samples | 41 | 41 |
| Average | 5.243 ms | 5.836 ms |
| Median | 5.345 ms | 5.359 ms |
| P95 | 5.804 ms | 10.212 ms |
| Max | 10.212 ms | 13.375 ms |

两笔重建 RTT：SOLUSDT `13.375032ms`，RAVEUSDT `10.940730ms`。因此标准 report 的 P95 被零值明显压低，分析延迟时应使用修正列。

Order send-to-finish 是完整本地时钟域数据：

- Average：`6.044ms`
- Median：`5.830ms`
- P95：`6.739ms`
- Max：`10.540ms`

39 个同时具有 Ack 与 terminal exchange timestamp 的样本中，23 个 `exchange_lifecycle=-1ms`，16 个为 `0ms`。这反映 Bitget 毫秒级字段和 Ack/terminal 消息排序语义，不能解释为真实的负处理耗时。

当前标准 report 中的“上行 / Gate x_in→x_out / 下行”标签是 Gate 通用模板；Bitget 没有提供对应 `x_in/x_out` 字段，本轮只能可信使用 total Ack RTT、send-to-finish 和本地消息顺序，不能做三段网络归因。

## 运行安全与收尾证据

- Guard preflight：flat
- 运行中 10/20/30/40/50 分钟 REST snapshots：均无 open orders、无 positions
- 首次 fill 后立即 REST snapshot：flat
- Strategy exit：`exit_code=0`、`runtime_exit_code=0`
- Order responses / feedbacks：`41 / 41`
- Unknown local feedback：0
- Duplicate/stale feedback：0
- Feedback continuity lost：0
- `needs_reconcile=false`
- `manual_intervention=false`
- Gateway/feedback quiescence：`ok=true`
- Final REST：open orders `[]`、positions `[]`
- Guard result：`normal_exit_flat`

## 前置失败尝试

正式 run 前的 `bitget_lead_lag_top20_20260715T124447Z` 使用了过低的 `open_notional=0.001`。生产 sizing 语义在低于最小数量时返回 `zero_quantity`，不会自动 clamp 到交易所最小数量；该 run 产生 5 个 VELVET signal，但没有向交易所提交订单。Guard 随即停止交易栈并完成 20-symbol final flat。

该失败 run 不计入本报告的 117 signals / 41 orders / PnL。它与正式 run 中再次出现的 1 次 RAVE `zero_quantity` 共同说明：按启动时价格设置固定 `1.05x` 最小名义金额，不能保证一小时内始终得到最小合法数量。

## 后续建议

1. 在下一轮真实订单前，把 Bitget taker fee 从 1.5 bps/side 校准到当前账户实际 2 bps/side，或使用更保守值；重新核对 signal threshold 的费用覆盖。
2. 明确最小数量策略：动态按最新价格生成交易所最小合法 quantity，或为固定 notional 留出可证明的价格漂移 buffer；避免 `zero_quantity` 造成样本选择偏差。
3. 修复 report 的 Bitget PnL 数据源：补齐 contract multiplier 语义并接入 REST/fill fee，避免标准报告把真实盈利显示为 0。
4. 修复 terminal-before-Ack 的 latency 聚合：订单完成后仍应保留 late Ack receive timestamp，避免 Ack P95 被零值压低。
5. 如果比较 normal 与 high-speed public endpoint，使用独立 fresh run / A-B market-data 进程；本轮不能支持 high-speed 更快或更慢的结论。
6. 继续积累多个时间窗口和更多 closed positions 后，再评价 symbol fillability、胜率、PnL 分布与 fee-adjusted edge；当前 `1/1` 胜率没有统计意义。

## 归档文件

- `report.md`：标准生成报告
- `signal.csv`：117 条 signal
- `order_detail.csv`：signal/order/reject 明细
- `position.csv`：1 条 closed position
- `latency.csv`：41 条 submitted order latency
- `runtime_configs/`：本轮实际 strategy/gateway/feedback/data 配置和 manifest
- `evidence/`：RAVE entry/exit REST order info、post-fill 与 final-flat REST snapshots
- `logs/`：strategy、gateway、feedback 与 guard 原始日志
- `inputs/`：instrument catalog、最小金额计划和 binary SHA-256

## 证据边界

本报告只陈述该 run 的实际证据。它不证明未来 PnL、持续 fillability、high-speed endpoint 优势、跨账户 fee 一致性或无人值守运行安全；历史 final flat 也不能替代下一轮真实订单前的新鲜 REST baseline。
