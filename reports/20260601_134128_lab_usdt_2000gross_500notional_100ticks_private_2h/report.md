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

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 30 | -464.6 | 0 | -5107 | 84 | 7 | 10 | 564.6 |
| entry | 13 | -392.718 | 0 | -3899 | 84 | 5 | 4 | 492.718 |
| exit | 17 | -519.568 | 0 | -5107 | 74 | 2 | 6 | 619.568 |

## PnL

- gross PnL: `8.8162`
- net PnL: `7.32256736064`

### Raw PnL 和胜率

Raw PnL 使用 entry / exit 的 `raw_price` 计算，fee 仍使用 report CSV 中的配置费率估算值；胜率按 net PnL > 0 计算。

- actual gross PnL: `8.8162`
- actual net PnL: `7.32256736064`
- actual win rate: `52.94%` (9/17)
- raw gross PnL: `5.2876`
- raw net PnL: `3.79396736064`
- raw win rate: `52.94%` (9/17)

#### LAB_USDT 实际费率复算

本报告 CSV 的 `fee_rate_config` 来自本次运行配置 `lag_taker_fee = 0.00016`，用于复现策略当时的成本假设。实盘后通过 Gate REST 查询到 `LAB_USDT` 合约级 `taker_fee = 0.00020`；按该实际费率复算：

- actual gross PnL: `8.8162`
- actual fee: `1.8670407992`
- actual net PnL: `6.9491592008`
- actual win rate: `52.94%` (9/17)
- raw gross PnL: `5.2876`
- raw fee: `1.8670407992`
- raw net PnL: `3.4205592008`
- raw win rate: `52.94%` (9/17)

| symbol | direction | matched | actual_gross | raw_gross | actual_net | raw_net | actual_minus_raw_gross |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LAB_USDT | kLong | 0.3 | 2.873502 | 2.8326 | 2.73871216032 | 2.69781016032 | 0.040902 |
| LAB_USDT | kShort | 0.1 | -0.0497 | -0.1845 | -0.094629328 | -0.229429328 | 0.1348 |
| LAB_USDT | kShort | 0.1 | -0.1767 | -0.3115 | -0.221649648 | -0.356449648 | 0.1348 |
| LAB_USDT | kShort | 0.1 | -0.5055 | -0.6403 | -0.550502256 | -0.685302256 | 0.1348 |
| LAB_USDT | kLong | 0.3 | 0.912198 | 0.2514 | 0.77592443232 | 0.11512643232 | 0.660798 |
| LAB_USDT | kShort | 0.1 | -0.2012 | -0.1961 | -0.247967808 | -0.242867808 | -0.0051 |
| LAB_USDT | kShort | 0.2 | -0.5902 | -0.58 | -0.683765664 | -0.673565664 | -0.0102 |
| LAB_USDT | kShort | 0.2 | 0.9862 | 0.7512 | 0.889877408 | 0.654877408 | 0.235 |
| LAB_USDT | kLong | 0.2 | 2.379 | 1.3738 | 2.281606048 | 1.276406048 | 1.0052 |
| LAB_USDT | kShort | 0.1 | 1.1107 | 1.1181 | 1.062369936 | 1.069769936 | -0.0074 |
| LAB_USDT | kShort | 0.2 | -0.4554 | -1.2326 | -0.556175456 | -1.333375456 | 0.7772 |
| LAB_USDT | kShort | 0.1 | -0.2052 | -0.6176 | -0.255584128 | -0.667984128 | 0.4124 |
| LAB_USDT | kShort | 0.2 | 1.9056 | 1.8914 | 1.804095104 | 1.789895104 | 0.0142 |
| LAB_USDT | kShort | 0.2 | 0.3038 | 0.3038 | 0.202648608 | 0.202648608 | 0 |
| LAB_USDT | kShort | 0.3 | -1.1271 | -1.1349 | -1.279687056 | -1.287487056 | 0.0078 |
| LAB_USDT | kLong | 0.2 | 1.4362 | 1.4428 | 1.335690528 | 1.342290528 | -0.0066 |
| LAB_USDT | kLong | 0.2 | 0.22 | 0.22 | 0.12160448 | 0.12160448 | 0 |

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

### Ack RTT 三段拆解

Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。

| stage | samples | min_ms | median_ms | avg_ms | p95_ms | p99_ms | max_ms | >1ms | >5ms | >10ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ack RTT total | 177 | 0.483 | 0.612 | 1.712 | 7.714 | 14.943 | 80.07 | 25 | 11 | 6 |
| 上行 send->Gate x_in | 177 | 0.192 | 0.231 | 0.274 | 0.326 | 1.284 | 4.529 | 2 | 0 | 0 |
| Gate x_in->x_out | 177 | 0.076 | 0.133 | 1.14 | 4.494 | 14.496 | 79.576 | 17 | 8 | 6 |
| 下行 Gate x_out->local | 177 | 0.157 | 0.241 | 0.297 | 0.393 | 3.053 | 3.208 | 3 | 0 | 0 |

- `>5ms` Ack tail dominant stage: Gate in->out=10, 上行=1
| local_order_id | ack_rtt_ms | upstream_ms | gate_in_to_out_ms | downstream_ms | dominant_stage |
| --- | --- | --- | --- | --- | --- |
| 288230376151711846 | 80.07 | 0.22 | 79.576 | 0.274 | Gate in->out |
| 288230376151711915 | 14.943 | 0.254 | 14.496 | 0.192 | Gate in->out |
| 288230376151711787 | 13.339 | 0.247 | 12.829 | 0.263 | Gate in->out |
| 288230376151711903 | 13.211 | 0.499 | 12.417 | 0.295 | Gate in->out |
| 288230376151711854 | 11.156 | 0.26 | 10.698 | 0.197 | Gate in->out |
| 288230376151711876 | 11.005 | 0.212 | 10.462 | 0.331 | Gate in->out |
| 288230376151711838 | 9.997 | 1.284 | 5.505 | 3.208 | Gate in->out |
| 288230376151711820 | 7.739 | 4.529 | 0.157 | 3.053 | 上行 |
| 288230376151711817 | 7.714 | 0.216 | 7.239 | 0.258 | Gate in->out |
| 288230376151711750 | 5.143 | 0.316 | 3.093 | 1.734 | Gate in->out |
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
