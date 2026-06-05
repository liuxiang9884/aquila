# LeadLag Live Run Report

## 基本信息

- run_id: `20260604_0646_30symbols_30d_private`
- 策略配置: `config/strategies/lead_lag_30symbols_live_strategy_20260604.toml`
- 源日志: `/home/liuxiang/tmp/20260604_0646_30symbols_30d_private/merged_live_and_feedback.log`
- guard stdout: `/home/liuxiang/tmp/20260604_0646_30symbols_30d_private/guarded_live.stdout`
- 首个 signal 时间: `2026-06-04 06:48:03.174577055`
- 最后 signal 时间: `2026-06-05 00:46:05.174619603`

## 同目录 CSV

- `signal.csv`: 7017 条 signal，并关联对应 order
- `order_detail.csv`: 7017 条 order 明细
- `position.csv`: 132 条 position 明细
- `latency.csv`: 7017 条 order latency 明细
- 字段参考: `lead_lag_live_report_csv_schema.md`

## Signal 和 Order

- signal: `7017`
- submitted order: `7017`
- Gate send ok: `7017`
- ack: `7017`
- order finished: `7017`
- 有成交 order: `242`

| symbol | signals |
| --- | --- |
| AIA_USDT | 452 |
| BEAT_USDT | 413 |
| CLO_USDT | 259 |
| ENA_USDT | 228 |
| EPIC_USDT | 675 |
| FARTCOIN_USDT | 30 |
| FET_USDT | 68 |
| FIL_USDT | 28 |
| H_USDT | 274 |
| ICP_USDT | 67 |
| INJ_USDT | 154 |
| LIT_USDT | 208 |
| MYX_USDT | 208 |
| NEAR_USDT | 230 |
| ONDO_USDT | 119 |
| OPN_USDT | 603 |
| PENGU_USDT | 99 |
| PIEVERSE_USDT | 110 |
| SKYAI_USDT | 360 |
| SLX_USDT | 259 |
| STO_USDT | 175 |
| SUI_USDT | 100 |
| TON_USDT | 143 |
| UB_USDT | 202 |
| VIRTUAL_USDT | 92 |
| WIF_USDT | 34 |
| WLD_USDT | 691 |
| XLM_USDT | 135 |
| XPL_USDT | 53 |
| ZEC_USDT | 548 |

| status | count |
| --- | --- |
| kCancelled | 6715 |
| kFilled | 162 |
| kPartiallyCancelled | 79 |
| kRejected | 61 |

### 滑点分析

`exec_slippage_ticks` 以 raw price 为基准，正数表示成交比 raw 更差，负数表示成交优于 raw；`limit_improvement_ticks` 表示相对 aggressive limit 的改善 tick。

| scope | filled_orders | avg_exec_slippage_ticks | median | min | max | adverse | favorable | avg_limit_improvement_ticks |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| all filled | 242 | -0.581 | 0 | -73 | 12 | 108 | 42 | 3.178 |
| entry | 112 | -0.143 | 0 | -73 | 5.43 | 55 | 13 | 2.759 |
| exit | 130 | -0.959 | 0 | -62 | 12 | 53 | 29 | 3.539 |

## PnL

- gross PnL: `-0.6040890000000000006`
- net PnL: `-3.0184750994000000010188`

### Raw PnL 和胜率

Raw PnL 使用 entry / exit 的 `raw_price` 计算，fee 仍使用 report CSV 中的配置费率估算值；胜率按 net PnL > 0 计算。

- actual gross PnL: `-0.6040890000000000006`
- actual net PnL: `-3.0184750994000000010188`
- actual win rate: `33.08%` (43/130)
- raw gross PnL: `-1.5133580000000000015`
- raw net PnL: `-3.9277440994000000019188`
- raw win rate: `41.54%` (54/130)

