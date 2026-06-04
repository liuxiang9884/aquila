# LeadLag Live Run Report

## 基本信息

- run_id: `20260604_0416_30symbols_2h_test_private`
- 策略配置: `config/strategies/lead_lag_30symbols_live_strategy_20260604.toml`
- 源日志: `/home/liuxiang/tmp/20260604_0416_30symbols_2h_test_private/lead_lag_live_merged.log`
- guard stdout: `/home/liuxiang/tmp/20260604_0416_30symbols_2h_test_private/guarded_live.stdout`
- 首个 signal 时间: `2026-06-04 04:19:33.247769587`
- 最后 signal 时间: `2026-06-04 06:18:37.592670583`

## 同目录 CSV

- `signal.csv`: 708 条 signal，并关联对应 order
- `order_detail.csv`: 708 条 order 明细
- `position.csv`: 15 条 position 明细
- `latency.csv`: 708 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `708`
- submitted order: `708`
- Gate send ok: `708`
- ack: `708`
- order finished: `708`
- 有成交 order: `23`

| symbol | signals |
| --- | --- |
| AIA_USDT | 108 |
| BEAT_USDT | 53 |
| CLO_USDT | 77 |
| ENA_USDT | 21 |
| EPIC_USDT | 72 |
| FARTCOIN_USDT | 3 |
| FET_USDT | 5 |
| FIL_USDT | 3 |
| H_USDT | 11 |
| ICP_USDT | 6 |
| INJ_USDT | 7 |
| LIT_USDT | 25 |
| MYX_USDT | 19 |
| NEAR_USDT | 12 |
| ONDO_USDT | 23 |
| OPN_USDT | 48 |
| PENGU_USDT | 4 |
| PIEVERSE_USDT | 20 |
| SKYAI_USDT | 29 |
| SLX_USDT | 23 |
| STO_USDT | 22 |
| SUI_USDT | 4 |
| TON_USDT | 8 |
| UB_USDT | 8 |
| VIRTUAL_USDT | 3 |
| WIF_USDT | 1 |
| WLD_USDT | 54 |
| XLM_USDT | 9 |
| XPL_USDT | 6 |
| ZEC_USDT | 24 |

| status | count |
| --- | --- |
| kCancelled | 684 |
| kFilled | 13 |
| kPartiallyCancelled | 10 |
| kRejected | 1 |

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 23 | -0.067 | 0 | -6 | 4 | 5 | 2 | 2.501 |
| entry | 8 | -0.375 | 0 | -6 | 2 | 2 | 1 | 2.875 |
| exit | 15 | 0.098 | 0 | -3.71 | 4 | 3 | 1 | 2.302 |

## PnL

- gross PnL: `-0.242157`
- net PnL: `-0.4326734394`

### Raw PnL 和胜率

Raw PnL 使用 entry / exit 的 `raw_price` 计算，fee 仍使用 report CSV 中的配置费率估算值；胜率按 net PnL > 0 计算。

- actual gross PnL: `-0.242157`
- actual net PnL: `-0.4326734394`
- actual win rate: `33.33%` (5/15)
- raw gross PnL: `-0.2317`
- raw net PnL: `-0.4222164394`
- raw win rate: `33.33%` (5/15)

