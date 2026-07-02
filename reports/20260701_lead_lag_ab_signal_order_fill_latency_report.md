# 2026-07-01 LeadLag A/B signal、order、fill 与 latency 分析报告

## 1. 背景和数据源

本报告汇总 2026-07-01 30-symbol LeadLag A/B live 现场中已经完成的 signal、order、PnL、latency 和成交路径分析。

数据源：

- A 组 partial report：`/home/liuxiang/tmp/aquila_partial_reports/20260701_102201_30symbols_ogw_24h_partial_20260702_010619/`
- B 组 partial report：`/home/liuxiang/tmp/aquila_partial_reports/20260701_143803_30symbols_single_thread_probe_ab_partial_20260702_010619/`
- A 组 raw gateway log：`/home/liuxiang/tmp/20260701_102201_30symbols_ogw_24h/logs/gate_order_gateway_20260701_102621.log`
- A 组 raw feedback log：`/home/liuxiang/tmp/20260701_102201_30symbols_ogw_24h/logs/gate_order_feedback_session_20260701_102649.log`
- A 组 strategy log：`/home/liuxiang/tmp/20260701_102201_30symbols_ogw_24h/guarded_live.stdout`

A/B 运行背景：

- A 组：当前 main，多 `OrderSession` order-gateway 24h run，TEST account。
- B 组：historical `d9f1457` single `OrderSession` same-thread baseline，PROBE account，复用 A 组 fusion canonical SHM。
- A/B account、执行链路和运行起止时间不同；直接比较完整 partial report 时需要注意窗口差异。本文中成交率重点使用 A/B 时间交集。

## 2. 统计口径

时间窗口：

- A 组 signal window：`2026-07-01 10:30:22.161770826` 到 `2026-07-02 01:08:02.823047436` UTC。
- B 组 signal window：`2026-07-01 15:12:47.233349080` 到 `2026-07-02 01:08:02.823047558` UTC。
- A/B 交集窗口：`2026-07-01 15:12:47.233349080` 到 `2026-07-02 01:08:02.823047436` UTC。

字段和定义：

- `signal 数量`：`signal.csv` 中落在统计窗口内的 signal 行数。
- `开仓订单`：`order_detail.csv` 中 `source_schema=submitted_v1`、`order_role=entry`、`action in (kOpenLong, kOpenShort)` 的 submitted order。
- `开仓成交订单`：上述开仓订单中 `cumulative_filled_quantity > 0` 的 order。
- A 组 `实际下开仓单的 signal`：A 组为多路 fanout，用 `parent_id` 对开仓 submitted order 去重，一个 `parent_id` 视为一个实际下开仓单的 signal。
- B 组 `实际下开仓单的 signal`：B 组为 single-order baseline，一个开仓 submitted order 视为一个实际下开仓单的 signal。
- `开仓有成交的 signal`：同一个实际下单 signal 下，至少一个开仓 order 有 `cumulative_filled_quantity > 0`。
- 修正后的成交率：`开仓有成交的 signal 数 / 实际下开仓单的 signal 数`。

## 3. 完整 partial report 摘要

以下是各自 partial report 全窗口摘要，不用于最终修正成交率的分母。

| 组别 | signal 数量 | submitted order 数量 | filled order 数量 | order 成交率 |
|---|---:|---:|---:|---:|
| A 组 | 3,937 | 2,308 | 37 | 1.60% |
| B 组 | 2,569 | 417 | 12 | 2.88% |

PnL 摘要：

| 组别 | position rows | actual gross PnL | actual net PnL | raw gross PnL | raw net PnL | actual win rate |
|---|---:|---:|---:|---:|---:|---:|
| A 组 | 24 | -0.18956 | -0.314054296 | -0.36994 | -0.494434296 | 33.33% |
| B 组 | 6 | 0.34902 | 0.212982052 | -1.53108 | -1.667117948 | 50.00% |

