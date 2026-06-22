# SKYAI_USDT Signal 到 Gate x 时间可成交性分析

本文档汇总 `SKYAI_USDT` 在订单成交分析之后，围绕 signal、`raw_price`、加滑点后的 `order_price`、Gate Ack response `x_in_time` / `x_out_time` 和 canonical BookTicker 的追加分析。分析对象仍是 run `20260619_095317_28symbols_no_h_30d_fusion_off_l0_live`。

## 背景问题

本轮追加分析从下面两个问题开始：

1. 如果按 `raw_price` 下单，signal 后多长时间内还能成交。
2. 如果按加滑点后的 `order_price` 下单，signal 后多长时间内还能成交。

初始建议口径是：

- 使用 `signal_lag_id` 对应 canonical BookTicker 的 `exchange_ns` 作为 signal 起点。
- 在订单生命周期内寻找最后一个满足 BBO `any` 可成交的 canonical BookTicker。
- duration 定义为 `last_marketable_exchange_ns - signal_exchange_ns`。

后续用户要求只看 `canonical.bin.zst`，并进一步提出两个假设：

1. 认为 Gate 在 `x_in_time` / `x_out_time` 之后，订单就会进入撮合引擎。
2. 认为 Gate 内部撮合时间戳和行情 `exchange_ns` 是对齐有序的。

因此本文同时保留两类分析：

- signal 后的 BBO 可成交窗口。
- 在 `x_in_time` / `x_out_time` 两个候选撮合时间点，订单是否从 canonical BBO 视角可成交。

## 数据和样本

输入文件均来自本 report：

- `inputs/orders.csv`
- `inputs/signals.csv`
- `market_data/canonical.bin.zst`

本轮只使用 `market_data/canonical.bin.zst`，没有使用 `source0..3` crosscheck。

主要样本：

- 开仓、完全未成交、`kCancelled` IOC：`93` 笔。
- open/entry 且有成交量的订单：`8` 笔，其中 `kFilled` 为 `6` 笔，`kPartiallyCancelled` 为 `2` 笔。
- 全 symbol 订单总数：`113` 笔。
- 全 symbol 有成交量订单：`17` 笔，其中 entry `8` 笔，exit `9` 笔。

临时订单级输出在分析时写入：

- `/home/liuxiang/tmp/20260619_SKYAI_price_lifetime/SKYAI_USDT_canonical_signal_price_lifetime_cancel.csv`
- `/home/liuxiang/tmp/20260619_SKYAI_price_lifetime/SKYAI_USDT_canonical_x_in_x_out_any_cancel.csv`
- `/home/liuxiang/tmp/20260619_SKYAI_price_lifetime/SKYAI_USDT_entry_filled_x_in_x_out_latency.csv`

本文档已经汇总关键结果；阅读本文不依赖这些 scratch 文件。

## 术语和口径

`any` 可成交：

- 买单：`ask_price <= price` 且 `ask_volume > 0`。
- 卖单：`bid_price >= price` 且 `bid_volume > 0`。

`full` 可成交：

- 满足 `any`，且对手一档量 `>= order quantity`。

`record lifetime`：

- 用户指定的严格口径。
- `last marketable BookTicker exchange_ns - signal BookTicker exchange_ns`。
- 如果 signal 那条 BBO 可成交、但下一条 BBO 已不可成交，则 lifetime 为 `0us`。

`stateful lifetime`：

- 辅助解释口径。
- 如果 signal BBO 可成交，则认为该 BBO 状态持续到第一条不可成交更新。
- 更接近“盘口状态持续多久”，但仍只基于 public/canonical BookTicker。

`x_in_time` / `x_out_time`：

- 来自 Gate Ack response header。
- 代码中分别保存为 `ack_exchange_request_ingress_ns` 和 `ack_exchange_response_egress_ns`。
- 原始 `x_in_time` / `x_out_time` 单位为 us，report 中已转为 ns。
- 本分析中，在某个 `x_time` 判断 BBO 时，取 `exchange_ns <= x_time` 的最新 canonical BookTicker。

## Signal 后可成交窗口

只看 `any`，93 笔开仓 cancel 单全部在 signal 点本身满足 `any`。

严格 `record lifetime`：

| 价格口径 | p50 | p90 | p95 | 说明 |
|---|---:|---:|---:|---|
| `raw_price` | `0us` | `1822us` | `4244us` | 多数订单最后可成交点就是 signal BBO 本身。 |
| `order_price` | `0us` | `2419us` | `4244us` | 加 4 ticks 滑点只小幅延长尾部。 |

