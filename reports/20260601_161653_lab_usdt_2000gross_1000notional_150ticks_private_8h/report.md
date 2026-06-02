# LeadLag Live Run Report

## 基本信息

- run_id: `20260601_161653_lab_usdt_2000gross_1000notional_150ticks_private_8h`
- 策略配置: `/home/liuxiang/tmp/20260601_161653_lab_usdt_2000gross_1000notional_150ticks_private_8h/configs/lead_lag_lab_usdt_live_strategy_20260601.toml`
- 源日志: `/home/liuxiang/tmp/20260601_161653_lab_usdt_2000gross_1000notional_150ticks_private_8h/merged_live.log`
- guard stdout: `/home/liuxiang/tmp/20260601_161653_lab_usdt_2000gross_1000notional_150ticks_private_8h/guarded_live.stdout`
- 首个 signal 时间: `2026-06-01 16:19:15.528009709`
- 最后 signal 时间: `2026-06-02 00:09:58.809111561`

## 运行结束状态

- guard result: `normal_exit_flat`
- strategy exit_code: `0`
- final_check: `flat=true`，`open_orders=[]`，`LAB_USDT position size=0`
- strategy summary: `needs_reconcile=false`，`manual_intervention=false`，`feedback_continuity_lost_events=0`

说明：feedback session 在策略正常结束后按自身 duration 退出，关闭阶段出现的 `feedback_global_continuity_lost` 不属于策略运行期间的回报连续性异常；本次策略 summary 中 `feedback_continuity_lost_events=0`。

## 同目录 CSV

- `signal.csv`: 223 条 signal，并关联对应 order
- `order_detail.csv`: 223 条 order 明细
- `position.csv`: 15 条 position 明细
- `latency.csv`: 223 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `223`
- submitted order: `223`
- Gate send ok: `223`
- ack: `223`
- order finished: `223`
- 有成交 order: `20`

| symbol | signals |
| --- | --- |
| LAB_USDT | 223 |

| status | count |
| --- | --- |
| kCancelled | 203 |
| kFilled | 14 |
| kPartiallyCancelled | 6 |

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 20 | -1143.192 | -9.75 | -8977.4 | 89 | 1 | 10 | 1293.322 |
| entry | 8 | -816.23 | -295.17 | -3473.33 | 0 | 0 | 4 | 966.23 |
| exit | 12 | -1361.166 | -9.75 | -8977.4 | 89 | 1 | 6 | 1511.382 |

## PnL

- gross PnL: `27.3285079999999980890724`
- net PnL: `24.90781954239999817128505148`

### Raw PnL 和胜率

Raw PnL 使用 entry / exit 的 `raw_price` 计算，fee 仍使用 report CSV 中的配置费率估算值；胜率按 net PnL > 0 计算。

- actual gross PnL: `27.3285079999999980890724`
- actual net PnL: `24.90781954239999817128505148`
- actual win rate: `75.00%` (9/12)
- raw gross PnL: `18.638319999999998491606`
- raw net PnL: `16.21763154239999857381865148`
- raw win rate: `66.67%` (8/12)