Ack RTT 摘要：

| 组别 | median | avg | p95 | p99 | max | `>5ms` | `>10ms` |
|---|---:|---:|---:|---:|---:|---:|---:|
| A 组 | 0.677 ms | 3.621 ms | 8.157 ms | 38.237 ms | 1030.249 ms | 250 | 89 |
| B 组 | 0.757 ms | 4.410 ms | 8.529 ms | 75.416 ms | 444.193 ms | 41 | 19 |

## 4. A/B 时间交集内的基础统计

| 组别 | signal 数量 | 开仓订单数量 | 开仓成交订单数 | 开仓成交量 | order 口径成交率 |
|---|---:|---:|---:|---:|---:|
| A 组 | 2,779 | 1,509 | 17 | 167.0 | 1.13% |
| B 组 | 2,568 | 408 | 6 | 45.0 | 1.47% |

`order 口径成交率` 是 `开仓成交订单数 / 开仓订单数量`。这个口径对 A 组会受 4 路 fanout 放大分母影响，不是最终推荐的 signal 口径。

## 5. 修正后的 signal 口径成交率

修正口径为 `开仓有成交的 signal 数 / 实际下开仓单的 signal 数`。

| 组别 | 实际下开仓单的 signal 数 | 开仓有成交的 signal 数 | 成交率 |
|---|---:|---:|---:|
| A 组 | 378 | 8 | 2.12% |
| B 组 | 408 | 6 | 1.47% |

计算：

- A 组：`8 / 378 = 2.116%`。
- B 组：`6 / 408 = 1.471%`。

这个口径下，A 组多路 fanout 的 signal-level fillability 高于 B 组 single-order baseline。但样本较小，尤其 A 组成交 signal 只有 8 个，B 组只有 6 个，不宜过度外推。

## 6. A 组开仓订单 parent_id 分布

在 A/B 交集窗口内，A 组开仓 submitted order 共 1,509 个，对应 378 个不同 `parent_id`。

| 指标 | 数值 |
|---|---:|
| A 组开仓 submitted order | 1,509 |
| 不同 `parent_id` 数量 | 378 |
| 空 `parent_id` order 数量 | 0 |
| `parent_id` 各 4 路 order | 377 |
| `parent_id` 只有 1 路 order | 1 |

A 组有开仓成交的 `parent_id` 共 8 个：

| parent_id | 成交订单数 | 成交量 |
|---:|---:|---:|
| 189 | 4 | 4.0 |
| 212 | 1 | 19.0 |
| 265 | 3 | 60.0 |
| 308 | 1 | 10.0 |
| 418 | 2 | 56.0 |
| 420 | 2 | 6.0 |
| 433 | 1 | 3.0 |
| 545 | 3 | 9.0 |

## 7. A/B 成交 signal 是否一致

严格匹配 key：

- `symbol`
- `action`
- `trigger_exchange_ns`
- `signal_lead_id`
- `signal_lag_id`

A 组按 `parent_id` 聚合成成交 signal，B 组按成交开仓订单聚合。

| 指标 | 数值 |
|---|---:|
| A 组成交 signal 数 | 8 |
| B 组成交 signal 数 | 6 |
| 严格一致的成交 signal 数 | 2 |
| 仅 A 成交 | 6 |
| 仅 B 成交 | 4 |

严格一致的成交 signal：

| symbol | action | trigger_exchange_time UTC | A parent_id | A 成交订单数 | A 成交量 | B 成交量 |
|---|---|---|---:|---:|---:|---:|
| BAS_USDT | `kOpenShort` | `2026-07-01 15:20:07.705000000` | 189 | 4 | 4.0 | 1.0 |
| BTW_USDT | `kOpenShort` | `2026-07-02 00:43:29.404000000` | 545 | 3 | 9.0 | 1.0 |

接近但不严格一致的 signal：