| symbol | direction | matched | actual_gross | raw_gross | actual_net | raw_net | actual_minus_raw_gross |
| --- | --- | --- | --- | --- | --- | --- | --- |
| AIA_USDT | kShort | 32 | -0.016576 | -0.0096 | -0.0253447552 | -0.0183687552 | -0.006976 |
| AIA_USDT | kShort | 26 | -0.04212 | -0.039 | -0.049224136 | -0.046104136 | -0.00312 |
| AIA_USDT | kShort | 27 | -0.05724 | -0.054 | -0.064620072 | -0.061380072 | -0.00324 |
| AIA_USDT | kShort | 73 | -0.18396 | -0.1679 | -0.203919368 | -0.187859368 | -0.01606 |
| AIA_USDT | kShort | 146 | 0.0292 | 0.0292 | -0.01078064 | -0.01078064 | 0 |
| AIA_USDT | kShort | 26 | 0.0598 | 0.0598 | 0.052533 | 0.052533 | 0 |
| AIA_USDT | kShort | 26 | 0.052 | 0.0494 | 0.04478968 | 0.04218968 | 0.0026 |
| AIA_USDT | kShort | 2 | -0.0048 | -0.0046 | -0.00536936 | -0.00516936 | -0.0002 |
| AIA_USDT | kShort | 2 | -0.0034 | -0.0034 | -0.00399668 | -0.00399668 | 0 |
| AIA_USDT | kShort | 54 | -0.081 | -0.0702 | -0.094176 | -0.083376 | -0.0108 |
| AIA_USDT | kShort | 41 | 0.2214 | 0.1886 | 0.2117404 | 0.1789404 | 0.0328 |
| AIA_USDT | kLong | 38 | 0.2128 | -0.3002 | 0.20414056 | -0.30885944 | 0.513 |
| AIA_USDT | kLong | 124 | -0.468844 | -1.3888 | -0.4968684712 | -1.4168244712 | 0.919956 |
| AIA_USDT | kLong | 12 | -0.0744 | -0.199224 | -0.07710624 | -0.20193024 | 0.124824 |
| AIA_USDT | kShort | 158 | -0.039658 | -0.0158 | -0.0748840684 | -0.0510260684 | -0.023858 |
| AIA_USDT | kLong | 170 | -0.1564 | -0.204 | -0.1962191 | -0.2438191 | 0.0476 |
| AIA_USDT | kShort | 168 | 0 | -0.0168 | -0.03978912 | -0.05658912 | 0.0168 |
| CLO_USDT | kShort | 69 | 0.00276 | -0.0069 | -0.037022088 | -0.046682088 | 0.00966 |
| ENA_USDT | kLong | 14 | -0.0308 | -0.0238 | -0.03660608 | -0.02960608 | -0.007 |
| EPIC_USDT | kShort | 1 | 0.0007 | 0.0007 | 0.00048226 | 0.00048226 | 0 |
| EPIC_USDT | kShort | 1 | -0.0003 | -0.0002 | -0.00052086 | -0.00042086 | -0.0001 |
| EPIC_USDT | kShort | 166 | 0.0664 | 0.083 | 0.02976048 | 0.04636048 | -0.0166 |
| EPIC_USDT | kShort | 14 | 0.0196 | 0.021 | 0.01651272 | 0.01791272 | -0.0014 |
| EPIC_USDT | kShort | 95 | 0.0475 | 0.0665 | 0.0250553 | 0.0440553 | -0.019 |
| EPIC_USDT | kShort | 74 | 0.074 | 0.074 | 0.05652416 | 0.05652416 | 0 |
| EPIC_USDT | kLong | 169 | -0.180661 | -0.1521 | -0.2206081722 | -0.1920471722 | -0.028561 |
| EPIC_USDT | kShort | 167 | 0.2004 | 0.2171 | 0.1604536 | 0.1771536 | -0.0167 |
| EPIC_USDT | kShort | 158 | -0.158 | -0.1106 | -0.19789816 | -0.15049816 | -0.0474 |
| EPIC_USDT | kShort | 158 | -0.037762 | -0.0158 | -0.0777182724 | -0.0557562724 | -0.021962 |
| EPIC_USDT | kShort | 25 | 0.0075 | 0.01 | 0.0014065 | 0.0039065 | -0.0025 |
| FET_USDT | kShort | 86 | -0.0086 | 0 | -0.01659628 | -0.00799628 | -0.0086 |
| FET_USDT | kLong | 90 | 0.009 | 0.009 | 0.0010566 | 0.0010566 | 0 |
| FET_USDT | kShort | 89 | 0.0089 | 0.0089 | 0.00091314 | 0.00091314 | 0 |
| H_USDT | kLong | 1 | 0.008 | 0.008 | 0.00552084 | 0.00552084 | 0 |
| ICP_USDT | kLong | 9 | -0.000018 | -0.000009 | -0.000027792 | -0.000018792 | -0.000009 |
| INJ_USDT | kShort | 1 | -0.0002 | 0 | -0.0004344 | -0.0002344 | -0.0002 |
| INJ_USDT | kShort | 172 | 0.1032 | -0.086 | 0.06317904 | -0.12602096 | 0.1892 |
| INJ_USDT | kLong | 179 | 0 | 0.0537 | -0.03990268 | 0.01379732 | -0.0537 |
| LIT_USDT | kShort | 62 | -0.07378 | 0 | -0.113673156 | -0.039893156 | -0.07378 |
| LIT_USDT | kLong | 62 | 0 | 0 | -0.03782 | -0.03782 | 0 |
| LIT_USDT | kLong | 66 | 0.02772 | 0.132 | -0.011964744 | 0.092315256 | -0.10428 |
| LIT_USDT | kLong | 66 | -0.132 | -0.264 | -0.1714416 | -0.3034416 | 0.132 |
| LIT_USDT | kShort | 66 | 0.066 | 0.132 | 0.0260436 | 0.0920436 | -0.066 |
| LIT_USDT | kShort | 66 | -0.132 | 0 | -0.171996 | -0.039996 | -0.132 |
| LIT_USDT | kShort | 8 | -0.008 | 0.008 | -0.01284 | 0.00316 | -0.016 |
| LIT_USDT | kShort | 65 | -0.065 | 0.065 | -0.104507 | 0.025493 | -0.13 |
| LIT_USDT | kShort | 14 | -0.056 | -0.028 | -0.0645568 | -0.0365568 | -0.028 |
| LIT_USDT | kShort | 15 | -0.045 | -0.015 | -0.054165 | -0.024165 | -0.03 |
| LIT_USDT | kShort | 66 | -0.066 | 0 | -0.1054812 | -0.0394812 | -0.066 |
| LIT_USDT | kLong | 47 | 0.047 | 0.094 | 0.0188846 | 0.0658846 | -0.047 |
| LIT_USDT | kLong | 67 | -0.067 | 0.067 | -0.106731 | 0.027269 | -0.134 |
| LIT_USDT | kShort | 24 | -0.048 | 0 | -0.0624192 | -0.0144192 | -0.048 |
| LIT_USDT | kShort | 24 | 0 | 0.024 | -0.0147264 | 0.0092736 | -0.024 |
| LIT_USDT | kShort | 67 | 0.02747 | 0.134 | -0.012134906 | 0.094395094 | -0.10653 |
| LIT_USDT | kLong | 28 | -0.028 | 0 | -0.044436 | -0.016436 | -0.028 |
| LIT_USDT | kLong | 28 | 0 | 0.056 | -0.0165312 | 0.0394688 | -0.056 |
| LIT_USDT | kShort | 24 | -0.06984 | -0.024 | -0.083802768 | -0.037962768 | -0.04584 |
| LIT_USDT | kShort | 69 | -0.138 | 0 | -0.1775784 | -0.0395784 | -0.138 |
| LIT_USDT | kShort | 65 | 0.3718 | 0.455 | 0.33418164 | 0.41738164 | -0.0832 |
| MYX_USDT | kLong | 18 | -0.027 | -0.05814 | -0.02892204 | -0.06006204 | 0.03114 |
| MYX_USDT | kLong | 74 | -0.0444 | -0.0444 | -0.05228544 | -0.05228544 | 0 |
| MYX_USDT | kShort | 36 | 0.0036 | 0.0036 | -0.00007128 | -0.00007128 | 0 |
| NEAR_USDT | kShort | 1 | 0.0021 | 0.0021 | 0.00114802 | 0.00114802 | 0 |
| OPN_USDT | kLong | 53 | -0.159 | -0.807455 | -0.1982094 | -0.8466644 | 0.648455 |
| OPN_USDT | kLong | 5 | 0.01 | 0.02 | 0.006348 | 0.016348 | -0.01 |
| OPN_USDT | kShort | 44 | 0 | -0.044 | -0.039952 | -0.083952 | 0.044 |
| OPN_USDT | kLong | 44 | -0.01408 | 0.044 | -0.053219584 | 0.004860416 | -0.05808 |
| OPN_USDT | kLong | 4 | 0 | 0.004 | -0.0036688 | 0.0003312 | -0.004 |
| OPN_USDT | kShort | 3 | 0.015 | 0.018 | 0.011535 | 0.014535 | -0.003 |
| PENGU_USDT | kShort | 146 | 0 | -0.0292 | -0.03998648 | -0.06918648 | 0.0292 |
| PIEVERSE_USDT | kLong | 2 | 0.012 | 0.012 | 0.0054328 | 0.0054328 | 0 |
| PIEVERSE_USDT | kLong | 10 | -0.09 | -0.07 | -0.122806 | -0.102806 | -0.02 |
| PIEVERSE_USDT | kLong | 12 | -0.012 | -0.048 | -0.0508392 | -0.0868392 | 0.036 |
| PIEVERSE_USDT | kShort | 7 | -0.06895 | -0.049 | -0.09158499 | -0.07163499 | -0.01995 |
| PIEVERSE_USDT | kShort | 4 | -0.052 | -0.04 | -0.0649368 | -0.0529368 | -0.012 |
| PIEVERSE_USDT | kShort | 1 | -0.004 | -0.002 | -0.0072324 | -0.0052324 | -0.002 |
| SKYAI_USDT | kLong | 2 | 0.0018 | 0.0024 | 0.0004066 | 0.0010066 | -0.0006 |
| SKYAI_USDT | kLong | 30 | 0.003 | -0.033 | -0.0178782 | -0.0538782 | 0.036 |
| SKYAI_USDT | kShort | 2 | -0.0004 | -0.0004 | -0.00179192 | -0.00179192 | 0 |
| SKYAI_USDT | kLong | 54 | -0.05157 | -0.054 | -0.09084555 | -0.09327555 | 0.00243 |
| SKYAI_USDT | kShort | 1 | 0.0016 | 0.0016 | 0.00089348 | 0.00089348 | 0 |
| SKYAI_USDT | kShort | 1 | 0.0006 | 0.0007 | -0.00008524 | 0.00001476 | -0.0001 |
| SKYAI_USDT | kShort | 4 | -0.0032 | -0.004 | -0.00591776 | -0.00671776 | 0.0008 |
| SKYAI_USDT | kLong | 2 | 0.01 | 0.01 | 0.0086652 | 0.0086652 | 0 |
| SKYAI_USDT | kShort | 1 | 0.002 | 0.002 | 0.00132212 | 0.00132212 | 0 |
| SKYAI_USDT | kShort | 1 | 0.001 | 0.001 | 0.0003246 | 0.0003246 | 0 |
| SLX_USDT | kShort | 2 | -0.0042 | -0.0042 | -0.0061354 | -0.0061354 | 0 |
| SLX_USDT | kLong | 1 | 0.0013 | 0.0014 | 0.00032738 | 0.00042738 | -0.0001 |
| SLX_USDT | kLong | 2 | 0.0018 | 0.002 | -0.00014732 | 0.00005268 | -0.0002 |
| SLX_USDT | kLong | 2 | 0.0008 | 0.0008 | -0.00113616 | -0.00113616 | 0 |
| SLX_USDT | kLong | 2 | 0.0032 | 0.0032 | 0.00131896 | 0.00131896 | 0 |
| SLX_USDT | kLong | 1 | 0.0033 | 0.0033 | 0.0023583 | 0.0023583 | 0 |
| SLX_USDT | kShort | 41 | 0.041 | -0.0082 | 0.00111356 | -0.04808644 | 0.0492 |
| SLX_USDT | kShort | 36 | -0.0108 | -0.0216 | -0.04415256 | -0.05495256 | 0.0108 |
| SLX_USDT | kLong | 16 | -0.0016 | 0.0128 | -0.0156944 | -0.0012944 | -0.0144 |
| SLX_USDT | kLong | 1 | 0.0023 | 0.0023 | 0.00142186 | 0.00142186 | 0 |
| STO_USDT | kShort | 3 | 0.0015 | 0.0012 | 0.00076038 | 0.00046038 | 0.0003 |
| STO_USDT | kShort | 164 | 0 | 0.0164 | -0.03992416 | -0.02352416 | -0.0164 |
| STO_USDT | kLong | 2 | 0.002 | 0.0022 | 0.00151472 | 0.00171472 | -0.0002 |
| STO_USDT | kShort | 1 | 0.0007 | 0.0007 | 0.00045026 | 0.00045026 | 0 |
| STO_USDT | kShort | 47 | 0.0376 | 0.0376 | 0.02586316 | 0.02586316 | 0 |
| STO_USDT | kShort | 112 | 0.071904 | 0.0784 | 0.0439318208 | 0.0504278208 | -0.006496 |
| STO_USDT | kShort | 155 | 1.661445 | 1.6895 | 1.621986371 | 1.650041371 | -0.028055 |
| STO_USDT | kShort | 9 | 0.0081 | 0 | 0.00583542 | -0.00226458 | 0.0081 |
| STO_USDT | kShort | 125 | -0.133125 | -0.15 | -0.163881525 | -0.180756525 | 0.016875 |
| STO_USDT | kShort | 166 | -0.239704 | -0.2158 | -0.2796384208 | -0.2557344208 | -0.023904 |
| STO_USDT | kLong | 61 | -0.1586 | -0.1586 | -0.17299356 | -0.17299356 | 0 |
| STO_USDT | kLong | 2 | -0.001 | -0.001 | -0.00147316 | -0.00147316 | 0 |
| TON_USDT | kShort | 73 | 0.00073 | 0.00292 | -0.00427853 | -0.00208853 | -0.00219 |
| TON_USDT | kShort | 57 | 0.00285 | 0.00285 | -0.001149918 | -0.001149918 | 0 |
| UB_USDT | kShort | 1 | -0.38 | -0.35 | -0.421944 | -0.391944 | -0.03 |
| UB_USDT | kShort | 0.19999999999999996 | -0.0339999999999999932 | -0.0279999999999999944 | -0.04238039999999999152392 | -0.03638039999999999272392 | -0.0059999999999999988 |
| UB_USDT | kShort | 0.20000000000000007 | -0.0280000000000000098 | -0.0260000000000000091 | -0.03637920000000001273272 | -0.03437920000000001203272 | -0.0020000000000000007 |
| UB_USDT | kShort | 0.2 | -0.012 | -0.012 | -0.020376 | -0.020376 | 0 |
| UB_USDT | kShort | 0.19999999999999998 | -0.0239999999999999976 | -0.019999999999999998 | -0.03237839999999999676216 | -0.02837839999999999716216 | -0.0039999999999999996 |
| UB_USDT | kShort | 0.1 | 0.001 | 0.001 | -0.0031866 | -0.0031866 | 0 |
| UB_USDT | kLong | 1.8 | 0.036 | 0.108 | -0.03492 | 0.03708 | -0.072 |
| UB_USDT | kLong | 0.2 | -0.002 | 0.008 | -0.0098788 | 0.0001212 | -0.01 |
| WIF_USDT | kLong | 120 | -0.012 | 0.012 | -0.01998 | 0.00402 | -0.024 |
| WIF_USDT | kShort | 120 | -0.024 | 0 | -0.0319728 | -0.0079728 | -0.024 |
| WLD_USDT | kShort | 188 | 0.3008 | 0.1128 | 0.26091392 | 0.07291392 | 0.188 |
| WLD_USDT | kLong | 1 | -0.001 | -0.0008 | -0.00121232 | -0.00101232 | -0.0002 |
| WLD_USDT | kLong | 193 | 0.041302 | 0.0579 | 0.0013350196 | 0.0179330196 | -0.016598 |
| WLD_USDT | kLong | 195 | -0.024765 | -0.0195 | -0.064563447 | -0.059298447 | -0.005265 |
| WLD_USDT | kLong | 205 | -0.369 | -0.3075 | -0.408811 | -0.347311 | -0.0615 |
| WLD_USDT | kLong | 210 | 0 | 0.021 | -0.0399756 | -0.0189756 | -0.021 |
| XLM_USDT | kLong | 3 | 0.0048 | 0.0048 | 0.00226968 | 0.00226968 | 0 |
| XLM_USDT | kLong | 48 | -0.0096 | -0.0048 | -0.049056 | -0.044256 | -0.0048 |
| XPL_USDT | kLong | 1 | 0.0013 | 0.001 | 0.00095574 | 0.00065574 | 0.0003 |
| ZEC_USDT | kShort | 19 | -0.086317 | -0.0532 | -0.1257946566 | -0.0926776566 | -0.033117 |

