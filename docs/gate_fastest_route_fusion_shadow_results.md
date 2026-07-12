# Fastest-Route Fusion Shadow 证据摘要

本文只保存可复现的 historical shadow 结果。当前算法、ABI、配置和验证入口见 `docs/market_data_fusion.md`。
结果只覆盖行情 pipeline，不代表订单 fillability、Ack latency 或 PnL。

## 2026-06-14 Gate/Binance 30-symbol、N=4、30-minute L4

运行目录：

```text
Gate:    /home/liuxiang/tmp/20260614_133704_gate_fusion_30symbols_4src_30m_l4_outlier_release/
Binance: /home/liuxiang/tmp/20260614_133704_binance_fusion_30symbols_4src_30m_l4_outlier_release/
```

条件：release build，4 source、1 fusion、5 recorder；Gate private plain，Binance public TLS；
`AQUILA_DATA_SESSION_DIAG_LEVEL=4`，outlier threshold `5ms`。Gate source/fusion/recorder CPU 为
`16-19/20/21-25`；Binance 为 `0-3/4/5-9`。

Latency，单位 ms：

| Exchange | Stream | p50 | p95 | p99 | p99.9 | max |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Gate | best single by percentile | 0.144 | 0.261 | 0.380 | 3.751 | - |
| Gate | canonical fusion | 0.144 | 0.234 | 0.259 | 0.301 | 17.099 |
| Binance | best single by percentile | 1.179 | 1.731 | 1.902 | 3.196 | - |
| Binance | canonical fusion | 1.109 | 1.641 | 1.754 | 1.926 | 3.544 |

Tail improvement：

| Exchange | p99 | p99.9 | fusion records >5ms |
| --- | ---: | ---: | ---: |
| Gate | 31.71% | 91.97% | 3 / 1,075,552 |
| Binance | 7.79% | 39.75% | 0 / 3,367,135 |

Fusion hop：

| Exchange | p50 | p99 | p99.9 | hop >5ms |
| --- | ---: | ---: | ---: | ---: |
| Gate | 0.544us | 0.794us | 3.790us | 0 |
| Binance | 0.920us | 1.395us | 9.458us | 0 |

Gate 有 kernel RX timestamp；`>5ms` source outlier 主导段为 `exchange_ns -> kernel_rx_ns`，不是 parser/SHM publish。
Binance TLS 没有 kernel RX timestamp，不能严格拆分 network/kernel，但 parser/SHM publish 不是主要来源。Binance source 3
当次异常慢且 winner ratio 仅 0.35%，fusion 避开了其 tail。

该 run 支持继续 shadow 和策略接入评估，但不是一次性切换依据。需要重复不同时间窗，确认异常 source 稳定性、L4 log 扰动、
recorder overrun 和 LeadLag `lag_freshness_ns/stale_lag_quote` 变化。

## 2026-06-14 Gate BTC/ETH、N=4、30-minute

Run：`/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/`。Gate private plain；source CPU
`16-19`，fusion CPU `20`，recorder CPU `21-25`。Recorder affinity 在启动约 1 分钟后手工设置，因此该 run 不证明 config
affinity 行为。

| Metric | p50 | p95 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| fusion latency | 190.598us | 296.827us | 379.303us | 686.858us | 7.071ms |
| fusion hop | 0.484us | 0.785us | 1.029us | 1.312us | 4.372ms |

Winner ratio：source 0/1/2/3 为 45.69%/3.29%/1.46%/49.56%。Fusion winner 与离线四路
`source.local_ns` fastest 在完整可比样本中的差异约 0.11%，差值 p50 93ns、p99 745ns、max 1.353us，符合
“fusion process first processed wins、不等待全路”的语义。

Metadata 比 recorder 早启动，多 147 条；identity-set 对齐中 fusion recorder 57,656/57,656 均匹配 metadata，
不能用文件位置逐行比较推断错位。

## 复现入口

```text
scripts/market_data/analyze_book_ticker_fusion_latency.py
core/market_data/fusion/
tools/gate/gate_data_fusion.cpp
tools/binance/binance_data_fusion.cpp
```

复测必须记录 binary/commit、symbols、duration、endpoint/TLS、source/fusion/recorder CPU、diagnostic level、recorder overrun 和
完整 p50/p99/p99.9/max。已清理或不存在的 tmp artifact 不得作为新运行的输入事实源。

## 结论边界

两次 2026-06-14 shadow 表明 fastest-route fusion 可降低当次 BBO tail，常态 SHM fusion hop 很小；同时观察到单个 ms 级
hop outlier。结论不外推到其他 endpoint、CPU、feed、日期或订单收益；生产选择需要新鲜重复证据。
