# LeadLag 单边平仓与 Drift Guard 评审

## 文档状态

- 评审日期：2026-07-21
- 代码基线：`5a69eaf`（`Merge Gate and Bitget trading latency optimizations`）
- 分析对象：Bitget 单边执行、Binance lead、`parallel=1`、`fanout=1` 的 LeadLag 实盘
- 证据截止：`2026-07-21 01:29:41.004235116 UTC`
- 性质：现状分析与后续设计建议，不修改当前交易行为

本文独立记录本次实盘中 `parallel_limit`、`drift_guard`、HUSDT 信号和平仓行为的发现、讨论结论及建议。文中计数和 PnL 均为证据截止时的快照，不是仍在运行的 12 小时任务的最终报告。

## 结论摘要

1. `parallel_limit=501` 主要是每个 pair 在 `parallel=1` 下的预期并发约束。它统计的是同一 pair 已有 active/pending group 时被拒绝的候选信号，不等于错过了 501 笔彼此独立且都可成交的交易，也不是此前已删除的额外 limiter。
2. `drift_guard=241` 高度集中于一次 HUSDT 市场事件：HUSDT 占 232 次，且这些拒绝集中在约 21 秒内的 7 个信号簇。生产日志把 `drift_guard` 作为一个汇总原因记录，不能直接区分 instant、`ratio_std` 和 `drift_mean` 子原因。
3. HUSDT 的前 9 个 OpenShort 信号出现在 Bitget 明显高于 Binance 的真实偏离中，并非明显的静态映射错误。随后 223 个 OpenLong 信号处在快速回归阶段；基于 recorder BBO 的复算表明，它们可由 1 分钟 `ratio_std` 的事件后余震解释。
4. 对第一笔 HUSDT OpenShort 做状态化反事实：若按首个信号成交，并在首个反向信号处平仓，理论 BBO 路径在扣除双边 4 bps fee 后仍约为 `+425.7 bps`；按配置的双边 2 tick 最差限价估算约为 `+419.1 bps`，10 USDT 名义金额约 `+0.419 USDT`。这是可成交性较强的反事实，不是实际成交或已实现 PnL。
5. 当前平仓的核心是“价差收敛即 alpha 失效”，而不是“盈利才平仓”。这一原则对单边 LeadLag 仍然合理，不能增加必须盈利才允许退出的条件；但仅依赖价差收敛和 1% trailing stop，不足以完整约束单边方向风险。
6. 不建议根据一次 HUSDT 事件直接关闭 drift guard 或调整阈值。优先做跨 symbol、跨时段的 stateful replay，区分“异常偏离保护”和“真实可交易偏离被事件后波动率阻断”，再决定是否修改 guard 语义。

## 实盘快照与证据边界

### 运行配置

本次分析对应本机运行目录：

```text
/home/liuxiang/tmp/20260720_162559_bitget_combined46_n6_fanout1_12h
```

关键配置如下：

- Bitget combined 46 symbols
- Bitget fusion `n=6`，其中 HS 3、HA 3
- Binance fusion `n=4`
- `order_session_fanout=1`
- `parallel=1`
- Release build，代码基线 `5a69eaf`
- 不包含已明确删除的额外 limiter

本次分析使用的主要运行日志为：

```text
logs/strategy_20260720_163613.log
```

### 截止时统计

| 指标 | 数值 |
| --- | ---: |
| 开仓候选信号 | 1,163 |
| OpenLong | 774 |
| OpenShort | 389 |
| 实际开仓下单 | 242 |
| signal → order | 20.81% |
| 开仓 full fill | 20 |
| 开仓 partial fill | 0 |
| order → fill | 8.26% |
| signal → fill | 1.72% |
| `parallel_limit` 拒绝 | 501 |
| `drift_guard` 拒绝 | 241 |
| `stale_lag_quote` 拒绝 | 166 |
| `stale_lead_quote` 拒绝 | 13 |

截至快照，20 个已开仓 position 均已平仓，REST 查询没有遗留 position。

实际 Bitget REST fills 汇总：

