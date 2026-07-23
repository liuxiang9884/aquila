# LeadLag Live Run Report

## 基本信息

- run_id: `20260721_142306_bitget_combined46_n6_fanout1_12h`
- exchange: `bitget`
- 策略配置: `/home/liuxiang/tmp/20260721_142306_bitget_combined46_n6_fanout1_12h/configs/strategy__strategy_source.toml`
- 源日志: `/home/liuxiang/tmp/20260721_142306_bitget_combined46_n6_fanout1_12h/logs/strategy_20260721_143359.log`
- guard stdout: `/home/liuxiang/tmp/20260721_142306_bitget_combined46_n6_fanout1_12h/logs/guarded_live.stdout`
- 首个 signal 时间: `2026-07-21 14:35:12.636839437`
- 最后 signal 时间: `2026-07-22 02:33:05.182994336`

## Live 安全结果

- ok: `true`
- result: `normal_exit_flat`
- exit_code: `0`
- final flat: `true`
- final open orders: `0`
- final positions: `0`
- quiescence ok: `true`
- quiescence result: `stopped`

## Strategy 终态审计

- exit_code: `0`
- runtime_exit_code: `0`
- emergency_handoff: `false`
- recovery_state: `normal`
- needs_reconcile: `false`
- manual_intervention: `false`
- new_entries_paused: `false`
- unknown_local_order_feedbacks: `0`
- duplicate_or_stale_feedbacks: `0`
- terminal_feedbacks_ignored: `0`
- feedback_continuity_lost_events: `0`

## Run Definition

- commit: `5a69eaf`
- duration_sec: `43200`
- limiter: `absent`
- order_fanout: `1`
- Bitget fusion: `6` (HA=`3`, HS=`3`)

## Pair Freshness 参数

单位为 `ms`，来自策略配置的 `max_lead_freshness_ms` / `max_lag_freshness_ms`。

| symbol | symbol_id | lead_exchange | lag_exchange | max_lead_freshness_ms | max_lag_freshness_ms |
| --- | --- | --- | --- | --- | --- |
| SKHY_USDT | 502 | binance | bitget | 3 | 500 |
| SNDK_USDT | 503 | binance | bitget | 3 | 500 |
| SKHYNIX_USDT | 501 | binance | bitget | 3 | 500 |
| SOXL_USDT | 504 | binance | bitget | 3 | 500 |
| MU_USDT | 499 | binance | bitget | 3 | 500 |
| HYPE_USDT | 210 | binance | bitget | 3 | 500 |
| ZEC_USDT | 480 | binance | bitget | 3 | 500 |
| KORU_USDT | 497 | binance | bitget | 3 | 500 |
| SAMSUNG_USDT | 500 | binance | bitget | 3 | 500 |
| DRAM_USDT | 495 | binance | bitget | 3 | 500 |
| ONDO_USDT | 305 | binance | bitget | 3 | 500 |
| WLD_USDT | 460 | binance | bitget | 3 | 500 |
| US_USDT | 445 | binance | bitget | 3 | 500 |
| XLM_USDT | 467 | binance | bitget | 3 | 500 |
| TAO_USDT | 415 | binance | bitget | 3 | 500 |
| NEAR_USDT | 293 | binance | bitget | 3 | 500 |
| DEXE_USDT | 135 | binance | bitget | 3 | 500 |
| KAITO_USDT | 236 | binance | bitget | 3 | 500 |
| 1000XEC_USDT | 494 | binance | bitget | 3 | 500 |
| BILL_USDT | 77 | binance | bitget | 3 | 500 |
| 0G_USDT | 0 | binance | bitget | 3 | 500 |
| MRVL_USDT | 498 | binance | bitget | 3 | 500 |
| ZBT_USDT | 479 | binance | bitget | 3 | 500 |
| BCH_USDT | 70 | binance | bitget | 3 | 500 |
| ENA_USDT | 152 | binance | bitget | 3 | 500 |
| ALLO_USDT | 25 | binance | bitget | 3 | 500 |
| BSB_USDT | 91 | binance | bitget | 3 | 500 |
| HOME_USDT | 207 | binance | bitget | 3 | 500 |
| EWY_USDT | 496 | binance | bitget | 3 | 500 |
| SKL_USDT | 376 | binance | bitget | 3 | 500 |
| BTC_USDT | 93 | binance | bitget | 3 | 500 |
| SOL_USDT | 384 | binance | bitget | 3 | 500 |
| DOGE_USDT | 137 | binance | bitget | 3 | 500 |
| XRP_USDT | 472 | binance | bitget | 3 | 500 |
| TAC_USDT | 411 | binance | bitget | 3 | 500 |
| ORDI_USDT | 316 | binance | bitget | 3 | 500 |
| SLX_USDT | 381 | binance | bitget | 3 | 500 |
| UB_USDT | 438 | binance | bitget | 3 | 500 |
| VELVET_USDT | 449 | binance | bitget | 3 | 500 |
| BTW_USDT | 95 | binance | bitget | 3 | 500 |
| RAVE_USDT | 347 | binance | bitget | 3 | 500 |
| SUI_USDT | 404 | binance | bitget | 3 | 500 |
| AVAX_USDT | 51 | binance | bitget | 3 | 500 |
| BAS_USDT | 67 | binance | bitget | 3 | 500 |
| H_USDT | 211 | binance | bitget | 3 | 500 |
| LINK_USDT | 253 | binance | bitget | 3 | 500 |

