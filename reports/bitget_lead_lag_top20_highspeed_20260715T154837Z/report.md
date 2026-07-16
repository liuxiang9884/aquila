# LeadLag Live Run Report

> **Bitget 数据源提示：** 本通用生成页无法从当前 catalog key 还原 Bitget
> `contract_multiplier`，因此下方 PnL 的 `0` 不是账户实际 PnL；另外 5 个
> terminal-before-Ack 样本的 `ack_rtt_ns=0` 会压低尾延迟。实际 fee/PnL、修正后的
> Ack RTT、胜率与重点结论见同目录 `analysis_report.md`，原始交易所 fill 证据见
> `evidence/bitget_fills.json`。

## 基本信息

- run_id: `bitget_lead_lag_top20_highspeed_20260715T154837Z`
- 策略配置: `/home/liuxiang/tmp/bitget_lead_lag_top20_highspeed_20260715T154837Z/configs/strategy__strategy.toml`
- 源日志: `/home/liuxiang/tmp/bitget_lead_lag_top20_highspeed_20260715T154837Z/logs/strategy_20260715_155545.log`
- guard stdout: `/home/liuxiang/tmp/bitget_lead_lag_top20_highspeed_20260715T154837Z/guarded_live.stdout`
- 首个 signal 时间: `2026-07-15 15:56:56.446462827`
- 最后 signal 时间: `2026-07-16 01:52:00.479056181`

## Pair Freshness 参数

单位为 `ms`，来自策略配置的 `max_lead_freshness_ms` / `max_lag_freshness_ms`。

| symbol | symbol_id | lead_exchange | lag_exchange | max_lead_freshness_ms | max_lag_freshness_ms |
| --- | --- | --- | --- | --- | --- |
| BTC_USDT | 93 | binance | bitget | 3 | 200 |
| SOL_USDT | 384 | binance | bitget | 3 | 200 |
| DOGE_USDT | 137 | binance | bitget | 3 | 200 |
| XRP_USDT | 472 | binance | bitget | 3 | 200 |
| HYPE_USDT | 210 | binance | bitget | 3 | 200 |
| TAC_USDT | 411 | binance | bitget | 3 | 200 |
| ZEC_USDT | 480 | binance | bitget | 3 | 200 |
| ORDI_USDT | 316 | binance | bitget | 3 | 200 |
| WLD_USDT | 460 | binance | bitget | 3 | 200 |
| SLX_USDT | 381 | binance | bitget | 3 | 200 |
| UB_USDT | 438 | binance | bitget | 3 | 200 |
| VELVET_USDT | 449 | binance | bitget | 3 | 200 |
| BTW_USDT | 95 | binance | bitget | 3 | 200 |
| RAVE_USDT | 347 | binance | bitget | 3 | 200 |
| SUI_USDT | 404 | binance | bitget | 3 | 200 |
| AVAX_USDT | 51 | binance | bitget | 3 | 200 |
| ENA_USDT | 152 | binance | bitget | 3 | 200 |
| BAS_USDT | 67 | binance | bitget | 3 | 200 |
| H_USDT | 211 | binance | bitget | 3 | 200 |
| LINK_USDT | 253 | binance | bitget | 3 | 200 |

## 同目录 CSV

- `signal.csv`: 644 条 signal，并关联对应 order
- `order_detail.csv`: 644 条 order 明细
- `position.csv`: 21 条 position 明细
- `latency.csv`: 211 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `644`
- submitted order: `211`
- Gate send ok: `0`
- ack: `211`
- order finished: `211`
- 有成交 order: `42`

| symbol | signals |
| --- | --- |
| BAS_USDT | 32 |
| BTW_USDT | 25 |
| ENA_USDT | 3 |
| H_USDT | 60 |
| ORDI_USDT | 17 |
| RAVE_USDT | 86 |
| TAC_USDT | 136 |
| UB_USDT | 59 |
| VELVET_USDT | 209 |
| WLD_USDT | 3 |
| ZEC_USDT | 14 |