| 指标 | 数值 |
| --- | ---: |
| Gross execution PnL | `-0.05599100 USDT` |
| Fee | `-0.03241536 USDT` |
| Net PnL | `-0.08840636 USDT` |
| Net winner | 5 / 20 |
| Gross winner | 7 / 20 |
| Entry slippage，按 signal raw 加权 | `0.4649 bps` |
| Exit slippage，按 signal raw 加权 | `0.6178 bps` |

以 `187.3439 USDT` entry notional 分解：

| 组成 | PnL | 折算 |
| --- | ---: | ---: |
| Raw-reference alpha | `-0.035711 USDT` | `-1.906 bps` |
| Execution slippage | `-0.020280 USDT` | `-1.083 bps` |
| Fee | `-0.032415 USDT` | `-1.730 bps` |
| Net | `-0.088406 USDT` | `-4.719 bps` |

全部 42 个 execution fill 均为 taker。本样本说明 entry alpha、执行滑点和费用都在侵蚀结果，不能仅凭亏损笔数把问题归因于平仓条件。

### 证据限制

- 本文只覆盖上述时间截面的日志、recorder BBO 和 REST fills；不能代替完整 12 小时最终 report。
- 被 guard 拒绝的订单没有实际送达交易所，因此其 fill 和 PnL 只能做反事实估计。
- 多个相邻 signal 高度相关。在 `parallel=1` 下不能把每个 signal 当作一笔独立可执行交易，更不能直接累加 232 个 HUSDT signal 的理论收益。
- 生产日志只记录最终 reject reason。某一 guard 先命中后，后续 guard 是否也会命中通常不可从同一条日志得知。

## `parallel_limit` 分析

`parallel_limit` 是 LeadLag 策略对每个 pair/symbol 的 active/pending order group 数量限制。本次各 pair 配置为 `parallel=1`：只要该 pair 已有一组订单正在下单、持仓、平仓或等待终态，该 pair 的新开仓候选就会被拒绝；不同 pair 之间不会因为这个计数相互阻断。

因此，`parallel_limit=501` 应按以下方式理解：

- 它是 501 次候选 signal 被当前单组状态占用所阻断，不是 501 笔相互独立的可执行订单。
- LeadLag signal 会在短时间内重复出现，同一市场机会可能产生多次候选；计数天然高于可独立交易的机会数。
- `parallel_limit` 在开仓检查顺序中早于 `drift_guard` 和 freshness 检查。被计为 `parallel_limit` 的 signal 可能同时存在 drift 或 stale 问题，但日志只保留先命中的原因。
- 该限制与此前讨论并删除的额外 limiter 不同；它是当前策略的并发 position/order group 风险边界。

在没有完成多 position 资金占用、方向聚合、异常恢复和 stop-and-flat 评审前，不建议为了提高 signal → order 比例直接增加 `parallel`。如需评估，应先用 replay 把重复 signal 合并成 stateful opportunity，再估计并发增加的边际收益和尾部风险。

## `drift_guard` 现状与本次发现

### 当前保护条件

实现入口为 `strategy/lead_lag/drift_guard.h`。对 raw BBO mid 计算：

```text
ratio = lag_mid / lead_mid
```

满足任一条件即拒绝开仓：

```text
abs(ratio - 1) > 1.5%
ratio_std_1m > 0.8%
abs(ratio_mean_1m - 1) > 2%
```

该 guard 只阻断开仓，不阻断正常平仓或 stoploss。

### 241 次拒绝的分布

| Symbol | 数量 | 占比 |
| --- | ---: | ---: |
| HUSDT | 232 | 96.27% |
| BSBUSDT | 8 | 3.32% |
| USUSDT | 1 | 0.41% |

基于拒绝时刻 recorder BBO 的直接 ratio：

- 18 次 signal 在该时刻已超过 1.5% instant threshold：HUSDT 9、BSBUSDT 8、USUSDT 1。
- HUSDT 其余 223 次的即时偏离已经回落到 1.5% 内，但用相邻 recorder BBO 复算可由 1 分钟 `ratio_std` 超限解释。
- 本次复算没有发现必须单独依赖 `drift_mean` 才能解释的样本。

这里必须保留证据边界：生产日志没有记录 drift 子原因，因此“223 次由 `ratio_std` 解释”是 recorder BBO 复算结论，不是 live log 对内部命中分支的逐条直接证明。

### Guard 与 freshness 的归因顺序

