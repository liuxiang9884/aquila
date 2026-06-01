# LeadLag Live Run Report

## 基本信息

- run_id: `20260601_134128_lab_usdt_2000gross_500notional_100ticks_private_2h`
- 策略配置: `/home/liuxiang/tmp/20260601_134128_lab_usdt_2000gross_500notional_100ticks_private_2h/configs/lead_lag_lab_usdt_live_strategy_20260601.toml`
- 源日志: `/home/liuxiang/tmp/20260601_134128_lab_usdt_2000gross_500notional_100ticks_private_2h/lead_lag_strategy_20260601_134248.log`
- guard stdout: `/home/liuxiang/tmp/20260601_134128_lab_usdt_2000gross_500notional_100ticks_private_2h/guarded_live.stdout`
- 首个 signal 时间: `2026-06-01 13:44:12.150285651`
- 最后 signal 时间: `2026-06-01 15:42:45.399228184`

## 同目录 CSV

- `signal.csv`: 177 条 signal，并关联对应 order
- `order_detail.csv`: 177 条 order 明细
- `position.csv`: 17 条 position 明细
- `latency.csv`: 177 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `177`
- submitted order: `177`
- Gate send ok: `177`
- ack: `177`
- order finished: `177`
- 有成交 order: `30`

| symbol | signals |
| --- | --- |
| LAB_USDT | 177 |

| status | count |
| --- | --- |
| kCancelled | 144 |
| kFilled | 19 |
| kPartiallyCancelled | 11 |
| kRejected | 3 |

## PnL

- gross PnL: `8.8162`
- net PnL: `7.32256736064`

### 实际费率复算

本报告 CSV 的 `fee_rate_config` 来自本次运行配置 `lag_taker_fee = 0.00016`，用于复现策略当时的成本假设。实盘后通过 Gate REST 查询到 `LAB_USDT` 合约级 `taker_fee = 0.00020`；按该实际费率复算：

- gross PnL: `8.8162`
- fee: `1.8670407992`
- net PnL: `6.9491592008`

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| LAB_USDT | kLong | 0.3 | 2.873502 | 2.73871216032 |
| LAB_USDT | kShort | 0.1 | -0.0497 | -0.094629328 |
| LAB_USDT | kShort | 0.1 | -0.1767 | -0.221649648 |
| LAB_USDT | kShort | 0.1 | -0.5055 | -0.550502256 |
| LAB_USDT | kLong | 0.3 | 0.912198 | 0.77592443232 |
| LAB_USDT | kShort | 0.1 | -0.2012 | -0.247967808 |
| LAB_USDT | kShort | 0.2 | -0.5902 | -0.683765664 |
| LAB_USDT | kShort | 0.2 | 0.9862 | 0.889877408 |
| LAB_USDT | kLong | 0.2 | 2.379 | 2.281606048 |
| LAB_USDT | kShort | 0.1 | 1.1107 | 1.062369936 |
| LAB_USDT | kShort | 0.2 | -0.4554 | -0.556175456 |
| LAB_USDT | kShort | 0.1 | -0.2052 | -0.255584128 |
| LAB_USDT | kShort | 0.2 | 1.9056 | 1.804095104 |
| LAB_USDT | kShort | 0.2 | 0.3038 | 0.202648608 |
| LAB_USDT | kShort | 0.3 | -1.1271 | -1.279687056 |
| LAB_USDT | kLong | 0.2 | 1.4362 | 1.335690528 |
| LAB_USDT | kLong | 0.2 | 0.22 | 0.12160448 |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0.483 ms`
- ack RTT median: `0.612 ms`
- ack RTT avg: `1.712 ms`
- ack RTT p95: `7.714 ms`
- ack RTT max: `80.07 ms`
- Gate Ack process min: `0.076 ms`
- Gate Ack process median: `0.133 ms`
- Gate Ack process avg: `1.14 ms`
- Gate Ack process p95: `4.494 ms`
- Gate Ack process max: `79.576 ms`
- latency diagnostic outliers: `11`
| local_order_id | reason | ack_rtt_ms | send_to_first_drive_read_ms | drive_read_duration_ms |
| --- | --- | --- | --- | --- |
| 288230376151711750 | kAckRttThreshold | 5.143 | 0.034 | 0.016 |
| 288230376151711770 | kAckRttThreshold | 5.026 | 0.033 | 0.014 |
| 288230376151711787 | kAckRttThreshold | 13.339 | 0.026 | 0.012 |
| 288230376151711817 | kAckRttThreshold | 7.714 | 0.034 | 0.013 |
| 288230376151711820 | kAckRttThreshold | 7.739 | 0.023 | 0.012 |
| 288230376151711838 | kAckRttThreshold | 9.997 | 0.029 | 0.013 |
| 288230376151711846 | kAckRttThreshold | 80.07 | 0.033 | 0.014 |
| 288230376151711854 | kAckRttThreshold | 11.156 | 0.033 | 0.015 |
| 288230376151711876 | kAckRttThreshold | 11.005 | 0.035 | 0.015 |
| 288230376151711903 | kAckRttThreshold | 13.211 | 0.032 | 0.014 |
- send-to-finish min: `1.555 ms`
- send-to-finish median: `10.356 ms`
- send-to-finish avg: `216.059 ms`
- send-to-finish p95: `2201.214 ms`
- send-to-finish max: `3166.441 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-0.171 ms`
- exchange Ack-to-finish median: `6.577 ms`
- exchange Ack-to-finish avg: `163.238 ms`
- exchange Ack-to-finish p95: `977.298 ms`
- exchange Ack-to-finish max: `3164.435 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 288230376151711791 | LAB_USDT | kFilled | kUnknown | 3164.435 | 3165.775 | 3166.441 |
| 288230376151711761 | LAB_USDT | kCancelled | kImmediateOrCancel | 3060.253 | 3061.271 | 3062.268 |
| 288230376151711760 | LAB_USDT | kCancelled | kImmediateOrCancel | 2938.063 | 2938.796 | 2939.376 |
| 288230376151711912 | LAB_USDT | kCancelled | kImmediateOrCancel | 2872.262 | 2874.613 | 2875.483 |
| 288230376151711790 | LAB_USDT | kCancelled | kImmediateOrCancel | 2397.157 | 2403.731 | 2404.811 |