## 同目录 CSV

- `signal.csv`: 1192 条 signal，并关联对应 order
- `order_detail.csv`: 1192 条 order 明细
- `position.csv`: 10 条 position 明细
- `latency.csv`: 351 条 order latency 明细
- `execution_detail.csv`: 23 条逐笔成交明细
- `order_fillability.csv`: 351 条 IOC BBO fillability 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `1192`
- submitted order: `351`
- order session send: `351`
- ack: `351`
- order finished: `351`
- 有成交 order: `20`

- entry submitted: `336`
- entry any-fill: `10`
- entry any-fill rate: `2.98%`


### 未下单 signal 原因

| reason | count |
| --- | --- |
| drift_guard | 35 |
| parallel_limit | 587 |
| stale_lag_quote | 219 |

| symbol | signals |
| --- | --- |
| 0G_USDT | 1 |
| 1000XEC_USDT | 84 |
| ALLO_USDT | 33 |
| BAS_USDT | 29 |
| BCH_USDT | 7 |
| BILL_USDT | 27 |
| BSB_USDT | 9 |
| BTC_USDT | 50 |
| BTW_USDT | 121 |
| DEXE_USDT | 230 |
| DRAM_USDT | 3 |
| HYPE_USDT | 8 |
| H_USDT | 10 |
| KAITO_USDT | 67 |
| KORU_USDT | 15 |
| MRVL_USDT | 14 |
| MU_USDT | 4 |
| NEAR_USDT | 1 |
| ONDO_USDT | 7 |
| RAVE_USDT | 11 |
| SAMSUNG_USDT | 10 |
| SKHYNIX_USDT | 12 |
| SKHY_USDT | 59 |
| SKL_USDT | 11 |
| SLX_USDT | 9 |
| SNDK_USDT | 46 |
| TAC_USDT | 102 |
| TAO_USDT | 12 |
| UB_USDT | 59 |
| US_USDT | 72 |
| VELVET_USDT | 34 |
| WLD_USDT | 6 |
| XLM_USDT | 1 |
| ZBT_USDT | 18 |
| ZEC_USDT | 10 |

| status | count |
| --- | --- |
| kCancelled | 331 |
| kFilled | 18 |
| kPartiallyCancelled | 2 |
| kRejected | 841 |

### Entry fill funnel（按 symbol）

| symbol | submitted | any_fill | fill_rate |
| --- | --- | --- | --- |
| 0GUSDT | 1 | 0 | 0.00% |
| 1000XECUSDT | 29 | 0 | 0.00% |
| ALLOUSDT | 9 | 0 | 0.00% |
| BASUSDT | 4 | 0 | 0.00% |
| BCHUSDT | 3 | 0 | 0.00% |
| BILLUSDT | 9 | 0 | 0.00% |
| BSBUSDT | 3 | 1 | 33.33% |
| BTCUSDT | 2 | 1 | 50.00% |
| BTWUSDT | 25 | 1 | 4.00% |
| DEXEUSDT | 89 | 0 | 0.00% |
| DRAMUSDT | 1 | 1 | 100.00% |
| HUSDT | 1 | 0 | 0.00% |
| HYPEUSDT | 3 | 0 | 0.00% |
| KAITOUSDT | 17 | 0 | 0.00% |
| KORUUSDT | 3 | 0 | 0.00% |
| MRVLUSDT | 1 | 1 | 100.00% |
| MUUSDT | 3 | 0 | 0.00% |
| ONDOUSDT | 4 | 0 | 0.00% |
| RAVEUSDT | 6 | 1 | 16.67% |
| SAMSUNGUSDT | 3 | 0 | 0.00% |
| SKHYNIXUSDT | 4 | 1 | 25.00% |
| SKHYUSDT | 18 | 0 | 0.00% |
| SKLUSDT | 3 | 0 | 0.00% |
| SLXUSDT | 2 | 0 | 0.00% |
| SNDKUSDT | 8 | 3 | 37.50% |
| TACUSDT | 22 | 0 | 0.00% |
| TAOUSDT | 4 | 0 | 0.00% |
| UBUSDT | 18 | 0 | 0.00% |
| USUSDT | 18 | 0 | 0.00% |
| VELVETUSDT | 8 | 0 | 0.00% |
| WLDUSDT | 3 | 0 | 0.00% |
| XLMUSDT | 1 | 0 | 0.00% |
| ZBTUSDT | 7 | 0 | 0.00% |
| ZECUSDT | 4 | 0 | 0.00% |

