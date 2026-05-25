# LeadLag Live Run Report

## 基本信息

- run_id: `20260525_133511_12pair_live_2ticks`
- 策略配置: `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`
- 源日志: `/home/liuxiang/log/lead_lag_strategy_requested_12symbols_live_20260522_20260525_133511.log`
- guard stdout: `/home/liuxiang/tmp/lead_lag_live_12pairs_1h_2ticks_20260525_132912/guarded_live_retry1.stdout`
- 首个 signal 时间: `2026-05-25 13:37:28.065544782`
- 最后 signal 时间: `2026-05-25 14:33:30.319958907`

## 同目录 CSV

- `signal.csv`: 58 条 signal，并关联对应 order
- `order_detail.csv`: 58 条 order 明细
- `position.csv`: 0 条 position 明细
- `latency.csv`: 58 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `58`
- submitted order: `58`
- Gate send ok: `58`
- ack: `58`
- order finished: `58`
- 有成交 order: `0`

| symbol | signals |
| --- | --- |
| DASH_USDT | 3 |
| ENA_USDT | 1 |
| ETC_USDT | 1 |
| ETH_USDT | 1 |
| INJ_USDT | 18 |
| PROVE_USDT | 4 |
| RAVE_USDT | 1 |
| SIREN_USDT | 1 |
| SUI_USDT | 3 |
| ZEC_USDT | 25 |

| status | count |
| --- | --- |
| kCancelled | 58 |

## PnL

- gross PnL: `0`
- net PnL: `0`

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `3.638 ms`
- ack RTT median: `5.628 ms`
- ack RTT avg: `9.42 ms`
- ack RTT p95: `10.845 ms`
- ack RTT max: `219.023 ms`
- send-to-finish min: `5.633 ms`
- send-to-finish median: `17.989 ms`
- send-to-finish avg: `44.613 ms`
- send-to-finish p95: `231.524 ms`
- send-to-finish max: `335.313 ms`
