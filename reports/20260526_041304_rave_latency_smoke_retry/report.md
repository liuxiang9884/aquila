# LeadLag Live Run Report

## 基本信息

- run_id: `20260526_041304_rave_latency_smoke_retry`
- 策略配置: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs/strategy__lead_lag_requested_11symbols_live_strategy_20260522.toml`
- 源日志: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/merged_live.log`
- guard stdout: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/guarded_live.stdout`

## Runtime Profile

- affinity profile: `lead_lag_requested_12symbols_node0`
- affinity split: `true`
- affinity output dir: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs`
- gate_market_data_cpu: `2`
- binance_market_data_cpu: `3`
- strategy_order_owner_cpu: `4`
- gate_order_feedback_cpu: `6`
- log_backend_cpu: `5`
- generated binance_market_data config: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs/binance_market_data__binance_data_session_requested_20260521.toml`
- generated gate_market_data config: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs/gate_market_data__gate_data_session_requested_20260521.toml`
- generated gate_order_feedback config: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs/gate_order_feedback__gate_order_feedback_session.toml`
- generated gate_order_session config: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs/gate_order_session__gate_order_session.toml`
- generated strategy config: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs/strategy__lead_lag_requested_11symbols_live_strategy_20260522.toml`
- generated strategy_data_reader config: `/home/liuxiang/tmp/20260526_041304_rave_latency_smoke_retry/configs/strategy_data_reader__strategy_data_reader_requested_20260521.toml`

## 同目录 CSV

- `signal.csv`: 0 条 signal，并关联对应 order
- `order_detail.csv`: 2 条 order 明细
- `position.csv`: 0 条 position 明细
- `latency.csv`: 2 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `0`
- submitted order: `2`
- Gate send ok: `2`
- ack: `2`
- order finished: `0`
- 有成交 order: `2`

| status | count |
| --- | --- |
| kFilled | 2 |

## PnL

- gross PnL: `0`
- net PnL: `0`

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `3.765 ms`
- ack RTT median: `3.765 ms`
- ack RTT avg: `3.864 ms`
- ack RTT p95: `3.963 ms`
- ack RTT max: `3.963 ms`
- send-to-finish min: `3.765 ms`
- send-to-finish median: `3.765 ms`
- send-to-finish avg: `3.864 ms`
- send-to-finish p95: `3.963 ms`
- send-to-finish max: `3.963 ms`