### Bitget cancel reason

| reason | orders |
| --- | --- |
| ioc_not_full_cancel | 333 |

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 20 | 8 | 0 | -7 | 61 | 8 | 6 | 24.4 |
| entry | 10 | 12.8 | 1 | 0 | 61 | 6 | 0 | 15.2 |
| exit | 10 | 3.2 | -1 | -7 | 33 | 2 | 6 | 33.6 |

### Notional-weighted slippage

跨 symbol 总览使用 `filled_notional` 加权的 `exec_slippage_bps`；p50/p95/min/max 是逐订单 bps 分布。ticks 只适合单 symbol 或相同 tick 规则下比较。

| scope | filled_orders | filled_notional | weighted_bps | p50_bps | p95_bps | min_bps | max_bps |
| --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 20 | 174.621748 | 0.289 | 0 | 1.72 | -2.439 | 2.202 |
| entry | 10 | 87.305054 | 0.907 | 0.921 | 2.202 | 0 | 2.202 |
| exit | 10 | 87.316694 | -0.329 | -0.398 | 1.416 | -2.439 | 1.416 |

## Fast-fill 成交对账

- REST fills: `/home/liuxiang/tmp/20260721_142306_bitget_combined46_n6_fanout1_12h/inputs/rest_fills_final.json`
- fast-fill subscribed: `true`
- fast-fill records: `23`
- fast-fill unique execIds: `23`
- fast-fill unique orders: `20`
- fast-fill duplicate execIds: `0`
- fast-fill validation errors: `0`
- authoritative filled orders: `20`
- filled orders missing fast-fill: `0`
- fast-fill orders without authoritative fill: `0`
- quantity mismatch orders: `0`
- REST matched executions: `23`
- REST unmatched executions: `0`

Fast-fill 只用于逐笔成交与到达时序诊断；订单终态、累计成交量和恢复语义仍以 authoritative `order` channel 为准。跨交易所/本机时钟的差值不解释为单向网络延迟。

- creation->exec（交易所时钟）: samples=`23`, median=`2 ms`, p95=`2 ms`, p99=`2 ms`, max=`2 ms`
- fast-fill after Ack（本机 realtime）: samples=`23`, median=`1.141 ms`, p95=`1.766 ms`, p99=`2.092 ms`, max=`2.092 ms`
- fast-fill after order feedback（本机 realtime）: samples=`23`, median=`0.746 ms`, p95=`1.265 ms`, p99=`1.507 ms`, max=`1.507 ms`

## IOC BBO Fillability

- book ticker manifest: `/home/liuxiang/tmp/20260721_142306_bitget_combined46_n6_fanout1_12h/records/bitget_book_ticker_manifest.jsonl`

| observation | orders |
| --- | --- |
| indeterminate | 18 |
| marketable_observed | 18 |
| not_marketable_observed | 315 |

### 终态 × marketability

| terminal_event | observation | orders |
| --- | --- | --- |
| cancel | indeterminate | 18 |
| cancel | not_marketable_observed | 315 |
| exec | marketable_observed | 18 |

- complete BBO windows: `346`
- incomplete BBO windows: `5`
| missing_reason | orders |
| --- | --- |
| missing_creation_bbo | 5 |

`marketable_observed` / `not_marketable_observed` 只描述已归档 BBO 窗口内是否观察到 IOC limit crossing，不证明交易所撮合队列位置，也不把缺失 BBO 判为未成交原因。

## PnL

- order feedback gross PnL: `0.02818`
- configured-fee net PnL estimate: `0.0101599421`

### Bitget REST 实际 PnL

以 `/api/v3/trade/fills` 返回的 `execPnl` 与 `feeDetail` 为准；配置费率 PnL 仅保留作估算对照。