- `IN_USDT kOpenShort`：B 的 trigger 是 `2026-07-01 22:38:40.892000000`，A 的 trigger 是 `2026-07-01 22:38:40.893000000`。两者 `symbol/action/raw_price/lag_id` 相同，本地触发时间只差 `230ns`，但 `trigger_exchange_ns` 差 `1ms` 且 `lead_id` 不同，因此严格口径下不算同一个 signal。

## 8. A 组 8 个成交 signal 的 4 路 order latency 差异

A 组 8 个成交 signal 均能找到 4 路开仓 submitted order，共 32 条；实际有成交数量的是 17 条，不是 32 条都成交。

字段：

- `send_spread_us`：同一 `parent_id` 内 4 路 `request_send_local_ns` 最大差值。
- `ack_spread_ms`：4 路 `ack_rtt_ns` 最大差值。
- `finish_spread_ms`：4 路 `order_finished_local_ns - request_send_local_ns` 最大差值。

| parent_id | symbol | 成交路数 | 成交量 | send_spread_us | ack_spread_ms | finish_spread_ms | 成交 route |
|---:|---|---:|---:|---:|---:|---:|---|
| 189 | BAS_USDT | 4 | 4.0 | 5.216 | 0.212 | 0.007 | r0/r1/r2/r3 |
| 212 | TAC_USDT | 1 | 19.0 | 4.748 | 3.338 | 7.595 | r2 |
| 265 | TAC_USDT | 3 | 60.0 | 4.260 | 0.236 | 1338.592 | r0/r1/r3 |
| 308 | ENA_USDT | 1 | 10.0 | 5.582 | 0.112 | 0.109 | r2 |
| 418 | ORDI_USDT | 2 | 56.0 | 4.811 | 5.271 | 537.938 | r0/r3 |
| 420 | BTC_USDT | 2 | 6.0 | 5.300 | 49.181 | 960.376 | r0/r3 |
| 433 | IN_USDT | 1 | 3.0 | 4.881 | 0.223 | 0.325 | r1 |
| 545 | BTW_USDT | 3 | 9.0 | 6.459 | 0.305 | 0.697 | r0/r1/r3 |

观察：

- 4 路 fanout 的本地发单时间非常接近，`send_spread_us` 全部在 `4.260us - 6.459us`。
- 差异主要出现在 Ack RTT tail 和 order lifecycle finish latency。
- `parent_id=420` 的 Ack RTT 差异最大，route1 达 `52.287ms`，而 route2 为 `3.106ms`。
- `finish latency` 已经混入成交、取消、timeout 和回报处理，不适合作为成交前因果指标。

## 9. 成交 route 与非成交 route 的 latency 对比

总体统计：

| 指标 | 成交 route | 非成交 route | 结论 |
|---|---:|---:|---|
| send offset 平均 | 3.057 us | 3.176 us | 基本无差异 |
| send offset 中位数 | 3.804 us | 3.530 us | 基本无差异 |
| ack RTT 平均 | 2.255 ms | 5.471 ms | 成交更低，但受非成交 outlier 影响 |
| ack RTT 中位数 | 0.688 ms | 0.785 ms | 成交略低，差异很小 |
| finish latency 平均 | 2817.695 ms | 2801.832 ms | 不支持判断 |
| finish latency 中位数 | 5.343 ms | 15.876 ms | 不能作为成交前因果指标 |

按每个 signal 内部看 Ack RTT：

