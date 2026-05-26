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