开仓路径先检查 `parallel_limit`，再检查 `drift_guard`，freshness 则在后续 external submit 路径检查。因此：

- `drift_guard` reject 日志中的 `freshness_guard_pass=true` 只是该字段在该阶段的默认状态，不能证明 quote 一定 fresh。
- HUSDT 232 次中仅 6 次 lead freshness 超过 3 ms，lag 没有超过 500 ms；绝大多数 HUSDT reject 不能用 stale quote 解释。
- BSBUSDT 8 次中有 1 次 lag quote 约 stale 3.899 s，其余 7 次 fresh。
- USUSDT 的 1 次 reject 为 fresh，instant deviation 约 1.506%，刚超过 1.5% 阈值。

### HUSDT 事件形态

HUSDT 的 232 次拒绝集中在约 21 秒，而不是 232 个独立事件：

| 时间段（UTC） | 方向 | 数量 |
| --- | --- | ---: |
| 18:12:35.640–18:12:35.722 | OpenShort | 9 |
| 18:12:37.326–18:12:37.657 | OpenLong | 98 |
| 18:12:38.901–18:12:38.972 | OpenLong | 17 |
| 18:12:40.144–18:12:40.199 | OpenLong | 10 |
| 18:12:41.579–18:12:42.200 | OpenLong | 81 |
| 18:12:46.580–18:12:47.000 | OpenLong | 6 |
| 18:12:56.448–18:12:56.478 | OpenLong | 11 |

首个 OpenShort signal 的 raw mid 偏离约为 `+5.92%`，事件期间观测到的最大偏离为 `+18.02%`，其余 8 个 OpenShort signal 的即时偏离约为 `11%–12.7%`。recorder 显示 Bitget 随后快速跟随，因而这更像一次真实、剧烈且快速收敛的市场事件，而非明显的静态 symbol 映射错误。

这次事件同时暴露了 guard 的两种作用：

- instant threshold 阻止在极端跨市场偏离时立即追单，提供异常行情保护。
- 事件开始回归后，1 分钟 `ratio_std` 仍会保留“余震”，继续阻断已经回到 instant threshold 内的反向 signal。

前者是明确的安全价值；后者是否过度阻断真实机会，需要更长窗口和更多 symbol 的 stateful replay 才能判断。

## HUSDT 反事实分析

### 第一笔 OpenShort → 首个反向信号

首个 HUSDT OpenShort signal：

```text
18:12:35.640 UTC
sell raw price = 0.05911
```

首个 OpenLong 条件出现：

```text
18:12:37.326 UTC
buy raw price = 0.05657
holding time = 1,685.483 ms
```

`SignalEngine::OnLeadTick` 会先为已有 position 检查 normal close，再检查新开仓。如果 short 已经存在，同一个 OpenLong 市场条件也满足 CloseShort。因此，在“首单成功成交”的假设下，以上两点可构成一条符合当前状态机顺序的开仓—平仓反事实路径。

| 估算口径 | 结果 |
| --- | ---: |
| Raw BBO gross | `+429.707 bps` |
| 扣双边 4 bps fee | `+425.707 bps` |
| 双边均按 2 tick 最差限价成交，扣 fee | `+419.083 bps` |
| 10 USDT notional 对应估算 | 约 `+0.419 USDT` |

首个 sell signal 的连续 marketability 窗口约为 4.136 ms。本次实盘实际 entry chain 的 decision → request send 为 p50 `0.007349 ms`、p95 `0.008632 ms`，decision → local ack 为 p50 `2.347 ms`、p95 `2.993 ms`。这些时延支持“首单具有实际成交可能性”，但 local ack 不是交易所到达或成交证明；由于 guard 已拒单，不能把该反事实写成实际收益。

### 后续 OpenLong signal

对 223 个 OpenLong signal 使用未来可执行 Bitget bid 计算 forward return。下表“正收益 signal 比例”已扣除 round-trip 4 bps fee，“Mean gross return”则为扣 fee 前的均值：

| Horizon | 正收益 signal 比例 | Mean gross return |
| --- | ---: | ---: |
| 100 ms | 13.90% | `-26.95 bps` |
| 500 ms | 59.20% | `+59.14 bps` |
| 1 s | 61.00% | `+63.58 bps` |
| 2 s | 94.17% | `+96.67 bps` |
| 5 s | 92.38% | `+134.93 bps` |
| 10 s | 97.31% | `+167.54 bps` |

