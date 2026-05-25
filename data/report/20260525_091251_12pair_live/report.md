# LeadLag 12 Pair Live Run Report

## 基本信息

- run_id: `20260525_091251_12pair_live`
- 策略配置: `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`
- 源日志: `/home/liuxiang/log/lead_lag_strategy_requested_12symbols_live_20260522_20260525_091251.log`
- guard stdout: `/home/liuxiang/tmp/lead_lag_live_12pairs_1h_20260525/guarded_live_retry1.stdout`
- 启动时间: `2026-05-25 09:12:51 UTC`
- 退出时间: `2026-05-25 09:48:41 UTC`
- 实际运行时长: 约 `35m49s`
- 退出原因: `feedback continuity lost`，Gate order feedback session 发布 `kSessionDisconnected`
- 收尾状态: guard 已执行 flatten，12 个合约最终 flat，无残留挂单或仓位

## 同目录 CSV

- `signal.csv`: 12 条 signal，并关联对应 order
- `order_detail.csv`: 12 条 order 明细
- `position.csv`: 4 条闭合 position 明细
- `latency.csv`: 12 条 order latency 明细

## Signal 和 Order

- signal: `12`
- submitted order: `12`
- Gate send ok: `12`
- ack: `12`
- order finished: `12`

每个 signal 都关联到了一个 submitted order，没有 signal 未下单。

按 symbol 分布：

| symbol | signals/orders |
|---|---:|
| PROVE_USDT | 4 |
| RAVE_USDT | 2 |
| INJ_USDT | 2 |
| DASH_USDT | 2 |
| SIREN_USDT | 1 |
| ZEC_USDT | 1 |

订单结果：

| status | count |
|---|---:|
| kFilled | 7 |
| kCancelled | 4 |
| kPartiallyCancelled | 1 |

## 成交率

按订单数口径：

- 有成交订单: `8 / 12 = 66.67%`
- 完全成交订单: `7 / 12 = 58.33%`
- 完全未成交 cancel: `4 / 12 = 33.33%`

按 entry / exit 分开：

- entry orders: `4 / 8` 有成交，成交率 `50.00%`
- entry 完全成交: `3 / 8 = 37.50%`
- exit orders: `4 / 4` 全部成交，成交率 `100.00%`

按 notional 口径：

- 全部订单 notional fill rate: `60.63%`
- entry notional fill rate: `43.50%`
- exit notional fill rate: 约 `100.02%`

主要问题在 entry，开仓 IOC 成交率只有 `50.00%`。

## PnL

当前 CSV 中 fee 字段为空，因此 `net_pnl` 暂时等于 `gross_pnl`；真实扣费后净值会更低。

按实际 fill price 计算，4 个闭合 position 合计：

- gross PnL: `-0.139741 USDT`
- net PnL: `-0.139741 USDT`

Position 明细：

| symbol | direction | matched | gross_pnl |
|---|---|---:|---:|
| PROVE_USDT | kLong | 37 | -0.05772 |
| PROVE_USDT | kShort | 17 | 0 |
| DASH_USDT | kLong | 220 | -0.04554 |
| INJ_USDT | kShort | 191 | -0.036481 |

按 raw price 重新计算：

- raw-price PnL: `+0.1291 USDT`
- actual fill gross PnL: `-0.139741 USDT`
- 差值: `-0.268841 USDT`

结论：signal 按 raw price 看是盈利的，但实际 IOC taker 成交滑点把利润吃掉并转成亏损。

## Tick 滑点

完全 cancel 的订单没有 fill，不计算执行滑点。已成交或部分成交订单的执行滑点：

| symbol | role | side | status | exec_slippage_ticks |
|---|---|---|---|---:|
| INJ_USDT | entry | kSell | kFilled | 2.28 |
| INJ_USDT | exit | kBuy | kFilled | 0.63 |
| DASH_USDT | entry | kBuy | kFilled | 2.53 |
| DASH_USDT | exit | kSell | kFilled | 0.54 |
| PROVE_USDT | entry | kBuy | kFilled | 2.56 |
| PROVE_USDT | exit | kSell | kFilled | 0 |
| PROVE_USDT | entry | kSell | kPartiallyCancelled | 3 |
| PROVE_USDT | exit | kBuy | kFilled | 0 |

整体看，entry 执行滑点约 `2.28-3 ticks`，exit 执行滑点约 `0-0.63 ticks`。

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

ack RTT 汇总：

- min: `0.776 ms`
- median: `0.895 ms`
- avg: `2.644 ms`
- max: `9.551 ms`

低延迟组多数在 `0.78-1.00 ms`，尾部主要是：

| symbol | role | status | ack_rtt |
|---|---|---|---:|
| RAVE_USDT | entry | kCancelled | 9.551 ms |
| SIREN_USDT | entry | kCancelled | 8.308 ms |
| RAVE_USDT | entry | kCancelled | 3.744 ms |
| INJ_USDT | entry | kFilled | 3.230 ms |

下单到终态回报 `send_to_finish_local_ns`：

- min: `5.991 ms`
- median: `13.615 ms`
- avg: `24.196 ms`
- max: `149.990 ms`

`INJ_USDT` exit 的 ack RTT 只有 `0.905 ms`，但终态回报约 `149.990 ms`，说明慢在后续成交回报 / 终态回报，不是 Ack。

## 结论

- 本轮没有跑满 1 小时，约 35 分钟后因 order feedback session disconnect 触发保护退出。
- guard 已确认账户最终 flat，风险收尾正常。
- 信号按 raw price 有正收益，但实际 fill 结果亏损，核心损耗来自 IOC taker 执行滑点。
- entry IOC 成交率偏低，exit 成交正常；后续重点应评估开仓价格策略、可接受滑点、订单类型和入场触发后的 quote drift。