| parent_id | 成交 route | 成交 ack 平均 ms | 非成交 ack 平均 ms | 最快 ack 是否成交 | 判断 |
|---:|---|---:|---:|---|---|
| 189 | r0/r1/r2/r3 | 0.675 | - | 是 | 4 路全成交，不能比较 |
| 212 | r2 | 7.790 | 4.816 | 否 | 成交 route 反而更慢 |
| 265 | r0/r1/r3 | 0.648 | 0.627 | 是 | 差异极小 |
| 308 | r2 | 0.605 | 0.650 | 否 | 成交略低均值，但最快是非成交 |
| 418 | r0/r3 | 2.010 | 3.421 | 否 | 均值受非成交慢 route 影响，最快是非成交 |
| 420 | r0/r3 | 9.400 | 27.696 | 否 | 非成交 r1 52ms outlier 拉高均值，最快是非成交 |
| 433 | r1 | 0.873 | 0.666 | 否 | 成交 route 反而更慢 |
| 545 | r0/r1/r3 | 0.536 | 0.804 | 是 | 支持更低 Ack 更容易成交 |

计数：

| 判断项 | 数量 |
|---|---:|
| 8 个 signal 中，最快 Ack route 成交 | 3 / 8 |
| 排除 4 路全成交的 `parent_id=189` 后 | 2 / 7 |

结论：

- 本样本不能证明 `latency 越小的 route 越容易成交`。
- 低 latency 可能有帮助，但不是主要解释变量。
- 成交更可能由盘口可成交量、价格位置、IOC matching 状态、队列和多路 order 到达交易所后的微观时序共同决定。

## 10. 按 Gate Ack `x_in/x_out` 比较

partial report 中 A 组这 32 条 order 的 `ack_exchange_request_ingress_ns`、`ack_exchange_response_egress_ns` 为空；本节使用原始 `gate_order_gateway` log 中的 `gate_order_response kind=kAck` 字段。

字段：

- `x_in` = `exchange_request_ingress_ns`
- `x_out` = `exchange_response_egress_ns`
- `x_out - x_in` = `exchange_process_ns`

注意：`x_in/x_out` 是 Gate response header 时间，属于 Gate 同一时钟域。可以互相相减，但不能直接和本机时间戳相减。

总体统计：

| 指标 | 成交 route | 非成交 route | 说明 |
|---|---:|---:|---|
| Ack x_in offset 中位数 | 32 us | 32 us | 看不出差异 |
| Ack x_out offset 中位数 | 63 us | 96 us | 成交略早，但差距小 |
| Ack `x_out - x_in` 中位数 | 160 us | 209 us | 成交略低 |
| 最早 x_in 是成交 | 5 / 8 | - | 排除 4 路全成交后是 4 / 7 |
| 最早 x_out 是成交 | 4 / 8 | - | 排除 4 路全成交后是 3 / 7 |

按 signal 内部看：

| parent_id | 成交 route | 最早 x_in 是否成交 | 最早 x_out 是否成交 | 判断 |
|---:|---|---|---|---|
| 189 | r0/r1/r2/r3 | 是 | 是 | 4 路全成交，不能区分 |
| 212 | r2 | 否 | 否 | 成交 route 反而更晚 |
| 265 | r0/r1/r3 | 是 | 是 | 支持更早成交 |
| 308 | r2 | 否 | 否 | 不支持 |
| 418 | r0/r3 | 是 | 否 | 混合 |
| 420 | r0/r3 | 否 | 是 | 混合 |
| 433 | r1 | 是 | 否 | 混合 |
| 545 | r0/r1/r3 | 是 | 是 | 支持更早成交 |

结论：

- `x_in` 基本不区分成交与否。
- `Ack x_out` 和 `x_out - x_in` 在总体中位数上成交 route 略好，但逐个 signal 不稳定。
- 仍不能推出 `x_in/x_out 越早的 route 越容易成交`。

补充：后续 terminal response 的 `x_out` 不适合判断成交优势，因为它已经混入 `kResult`、`kError`、取消和 timeout 处理。A 组 `parent_id=265 route1` 是一个典型例子：该 route 通过 feedback 已成交，但 gateway 后续出现 `kError http_status=504 Request Timeout`，属于 A 组 17:00 unknown/reconcile 现场的一部分。

## 11. Feedback 是否有类似 `x_in/x_out` 的字段

没有严格等价字段。

feedback 中主要有：