| symbol | direction | matched | gross_pnl | net_pnl |
| --- | --- | --- | --- | --- |
| AIA_USDT | kShort | 32 | -0.016576 | -0.0253447552 |
| AIA_USDT | kShort | 26 | -0.04212 | -0.049224136 |
| AIA_USDT | kShort | 27 | -0.05724 | -0.064620072 |
| AIA_USDT | kShort | 73 | -0.18396 | -0.203919368 |
| AIA_USDT | kShort | 146 | 0.0292 | -0.01078064 |
| AIA_USDT | kShort | 26 | 0.0598 | 0.052533 |
| AIA_USDT | kShort | 26 | 0.052 | 0.04478968 |
| AIA_USDT | kShort | 2 | -0.0048 | -0.00536936 |
| AIA_USDT | kShort | 2 | -0.0034 | -0.00399668 |
| AIA_USDT | kShort | 54 | -0.081 | -0.094176 |
| AIA_USDT | kShort | 41 | 0.2214 | 0.2117404 |
| AIA_USDT | kLong | 38 | 0.2128 | 0.20414056 |
| AIA_USDT | kLong | 124 | -0.468844 | -0.4968684712 |
| AIA_USDT | kLong | 12 | -0.0744 | -0.07710624 |
| AIA_USDT | kShort | 158 | -0.039658 | -0.0748840684 |
| AIA_USDT | kLong | 170 | -0.1564 | -0.1962191 |
| AIA_USDT | kShort | 168 | 0 | -0.03978912 |
| CLO_USDT | kShort | 69 | 0.00276 | -0.037022088 |
| ENA_USDT | kLong | 14 | -0.0308 | -0.03660608 |
| EPIC_USDT | kShort | 1 | 0.0007 | 0.00048226 |
| EPIC_USDT | kShort | 1 | -0.0003 | -0.00052086 |
| EPIC_USDT | kShort | 166 | 0.0664 | 0.02976048 |
| EPIC_USDT | kShort | 14 | 0.0196 | 0.01651272 |
| EPIC_USDT | kShort | 95 | 0.0475 | 0.0250553 |
| EPIC_USDT | kShort | 74 | 0.074 | 0.05652416 |
| EPIC_USDT | kLong | 169 | -0.180661 | -0.2206081722 |
| EPIC_USDT | kShort | 167 | 0.2004 | 0.1604536 |
| EPIC_USDT | kShort | 158 | -0.158 | -0.19789816 |
| EPIC_USDT | kShort | 158 | -0.037762 | -0.0777182724 |
| EPIC_USDT | kShort | 25 | 0.0075 | 0.0014065 |
| FET_USDT | kShort | 86 | -0.0086 | -0.01659628 |
| FET_USDT | kLong | 90 | 0.009 | 0.0010566 |
| FET_USDT | kShort | 89 | 0.0089 | 0.00091314 |
| H_USDT | kLong | 1 | 0.008 | 0.00552084 |
| ICP_USDT | kLong | 9 | -0.000018 | -0.000027792 |
| INJ_USDT | kShort | 1 | -0.0002 | -0.0004344 |
| INJ_USDT | kShort | 172 | 0.1032 | 0.06317904 |
| INJ_USDT | kLong | 179 | 0 | -0.03990268 |
| LIT_USDT | kShort | 62 | -0.07378 | -0.113673156 |
| LIT_USDT | kLong | 62 | 0 | -0.03782 |
| LIT_USDT | kLong | 66 | 0.02772 | -0.011964744 |
| LIT_USDT | kLong | 66 | -0.132 | -0.1714416 |
| LIT_USDT | kShort | 66 | 0.066 | 0.0260436 |
| LIT_USDT | kShort | 66 | -0.132 | -0.171996 |
| LIT_USDT | kShort | 8 | -0.008 | -0.01284 |
| LIT_USDT | kShort | 65 | -0.065 | -0.104507 |
| LIT_USDT | kShort | 14 | -0.056 | -0.0645568 |
| LIT_USDT | kShort | 15 | -0.045 | -0.054165 |
| LIT_USDT | kShort | 66 | -0.066 | -0.1054812 |
| LIT_USDT | kLong | 47 | 0.047 | 0.0188846 |
| LIT_USDT | kLong | 67 | -0.067 | -0.106731 |
| LIT_USDT | kShort | 24 | -0.048 | -0.0624192 |
| LIT_USDT | kShort | 24 | 0 | -0.0147264 |
| LIT_USDT | kShort | 67 | 0.02747 | -0.012134906 |
| LIT_USDT | kLong | 28 | -0.028 | -0.044436 |
| LIT_USDT | kLong | 28 | 0 | -0.0165312 |
| LIT_USDT | kShort | 24 | -0.06984 | -0.083802768 |
| LIT_USDT | kShort | 69 | -0.138 | -0.1775784 |
| LIT_USDT | kShort | 65 | 0.3718 | 0.33418164 |
| MYX_USDT | kLong | 18 | -0.027 | -0.02892204 |
| MYX_USDT | kLong | 74 | -0.0444 | -0.05228544 |
| MYX_USDT | kShort | 36 | 0.0036 | -0.00007128 |
| NEAR_USDT | kShort | 1 | 0.0021 | 0.00114802 |
| OPN_USDT | kLong | 53 | -0.159 | -0.1982094 |
| OPN_USDT | kLong | 5 | 0.01 | 0.006348 |
| OPN_USDT | kShort | 44 | 0 | -0.039952 |
| OPN_USDT | kLong | 44 | -0.01408 | -0.053219584 |
| OPN_USDT | kLong | 4 | 0 | -0.0036688 |
| OPN_USDT | kShort | 3 | 0.015 | 0.011535 |
| PENGU_USDT | kShort | 146 | 0 | -0.03998648 |
| PENGU_USDT | kShort |  |  |  |
| PIEVERSE_USDT | kLong | 2 | 0.012 | 0.0054328 |
| PIEVERSE_USDT | kLong | 10 | -0.09 | -0.122806 |
| PIEVERSE_USDT | kLong | 12 | -0.012 | -0.0508392 |
| PIEVERSE_USDT | kShort | 7 | -0.06895 | -0.09158499 |
| PIEVERSE_USDT | kShort | 4 | -0.052 | -0.0649368 |
| PIEVERSE_USDT | kShort | 1 | -0.004 | -0.0072324 |
| SKYAI_USDT | kLong | 2 | 0.0018 | 0.0004066 |
| SKYAI_USDT | kLong | 30 | 0.003 | -0.0178782 |
| SKYAI_USDT | kShort | 2 | -0.0004 | -0.00179192 |
| SKYAI_USDT | kLong | 54 | -0.05157 | -0.09084555 |
| SKYAI_USDT | kShort | 1 | 0.0016 | 0.00089348 |
| SKYAI_USDT | kShort | 1 | 0.0006 | -0.00008524 |
| SKYAI_USDT | kShort | 4 | -0.0032 | -0.00591776 |
| SKYAI_USDT | kLong | 2 | 0.01 | 0.0086652 |
| SKYAI_USDT | kShort | 1 | 0.002 | 0.00132212 |
| SKYAI_USDT | kShort | 1 | 0.001 | 0.0003246 |
| SLX_USDT | kShort | 2 | -0.0042 | -0.0061354 |
| SLX_USDT | kLong | 1 | 0.0013 | 0.00032738 |
| SLX_USDT | kLong | 2 | 0.0018 | -0.00014732 |
| SLX_USDT | kLong | 2 | 0.0008 | -0.00113616 |
| SLX_USDT | kLong | 2 | 0.0032 | 0.00131896 |
| SLX_USDT | kLong | 1 | 0.0033 | 0.0023583 |
| SLX_USDT | kShort | 41 | 0.041 | 0.00111356 |
| SLX_USDT | kShort | 36 | -0.0108 | -0.04415256 |
| SLX_USDT | kLong | 16 | -0.0016 | -0.0156944 |
| SLX_USDT | kLong | 1 | 0.0023 | 0.00142186 |
| STO_USDT | kShort | 3 | 0.0015 | 0.00076038 |
| STO_USDT | kShort | 164 | 0 | -0.03992416 |
| STO_USDT | kLong | 2 | 0.002 | 0.00151472 |
| STO_USDT | kShort | 1 | 0.0007 | 0.00045026 |
| STO_USDT | kShort | 47 | 0.0376 | 0.02586316 |
| STO_USDT | kShort | 112 | 0.071904 | 0.0439318208 |
| STO_USDT | kShort | 155 | 1.661445 | 1.621986371 |
| STO_USDT | kShort | 9 | 0.0081 | 0.00583542 |
| STO_USDT | kShort | 125 | -0.133125 | -0.163881525 |
| STO_USDT | kShort | 166 | -0.239704 | -0.2796384208 |
| STO_USDT | kLong | 61 | -0.1586 | -0.17299356 |
| STO_USDT | kLong | 2 | -0.001 | -0.00147316 |
| TON_USDT | kShort | 73 | 0.00073 | -0.00427853 |
| TON_USDT | kShort | 57 | 0.00285 | -0.001149918 |
| UB_USDT | kShort | 1 | -0.38 | -0.421944 |
| UB_USDT | kShort | 0.19999999999999996 | -0.0339999999999999932 | -0.04238039999999999152392 |
| UB_USDT | kShort | 0.20000000000000007 | -0.0280000000000000098 | -0.03637920000000001273272 |
| UB_USDT | kShort | 0.2 | -0.012 | -0.020376 |
| UB_USDT | kShort | 0.19999999999999998 | -0.0239999999999999976 | -0.03237839999999999676216 |
| UB_USDT | kShort | 0.1 | 0.001 | -0.0031866 |
| UB_USDT | kShort |  |  |  |
| UB_USDT | kLong | 1.8 | 0.036 | -0.03492 |
| UB_USDT | kLong | 0.2 | -0.002 | -0.0098788 |
| WIF_USDT | kLong | 120 | -0.012 | -0.01998 |
| WIF_USDT | kShort | 120 | -0.024 | -0.0319728 |
| WLD_USDT | kShort | 188 | 0.3008 | 0.26091392 |
| WLD_USDT | kLong | 1 | -0.001 | -0.00121232 |
| WLD_USDT | kLong | 193 | 0.041302 | 0.0013350196 |
| WLD_USDT | kLong | 195 | -0.024765 | -0.064563447 |
| WLD_USDT | kLong | 205 | -0.369 | -0.408811 |
| WLD_USDT | kLong | 210 | 0 | -0.0399756 |
| XLM_USDT | kLong | 3 | 0.0048 | 0.00226968 |
| XLM_USDT | kLong | 48 | -0.0096 | -0.049056 |
| XPL_USDT | kLong | 1 | 0.0013 | 0.00095574 |
| ZEC_USDT | kShort | 19 | -0.086317 | -0.1257946566 |