| symbol | direction | matched | actual_gross | raw_gross | actual_net | raw_net | actual_minus_raw_gross |
| --- | --- | --- | --- | --- | --- | --- | --- |
| LAB_USDT | kLong | 0.6 | 4.602204 | 3.1992 | 4.2148822008 | 2.8118782008 | 1.403004 |
| LAB_USDT | kLong | 0.39999999999999997 | 0.5331679999999999600124 | 0.41119999999999996916 | 0.28211915359999997884106348 | 0.16015115359999998798866348 | 0.1219679999999999908524 |
| LAB_USDT | kLong | 0.2 | 0.170434 | 0.1114 | 0.0449288068 | -0.0141051932 | 0.059034 |
| LAB_USDT | kShort | 0.5 | 14.9885 | 12.3435 | 14.6570749 | 12.0120749 | 2.645 |
| LAB_USDT | kShort | 0.19999999999999996 | 9.31679999999999813664 | 7.521319999999998495736 | 9.187699839999998149736352 | 7.392219839999998508832352 | 1.795479999999999640904 |
| LAB_USDT | kLong | 0.09999999999999998 | 0.51369999999999989726 | 0.32889999999999993422 | 0.449176699999999903702056 | 0.264376699999999940662056 | 0.18479999999999996304 |
| LAB_USDT | kShort | 0.39999999999999997 | -3.27039999999999975472 | -3.23479999999999975739 | -3.525320159999999735600988 | -3.489720159999999738270988 | -0.03559999999999999733 |
| LAB_USDT | kShort | 0.2 | -1.6632 | -1.6632 | -1.79066568 | -1.79066568 | 0 |
| LAB_USDT | kShort | 0.19999999999999996 | 0.75059999999999984988 | 0.75059999999999984988 | 0.626967159999999874606568 | 0.626967159999999874606568 | 0 |
| LAB_USDT | kShort | 0.2 | 0.6764 | 0.6764 | 0.55275232 | 0.55275232 | 0 |
| LAB_USDT | kShort | 0.2 | 0.7114 | 0.7114 | 0.58775932 | 0.58775932 | 0 |
| LAB_USDT | kShort | 0.6 | -0.001098 | -2.5176 | -0.3795550188 | -2.8960570188 | 2.516502 |

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| LAB_USDT | kLong | 0.6 | 4.602204 | 4.2148822008 |
| LAB_USDT | kLong | 0.39999999999999997 | 0.5331679999999999600124 | 0.28211915359999997884106348 |
| LAB_USDT | kLong | 0.2 | 0.170434 | 0.0449288068 |
| LAB_USDT | kLong |  |  |  |
| LAB_USDT | kShort | 0.5 | 14.9885 | 14.6570749 |
| LAB_USDT | kShort | 0.19999999999999996 | 9.31679999999999813664 | 9.187699839999998149736352 |
| LAB_USDT | kLong | 0.09999999999999998 | 0.51369999999999989726 | 0.449176699999999903702056 |
| LAB_USDT | kShort | 0.39999999999999997 | -3.27039999999999975472 | -3.525320159999999735600988 |
| LAB_USDT | kShort | 0.2 | -1.6632 | -1.79066568 |
| LAB_USDT | kShort |  |  |  |
| LAB_USDT | kShort | 0.19999999999999996 | 0.75059999999999984988 | 0.626967159999999874606568 |
| LAB_USDT | kShort | 0.2 | 0.6764 | 0.55275232 |
| LAB_USDT | kShort | 0.2 | 0.7114 | 0.58775932 |
| LAB_USDT | kShort |  |  |  |
| LAB_USDT | kShort | 0.6 | -0.001098 | -0.3795550188 |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0.511 ms`
- ack RTT median: `0.615 ms`
- ack RTT avg: `2.744 ms`
- ack RTT p95: `7.313 ms`
- ack RTT max: `243.996 ms`
- Gate Ack process min: `0.081 ms`
- Gate Ack process median: `0.136 ms`
- Gate Ack process avg: `2.122 ms`
- Gate Ack process p95: `6.458 ms`
- Gate Ack process max: `243.5 ms`

### Ack RTT 三段拆解

Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。

| stage | samples | min_ms | median_ms | avg_ms | p95_ms | p99_ms | max_ms | >1ms | >5ms | >10ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ack RTT total | 223 | 0.511 | 0.615 | 2.744 | 7.313 | 14.068 | 243.996 | 44 | 23 | 5 |
| 上行 send->Gate x_in | 223 | 0.18 | 0.221 | 0.304 | 0.273 | 0.994 | 15.19 | 2 | 1 | 1 |
| Gate x_in->x_out | 223 | 0.081 | 0.136 | 2.122 | 6.458 | 13.012 | 243.5 | 35 | 18 | 4 |
| 下行 Gate x_out->local | 223 | 0.148 | 0.245 | 0.318 | 0.353 | 2.622 | 4.79 | 6 | 0 | 0 |

- `>5ms` Ack tail dominant stage: Gate in->out=21, 上行=1, 下行=1
| local_order_id | ack_rtt_ms | upstream_ms | gate_in_to_out_ms | downstream_ms | dominant_stage |
| --- | --- | --- | --- | --- | --- |
| 288230376151711806 | 243.996 | 0.21 | 243.5 | 0.286 | Gate in->out |
| 288230376151711965 | 20.471 | 15.19 | 0.491 | 4.79 | 上行 |
| 288230376151711894 | 14.068 | 0.319 | 13.486 | 0.264 | Gate in->out |
| 288230376151711964 | 13.523 | 0.208 | 13.012 | 0.303 | Gate in->out |
| 288230376151711909 | 13.2 | 0.238 | 12.728 | 0.234 | Gate in->out |
| 288230376151711962 | 9.351 | 0.271 | 6.458 | 2.622 | Gate in->out |
| 288230376151711940 | 9.208 | 0.203 | 8.718 | 0.287 | Gate in->out |
| 288230376151711967 | 8.473 | 0.251 | 7.948 | 0.274 | Gate in->out |
| 288230376151711852 | 8.055 | 0.218 | 7.532 | 0.305 | Gate in->out |
| 288230376151711882 | 7.503 | 0.225 | 7.009 | 0.269 | Gate in->out |
- latency diagnostic outliers: `23`
| local_order_id | reason | ack_rtt_ms | send_to_first_drive_read_ms | drive_read_duration_ms |
| --- | --- | --- | --- | --- |
| 288230376151711766 | kAckRttThreshold | 7.415 | 0.03 | 0.014 |
| 288230376151711774 | kAckRttThreshold | 6.57 | 0.029 | 0.014 |
| 288230376151711806 | kAckRttThreshold | 243.996 | 0.022 | 0.013 |
| 288230376151711812 | kAckRttThreshold | 5.097 | 0.031 | 0.014 |
| 288230376151711844 | kAckRttThreshold | 7.247 | 0.035 | 0.013 |
| 288230376151711852 | kAckRttThreshold | 8.055 | 0.031 | 0.013 |
| 288230376151711872 | kAckRttThreshold | 5.475 | 0.031 | 0.014 |
| 288230376151711882 | kAckRttThreshold | 7.503 | 0.03 | 0.025 |
| 288230376151711894 | kAckRttThreshold | 14.068 | 0.033 | 0.013 |
| 288230376151711897 | kAckRttThreshold | 5.192 | 0.032 | 0.017 |
- send-to-finish min: `1.394 ms`
- send-to-finish median: `8.949 ms`
- send-to-finish avg: `78.141 ms`
- send-to-finish p95: `397.957 ms`
- send-to-finish max: `2803.566 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-0.067 ms`
- exchange Ack-to-finish median: `5.82 ms`
- exchange Ack-to-finish avg: `74.118 ms`
- exchange Ack-to-finish p95: `379.885 ms`
- exchange Ack-to-finish max: `2797.617 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 288230376151711800 | LAB_USDT | kCancelled | kImmediateOrCancel | 2797.617 | 2798.857 | 2803.566 |
| 288230376151711801 | LAB_USDT | kCancelled | kImmediateOrCancel | 1849.863 | 1851.313 | 1851.926 |
| 288230376151711965 | LAB_USDT | kFilled | kUnknown | 1272.721 | 1269.09 | 1289.562 |
| 288230376151711966 | LAB_USDT | kFilled | kUnknown | 1271.409 | 1272.938 | 1273.596 |
| 288230376151711802 | LAB_USDT | kFilled | kUnknown | 1040.088 | 1041.932 | 1042.504 |