| 字段 | 含义 | 能否替代 `x_in/x_out` |
|---|---|---|
| `exchange_update_ns` | Gate private feedback 的订单更新时间，来自 `update_time_us` 转 ns | 不能。它是成交/取消事件时间，不是 request ingress |
| `local_receive_ns` | 本机收到 feedback 的时间 | 不能。它是本地接收时间，不是 Gate response egress |

`x_in/x_out` 只来自下单 REST response header，即 `gate_order_response.exchange_request_ingress_ns` 和 `gate_order_response.exchange_response_egress_ns`。

## 12. Feedback `exchange_update_ns` 是否说明更早成交

口径：A 组 8 个开仓成交 signal、32 路 entry order；每路取 private feedback 的 `exchange_update_ns`，在同一个 `parent_id` 内做相对时间比较。

| parent_id | symbol | 成交 route | `exchange_update_ns` 结论 |
|---:|---|---|---|
| 189 | BAS_USDT | r0/r1/r2/r3 | 4 路全成交，且时间相同，不能比较 |
| 212 | TAC_USDT | r2 | 成交 r2 比非成交路晚约 10-12 ms |
| 265 | TAC_USDT | r0/r1/r3 | 混合：r1 最早成交，但 r0/r3 比非成交 r2 晚约 1067 ms |
| 308 | ENA_USDT | r2 | 4 路 `exchange_update_ns` 相同，不能区分 |
| 418 | ORDI_USDT | r0/r3 | 成交路比非成交路晚约 471-561 ms |
| 420 | BTC_USDT | r0/r3 | 成交路比非成交路晚约 544-594 ms |
| 433 | IN_USDT | r1 | 4 路 `exchange_update_ns` 相同，不能区分 |
| 545 | BTW_USDT | r0/r1/r3 | 4 路 `exchange_update_ns` 相同，不能区分 |

结论：

- `exchange_update_ns` 更早不等价于更早成交。
- 对成交 route，它通常是 fill/update 时间；对非成交 route，它通常是 cancel/update 时间。
- IOC 场景下，未成交取消事件可能先出现，成交 fill 事件反而后出现。
- `exchange_update_ns` 更适合解释 feedback 事件何时发生，不适合证明更早 route 更容易成交。

## 13. 总结

核心结论：

1. 使用修正 signal 口径，A/B 交集窗口内成交率为：
   - A 组：`8 / 378 = 2.12%`
   - B 组：`6 / 408 = 1.47%`
2. A 组 4 路 fanout 的本地发单时间差非常小，8 个成交 signal 的 `send_spread` 均在 `4.260us - 6.459us`。
3. 单看 route latency，不能证明 `latency 越小越容易成交`：
   - 最快 Ack route 成交仅 `3 / 8`，排除 4 路全成交后是 `2 / 7`。
   - 最早 Ack `x_in` 成交是 `5 / 8`，排除 4 路全成交后是 `4 / 7`。
   - 最早 Ack `x_out` 成交是 `4 / 8`，排除 4 路全成交后是 `3 / 7`。
4. Feedback 没有等价的 `x_in/x_out`；`exchange_update_ns` 是订单事件时间，不是 request 到达交易所时间。
5. `exchange_update_ns` 也不能说明更早的 route 更容易成交；多个 signal 中非成交 cancel update 早于成交 fill update。

限制和注意事项：

- 样本量很小：A 组成交 signal 8 个，B 组成交 signal 6 个。
- A/B account 和执行链路不同，A 为多 `OrderSession` order-gateway，B 为 single `OrderSession` same-thread baseline。
- A 组 17:00 附近存在 `kUnknownResult` / `needs_reconcile` 现场，其中 `parent_id=265 route1` 有 feedback 成交但 gateway terminal response 为 `kError 504 Request Timeout`。
- `finish latency` 和 terminal response 不能作为成交前的因果指标。
- 若要进一步证明 fanout 对成交率的贡献，需要更长窗口、更多成交样本，并结合盘口深度、order price 与当时 best bid/ask、IOC result、private feedback 和 raw response header 做联合分析。

