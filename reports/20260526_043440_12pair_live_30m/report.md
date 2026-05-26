# LeadLag Live Run Report

## 基本信息

- run_id: `20260526_043440_12pair_live_30m`
- 策略配置: `/home/liuxiang/tmp/20260526_043440_12pair_live_30m/configs/strategy__lead_lag_requested_11symbols_live_strategy_20260522.toml`
- 源日志: `/home/liuxiang/tmp/20260526_043440_12pair_live_30m/merged_live.log`
- guard stdout: `/home/liuxiang/tmp/20260526_043440_12pair_live_30m/guarded_live.stdout`
- 首个 signal 时间: `2026-05-26 04:40:39.512062579`
- 最后 signal 时间: `2026-05-26 05:04:51.433766282`

## 同目录 CSV

- `signal.csv`: 10 条 signal，并关联对应 order
- `order_detail.csv`: 10 条 order 明细
- `position.csv`: 0 条 position 明细
- `latency.csv`: 10 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `10`
- submitted order: `10`
- Gate send ok: `10`
- ack: `10`
- order finished: `10`
- 有成交 order: `0`

| symbol | signals |
| --- | --- |
| DASH_USDT | 1 |
| INJ_USDT | 3 |
| PROVE_USDT | 1 |
| RAVE_USDT | 1 |
| SIREN_USDT | 2 |
| SUI_USDT | 1 |
| ZEC_USDT | 1 |

| status | count |
| --- | --- |
| kCancelled | 10 |

## PnL

- gross PnL: `0`
- net PnL: `0`

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `3.016 ms`
- ack RTT median: `3.193 ms`
- ack RTT avg: `3.89 ms`
- ack RTT p95: `6.738 ms`
- ack RTT max: `6.738 ms`
- send-to-finish min: `4.578 ms`
- send-to-finish median: `9.303 ms`
- send-to-finish avg: `16.144 ms`
- send-to-finish p95: `45.977 ms`
- send-to-finish max: `45.977 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `0.079 ms`
- exchange Ack-to-finish median: `5.423 ms`
- exchange Ack-to-finish avg: `10.937 ms`
- exchange Ack-to-finish p95: `37.336 ms`
- exchange Ack-to-finish max: `37.336 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 288230376151711749 | DASH_USDT | kCancelled | kImmediateOrCancel | 37.336 | 39.239 | 45.977 |
| 288230376151711748 | SUI_USDT | kCancelled | kImmediateOrCancel | 24.384 | 25.59 | 28.783 |
| 288230376151711747 | INJ_USDT | kCancelled | kImmediateOrCancel | 22.52 | 23.336 | 26.644 |
| 288230376151711746 | ZEC_USDT | kCancelled | kImmediateOrCancel | 12.133 | 13.075 | 16.138 |
| 288230376151711745 | PROVE_USDT | kCancelled | kImmediateOrCancel | 5.577 | 6.286 | 9.303 |