辅助 `stateful lifetime`：

| 价格口径 | p50 | p75 | p90 | 说明 |
|---|---:|---:|---:|---|
| `raw_price` | `1117us` | `2577us` | `11769us` | 典型状态持续约 1.1ms。 |
| `order_price` | `1122us` | `2966us` | `11769us` | 加滑点对中位数几乎无影响。 |

判断：

- 如果按严格 record 口径，很多机会只存在于 signal 那条 BBO。
- 如果按 stateful 口径，典型可成交窗口大约是 `1.1ms`。
- 加 4 ticks 滑点对 `any` 的改善很有限，主要只把 `record lifetime` p90 从 `1.82ms` 提到 `2.42ms`。

## x_in / x_out 时是否可成交

在用户提出的两个假设下，分别把 `x_in_time` 和 `x_out_time` 作为候选撮合到达时间点。只看 `any`。

| 假设撮合时间点 | `raw_price` 可成交 | `order_price` 可成交 |
|---|---:|---:|
| `x_in_time` | `67/93` | `68/93` |
| `x_out_time` | `49/93` | `50/93` |

状态变化：

| 价格口径 | x_in 和 x_out 均可成交 | x_in 可成交但 x_out 不可成交 | x_in 不可成交但 x_out 可成交 | 均不可成交 |
|---|---:|---:|---:|---:|
| `raw_price` | `49` | `18` | `0` | `26` |
| `order_price` | `50` | `18` | `0` | `25` |

相关延迟：

| 指标 | p50 | p90 | p95 | max |
|---|---:|---:|---:|---:|
| `x_in_time - signal_exchange_ns` | `1150us` | `5438us` | `12371us` | `18610us` |
| `x_out_time - signal_exchange_ns` | `1330us` | `7128us` | `13851us` | `19297us` |
| `x_out_time - x_in_time` | `149us` | `295us` | `3291us` | `17597us` |

判断：

- 如果认为订单在 `x_in_time` 后很快进入撮合，则约 `73%` 的 cancel 单在加滑点 `order_price` 视角仍满足 BBO `any`。
- 如果保守认为到 `x_out_time` 后才进入撮合，则该比例降到约 `54%`。
- 有 `18` 笔订单在 `x_in_time` 可成交，但到 `x_out_time` 已不可成交；这是后续复核 Gate 内部处理时延和盘口变化的优先样本。
- 加滑点只把 `x_in_time` / `x_out_time` 可成交数各提高 1 笔，不是主要解释变量。

## Open 成交订单的 x_in / x_out 延迟

只看 open/entry 且有成交量的 `8` 笔订单，统计 `signal_lag_id` 对应 canonical BBO 的 `exchange_ns` 到 Gate Ack header 时间。

| 指标 | `x_in_time - signal` | `x_out_time - signal` |
|---|---:|---:|
| min | `625us` | `785us` |
| p25 | `926us` | `1062us` |
| p50 | `1614us` | `1754us` |
| p75 | `2191us` | `2337us` |
| p90 | `4258us` | `4382us` |
| max | `8480us` | `8623us` |

`x_out_time - x_in_time` 在这 8 笔成交/部分成交订单中很短：

- p50：`140us`
- p90：`161us`
- max：`164us`

逐笔明细：

| local_order_id | status | side | filled / quantity | x_in - signal | x_out - signal |
|---|---|---|---:|---:|---:|
| `288230376151712148` | `kFilled` | `kBuy` | `29/29` | `763us` | `894us` |
| `288230376151712153` | `kFilled` | `kBuy` | `29/29` | `625us` | `785us` |
| `288230376151712156` | `kFilled` | `kBuy` | `29/29` | `8480us` | `8623us` |
| `288230376151712159` | `kFilled` | `kBuy` | `29/29` | `981us` | `1118us` |
| `288230376151712172` | `kFilled` | `kBuy` | `29/29` | `1921us` | `2085us` |
| `288230376151712174` | `kPartiallyCancelled` | `kBuy` | `1/29` | `2448us` | `2564us` |
| `288230376151712432` | `kPartiallyCancelled` | `kBuy` | `20/28` | `1307us` | `1424us` |
| `288230376151712497` | `kFilled` | `kSell` | `28/28` | `2105us` | `2261us` |

## Cancel 单 x_in 延迟对比

93 笔开仓 cancel 单的 `x_in_time - signal_exchange_ns`：

