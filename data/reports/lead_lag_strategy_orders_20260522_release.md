# LeadLag 2026-05-22 Release 实盘 Strategy 订单汇总

## 范围

- Run dir: `run_logs/live_release_20260522_155232`
- Strategy log: `/home/liuxiang/log/lead_lag_strategy_requested_11symbols_live_20260522_20260522_155243.log`
- Feedback log: `/home/liuxiang/log/gate_order_feedback_session_live_smoke_20260522_20260522_155237.log`
- Mode: release `lead_lag_strategy --execute --duration-sec 3600`
- 统计口径：只统计 strategy 发出的 `lead_lag_order_intent`。`run_live_with_guard.py` 的 emergency flatten / 后续手动清理订单不计入 strategy 订单。

本次测试因 `RAVE_USDT` partial fill 没有进入 strategy terminal feedback 而中止，未跑满 1 小时。

触发 `BookTicker.id` 口径：本轮运行的 `lead_lag_order_intent` 日志没有记录触发订单的 `BookTicker.id`，`signals_output=-`
也没有落地 signal CSV；data session 日志只保留启动和汇总信息，没有逐条行情记录。当前 `/dev/shm` 中残留的旧共享内存对象也无法重新
挂载回放，因此本轮历史订单无法从现有 artifact 精确恢复触发行情 id。后续 live order 报告需要依赖策略 order-intent 日志直接写入触发
`BookTicker.id`，或同时开启 signal CSV / SHM binary recorder。

## 总览

| 指标 | 数量 |
| --- | ---: |
| Strategy order intent | 8 |
| Open intent | 7 |
| Close intent | 1 |
| Gate place send ok | 8 |
| Gate Ack | 8 |
| Strategy terminal feedback | 7 |
| Strategy filled feedback | 2 |
| Strategy cancelled feedback | 5 |
| 缺失 terminal feedback | 1 |
| Strategy 完整 open -> close 交易 | 1 |

## Strategy 订单明细

| # | Time UTC | Trigger BookTicker.id | Symbol | Local order id | Action | Side | Reduce-only | Qty | Raw price | Order price | Slippage cfg | Strategy / REST 结果 | Fill |
| ---: | --- | --- | --- | ---: | --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| 1 | 15:55:43.378 | 未记录 | DASH_USDT | 288230376151711745 | kOpenShort | Sell | false | 223 | 44.71 | 44.68 | 3 ticks | `kCancelled` | 0 / left 223 |
| 2 | 15:56:32.785 | 未记录 | ZEC_USDT | 288230376151711746 | kOpenShort | Sell | false | 15 | 631.17 | 631.12 | 5 ticks | `kCancelled` | 0 / left 15 |
| 3 | 15:58:31.474 | 未记录 | RIVER_USDT | 288230376151711747 | kOpenShort | Sell | false | 14 | 6.809 | 6.806 | 3 ticks | `kCancelled` | 0 / left 14 |
| 4 | 16:00:04.501 | 未记录 | RIVER_USDT | 288230376151711748 | kOpenShort | Sell | false | 14 | 6.816 | 6.813 | 3 ticks | `kFilled` | 14 @ 6.813 |
| 5 | 16:00:04.592 | 未记录 | RIVER_USDT | 288230376151711749 | kCloseShort | Buy | true | 14 | 6.814 | 6.817 | 3 ticks | `kFilled` | 14 @ 6.8145 |
| 6 | 16:00:16.100 | 未记录 | ZEC_USDT | 288230376151711750 | kOpenLong | Buy | false | 15 | 629.24 | 629.29 | 5 ticks | `kCancelled` | 0 / left 15 |
| 7 | 16:02:03.561 | 未记录 | RAVE_USDT | 288230376151711751 | kOpenLong | Buy | false | 17 | 0.5619 | 0.5624 | 5 ticks | Strategy 只有 Ack；REST 显示 `finish_as=ioc` partial fill | REST: `size=17`, `left=6`, `fill_price=0.562399019608` |
| 8 | 16:04:02.825 | 未记录 | DASH_USDT | 288230376151711752 | kOpenLong | Buy | false | 222 | 44.95 | 44.98 | 3 ticks | `kCancelled` | 0 / left 222 |

## 完整成交交易：RIVER_USDT

这组是本轮唯一由 strategy 自己完成 open 和 close 的交易。

| Leg | Local order id | Side | Qty | Raw price | Order price | Fill price | 实际滑点 |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| Open short | 288230376151711748 | Sell | 14 | 6.816 | 6.813 | 6.813 | 3 ticks |
| Close short | 288230376151711749 | Buy | 14 | 6.814 | 6.817 | 6.8145 | 0.5 ticks |

`RIVER_USDT` Gate multiplier 为 `1.0`。

| 项目 | 计算 | USDT |
| --- | --- | ---: |
| 信号价理论 PnL | `(6.816 - 6.814) * 14` | 0.028000 |
| 实际 gross PnL | `(6.813 - 6.8145) * 14` | -0.021000 |
| 滑点成本 | `0.028000 - (-0.021000)` | 0.049000 |
| 手续费，费率 0.0002 | `(6.813 * 14 + 6.8145 * 14) * 0.0002` | 0.038157 |
| Net PnL | `-0.021000 - 0.038157` | -0.059157 |

## RAVE_USDT 异常边界

- Strategy 发出 `kOpenLong` 17 张，order price `0.5624`，raw price `0.5619`。
- Strategy 和 feedback session 没有收到该 order 的 terminal feedback。
- REST finished order 显示该 IOC order 为 partial fill：`size=17`、`left=6`、`fill_price=0.562399019608`、`finish_as=ioc`。
- 停止 runner 后，guard final check 发现 `RAVE_USDT size=10`，随后提交 reduce-only IOC market sell 10 张平仓；这是 guard 应急动作，不计入 strategy close。
- 后续 REST 还出现非 strategy 的 `close_all` 清理订单；同样不计入本报告的 strategy 订单统计。

结论：`RAVE_USDT` 暴露了两个问题：IOC partial-fill terminal feedback 缺失，以及 decimal-size 合约下只看 integer `size` 的 flat 判断不足。修复前不要把本轮视为完整实盘通过证据。