## 延迟

`ack_rtt_ns` 是本地下单发送到收到 Ack 的 RTT，不是收到 Filled / Cancelled 终态回报的时间。

- ack RTT min: `0.496 ms`
- ack RTT median: `0.674 ms`
- ack RTT avg: `1.385 ms`
- ack RTT p95: `2.17 ms`
- ack RTT max: `188.819 ms`
- Gate Ack process min: `0.079 ms`
- Gate Ack process median: `0.14 ms`
- Gate Ack process avg: `0.678 ms`
- Gate Ack process p95: `0.567 ms`
- Gate Ack process max: `188.227 ms`

### Ack RTT 三段拆解

Ack RTT 拆为上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`。上行和下行跨本机 / Gate 时钟，只用于同机时钟同步后的诊断。

| stage | samples | min_ms | median_ms | avg_ms | p95_ms | p99_ms | max_ms | >1ms | >5ms | >10ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Ack RTT total | 7017 | 0.496 | 0.674 | 1.385 | 2.17 | 13.347 | 188.819 | 872 | 169 | 83 |
| 上行 send->Gate x_in | 7017 | 0.142 | 0.211 | 0.32 | 0.399 | 1.603 | 146.004 | 113 | 16 | 7 |
| Gate x_in->x_out | 7017 | 0.079 | 0.14 | 0.678 | 0.567 | 8.859 | 188.227 | 238 | 109 | 68 |
| 下行 Gate x_out->local | 7017 | 0.195 | 0.321 | 0.387 | 0.565 | 1.718 | 25.328 | 157 | 8 | 4 |

- `>5ms` Ack tail dominant stage: Gate in->out=128, 上行=21, 下行=20
| local_order_id | ack_rtt_ms | upstream_ms | gate_in_to_out_ms | downstream_ms | dominant_stage |
| --- | --- | --- | --- | --- | --- |
| 288230376151718655 | 188.819 | 0.201 | 188.227 | 0.391 | Gate in->out |
| 288230376151713797 | 150.673 | 0.25 | 149.694 | 0.729 | Gate in->out |
| 288230376151716203 | 149.198 | 146.004 | 1.807 | 1.387 | 上行 |
| 288230376151715469 | 132.145 | 0.204 | 131.512 | 0.429 | Gate in->out |
| 288230376151716409 | 124.514 | 0.217 | 123.786 | 0.512 | Gate in->out |
| 288230376151714654 | 122.238 | 0.19 | 121.614 | 0.434 | Gate in->out |
| 288230376151714915 | 121.381 | 0.224 | 120.794 | 0.362 | Gate in->out |
| 288230376151716204 | 104.811 | 101.634 | 2.136 | 1.04 | 上行 |
| 288230376151712809 | 98.735 | 0.214 | 98.049 | 0.472 | Gate in->out |
| 288230376151717813 | 88.923 | 0.228 | 88.257 | 0.437 | Gate in->out |
- latency diagnostic outliers: `169`
| local_order_id | reason | ack_rtt_ms | send_to_first_drive_read_ms | drive_read_duration_ms |
| --- | --- | --- | --- | --- |
| 288230376151711751 | kAckRttThreshold | 23.045 | 0.038 | 0.023 |
| 288230376151711856 | kAckRttThreshold | 66.551 | 0.044 | 0.021 |
| 288230376151711889 | kAckRttThreshold | 35.455 | 0.039 | 0.019 |
| 288230376151711914 | kAckRttThreshold | 13.19 | 0.042 | 0.014 |
| 288230376151711916 | kAckRttThreshold | 5.47 | 0.028 | 0.021 |
| 288230376151711915 | kAckRttThreshold | 20.162 | 0.043 | 0.024 |
| 288230376151711919 | kAckRttThreshold | 6.84 | 0.025 | 0.013 |
| 288230376151711918 | kAckRttThreshold | 12.01 | 0.027 | 0.013 |
| 288230376151711979 | kAckRttThreshold | 12.552 | 0.048 | 0.014 |
| 288230376151712028 | kAckRttThreshold | 6.702 | 0.055 | 0.023 |
- send-to-finish min: `1.468 ms`
- send-to-finish median: `34.527 ms`
- send-to-finish avg: `303.9 ms`
- send-to-finish p95: `1866.213 ms`
- send-to-finish max: `3550.063 ms`

`exchange_lifecycle_ns` 是 Gate exchange timestamp 中 Ack 到订单终态 update 的差值；它不混用本地时钟，但仍受 Gate timestamp 字段语义限制，只作交易所侧 lifecycle 诊断。

- exchange Ack-to-finish min: `-0.291 ms`
- exchange Ack-to-finish median: `30.595 ms`
- exchange Ack-to-finish avg: `281.73 ms`
- exchange Ack-to-finish p95: `1696.017 ms`
- exchange Ack-to-finish max: `3548.694 ms`

| local_order_id | symbol | status | finish_reason | exchange_ack_to_finish_ms | ack_to_finish_local_ms | send_to_finish_ms |
| --- | --- | --- | --- | --- | --- | --- |
| 288230376151717156 | PIEVERSE_USDT | kCancelled | kImmediateOrCancel | 3548.694 | 3549.032 | 3550.063 |
| 288230376151713700 | FIL_USDT | kCancelled | kImmediateOrCancel | 3527.162 | 3528.258 | 3528.965 |
| 288230376151713701 | MYX_USDT | kCancelled | kImmediateOrCancel | 3526.384 | 3527.116 | 3528.655 |
| 288230376151714768 | EPIC_USDT | kCancelled | kImmediateOrCancel | 3491.486 | 3492.001 | 3493.821 |
| 288230376151713702 | EPIC_USDT | kCancelled | kImmediateOrCancel | 3488.94 | 3490.122 | 3490.846 |