- REST executions: `23`
- REST execPnl: `0.02799999 USDT`
- REST actual fee: `0.01802 USDT`
- REST net PnL: `0.00997999 USDT`

### Order feedback PnL 与 Raw PnL

Order feedback PnL 使用 feedback average fill price，fee 使用配置费率估算；Raw PnL 使用 entry / exit 的 `raw_price`。两者都不是 Bitget REST 实际手续费 PnL。

- feedback-price gross PnL: `0.02818`
- feedback-price configured-fee net PnL: `0.0101599421`
- feedback-price configured-fee win rate: `40.00%` (4/10)
- raw gross PnL: `0.033226`
- raw net PnL: `0.0152059421`
- raw win rate: `50.00%` (5/10)

| symbol | direction | matched | feedback-price_gross | raw_gross | feedback-price_configured_fee_net | raw_net | actual_minus_raw_gross |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BSBUSDT | kLong | 47 | -0.00423 | -0.00329 | -0.006791406 | -0.005851406 | -0.00094 |
| BTCUSDT | kShort | 0.0001 | 0.00118 | 0.00212 | -0.000806021 | 0.000133979 | -0.00094 |
| BTWUSDT | kShort | 58 | 0.01595 | 0.015776 | 0.0141974604 | 0.0140234604 | 0.000174 |
| RAVEUSDT | kLong | 34 | 0.0255 | 0.02312 | 0.021600268 | 0.019220268 | 0.00238 |
| DRAMUSDT | kLong | 0.16 | -0.0016 | -0.0016 | -0.002823768 | -0.002823768 | 0 |
| MRVLUSDT | kShort | 0.04 | -0.0004 | -0.0004 | -0.00148043 | -0.00148043 | 0 |
| SKHYNIXUSDT | kShort | 0.01 | -0.0159 | -0.0127 | -0.0176440345 | -0.0144440345 | -0.0032 |
| SNDKUSDT | kLong | 0.006 | -0.00222 | -0.0006 | -0.0034444791 | -0.0018244791 | -0.00162 |
| SNDKUSDT | kLong | 0.006 | 0.00246 | 0.00378 | 0.0011882607 | 0.0025082607 | -0.00132 |
| SNDKUSDT | kShort | 0.006 | 0.00744 | 0.00702 | 0.0061640916 | 0.0057440916 | 0.00042 |

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| BSBUSDT | kLong | 47 | -0.00423 | -0.006791406 |
| BTCUSDT | kShort | 0.0001 | 0.00118 | -0.000806021 |
| BTWUSDT | kShort | 58 | 0.01595 | 0.0141974604 |
| RAVEUSDT | kLong | 34 | 0.0255 | 0.021600268 |
| DRAMUSDT | kLong | 0.16 | -0.0016 | -0.002823768 |
| MRVLUSDT | kShort | 0.04 | -0.0004 | -0.00148043 |
| SKHYNIXUSDT | kShort | 0.01 | -0.0159 | -0.0176440345 |
| SNDKUSDT | kLong | 0.006 | -0.00222 | -0.0034444791 |
| SNDKUSDT | kLong | 0.006 | 0.00246 | 0.0011882607 |
| SNDKUSDT | kShort | 0.006 | 0.00744 | 0.0061640916 |

## Close retry 与持仓时间

- positions with submitted exits: `10`
- positions requiring close retry: `4`
- max submitted exit attempts: `3`

| submitted_exit_attempts | positions |
| --- | --- |
| 1 | 6 |
| 2 | 3 |
| 3 | 1 |

- final holding time: samples=`10`, median=`50.392 ms`, p95=`11984.688 ms`, p99=`11984.688 ms`, max=`11984.688 ms`

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `5.149 ms`
- ack RTT median: `5.51 ms`
- ack RTT avg: `5.613 ms`
- ack RTT p95: `5.973 ms`
- ack RTT max: `23.062 ms`
- send->write complete（本机时钟）: samples=`351`, median=`0.008 ms`, p95=`0.01 ms`, p99=`0.014 ms`, max=`0.042 ms`
- write complete->Ack（monotonic）: samples=`351`, median=`5.503 ms`, p95=`5.965 ms`, p99=`6.613 ms`, max=`23.054 ms`
- creation->terminal（交易所时钟）: samples=`351`, median=`2 ms`, p95=`2 ms`, p99=`2 ms`, max=`2 ms`
- send-to-finish min: `5.332 ms`
- send-to-finish median: `5.805 ms`
- send-to-finish avg: `5.909 ms`
- send-to-finish p95: `6.307 ms`
- send-to-finish max: `23.332 ms`
