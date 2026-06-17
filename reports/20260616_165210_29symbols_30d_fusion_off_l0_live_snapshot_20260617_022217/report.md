# LeadLag Live Run Report

## 基本信息

- run_id: `20260616_165210_29symbols_30d_fusion_off_l0_live_snapshot_20260617_022217`
- 说明: 当前 30 天实盘仍在运行；本报告是截至 `2026-06-17 02:22:17 UTC` 的成交快照，不是最终停机报告。
- 策略配置: `/home/liuxiang/tmp/20260616_165210_29symbols_30d_fusion_off_l0_live/configs/lead_lag_29symbols_no_ton_fusion_live_strategy.toml`
- 源日志: `/home/liuxiang/tmp/20260616_165210_29symbols_30d_fusion_off_l0_live/report_snapshot_20260617_022217/strategy.log`
- guard stdout: `/home/liuxiang/tmp/20260616_165210_29symbols_30d_fusion_off_l0_live/report_snapshot_20260617_022217/guarded_live.stdout.log`
- 首个 signal 时间: `2026-06-16 16:54:58.075435917`
- 最后 signal 时间: `2026-06-17 02:21:44.389269313`

## Pair Freshness 参数

单位为 `ms`，来自策略配置的 `max_lead_freshness_ms` / `max_lag_freshness_ms`。

| symbol | symbol_id | lead_exchange | lag_exchange | max_lead_freshness_ms | max_lag_freshness_ms |
| --- | --- | --- | --- | --- | --- |
| CLO_USDT | 113 | binance | gate | 5 | 20 |
| SLX_USDT | 381 | binance | gate | 5 | 20 |
| AIA_USDT | 14 | binance | gate | 5 | 20 |
| WLD_USDT | 461 | binance | gate | 5 | 20 |
| PIEVERSE_USDT | 325 | binance | gate | 5 | 20 |
| OPN_USDT | 314 | binance | gate | 5 | 20 |
| LIT_USDT | 256 | binance | gate | 5 | 20 |
| MYX_USDT | 292 | binance | gate | 5 | 20 |
| ENA_USDT | 151 | binance | gate | 5 | 20 |
| STO_USDT | 401 | binance | gate | 5 | 20 |
| ICP_USDT | 213 | binance | gate | 5 | 20 |
| UB_USDT | 439 | binance | gate | 5 | 20 |
| BEAT_USDT | 71 | binance | gate | 5 | 20 |
| XPL_USDT | 472 | binance | gate | 5 | 20 |
| FARTCOIN_USDT | 165 | binance | gate | 5 | 20 |
| ZEC_USDT | 481 | binance | gate | 5 | 20 |
| SKYAI_USDT | 378 | binance | gate | 5 | 20 |
| VIRTUAL_USDT | 453 | binance | gate | 5 | 20 |
| EPIC_USDT | 155 | binance | gate | 5 | 20 |
| INJ_USDT | 220 | binance | gate | 5 | 20 |
| PENGU_USDT | 322 | binance | gate | 5 | 20 |
| H_USDT | 211 | binance | gate | 5 | 20 |
| NEAR_USDT | 295 | binance | gate | 5 | 20 |
| XLM_USDT | 468 | binance | gate | 5 | 20 |
| FIL_USDT | 171 | binance | gate | 5 | 20 |
| ONDO_USDT | 307 | binance | gate | 5 | 20 |
| SUI_USDT | 404 | binance | gate | 5 | 20 |
| WIF_USDT | 460 | binance | gate | 5 | 20 |
| FET_USDT | 166 | binance | gate | 5 | 20 |

## 同目录 CSV

- `signal.csv`: 2561 条 signal，并关联对应 order
- `order_detail.csv`: 1022 条 order 明细
- `position.csv`: 33 条 position 明细
- `latency.csv`: 1022 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `2561`
- submitted order: `1022`
- Gate send ok: `1022`
- ack: `1022`
- order finished: `1022`
- 有成交 order: `54`

| symbol | signals |
| --- | --- |
| AIA_USDT | 30 |
| BEAT_USDT | 343 |
| CLO_USDT | 119 |
| ENA_USDT | 23 |
| EPIC_USDT | 131 |
| FARTCOIN_USDT | 18 |
| FET_USDT | 39 |
| FIL_USDT | 8 |
| H_USDT | 803 |
| ICP_USDT | 3 |
| INJ_USDT | 15 |
| LIT_USDT | 71 |
| MYX_USDT | 28 |
| NEAR_USDT | 15 |
| ONDO_USDT | 11 |
| OPN_USDT | 7 |
| PENGU_USDT | 41 |
| PIEVERSE_USDT | 16 |
| SKYAI_USDT | 234 |
| SLX_USDT | 70 |
| STO_USDT | 4 |
| SUI_USDT | 10 |
| UB_USDT | 83 |
| VIRTUAL_USDT | 21 |
| WIF_USDT | 5 |
| WLD_USDT | 188 |
| XLM_USDT | 82 |
| XPL_USDT | 51 |
| ZEC_USDT | 92 |