## 14. 停止后最终交集复算

本节为 2026-07-02 03:23 UTC 停止 A/B 实盘测试后的最终复算，优先级高于前文 01:06 partial 快照中的交集数字。

停止和账户状态：

- 已停止 A 组 trading 组件：`lead_lag_strategy`、`run_live_with_guard.py`、`gate_order_gateway`、`gate_order_feedback_session`、`health_monitor`。
- 已停止 B 组 trading 组件：`lead_lag_strategy`、`run_live_with_guard.py`、`gate_order_feedback_session`、`health_monitor`。
- A 组行情 fusion 未停止：`gate_data_fusion`、`binance_data_fusion`、Gate/Binance canonical recorder 均仍 alive。
- REST read-only 检查：A/B `open_orders=0`，非零 positions 均为 0。

最终 stopped report：

- A 组：`/home/liuxiang/tmp/aquila_partial_reports/20260701_102201_30symbols_ogw_24h_stopped_20260702_032345/`
- B 组：`/home/liuxiang/tmp/aquila_partial_reports/20260701_143803_30symbols_single_thread_probe_ab_stopped_20260702_032345/`
- A/B 交集窗口：`2026-07-01 15:12:47.233349080` 到 `2026-07-02 03:21:36.184302354` UTC。

### 14.1 交集窗口基础统计

| 组别 | signal 数量 | 开仓 order 数 | 开仓成交 order 数 | 开仓成交量 | order 口径成交率 |
|---|---:|---:|---:|---:|---:|
| A 组 | 3,130 | 1,885 | 21 | 267.0 | 1.11% |
| B 组 | 2,857 | 498 | 7 | 70.0 | 1.41% |

Signal 口径使用 `开仓有成交的 signal 数 / 实际下开仓单的 signal 数`：

| 组别 | 实际下开仓单的 signal 数 | 开仓有成交的 signal 数 | signal 口径成交率 |
|---|---:|---:|---:|
| A 组 | 472 | 9 | 1.91% |
| B 组 | 498 | 7 | 1.41% |

完整 stopped report 的 PnL 摘要：

| 组别 | actual gross PnL | actual net PnL | raw gross PnL | raw net PnL | actual win rate |
|---|---:|---:|---:|---:|---:|
| A 组 | -0.16436 | -0.273004392 | -0.25876 | -0.367404392 | 32.00% |
| B 组 | 0.34582 | 0.208469232 | -1.53528 | -1.672630768 | 50.00% |

### 14.2 A 组开仓成交 signal

A 组交集窗口内 1,885 个开仓 order 对应 472 个 `parent_id`：471 个 `parent_id` 各 4 路 order，1 个 `parent_id` 只有 1 路 order。开仓有成交的 `parent_id` 共 9 个。

| parent_id | symbol | action | 成交 route | 成交 order 数 | 成交量 | send_spread_us | ack_spread_ms | finish_spread_ms |
|---:|---|---|---|---:|---:|---:|---:|---:|
| 189 | BAS_USDT | `kOpenShort` | r0/r1/r2/r3 | 4 | 4.0 | 5.216 | 0.212 | 0.007 |
| 212 | TAC_USDT | `kOpenLong` | r2 | 1 | 19.0 | 4.748 | 3.338 | 7.595 |
| 265 | TAC_USDT | `kOpenShort` | r0/r1/r3 | 3 | 60.0 | 4.260 | 0.236 | 1338.592 |
| 308 | ENA_USDT | `kOpenShort` | r2 | 1 | 10.0 | 5.582 | 0.112 | 0.109 |
| 418 | ORDI_USDT | `kOpenShort` | r0/r3 | 2 | 56.0 | 4.811 | 5.271 | 537.938 |
| 420 | BTC_USDT | `kOpenShort` | r0/r3 | 2 | 6.0 | 5.300 | 49.181 | 960.376 |
| 433 | IN_USDT | `kOpenShort` | r1 | 1 | 3.0 | 4.881 | 0.223 | 0.325 |
| 545 | BTW_USDT | `kOpenShort` | r0/r1/r3 | 3 | 9.0 | 6.459 | 0.305 | 0.697 |
| 635 | TAC_USDT | `kOpenShort` | r0/r1/r2/r3 | 4 | 100.0 | 12.235 | 0.307 | 0.400 |