| status | count |
| --- | --- |
| kCancelled | 169 |
| kFilled | 38 |
| kPartiallyCancelled | 4 |
| kRejected | 433 |

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 42 | -4.333 | 0 | -76 | 12 | 6 | 15 | 11.095 |
| entry | 21 | -8.667 | 0 | -76 | 7 | 2 | 10 | 13.857 |
| exit | 21 | 0 | 0 | -21 | 12 | 4 | 5 | 8.333 |

## PnL

- gross PnL: `0`
- net PnL: `0`

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| BTWUSDT | kLong | 88 |  |  |
| HUSDT | kLong | 86 |  |  |
| HUSDT | kLong | 71 |  |  |
| HUSDT | kLong | 44 |  |  |
| HUSDT | kLong | 90 |  |  |
| HUSDT | kShort | 20 |  |  |
| HUSDT | kShort | 89 |  |  |
| RAVEUSDT | kLong | 19 |  |  |
| RAVEUSDT | kShort | 18 |  |  |
| RAVEUSDT | kShort | 18 |  |  |
| RAVEUSDT | kLong | 18 |  |  |
| RAVEUSDT | kLong | 18 |  |  |
| RAVEUSDT | kLong | 18 |  |  |
| RAVEUSDT | kLong | 18 |  |  |
| TACUSDT | kShort | 1726 |  |  |
| TACUSDT | kLong | 1712 |  |  |
| TACUSDT | kShort | 1811 |  |  |
| VELVETUSDT | kLong | 6 |  |  |
| VELVETUSDT | kLong | 10 |  |  |
| VELVETUSDT | kShort | 10 |  |  |
| WLDUSDT | kShort | 13 |  |  |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0 ms`
- ack RTT median: `2.321 ms`
- ack RTT avg: `2.503 ms`
- ack RTT p95: `2.952 ms`
- ack RTT max: `16.402 ms`

### Ack RTT 三段拆解

Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。

| stage | samples | min_ms | median_ms | avg_ms | p95_ms | p99_ms | max_ms | >1ms | >5ms | >10ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ack RTT total | 211 | 0 | 2.321 | 2.503 | 2.952 | 7.259 | 16.402 | 206 | 5 | 2 |
| 上行 send->Gate x_in | 0 |  |  |  |  |  |  | 0 | 0 | 0 |
| Gate x_in->x_out | 0 |  |  |  |  |  |  | 0 | 0 | 0 |
| 下行 Gate x_out->local | 0 |  |  |  |  |  |  | 0 | 0 | 0 |

- `>5ms` Ack tail dominant stage: unknown=5
| local_order_id | ack_rtt_ms | upstream_ms | gate_in_to_out_ms | downstream_ms | dominant_stage |
| --- | --- | --- | --- | --- | --- |
| 432345564227567617 | 16.402 |  |  |  |  |
| 432345564227567705 | 14.208 |  |  |  |  |
| 432345564227567704 | 7.259 |  |  |  |  |
| 432345564227567760 | 5.57 |  |  |  |  |
| 432345564227567774 | 5.541 |  |  |  |  |
- send-to-finish min: `3.919 ms`
- send-to-finish median: `4.3 ms`
- send-to-finish avg: `5.335 ms`
- send-to-finish p95: `6.822 ms`
- send-to-finish max: `65.457 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-1 ms`
- exchange Ack-to-finish median: `0 ms`
- exchange Ack-to-finish avg: `-0.092 ms`
- exchange Ack-to-finish p95: `0 ms`
- exchange Ack-to-finish max: `1 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 432345564227567786 | UBUSDT | kCancelled | kImmediateOrCancel | 1 | 1.876 | 4.133 |
| 432345564227567617 | ORDIUSDT | kCancelled | kImmediateOrCancel | 0 | 1.986 | 18.389 |
| 432345564227567618 | ZECUSDT | kCancelled | kImmediateOrCancel | 0 | 2.167 | 4.676 |
| 432345564227567619 | RAVEUSDT | kCancelled | kImmediateOrCancel | 0 | 1.887 | 4.687 |
| 432345564227567620 | BTWUSDT | kFilled | kUnknown | 0 | 2.224 | 4.685 |