| 指标 | 延迟 |
|---|---:|
| min | `348us` |
| p10 | `598us` |
| p25 | `831us` |
| p50 | `1150us` |
| p75 | `1700us` |
| p90 | `5438us` |
| p95 | `12371us` |
| max | `18610us` |
| mean | `2396us` |

按 `x_in_time` 是否仍满足 `any` 拆分：

| 价格口径 | x_in 可成交数 | 可成交样本 p50 / p90 | 不可成交样本 p50 / p90 |
|---|---:|---:|---:|
| `raw_price` | `67/93` | `969us / 4998us` | `1291us / 4803us` |
| `order_price` | `68/93` | `976us / 4888us` | `1286us / 4673us` |

与 open 成交/部分成交订单对比：

- cancel 单 `x_in_time - signal` p50：`1150us`。
- open 成交/部分成交订单 `x_in_time - signal` p50：`1614us`。

因此，单看 signal 到 Gate `x_in_time` 的延迟，cancel 单并不比成交单慢；中位数反而更快。未成交不能简单归因于本地发单到 Gate 入口太慢。

## 和 full 口径的关系

虽然本轮 `x_in/x_out` 分析按用户要求只看 `any`，但解释未成交原因时必须注意：`any` 只表示价格穿越，不表示数量足够。

已有同一 report 中的 volume / window 分析显示：

- 加滑点后的 `order_price` 在 `x_in_time` 附近满足 `any`：`68/93`。
- 加滑点后的 `order_price` 在 `x_in_time` 附近满足 `full`：`21/93`。
- 订单生命周期内能看到 `full` 的 cancel 单只有 `24/93`。

这说明 `any` 会显著高估 IOC 的实际可成交性：大量订单价格上可成交，但对手一档量不足以覆盖整单，真实撮合簿在订单到达前还可能继续被消耗或变化。

## 综合结论

当前证据下，`SKYAI_USDT` 未成交的主要原因排序如下。

1. 盘口机会短且薄。
   Signal 触发后的 `any` 可成交状态典型只有约 `1.1ms`，严格 record 口径下很多订单下一条 BBO 更新就不可成交。BBO 一档量也经常不足以覆盖整单。

2. `signal -> x_in_time` 延迟不是主因。
   Cancel 单的 `x_in_time - signal` p50 为 `1150us`，open 成交/部分成交订单 p50 为 `1614us`。未成交样本并没有表现出明显更慢的 Gate 入口到达时间。

3. 滑点不是主瓶颈。
   加 4 ticks 后，`x_in_time` 的 `any` 只从 `67/93` 提到 `68/93`，`x_out_time` 只从 `49/93` 提到 `50/93`。滑点对本批 SKYAI 订单的边际改善很小。

4. `x_in_time -> x_out_time` 期间仍有机会丢失。
   有 `18` 笔订单在 `x_in_time` 可成交但在 `x_out_time` 不可成交。若撮合实际发生在更接近 `x_out_time` 或之后的位置，这部分会自然解释为盘口窗口在 Gate 内部处理期间消失。

5. canonical public BBO 不能等价为撮合引擎快照。
   即使在 `x_in_time` 或 `x_out_time` 的 canonical BBO 视角满足 `any`，也不能直接证明订单应当成交。BookTicker 不是 matching engine 收到 IOC 时的真实簿快照，`x_in_time` 也更接近 Gate API/gateway 入口时间。

最终判断：这批未成交订单更像是策略在极短、极薄的 BBO 机会窗口上发 IOC，订单到达 Gate 入口时部分仍能从 public BBO 看到价格穿越，但一档量和撮合时序不足以支持稳定成交。问题不主要在 raw/order price 差几个 tick，也不主要在本地发单慢，而在机会质量、可见量和 public BBO 与真实撮合视角之间的差异。

## 后续建议

1. 对这类 symbol，不要只用 `any` 触发下单；至少加入可见对手量、订单数量占一档量比例、和短时间内 BBO 持续性的过滤。
2. 对 IOC entry 做动态 sizing：订单数量不应明显超过可见对手一档量，尤其是盘口更新频繁的 symbol。
3. 单独复核 `x_in_time` 可成交但 `x_out_time` 不可成交的 18 笔订单，重点看 Gate `x_in_time -> x_out_time` tail 和对应 BBO 更新。
4. 对 `ack -> cancel full` 的 5 笔候选继续做逐笔 source crosscheck 和日志复核，但不要把 public BBO `full` 直接等价为撮合异常。
5. 若后续要改策略，优先提升触发质量和 sizing，而不是单纯增加滑点。