| status | count |
| --- | --- |
| kCancelled | 488 |
| kFilled | 36 |
| kPartiallyCancelled | 18 |
| kRejected | 480 |

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 54 | 0.74 | 0.49 | -3 | 4 | 31 | 2 | 1.723 |
| entry | 21 | 1.258 | 1 | 0 | 4 | 15 | 0 | 1.266 |
| exit | 33 | 0.411 | 0 | -3 | 2 | 16 | 2 | 2.014 |

## PnL

- gross PnL: `-0.250204`
- net PnL: `-0.9980968672`

### Raw PnL 和胜率

Raw PnL 使用 entry / exit 的 `raw_price` 计算，fee 仍使用 report CSV 中的配置费率估算值；胜率按 net PnL > 0 计算。

- actual gross PnL: `-0.250204`
- actual net PnL: `-0.9980968672`
- actual win rate: `18.18%` (6/33)
- raw gross PnL: `0.3103`
- raw net PnL: `-0.4375928672`
- raw win rate: `33.33%` (11/33)

| symbol | direction | matched | actual_gross | raw_gross | actual_net | raw_net | actual_minus_raw_gross |
| --- | --- | --- | --- | --- | --- | --- | --- |
| AIA_USDT | kShort | 94 | -0.171174 | -0.1504 | -0.1925161548 | -0.1717421548 | -0.020774 |
| AIA_USDT | kShort | 42 | -0.09177 | -0.0798 | -0.101308914 | -0.089338914 | -0.01197 |
| AIA_USDT | kShort | 40 | -0.0922 | -0.088 | -0.10128564 | -0.09708564 | -0.0042 |
| BEAT_USDT | kLong | 4 | 0.488 | 0.488 | 0.449048 | 0.449048 | 0 |
| BEAT_USDT | kLong | 4 | -0.108 | -0.108 | -0.1462696 | -0.1462696 | 0 |
| CLO_USDT | kLong | 61 | -0.0732 | -0.0549 | -0.1063352 | -0.0880352 | -0.0183 |
| CLO_USDT | kLong | 6 | -0.0072 | -0.0054 | -0.0104592 | -0.0086592 | -0.0018 |
| ENA_USDT | kShort | 75 | 0.015 | 0.045 | -0.010428 | 0.019572 | -0.03 |
| EPIC_USDT | kShort | 157 | 0.0628 | 0.0628 | 0.0230162 | 0.0230162 | 0 |
| EPIC_USDT | kLong | 155 | -0.093 | -0.0775 | -0.1329032 | -0.1174032 | -0.0155 |
| EPIC_USDT | kLong | 155 | 0.0713 | 0.0775 | 0.03137014 | 0.03757014 | -0.0062 |
| EPIC_USDT | kLong | 155 | 0.1085 | 0.1085 | 0.0685937 | 0.0685937 | 0 |
| EPIC_USDT | kLong | 155 | -0.0155 | 0 | -0.0554435 | -0.0399435 | -0.0155 |
| EPIC_USDT | kShort | 24 | -0.003984 | 0 | -0.0101614368 | -0.0061774368 | -0.003984 |
| EPIC_USDT | kShort | 11 | -0.0066 | -0.0033 | -0.00943228 | -0.00613228 | -0.0033 |
| EPIC_USDT | kShort | 79 | -0.0711 | -0.0474 | -0.09144566 | -0.06774566 | -0.0237 |
| EPIC_USDT | kShort | 41 | -0.041779 | -0.0328 | -0.0523391158 | -0.0433601158 | -0.008979 |
| LIT_USDT | kLong | 51 | -0.051 | 0.051 | -0.086649 | 0.015351 | -0.102 |
| LIT_USDT | kLong | 6 | -0.006 | 0 | -0.010194 | -0.004194 | -0.006 |
| LIT_USDT | kLong | 56 | -0.112 | -0.056 | -0.1518272 | -0.0958272 | -0.056 |
| LIT_USDT | kShort | 51 | -0.02499 | 0.051 | -0.060878598 | 0.015111402 | -0.07599 |
| OPN_USDT | kLong | 61 | -0.061 | 0 | -0.079605 | -0.018605 | -0.061 |
| PENGU_USDT | kShort | 20 | -0.01666 | -0.01 | -0.022441308 | -0.015781308 | -0.00666 |
| PENGU_USDT | kShort | 3 | -0.002559 | -0.0021 | -0.0034262082 | -0.0029672082 | -0.000459 |
| PENGU_USDT | kShort | 40 | -0.05072 | -0.044 | -0.062286096 | -0.055566096 | -0.00672 |
| PENGU_USDT | kShort | 2 | -0.001906 | -0.0022 | -0.0024841788 | -0.0027781788 | 0.000294 |
| PENGU_USDT | kShort | 73 | -0.112712 | -0.1022 | -0.1338241548 | -0.1233121548 | -0.010512 |
| PIEVERSE_USDT | kShort | 14 | 0.10402 | 0.112 | 0.064280804 | 0.072260804 | -0.00798 |
| SKYAI_USDT | kLong | 12 | 0.003192 | 0.0072 | -0.0178537584 | -0.0138457584 | -0.004008 |
| SKYAI_USDT | kLong | 21 | 0.2289 | 0.2373 | 0.1897623 | 0.1981623 | -0.0084 |
| SLX_USDT | kShort | 18 | 0.012816 | 0.018 | -0.0003553632 | 0.0048286368 | -0.005184 |
| SLX_USDT | kShort | 6 | 0.004272 | 0.006 | -0.0001184544 | 0.0016095456 | -0.001728 |
| VIRTUAL_USDT | kShort | 15 | -0.13395 | -0.09 | -0.17189679 | -0.12794679 | -0.04395 |

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| AIA_USDT | kShort | 94 | -0.171174 | -0.1925161548 |
| AIA_USDT | kShort | 42 | -0.09177 | -0.101308914 |
| AIA_USDT | kShort | 40 | -0.0922 | -0.10128564 |
| BEAT_USDT | kLong | 4 | 0.488 | 0.449048 |
| BEAT_USDT | kLong | 4 | -0.108 | -0.1462696 |
| CLO_USDT | kLong | 61 | -0.0732 | -0.1063352 |
| CLO_USDT | kLong | 6 | -0.0072 | -0.0104592 |
| ENA_USDT | kShort | 75 | 0.015 | -0.010428 |
| EPIC_USDT | kShort | 157 | 0.0628 | 0.0230162 |
| EPIC_USDT | kLong | 155 | -0.093 | -0.1329032 |
| EPIC_USDT | kLong | 155 | 0.0713 | 0.03137014 |
| EPIC_USDT | kLong | 155 | 0.1085 | 0.0685937 |
| EPIC_USDT | kLong | 155 | -0.0155 | -0.0554435 |
| EPIC_USDT | kShort | 24 | -0.003984 | -0.0101614368 |
| EPIC_USDT | kShort | 11 | -0.0066 | -0.00943228 |
| EPIC_USDT | kShort | 79 | -0.0711 | -0.09144566 |
| EPIC_USDT | kShort | 41 | -0.041779 | -0.0523391158 |
| LIT_USDT | kLong | 51 | -0.051 | -0.086649 |
| LIT_USDT | kLong | 6 | -0.006 | -0.010194 |
| LIT_USDT | kLong | 56 | -0.112 | -0.1518272 |
| LIT_USDT | kShort | 51 | -0.02499 | -0.060878598 |
| OPN_USDT | kLong | 61 | -0.061 | -0.079605 |
| PENGU_USDT | kShort | 20 | -0.01666 | -0.022441308 |
| PENGU_USDT | kShort | 3 | -0.002559 | -0.0034262082 |
| PENGU_USDT | kShort | 40 | -0.05072 | -0.062286096 |
| PENGU_USDT | kShort | 2 | -0.001906 | -0.0024841788 |
| PENGU_USDT | kShort | 73 | -0.112712 | -0.1338241548 |
| PIEVERSE_USDT | kShort | 14 | 0.10402 | 0.064280804 |
| SKYAI_USDT | kLong | 12 | 0.003192 | -0.0178537584 |
| SKYAI_USDT | kLong | 21 | 0.2289 | 0.1897623 |
| SLX_USDT | kShort | 18 | 0.012816 | -0.0003553632 |
| SLX_USDT | kShort | 6 | 0.004272 | -0.0001184544 |
| VIRTUAL_USDT | kShort | 15 | -0.13395 | -0.17189679 |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0.491 ms`
- ack RTT median: `0.632 ms`
- ack RTT avg: `1.646 ms`
- ack RTT p95: `4.836 ms`
- ack RTT max: `206.26 ms`
- Gate Ack process min: `0.083 ms`
- Gate Ack process median: `0.139 ms`
- Gate Ack process avg: `1.021 ms`
- Gate Ack process p95: `4.175 ms`
- Gate Ack process max: `205.633 ms`

### Ack RTT 三段拆解

Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。

| stage | samples | min_ms | median_ms | avg_ms | p95_ms | p99_ms | max_ms | >1ms | >5ms | >10ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ack RTT total | 1022 | 0.491 | 0.632 | 1.646 | 4.836 | 13.337 | 206.26 | 114 | 46 | 20 |
| 上行 send->Gate x_in | 1022 | 0.159 | 0.199 | 0.317 | 0.276 | 1.477 | 40.371 | 12 | 4 | 2 |
| Gate x_in->x_out | 1022 | 0.083 | 0.139 | 1.021 | 4.175 | 12.678 | 205.633 | 69 | 37 | 17 |
| 下行 Gate x_out->local | 1022 | 0.165 | 0.292 | 0.309 | 0.4 | 0.967 | 2.249 | 10 | 0 | 0 |

- `>5ms` Ack tail dominant stage: Gate in->out=41, 上行=5
| local_order_id | ack_rtt_ms | upstream_ms | gate_in_to_out_ms | downstream_ms | dominant_stage |
| --- | --- | --- | --- | --- | --- |
| 288230376151712227 | 206.26 | 0.189 | 205.633 | 0.438 | Gate in->out |
| 288230376151711991 | 117.14 | 0.204 | 116.548 | 0.388 | Gate in->out |
| 288230376151711847 | 55.023 | 0.239 | 54.556 | 0.228 | Gate in->out |
| 288230376151711848 | 51.751 | 40.371 | 11.125 | 0.255 | 上行 |
| 288230376151711750 | 47.689 | 3.757 | 43.651 | 0.281 | Gate in->out |
| 288230376151711849 | 44.15 | 39.101 | 4.719 | 0.33 | 上行 |
| 288230376151712145 | 30.028 | 0.199 | 29.356 | 0.473 | Gate in->out |
| 288230376151711871 | 27.417 | 2.453 | 23.951 | 1.013 | Gate in->out |
| 288230376151712681 | 19.094 | 0.182 | 18.574 | 0.338 | Gate in->out |
| 288230376151711777 | 13.564 | 0.204 | 13.038 | 0.322 | Gate in->out |
- latency diagnostic outliers: `46`
| local_order_id | reason | ack_rtt_ms | send_to_first_drive_read_ms | drive_read_duration_ms |
| --- | --- | --- | --- | --- |
| 288230376151711745 | kAckRttThreshold | 13.258 | 0.032 | 0.014 |
| 288230376151711750 | kAckRttThreshold | 47.689 | 0.019 | 0.014 |
| 288230376151711767 | kAckRttThreshold | 13.337 | 0.032 | 0.013 |
| 288230376151711777 | kAckRttThreshold | 13.564 | 0.031 | 0.015 |
| 288230376151711796 | kAckRttThreshold | 7.97 | 0.032 | 0.014 |
| 288230376151711826 | kAckRttThreshold | 7.338 | 0.038 | 0.012 |
| 288230376151711849 | kAckRttThreshold | 44.15 | 0.014 | 0.013 |
| 288230376151711847 | kAckRttThreshold | 55.023 | 0.035 | 0.012 |
| 288230376151711848 | kAckRttThreshold | 51.751 | 0.016 | 0.013 |
| 288230376151711871 | kAckRttThreshold | 27.417 | 0.018 | 0.013 |
- send-to-finish min: `1.331 ms`
- send-to-finish median: `3.362 ms`
- send-to-finish avg: `61.281 ms`
- send-to-finish p95: `63.096 ms`
- send-to-finish max: `5615.594 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-0.253 ms`
- exchange Ack-to-finish median: `3.348 ms`
- exchange Ack-to-finish avg: `81.105 ms`
- exchange Ack-to-finish p95: `222.965 ms`
- exchange Ack-to-finish max: `2503.661 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 288230376151711863 | XLM_USDT | kCancelled | kImmediateOrCancel | 2503.661 | 5614.897 | 5615.594 |
| 288230376151711862 | PENGU_USDT | kCancelled | kImmediateOrCancel | 2476.624 | 2732.687 | 2733.956 |
| 288230376151711864 | ZEC_USDT | kCancelled | kImmediateOrCancel | 2371.623 | 5365.918 | 5366.963 |
| 288230376151711861 | FET_USDT | kCancelled | kImmediateOrCancel | 2335.87 | 2585.308 | 2586.083 |
| 288230376151711860 | AIA_USDT | kCancelled | kImmediateOrCancel | 2181.74 | 2766.153 | 2766.82 |
