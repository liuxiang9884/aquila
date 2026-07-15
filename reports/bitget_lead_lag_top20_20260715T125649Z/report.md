# LeadLag Live Run Report

## 基本信息

- run_id: `bitget_lead_lag_top20_20260715T125649Z`
- 策略配置: `/home/liuxiang/tmp/bitget_lead_lag_top20_20260715T125649Z/configs/strategy__strategy.toml`
- 源日志: `/home/liuxiang/tmp/bitget_lead_lag_top20_20260715T125649Z/logs/strategy_20260715_130119.log`
- guard stdout: `/home/liuxiang/tmp/bitget_lead_lag_top20_20260715T125649Z/guarded_live.stdout`
- 首个 signal 时间: `2026-07-15 13:03:39.022530144`
- 最后 signal 时间: `2026-07-15 13:55:05.550905617`

## Pair Freshness 参数

单位为 `ms`，来自策略配置的 `max_lead_freshness_ms` / `max_lag_freshness_ms`。

| symbol | symbol_id | lead_exchange | lag_exchange | max_lead_freshness_ms | max_lag_freshness_ms |
| --- | --- | --- | --- | --- | --- |
| BTC_USDT | 93 | binance | bitget | 5 | 200 |
| SOL_USDT | 384 | binance | bitget | 5 | 200 |
| DOGE_USDT | 137 | binance | bitget | 5 | 200 |
| XRP_USDT | 472 | binance | bitget | 5 | 200 |
| HYPE_USDT | 210 | binance | bitget | 5 | 200 |
| TAC_USDT | 411 | binance | bitget | 5 | 200 |
| ZEC_USDT | 480 | binance | bitget | 5 | 200 |
| ORDI_USDT | 316 | binance | bitget | 5 | 200 |
| WLD_USDT | 460 | binance | bitget | 5 | 200 |
| SLX_USDT | 381 | binance | bitget | 5 | 200 |
| UB_USDT | 438 | binance | bitget | 5 | 200 |
| VELVET_USDT | 449 | binance | bitget | 5 | 200 |
| BTW_USDT | 95 | binance | bitget | 5 | 200 |
| RAVE_USDT | 347 | binance | bitget | 5 | 200 |
| SUI_USDT | 404 | binance | bitget | 5 | 200 |
| AVAX_USDT | 51 | binance | bitget | 5 | 200 |
| ENA_USDT | 152 | binance | bitget | 5 | 200 |
| BAS_USDT | 67 | binance | bitget | 5 | 200 |
| H_USDT | 211 | binance | bitget | 5 | 200 |
| LINK_USDT | 253 | binance | bitget | 5 | 200 |

## 同目录 CSV

- `signal.csv`: 117 条 signal，并关联对应 order
- `order_detail.csv`: 117 条 order 明细
- `position.csv`: 1 条 position 明细
- `latency.csv`: 41 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `117`
- submitted order: `41`
- Gate send ok: `0`
- ack: `41`
- order finished: `41`
- 有成交 order: `2`

| symbol | signals |
| --- | --- |
| BAS_USDT | 15 |
| BTW_USDT | 3 |
| H_USDT | 14 |
| ORDI_USDT | 10 |
| RAVE_USDT | 22 |
| SOL_USDT | 1 |
| TAC_USDT | 14 |
| UB_USDT | 14 |
| VELVET_USDT | 19 |
| ZEC_USDT | 5 |

| status | count |
| --- | --- |
| kCancelled | 39 |
| kFilled | 2 |
| kRejected | 76 |

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 2 | -19 | -50 | -50 | 12 | 1 | 1 | 28 |
| entry | 1 | -50 | -50 | -50 | -50 | 0 | 1 | 56 |
| exit | 1 | 12 | 12 | 12 | 12 | 1 | 0 | 0 |

## PnL

- gross PnL: `0`
- net PnL: `0`

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| RAVEUSDT | kShort | 17 |  |  |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0 ms`
- ack RTT median: `5.345 ms`
- ack RTT avg: `5.243 ms`
- ack RTT p95: `5.804 ms`
- ack RTT max: `10.212 ms`

### Ack RTT 三段拆解

Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。

| stage | samples | min_ms | median_ms | avg_ms | p95_ms | p99_ms | max_ms | >1ms | >5ms | >10ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ack RTT total | 41 | 0 | 5.345 | 5.243 | 5.804 | 10.212 | 10.212 | 39 | 39 | 1 |
| 上行 send->Gate x_in | 0 |  |  |  |  |  |  | 0 | 0 | 0 |
| Gate x_in->x_out | 0 |  |  |  |  |  |  | 0 | 0 | 0 |
| 下行 Gate x_out->local | 0 |  |  |  |  |  |  | 0 | 0 | 0 |

- `>5ms` Ack tail dominant stage: unknown=39
| local_order_id | ack_rtt_ms | upstream_ms | gate_in_to_out_ms | downstream_ms | dominant_stage |
| --- | --- | --- | --- | --- | --- |
| 432345564227567617 | 10.212 |  |  |  |  |
| 432345564227567619 | 6.331 |  |  |  |  |
| 432345564227567628 | 5.804 |  |  |  |  |
| 432345564227567621 | 5.796 |  |  |  |  |
| 432345564227567627 | 5.587 |  |  |  |  |
| 432345564227567636 | 5.58 |  |  |  |  |
| 432345564227567640 | 5.523 |  |  |  |  |
| 432345564227567620 | 5.496 |  |  |  |  |
| 432345564227567655 | 5.471 |  |  |  |  |
| 432345564227567652 | 5.464 |  |  |  |  |
- send-to-finish min: `5.533 ms`
- send-to-finish median: `5.83 ms`
- send-to-finish avg: `6.044 ms`
- send-to-finish p95: `6.739 ms`
- send-to-finish max: `10.54 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-1 ms`
- exchange Ack-to-finish median: `-1 ms`
- exchange Ack-to-finish avg: `-0.59 ms`
- exchange Ack-to-finish p95: `0 ms`
- exchange Ack-to-finish max: `0 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 432345564227567621 | UBUSDT | kCancelled | kImmediateOrCancel | 0 | 0.437 | 6.233 |
| 432345564227567623 | HUSDT | kCancelled | kImmediateOrCancel | 0 | 0.558 | 5.984 |
| 432345564227567629 | RAVEUSDT | kFilled | kUnknown | 0 | 0.328 | 5.688 |
| 432345564227567630 | RAVEUSDT | kCancelled | kImmediateOrCancel | 0 | 0.467 | 5.543 |
| 432345564227567631 | RAVEUSDT | kFilled | kUnknown | 0 | 0.438 | 5.579 |