| symbol | direction | matched | actual_gross | raw_gross | actual_net | raw_net | actual_minus_raw_gross |
| --- | --- | --- | --- | --- | --- | --- | --- |
| AIA_USDT | kShort | 2 | -0.0038 | -0.0038 | -0.00443396 | -0.00443396 | 0 |
| AIA_USDT | kShort | 5 | -0.0115 | -0.0115 | -0.0130853 | -0.0130853 | 0 |
| AIA_USDT | kShort | 1 | -0.0026 | -0.0026 | -0.00291712 | -0.00291712 | 0 |
| AIA_USDT | kShort | 59 | -0.200777 | -0.2006 | -0.2194965554 | -0.2193195554 | -0.000177 |
| AIA_USDT | kShort | 1 | -0.0037 | -0.0037 | -0.00401734 | -0.00401734 | 0 |
| AIA_USDT | kShort | 58 | -0.2494 | -0.2494 | -0.26781268 | -0.26781268 | 0 |
| BEAT_USDT | kLong | 7 | 0.27797 | 0.252 | 0.241503206 | 0.215533206 | 0.02597 |
| CLO_USDT | kShort | 1 | 0.0043 | 0.0043 | 0.00367986 | 0.00367986 | 0 |
| CLO_USDT | kShort | 8 | -0.0232 | -0.02 | -0.02805392 | -0.02485392 | -0.0032 |
| CLO_USDT | kShort | 34 | -0.102 | -0.102 | -0.12262984 | -0.12262984 | 0 |
| ONDO_USDT | kLong | 51 | 0.051 | 0.0204 | 0.0430746 | 0.0124746 | 0.0306 |
| STO_USDT | kLong | 1 | 0.0006 | 0.0006 | 0.00033468 | 0.00033468 | 0 |
| STO_USDT | kLong | 47 | -0.0094 | 0 | -0.02135868 | -0.01195868 | -0.0094 |
| STO_USDT | kLong | 110 | -0.06765 | -0.033 | -0.09562927 | -0.06097927 | -0.03465 |
| WLD_USDT | kLong | 196 | 0.098 | 0.1176 | 0.05816888 | 0.07776888 | -0.0196 |

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| AIA_USDT | kShort | 2 | -0.0038 | -0.00443396 |
| AIA_USDT | kShort | 5 | -0.0115 | -0.0130853 |
| AIA_USDT | kShort | 1 | -0.0026 | -0.00291712 |
| AIA_USDT | kShort | 59 | -0.200777 | -0.2194965554 |
| AIA_USDT | kShort | 1 | -0.0037 | -0.00401734 |
| AIA_USDT | kShort | 58 | -0.2494 | -0.26781268 |
| BEAT_USDT | kLong | 7 | 0.27797 | 0.241503206 |
| CLO_USDT | kShort | 1 | 0.0043 | 0.00367986 |
| CLO_USDT | kShort | 8 | -0.0232 | -0.02805392 |
| CLO_USDT | kShort | 34 | -0.102 | -0.12262984 |
| ONDO_USDT | kLong | 51 | 0.051 | 0.0430746 |
| STO_USDT | kLong | 1 | 0.0006 | 0.00033468 |
| STO_USDT | kLong | 47 | -0.0094 | -0.02135868 |
| STO_USDT | kLong | 110 | -0.06765 | -0.09562927 |
| WLD_USDT | kLong | 196 | 0.098 | 0.05816888 |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0.518 ms`
- ack RTT median: `0.67 ms`
- ack RTT avg: `1.361 ms`
- ack RTT p95: `1.785 ms`
- ack RTT max: `129.713 ms`
- Gate Ack process min: `0.084 ms`
- Gate Ack process median: `0.138 ms`
- Gate Ack process avg: `0.737 ms`
- Gate Ack process p95: `0.535 ms`
- Gate Ack process max: `129.131 ms`

### Ack RTT 三段拆解

Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。

| stage | samples | min_ms | median_ms | avg_ms | p95_ms | p99_ms | max_ms | >1ms | >5ms | >10ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ack RTT total | 708 | 0.518 | 0.67 | 1.361 | 1.785 | 7.617 | 129.713 | 71 | 18 | 5 |
| 上行 send->Gate x_in | 708 | 0.154 | 0.21 | 0.255 | 0.344 | 1.468 | 5.236 | 11 | 1 | 0 |
| Gate x_in->x_out | 708 | 0.084 | 0.138 | 0.737 | 0.535 | 6.432 | 129.131 | 24 | 10 | 5 |
| 下行 Gate x_out->local | 708 | 0.205 | 0.322 | 0.369 | 0.518 | 1.524 | 4.128 | 14 | 0 | 0 |

- `>5ms` Ack tail dominant stage: Gate in->out=12, 上行=2, 下行=4
| local_order_id | ack_rtt_ms | upstream_ms | gate_in_to_out_ms | downstream_ms | dominant_stage |
| --- | --- | --- | --- | --- | --- |
| 288230376151712189 | 129.713 | 0.204 | 129.131 | 0.378 | Gate in->out |
| 288230376151712096 | 121.653 | 0.192 | 121.011 | 0.451 | Gate in->out |
| 288230376151711855 | 52.267 | 0.211 | 51.66 | 0.396 | Gate in->out |
| 288230376151712079 | 19.05 | 1.173 | 17.511 | 0.365 | Gate in->out |
| 288230376151712121 | 15.649 | 0.193 | 15.127 | 0.329 | Gate in->out |
| 288230376151711801 | 9.902 | 0.195 | 9.345 | 0.362 | Gate in->out |
| 288230376151711991 | 7.692 | 4.41 | 0.221 | 3.062 | 上行 |
| 288230376151712423 | 7.617 | 0.746 | 6.467 | 0.404 | Gate in->out |
| 288230376151712276 | 7.143 | 1.807 | 4.818 | 0.519 | Gate in->out |
| 288230376151712211 | 6.986 | 0.205 | 6.432 | 0.349 | Gate in->out |
- latency diagnostic outliers: `18`
| local_order_id | reason | ack_rtt_ms | send_to_first_drive_read_ms | drive_read_duration_ms |
| --- | --- | --- | --- | --- |
| 288230376151711801 | kAckRttThreshold | 9.902 | 0.047 | 0.023 |
| 288230376151711855 | kAckRttThreshold | 52.267 | 0.03 | 0.022 |
| 288230376151711857 | kAckRttThreshold | 5.235 | 0.041 | 0.013 |
| 288230376151711930 | kAckRttThreshold | 6.573 | 0.029 | 0.022 |
| 288230376151711991 | kAckRttThreshold | 7.692 | 0.042 | 0.028 |
| 288230376151712057 | kAckRttThreshold | 6.561 | 0.034 | 0.017 |
| 288230376151712079 | kAckRttThreshold | 19.05 | 0.04 | 0.025 |
| 288230376151712080 | kAckRttThreshold | 5.515 | 0.037 | 0.022 |
| 288230376151712096 | kAckRttThreshold | 121.653 | 0.038 | 0.023 |
| 288230376151712121 | kAckRttThreshold | 15.649 | 0.039 | 0.022 |
- send-to-finish min: `1.519 ms`
- send-to-finish median: `5.746 ms`
- send-to-finish avg: `24.967 ms`
- send-to-finish p95: `102.589 ms`
- send-to-finish max: `1018.516 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-0.235 ms`
- exchange Ack-to-finish median: `3.046 ms`
- exchange Ack-to-finish avg: `20.054 ms`
- exchange Ack-to-finish p95: `89.3 ms`
- exchange Ack-to-finish max: `522.222 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 288230376151712450 | PENGU_USDT | kCancelled | kImmediateOrCancel | 522.222 | 522.455 | 523.914 |
| 288230376151712191 | MYX_USDT | kCancelled | kImmediateOrCancel | 445.443 | 447.828 | 448.493 |
| 288230376151712087 | ZEC_USDT | kCancelled | kImmediateOrCancel | 416.616 | 418.053 | 418.735 |
| 288230376151712088 | MYX_USDT | kCancelled | kImmediateOrCancel | 390.767 | 391.968 | 392.659 |
| 288230376151712370 | ENA_USDT | kCancelled | kImmediateOrCancel | 385.834 | 387.355 | 387.979 |
