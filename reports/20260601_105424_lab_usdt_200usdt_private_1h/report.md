# LeadLag Live Run Report

## 基本信息

- run_id: `20260601_105424_lab_usdt_200usdt_private_1h`
- 策略配置: `config/strategies/lead_lag_lab_usdt_live_strategy_20260601.toml`
- 源日志: `/home/liuxiang/tmp/20260601_105424_lab_usdt_200usdt_private_1h/merged_report.log`
- guard stdout: `/home/liuxiang/tmp/20260601_105424_lab_usdt_200usdt_private_1h/guarded_live.stdout`
- 首个 signal 时间: `2026-06-01 10:56:21.558023477`
- 最后 signal 时间: `2026-06-01 11:54:38.930643274`

## 同目录 CSV

- `signal.csv`: 70 条 signal，并关联对应 order
- `order_detail.csv`: 70 条 order 明细
- `position.csv`: 3 条 position 明细
- `latency.csv`: 70 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `70`
- submitted order: `70`
- Gate send ok: `70`
- ack: `70`
- order finished: `70`
- 有成交 order: `6`

| symbol | signals |
| --- | --- |
| LAB_USDT | 70 |

| status | count |
| --- | --- |
| kCancelled | 64 |
| kFilled | 6 |

## PnL

- gross PnL: `1.3856`
- net PnL: `1.245526784`

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| LAB_USDT | kLong | 0.1 | -1.4561 | -1.503853488 |
| LAB_USDT | kLong | 0.1 | 0.5183 | 0.470636688 |
| LAB_USDT | kShort | 0.1 | 2.3234 | 2.278743584 |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0.475 ms`
- ack RTT median: `0.581 ms`
- ack RTT avg: `1.055 ms`
- ack RTT p95: `4.188 ms`
- ack RTT max: `14.396 ms`
- latency diagnostic outliers: `2`
| local_order_id | reason | ack_rtt_ms | send_to_first_drive_read_ms | drive_read_duration_ms |
| --- | --- | --- | --- | --- |
| 288230376151711762 | kAckRttThreshold | 5.402 | 0.032 | 0.015 |
| 288230376151711784 | kAckRttThreshold | 14.396 | 0.035 | 0.014 |
- send-to-finish min: `1.233 ms`
- send-to-finish median: `4.176 ms`
- send-to-finish avg: `7.706 ms`
- send-to-finish p95: `33.12 ms`
- send-to-finish max: `51.45 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-0.075 ms`
- exchange Ack-to-finish median: `1.749 ms`
- exchange Ack-to-finish avg: `5.074 ms`
- exchange Ack-to-finish p95: `25.872 ms`
- exchange Ack-to-finish max: `49.93 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 288230376151711785 | LAB_USDT | kCancelled | kImmediateOrCancel | 49.93 | 50.915 | 51.45 |
| 288230376151711799 | LAB_USDT | kCancelled | kImmediateOrCancel | 32.624 | 33.841 | 34.377 |
| 288230376151711776 | LAB_USDT | kCancelled | kImmediateOrCancel | 29.269 | 32.527 | 33.12 |
| 288230376151711781 | LAB_USDT | kCancelled | kImmediateOrCancel | 25.872 | 42.455 | 43.082 |
| 288230376151711761 | LAB_USDT | kCancelled | kImmediateOrCancel | 21.647 | 24.916 | 25.448 |