最终窗口比 01:06 partial 多出 `parent_id=635`，这是 A/B 都成交的 `TAC_USDT kOpenShort` signal。

### 14.3 A/B 成交 signal 一致性

严格匹配 key 仍为 `symbol + action + trigger_exchange_ns + signal_lead_id + signal_lag_id`。

| 指标 | 数值 |
|---|---:|
| A 组成交 signal 数 | 9 |
| B 组成交 signal 数 | 7 |
| 严格一致的成交 signal 数 | 3 |
| 仅 A 成交 | 6 |
| 仅 B 成交 | 4 |

严格一致的成交 signal：

| symbol | action | trigger_exchange_time UTC | A parent_id | A 成交 order 数 | A 成交量 | B 成交量 |
|---|---|---|---:|---:|---:|---:|
| BAS_USDT | `kOpenShort` | `2026-07-01 15:20:07.705000000` | 189 | 4 | 4.0 | 1.0 |
| BTW_USDT | `kOpenShort` | `2026-07-02 00:43:29.404000000` | 545 | 3 | 9.0 | 1.0 |
| TAC_USDT | `kOpenShort` | `2026-07-02 03:03:31.242000000` | 635 | 4 | 100.0 | 25.0 |

### 14.4 成交 route 和非成交 route latency

A 组 9 个开仓成交 signal 共 36 路 order，其中 21 路成交、15 路未成交。

| 指标 | 成交 route | 非成交 route | 结论 |
|---|---:|---:|---|
| send offset 平均 | 3.739 us | 3.176 us | 差异仍很小 |
| send offset 中位数 | 3.804 us | 3.530 us | 差异仍很小 |
| Ack RTT 平均 | 1.959 ms | 5.471 ms | 成交更低，但受非成交 outlier 影响 |
| Ack RTT 中位数 | 0.669 ms | 0.785 ms | 成交略低 |
| finish latency 平均 | 2281.876 ms | 2801.832 ms | 不适合作成交前因果判断 |
| finish latency 中位数 | 5.037 ms | 15.876 ms | 不适合作成交前因果判断 |

最快 Ack route 成交计数：

- 含 4 路全成交 signal：`4 / 9`。
- 排除 4 路全成交的 `parent_id=189` 和 `parent_id=635` 后：`2 / 7`。

结论不变：本样本不能证明 `latency 越小的 route 越容易成交`。

### 14.5 Gate Ack `x_in/x_out`

以下使用原始 A 组 `gate_order_gateway` log 的 `gate_order_response kind=kAck` 字段。

| 指标 | 成交 route | 非成交 route | 说明 |
|---|---:|---:|---|
| Ack x_in offset 中位数 | 28 us | 32 us | 差异很小 |
| Ack x_out offset 中位数 | 56 us | 96 us | 成交略早 |
| Ack `x_out - x_in` 中位数 | 157 us | 209 us | 成交略低 |
| 最早 x_in 是成交 | 6 / 9 | - | 排除两组 4 路全成交后为 4 / 7 |
| 最早 x_out 是成交 | 5 / 9 | - | 排除两组 4 路全成交后为 3 / 7 |

结论仍然是：`x_in/x_out` 在总体中位数上对成交 route 略有利，但逐个 signal 不稳定，不能推出 `x_in/x_out 越早越容易成交`。