在配置的 2 tick buy limit 内，未来可成交性随时间下降：

| Horizon | Limit 内可成交比例 |
| --- | ---: |
| 0.5 ms | 99.10% |
| 1 ms | 98.20% |
| 2 ms | 94.17% |
| 3 ms | 92.38% |
| 5 ms | 85.65% |
| 10 ms | 74.89% |
| 20 ms | 65.92% |

这些 signal 是同一次事件中的重复、相关观察。`parallel=1` 时最多只能形成状态化的“一笔 short、随后平 short、再视状态决定是否开一笔 long”，不能把 223 次理论 return 累加。精确结论仍需带 order feedback、pending state、成交时序、fee 和重试规则的 replay。

## 当前单边平仓语义

实现入口主要为：

- `strategy/lead_lag/signal.h`
- `strategy/lead_lag/threshold.h`
- `strategy/lead_lag/strategy.h`
- `strategy/lead_lag/execution_state.h`

策略传入 `SignalEngine` 的 lead 是经 rolling mean 对齐的 Binance lead，lag 是 raw Bitget quote。

### Normal close

Long position：

```text
gap = drifted_lead.bid / lag.bid - 1
close long when gap < up_exit
```

Short position：

```text
gap = drifted_lead.ask / lag.ask - 1
close short when gap > down_exit
```

其中：

```text
up_exit = +exit_band
down_exit = -exit_band
```

`pair.trigger.close` 初始为 `0.0005`（5 bps）；threshold rolling update 后，`exit_band` 使用 `alignment.drift_std_ema`，因此持仓期间的 close threshold 可能随滚动统计变化。

Normal close 使用 Bitget 可执行侧 raw price：long 按 lag bid 卖出，short 按 lag ask 买回。初次限价应用 `close_slippage_ticks`，失败重试每次再增加 `close_retry_slippage_step_ticks`。以 HUSDT 当前 2 tick、两次 retry 为例，价格容忍依次为 2、4、6 tick。

### Trailing stop

- Long：记录 fill 后最高 lag bid；当前 bid 相对 peak 下跌达到 1% 时 stop sell。
- Short：记录 fill 后最低 lag ask；当前 ask 相对 trough 上涨达到 1% 时 stop buy。
- Stoploss 只在 lag tick 上检查，并早于 normal close。
- pending close 会阻止重复提交 close order。

### 当前行为没有包含的条件

- 没有要求 position PnL 为正才允许 normal close。
- 没有固定 take-profit target。
- 没有 max holding time。
- `target_profit_rate` 用于 entry 成本/阈值，不是持仓后的固定止盈条件。
- drift guard 和 freshness guard 都只控制开仓，不阻止已有 position 退出。

## 对单边模式的评审结论

### 可接受的部分

价差收敛代表最初的 LeadLag alpha 已失效。即使该笔 position 当前亏损，继续持有也不再由原始信号支持。因此：

- Normal close 应继续作为强制的 alpha-invalidation exit。
- 不应增加“只有盈利才允许平仓”的 gate。
- 当收敛由不利的 lead 回撤造成时，仍应退出，而不是等待价格回到盈利区间。

### 不足的部分

在双边或已对冲策略中，spread convergence 本身更接近完整收益逻辑；在只交易 Bitget 单腿时，position 仍暴露于绝对价格方向、beta 和突发行情风险。当前机制存在以下不足：

- 相同的 close condition 同时覆盖“lag 有利追随 lead”和“lead 不利回撤到 lag”，但统计上没有区分两者。
- 预期 edge 以 bps 计，而 1% trailing stop 相对较宽，可能在 alpha 早已失效后仍承受较大单边 MAE。
- 没有 max holding time；异常情况下 position 的生命期主要由价差条件和 1% trailing stop 决定。
- 动态 `exit_band` 会在 position 生命周期内变化，使入场时的退出预期不完全固定。
- 本次实际样本中 15/20 net loser、13/20 gross loser，且 raw-reference alpha 本身为负，说明 entry selection、退出路径、费用和执行成本需要一起归因，不能只改 exit threshold。

