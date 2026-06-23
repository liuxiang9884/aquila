# SKYAI_USDT Preflight Shadow 闭环验证

本文档记录在 `SKYAI_USDT` 已有订单成交分析基础上，继续验证“启动前生成固定 taker buffer / freshness threshold，然后在实时策略中只做 shadow 诊断”的效果。

## 输入和边界

输入文件均来自本 report：

- `inputs/orders.csv`
- `inputs/signals.csv`
- `market_data/canonical.bin.zst`

本轮没有使用 `source0..3`。run 目录中只归档了 Gate lag 侧 fusion canonical/source BookTicker bin，没有找到 Binance lead 侧 BookTicker bin。因此：

- taker buffer 使用 Gate canonical BBO spread 精确生成。
- freshness 的 `quote local_ns - exchange_ns` 只能用 `signals.csv` 中的 lead/lag quote timestamp 做 proxy，不等同于正式 preflight raw BookTicker 全量输入。
- 另补一个 `runtime signal freshness` proxy，使用 `signals.csv` 中的 `lead_freshness_ns` / `lag_freshness_ns`。这是当前策略 `freshness_shadow` 实际比较的字段口径。

## 生成参数

Gate canonical 有效 BBO spread 样本为 `1,041,851` 条。spread ratio 分布：

| percentile | ratio | bps |
|---:|---:|---:|
| p50 | `0.000466935154` | `4.67` |
| p95 | `0.001055746177` | `10.56` |
| p99 | `0.001502607892` | `15.03` |
| p100 | `0.009791176877` | `97.91` |

为复现上限诊断口径，本轮主 TOML 显式使用 p100：

```toml
[lead_lag.pairs.execute.taker_buffer]
mode = "shadow"
entry_fixed_pct = 0.00979117688
normal_close_fixed_pct = 0.00979117688
exclude_from_cost_model = false
source = "generated"
```

这个 p100 buffer 是接近 `98 bps` 的上限诊断参数，明显比当前订单的 `4 ticks` 滑点激进，不适合作为直接 enforce 的候选值。

freshness 有两个 proxy：

| 口径 | lead threshold | lag threshold | 说明 |
|---|---:|---:|---|
| quote latency proxy | `3ms` | `1ms` | `quote.local_ns - quote.exchange_ns`，接近启动前 raw BookTicker latency 生成口径。 |
| runtime signal proxy | `3ms` | `721ms` | `signal_decision_ns - quote.exchange_ns`，匹配当前策略 `freshness_shadow` 比较字段。 |

两者的 lag 阈值差异很大，说明 freshness 设计里存在一个必须先解决的口径问题：如果配置生成基于 `local_ns - exchange_ns`，但策略实际比较 `signal_decision_ns - exchange_ns`，shadow/enforce 的含义会不一致。

## 订单级结果

样本：

- open no-fill cancel：`93` 笔。
- open filled / partially filled control：`8` 笔。

以 Gate `x_in_time` / `x_out_time` 作为撮合时间候选点，并使用 canonical BookTicker 的最新 `exchange_ns <= x_time` 状态判断 BBO any 可成交。

| 样本 | price | signal any | x_in any | x_out any |
|---|---|---:|---:|---:|
| cancel `93` | raw price | `62/93` | `46/93` | `34/93` |
| cancel `93` | order price | `93/93` | `68/93` | `50/93` |
| cancel `93` | p100 reference price | `93/93` | `93/93` | `93/93` |
| filled control `8` | raw price | `4/8` | `4/8` | `4/8` |
| filled control `8` | order price | `8/8` | `8/8` | `8/8` |
| filled control `8` | p100 reference price | `8/8` | `8/8` | `8/8` |

order price 本身已经有区分度：filled control 在 `x_in/x_out` 都是 `8/8` any 可成交，cancel 则是 `68/93` 和 `50/93`。但仍有不少 cancel 在 `x_in` 视角可成交，这与前一轮结论一致：public/canonical BBO 不能直接等同于 Gate matching engine 收到 IOC 瞬间的真实撮合簿。

## Buffer 敏感性

下表只看 reference price 在 cancel 样本的 BBO any 可成交性：

| buffer percentile | buffer bps | median offset ticks | x_in any | x_out any | lifetime p50 |
|---:|---:|---:|---:|---:|---:|
| p50 | `4.67` | `17` | `73/93` | `57/93` | `0us` |
| p95 | `10.56` | `38` | `84/93` | `74/93` | `1171us` |
| p99 | `15.03` | `54` | `89/93` | `81/93` | `2151us` |
| p100 | `97.91` | `348` | `93/93` | `93/93` | `4055us` |

filled control 在这四个 buffer 下都是 `8/8` x_in/x_out any 可成交。

结论是：taker buffer 对 BBO any 可成交性有明确解释力，但 p100 过于激进。p95/p99 更适合作为下一轮 shadow 对比候选，前提是先把成本、成交质量和潜在 overpay 风险一起纳入评估。

## Freshness 结果

如果用 quote latency proxy 的 `lead=3ms / lag=1ms` 作为配置，再按当前策略实际比较字段 `lead_freshness_ns / lag_freshness_ns` 判断：

- cancel 会 shadow block `43/93`。
- filled control 会 shadow block `5/8`。

这个结果没有区分度，甚至会阻断大量已成交 open control。

如果用 runtime signal proxy 的 `lead=3ms / lag=721ms` 判断：

- cancel shadow block `0/93`。
- filled control shadow block `0/8`。

这个结果也没有区分度。它说明当前 SKYAI 样本里，freshness auto 不是解释未成交 IOC 的主要因素，至少不能直接按现有 `mean + 3 * std` 生成方式进入 enforce。

## 输出文件

- `analysis/preflight_shadow_params.json`：生成参数、spread 分布、两种 freshness proxy 的统计。
- `analysis/preflight_shadow_quote_latency.toml`：quote latency proxy TOML patch。
- `analysis/preflight_shadow_runtime_freshness.toml`：runtime signal freshness proxy TOML patch。
- `analysis/preflight_shadow_orders.csv`：订单级闭环表，覆盖 `93` 笔 cancel 和 `8` 笔 open filled control。
- `analysis/preflight_shadow_sensitivity.csv`：p50/p95/p99/p100 taker buffer 的 sensitivity 汇总。
- `analysis/preflight_shadow_summary.json`：上述结果的机器可读汇总。

## 结论

1. 当前启动前生成 taker buffer 的方向是有用的，但本轮 p100 spread 会给 SKYAI 生成约 `98 bps` 的 buffer，明显过大，只适合证明“足够激进的价格会保持 BBO any 可成交”，不适合直接进入实盘 enforce。
2. 更合理的下一轮是用 p95 或 p99 作为 shadow 参数，结合成交质量、overpay 和 full-volume 口径继续评估，而不是直接用 p100。
3. freshness auto 暂时不应 enforce。当前生成口径和策略比较口径不一致，且两种 proxy 在本样本上都没有区分 fill/cancel 的能力。
4. 在修改策略前，应该先决定 freshness shadow 到底比较 `quote.local_ns - quote.exchange_ns`，还是比较 `signal_decision_ns - quote.exchange_ns`；配置生成和策略比较必须统一。