### 14.6 Feedback `exchange_update_ns`

最终窗口新增的 `parent_id=635` 是 4 路全成交，且 4 路 `exchange_update_ns` 相同，因此不能提供新的成交/非成交区分信息。最终 9 个 A 组成交 signal 中，最早 `exchange_update_ns` 是成交 route 的计数为 `4 / 9`；排除 4 路全成交的 `parent_id=189` 和 `parent_id=635` 后为 `2 / 7`。

结论不变：feedback 的 `exchange_update_ns` 是订单事件时间，不是 request 到达交易所时间；它不能证明更早 route 更容易成交。

### 14.7 成交价和滑点

A 组多路成交中，成交价是否一致：

| parent_id | symbol | 成交 route | 成交价是否一致 | 成交价 |
|---:|---|---|---|---|
| 189 | BAS_USDT | r0/r1/r2/r3 | 是 | `0.050409` |
| 265 | TAC_USDT | r0/r1/r3 | 否 | r0/r3 `0.047725`，r1 `0.047690` |
| 418 | ORDI_USDT | r0/r3 | 是 | `3.526` |
| 420 | BTC_USDT | r0/r3 | 否 | r0 `61228.8`，r3 `61226.3` |
| 545 | BTW_USDT | r0/r1/r3 | 是 | `0.065778` |
| 635 | TAC_USDT | r0/r1/r2/r3 | 否 | `0.039469/0.039371/0.03943876/0.039433` |

Signal 级加权执行滑点：

| parent_id | symbol | 成交 route | 成交量 | 加权 exec_slippage_ticks | 加权 exec_slippage_bps | limit_improvement_min |
|---:|---|---|---:|---:|---:|---:|
| 189 | BAS_USDT | r0/r1/r2/r3 | 4.0 | -62.00 | -12.31 | 73 |
| 212 | TAC_USDT | r2 | 19.0 | -84.00 | -16.21 | 97 |
| 265 | TAC_USDT | r0/r1/r3 | 60.0 | -18.33 | -3.84 | 8 |
| 308 | ENA_USDT | r2 | 10.0 | 2.00 | 2.77 | 0 |
| 418 | ORDI_USDT | r0/r3 | 56.0 | 0.00 | 0.00 | 1 |
| 420 | BTC_USDT | r0/r3 | 6.0 | 93.50 | 1.53 | 12 |
| 433 | IN_USDT | r1 | 3.0 | 2.00 | 3.14 | 0 |
| 545 | BTW_USDT | r0/r1/r3 | 9.0 | -145.00 | -22.09 | 159 |
| 635 | TAC_USDT | r0/r1/r2/r3 | 100.0 | -64.94 | -16.50 | 21 |

滑点结论：

- A 组 9 个开仓成交 signal 中，5 个 signal 加权滑点优于 raw，1 个持平，3 个差于 raw。
- 所有成交路相对实际 IOC limit 都没有更差，`limit_improvement_min >= 0`。
- `BTW_USDT parent_id=545` 在 stopped report 中缺少 `contract_multiplier`，因此 `exec_slippage_quote` 为空；其 tick/bps 滑点仍可用。
- 已有 quote 字段的 A 组开仓成交净滑点为 `-0.10309` quote，实际更偏保守，因为上述 BTW 的有利滑点未计入 quote 汇总。

### 14.8 最终判断

停止后的最终交集口径下：

- A 组 signal 口径开仓成交率为 `9 / 472 = 1.91%`。
- B 组 signal 口径开仓成交率为 `7 / 498 = 1.41%`。
- A 组多路 fanout 在 signal-level fillability 上仍高于 B 组 single-order baseline。
- 但成交样本仍然很小，且 latency / Ack `x_in/x_out` / feedback `exchange_update_ns` 都不能单独解释成交与否。更可靠的后续分析需要结合当时盘口深度、limit price 可达性、IOC matching 结果和 private feedback 序列。