因此，当前条件作为“alpha 失效退出”可以接受，但作为单边策略唯一的正常获利与风险管理机制并不充分。

## 建议

### 1. 保留价差收敛强制退出

保持 normal close 不依赖当前 PnL。若 alpha 已失效，应释放单边风险；为等待回本而继续持仓会把 LeadLag 交易变成没有新信号支持的方向性押注。

### 2. 先做退出归因，不立即修改阈值

使用 recorder/replay 对每笔实际和候选 position 增加离线分类：

- favorable convergence：lag 向 lead 收敛；
- adverse convergence：lead 向 lag 回撤；
- entry gap、exit gap；
- holding duration；
- MFE、MAE；
- raw alpha、execution slippage、fee、gross PnL、net PnL；
- normal close、retry close、trailing stop 等退出原因。

这一步优先复用已有 recorder、replay 和 report 数据，不在实盘热路径增加 shadow 双路逻辑。只有现有证据无法完成归因时，才考虑增加最小、必要且遵循 `NOVA_` 路径的诊断字段，并同步 `docs/diagnostic_fields.md`。

### 3. 独立评估时间与单边风险边界

在不削弱 alpha-invalidation exit 的前提下，离线比较：

- max holding time / time stop；
- 基于 MAE 或绝对方向风险的 loss boundary；
- 比固定 1% 更符合各 symbol 波动率和预期 edge 的 trailing boundary；
- 入场时冻结 exit threshold 与持仓中动态 threshold 的差异。

这些候选必须通过独立 replay、signal-only 或独立 live candidate 进程评估，不应直接注入现有实盘策略热路径。

### 4. 单独评估 drift guard 的事件后余震

保留 instant guard 的安全语义，不根据 HUSDT 单次事件直接降低或关闭阈值。构建跨 symbol、跨时段的 stateful replay，至少回答：

- 极端偏离中，instant guard 阻止了多少不可成交、错误映射或明显异常 signal；
- ratio 回归到 instant threshold 内后，`ratio_std_1m` 继续阻断多久；
- 被余震阻断的 signal 在考虑 `parallel=1`、真实限价、订单时延、fee 和 close state 后，仍有多少独立可交易机会；
- 固定 1 分钟窗口、阈值衰减、事件 reset 或方向感知 guard 的收益与风险差异。

HUSDT 反事实表明当前 guard 可能漏掉一笔高价值机会，但单个成功反例不足以证明 guard 的总体期望为负。

### 5. 保持 stateful 评估口径

所有 signal 评估都应遵循当前状态机：

```text
candidate signal
→ parallel / guard / freshness
→ place pending
→ order feedback / fill
→ one active position
→ close pending / retry / terminal
```

不能用逐 signal 的无状态 forward return 代替交易结果，也不能把同一簇 signal 的 return 直接累加。性能和策略收益结论都应以整链 replay 或独立 live A/B 为最终证据。

## 不应从本次样本得出的结论

- 不能说 HUSDT 首单“一定会成交”或“实际赚了 0.419 USDT”。
- 不能说 232 个被拒 HUSDT signal 等于 232 笔错失订单。
- 不能仅凭 20 笔实际交易就断言 normal close 阈值错误。
- 不能仅凭一次高收益反事实就关闭 drift guard。
- 不能把 `parallel_limit` 与已删除的额外 limiter 混为一谈。
- 不能用 live log 的 `freshness_guard_pass=true` 证明 drift reject 时 quote 一定 fresh。

## 后续验证的最小闭环

在修改 drift guard 或单边 exit 前，建议完成以下闭环：

1. 生成完整 12 小时 report，并固定最终 signals、orders、fills、PnL、slippage 和 reject reason。
2. 对全部实际 position 做 favorable/adverse convergence、MFE/MAE 和 holding-time 归因。
3. 对 HUSDT 事件做包含 order feedback 和 `parallel=1` 的 stateful replay。
4. 扩展到更多 symbol 和不同时段，评估 instant、`ratio_std`、`drift_mean` 各自的保护价值和机会成本。
5. 只对有稳定样本收益的候选行为做独立 A/B；通过等价性、安全、性能、replay 和 review 后再进入生产路径。

在这些证据完成前，建议保持当前生产行为不变。
