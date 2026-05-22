# LeadLag 8h Signal-Only 运行总结

## 结论

本轮是 signal-only 实盘行情观察，不提交真实订单。`lead_lag_strategy` 正常运行到 `duration=28800s` 并返回 `exit_code=0`。运行期共处理 `13,465,702` 条 book ticker，生成 `820` 条 signal，其中 `open=410`、`close=410`、`stoploss=0`。恢复状态保持 `normal`，`needs_reconcile=false`，没有 order response 或 order feedback。

需要注意：这轮使用的是 `config/strategies/lead_lag_requested_strategy_20260521.toml`，策略 pair 为 8 个；data session 订阅了 requested 11 个 symbol，但当时策略配置未包含 `RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT`。

## 运行信息

| 项目 | 值 |
| --- | --- |
| 源目录 | `/tmp/aquila_lead_lag_requested_20260521_164719` |
| 启动标记 | `2026-05-21T16:48:45Z` |
| runner | `./build/debug/tools/lead_lag_strategy` |
| config | `config/strategies/lead_lag_requested_strategy_20260521.toml` |
| run mode | `signal_only` |
| execute | `false` |
| connect_data | `true` |
| strategy mode | `dry_run` |
| max loop seconds | `28800` |
| strategy pairs | `8` |
| signal CSV | `signals.csv` |

## 时间范围

| 项目 | UTC 时间 |
| --- | --- |
| 第一条 signal | `2026-05-21T16:50:28.269000Z` |
| 最后一条 signal | `2026-05-22T00:48:35.836000Z` |
| signal 覆盖跨度 | `28687.567s`，约 `7h58m08s` |

## Runner Summary

| 指标 | 值 |
| --- | ---: |
| exit_code | 0 |
| book_tickers | 13,465,702 |
| data_reader_events | 13,465,702 |
| signals | 820 |
| open | 410 |
| close | 410 |
| stoploss | 0 |
| order_responses | 0 |
| order_feedbacks | 0 |
| loop_iterations | 111,694,644,494 |
| idle_iterations | 111,681,178,791 |
| data_reader_polls | 111,694,644,494 |
| data_reader_empty_polls | 111,681,178,792 |

## Data Session Summary

| session | result | book_tickers | rx_messages | tx_messages |
| --- | --- | ---: | ---: | ---: |
| Gate requested | ok | 2,650,399 | 2,650,403 | 5,880 |
| Binance requested | ok | 11,026,479 | 11,026,479 | 6,037 |

## Signal 总览

| 分类 | 计数 |
| --- | ---: |
| CSV 数据行 | 820 |
| CSV 文件大小 | 338,311 bytes |
| SHA256 | `b9993f94e4dc70e8d79c41d7d452360b4a197fa61dbbc3ec0cef13d3cc9bef15` |
| kOpenLong | 185 |
| kCloseLong | 185 |
| kOpenShort | 225 |
| kCloseShort | 225 |
| kBuy | 410 |
| kSell | 410 |
| reduce_only=false | 410 |
| reduce_only=true | 410 |
| drift_ready=true | 820 |

## 按 Symbol 汇总

| symbol | total | open_long | close_long | open_short | close_short |
| --- | ---: | ---: | ---: | ---: | ---: |
| PROVE_USDT | 154 | 35 | 35 | 42 | 42 |
| ZEC_USDT | 270 | 67 | 67 | 68 | 68 |
| ETC_USDT | 28 | 4 | 4 | 10 | 10 |
| DASH_USDT | 102 | 21 | 21 | 30 | 30 |
| SUI_USDT | 92 | 24 | 24 | 22 | 22 |
| INJ_USDT | 112 | 22 | 22 | 34 | 34 |
| ENA_USDT | 42 | 7 | 7 | 14 | 14 |
| BRETT_USDT | 20 | 5 | 5 | 5 | 5 |

## 按小时汇总

| UTC hour | total | open_long | close_long | open_short | close_short |
| --- | ---: | ---: | ---: | ---: | ---: |
| 2026-05-21 16:00 | 14 | 4 | 4 | 3 | 3 |
| 2026-05-21 17:00 | 220 | 63 | 63 | 47 | 47 |
| 2026-05-21 18:00 | 154 | 25 | 25 | 52 | 52 |
| 2026-05-21 19:00 | 102 | 18 | 18 | 33 | 33 |
| 2026-05-21 20:00 | 42 | 10 | 10 | 11 | 11 |
| 2026-05-21 21:00 | 70 | 22 | 22 | 13 | 13 |
| 2026-05-21 22:00 | 66 | 18 | 18 | 15 | 15 |
| 2026-05-21 23:00 | 82 | 14 | 14 | 27 | 27 |
| 2026-05-22 00:00 | 70 | 11 | 11 | 24 | 24 |

## 边界说明

- 本报告只总结 signal-only 行情观察，不代表真实下单、成交质量或 PnL。
- `order_responses=0`、`order_feedbacks=0` 是预期结果，因为本轮 `execute=false`。
- open / close 数量完全配对，未出现 stoploss signal。
- data session 订阅 11 个 requested symbol；本轮策略配置只启用 8 个 pair。
