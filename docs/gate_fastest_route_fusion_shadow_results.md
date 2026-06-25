# Gate / Binance 多路最快行情融合 Shadow 结果

本文记录 Gate / Binance `BookTicker` fastest-route fusion 的 shadow 实测结果。设计背景、算法语义和当前入口见
`docs/gate_fastest_route_fusion_design.md`。

## 2026-06-14 30-symbol Gate / Binance 4-source 30m L4

### 运行条件

- Gate run directory: `/home/liuxiang/tmp/20260614_133704_gate_fusion_30symbols_4src_30m_l4_outlier_release/`
- Binance run directory: `/home/liuxiang/tmp/20260614_133704_binance_fusion_30symbols_4src_30m_l4_outlier_release/`
- time: `2026-06-14T13:37:17Z` 到 `2026-06-14T14:07:17Z`
- duration: `1800s`
- binary: `build/release/tools`
- build diagnostics: `AQUILA_DATA_SESSION_DIAG_LEVEL=4`
- latency outlier threshold: `5ms`
- outlier log rate limit: `max_logs_per_second=1000`
- symbols: `config/instruments/usdt_futures_common_gate_binance_20260602.csv` 对应的 30 个实盘测试 symbol
- source count: `N=4`
- Gate source connection: 4 条独立 Gate private plain WebSocket connection
- Binance source connection: 4 条独立 Binance public TLS WebSocket connection
- Gate CPU: sources `16-19`，fusion `20`，recorders `21-25`，log backend `31`
- Binance CPU: sources `0-3`，fusion `4`，recorders `5-9`，log backend `15`

运行边界：

- Gate / Binance data session、fusion 和 recorder 日志均显示本次记录窗口 `result=ok`。
- 启动脚本的最终 process wait / exit 记录不作为证据源：记录窗口结束后部分 source 进程没有按脚本预期退出，
  后续只清理了本次 shadow run 的残留 pid。结论以 data session / fusion / recorder 的 `result=ok` 和 bin 文件为准。
- Gate private plain 开启了 RX software timestamping，因此 L4 能把 `exchange_ns -> kernel_rx_ns` 与本机
  kernel / read / user path 拆开。
- Binance public TLS 当前拿不到 socket RX timestamp，因此 L4 只能说明 parser / publish 不是主因，
  不能把 TLS 前的延迟进一步严格拆成网络或本机 kernel。
- Binance `source_3` 的 `>5ms` outlier 非常密集，outlier log 受 `max_logs_per_second=1000`
  rate limit 影响；`>5ms` 数量以 recorder bin 统计为准，log 数量只作为分段样本。
- 这是 shadow 运行；策略没有消费 canonical fusion SHM。

分析输出目录：

```text
/home/liuxiang/tmp/20260614_133704_gate_fusion_30symbols_4src_30m_l4_outlier_release/analysis/published_fusion_l4/
/home/liuxiang/tmp/20260614_133704_binance_fusion_30symbols_4src_30m_l4_outlier_release/analysis/published_fusion_l4/
```

### Gate Latency

下表单位为 `ms`。source latency 是 `source.local_ns - source.exchange_ns`；
canonical fusion latency 是 `fusion.local_ns - fusion.exchange_ns`；
fusion hop 是 `fusion_publish_ns - source_local_ns`。

| stream | count | p50 | p95 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| source_0 | 1,072,720 | 0.211 | 0.289 | 0.438 | 5.521 | 133.787 |
| source_1 | 1,072,640 | 0.278 | 0.354 | 0.505 | 6.217 | 156.249 |
| source_2 | 1,072,914 | 0.144 | 0.261 | 0.380 | 3.751 | 206.863 |
| source_3 | 1,072,638 | 0.249 | 0.322 | 0.482 | 5.616 | 156.272 |
| fusion | 1,075,552 | 0.144 | 0.234 | 0.259 | 0.301 | 17.099 |
| fusion_hop | 1,076,803 | 0.000544 | 0.000686 | 0.000794 | 0.003790 | 0.494518 |

Gate fusion 相对每个 percentile 上的最佳单路 source：

| percentile | best single source | best single | fusion | improvement |
| --- | --- | ---: | ---: | ---: |
| p50 | source_2 | 0.144 | 0.144 | -0.001 / -0.58% |
| p95 | source_2 | 0.261 | 0.234 | 0.028 / 10.57% |
| p99 | source_2 | 0.380 | 0.259 | 0.120 / 31.71% |
| p99.9 | source_2 | 3.751 | 0.301 | 3.450 / 91.97% |

Gate winner ratio：

| source | winner count | ratio |
| --- | ---: | ---: |
| 0 | 215,587 | 20.02% |
| 1 | 58,857 | 5.47% |
| 2 | 736,685 | 68.41% |
| 3 | 65,674 | 6.10% |

Gate `>5ms` recorder bin 统计：

| stream | count | total | ratio |
| --- | ---: | ---: | ---: |
| source_0 | 1,136 | 1,072,720 | 0.105899% |
| source_1 | 1,132 | 1,072,640 | 0.105534% |
| source_2 | 971 | 1,072,914 | 0.090501% |
| source_3 | 1,125 | 1,072,638 | 0.104882% |
| fusion | 3 | 1,075,552 | 0.000279% |
| fusion_hop | 0 | 1,076,803 | 0.000000% |

Gate L4 outlier log 显示所有 `>5ms` source 样本都有 `kernel_rx_ns`。这些样本的主导阶段是
`exchange_ns -> kernel_rx_ns`，也就是 `network_or_exchange_ns`；本机 `kernel_rx_ns -> read_return_ns`、
read callback、parser 和 SHM publish 基本是 us 级，不是 `>5ms` outlier 的主要来源。这个结论不能继续
把 `network_or_exchange_ns` 严格拆成公网网络、Gate server send timestamp 误差或交易所侧排队；它只能排除
本机 kernel queue / 用户态 parser / SHM publish 是主要原因。

### Binance Latency

下表单位为 `ms`。

| stream | count | p50 | p95 | p99 | p99.9 | max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| source_0 | 3,367,135 | 1.266 | 1.813 | 1.974 | 3.196 | 28.051 |
| source_1 | 3,364,800 | 1.179 | 1.731 | 1.902 | 4.172 | 28.033 |
| source_2 | 3,365,385 | 1.215 | 1.782 | 2.125 | 26.283 | 99.431 |
| source_3 | 3,367,135 | 10.697 | 29.581 | 33.563 | 45.589 | 131.551 |
| fusion | 3,367,135 | 1.109 | 1.641 | 1.754 | 1.926 | 3.544 |
| fusion_hop | 3,370,074 | 0.000920 | 0.001178 | 0.001395 | 0.009458 | 1.377708 |

Binance fusion 相对每个 percentile 上的最佳单路 source：

| percentile | best single source | best single | fusion | improvement |
| --- | --- | ---: | ---: | ---: |
| p50 | source_1 | 1.179 | 1.109 | 0.069 / 5.88% |
| p95 | source_1 | 1.731 | 1.641 | 0.090 / 5.18% |
| p99 | source_1 | 1.902 | 1.754 | 0.148 / 7.79% |
| p99.9 | source_0 | 3.196 | 1.926 | 1.271 / 39.75% |

Binance winner ratio：

| source | winner count | ratio |
| --- | ---: | ---: |
| 0 | 314,382 | 9.33% |
| 1 | 1,850,383 | 54.91% |
| 2 | 1,193,458 | 35.41% |
| 3 | 11,851 | 0.35% |

Binance `>5ms` recorder bin 统计：

| stream | count | total | ratio |
| --- | ---: | ---: | ---: |
| source_0 | 1,381 | 3,367,135 | 0.041014% |
| source_1 | 2,510 | 3,364,800 | 0.074596% |
| source_2 | 17,237 | 3,365,385 | 0.512185% |
| source_3 | 2,562,688 | 3,367,135 | 76.108858% |
| fusion | 0 | 3,367,135 | 0.000000% |
| fusion_hop | 0 | 3,370,074 | 0.000000% |

Binance L4 outlier log 中 `kernel_rx_available=false`，因为当前 Binance source 走 TLS。L4 结果可以说明
parser / SHM publish 不是主要来源，但不能严格判断 TLS 前的主导延迟属于网络还是本机 kernel。
`source_3` 明显异常：单路 `>5ms` 比例超过 `76%`，但它在 fusion winner 中只占 `0.35%`，
canonical fusion 在本次 30 分钟窗口内没有 `>5ms` 记录。

### 当前结论

1. 30-symbol / 30m / N=4 shadow 显示 fastest-route fusion 对 tail 非常有效。Gate p99 比最佳单路降低
   `31.71%`，p99.9 降低 `91.97%`；Binance p99 比最佳单路降低 `7.79%`，p99.9 降低 `39.75%`。
2. fusion hop 常态很小：Gate hop p99 `0.794us`，Binance hop p99 `1.395us`。本次 Gate / Binance
   `fusion_hop > 5ms` 均为 `0`。
3. Gate `>5ms` source outlier 的主导阶段是 `exchange_ns -> kernel_rx_ns`；本机 kernel queue、
   read callback、parser 和 SHM publish 不是主要来源。
4. Binance 因 TLS 暂时不能确认网络 / kernel 细分，但可以排除 parser / SHM publish 是主因。
5. 本次结果支持继续推进 canonical fusion shadow 和接策略方案设计，但还不是策略切换证据。策略接入前需要
   多轮重复 shadow、确认 source 异常是否稳定、评估 L4 outlier log 对热路径的扰动，并把 canonical stream
   与 LeadLag `lag_freshness_ns` / `stale_lag_quote` reject 对齐。

## 2026-06-14 BTC / ETH 4-source 30m

### 运行条件

- run directory: `/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/`
- time: `2026-06-14T05:16:02Z` 到 `2026-06-14T05:46:02Z`
- duration: `1800s`
- binary: `build/release/tools`
- symbols: `BTC_USDT` (`symbol_id=92`), `ETH_USDT` (`symbol_id=162`)
- source count: `N=4`
- source connection: 4 条独立 Gate private plain WebSocket connection
- endpoint: `fxws-private.gateapi.io:80`, `connect_ip=10.0.1.154`, `enable_tls=false`
- source CPUs: `16,17,18,19`
- fusion CPU: `20`
- recorder CPUs: `21,22,23,24,25`

运行边界：

- 所有 source / fusion / recorder process exit status 都是 `0`。
- 所有 started process 的 stderr 为空。
- 5 个 recorder 的 `skipped=0`、`overruns=0`。
- `data_reader_recorder` config 中的 affinity 未实际生效；启动后约 1 分钟在
  `2026-06-14T05:17:02Z` 用 `taskset` 手动把 5 个 recorder 固定到 `21-25`。
  因此本次数据前约 1 分钟存在 recorder 未绑核边界，不应用它证明 recorder affinity
  config 行为。

### 产物

```text
/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/
  bin/source_0.bin
  bin/source_1.bin
  bin/source_2.bin
  bin/source_3.bin
  bin/fusion.bin
  bin/fusion_metadata.bin
  analysis/book_ticker_fusion_latency_summary.json
  analysis/book_ticker_fusion_latency_summary.csv
  analysis/book_ticker_fusion_latency_top_outliers.csv
  analysis/report.md
  analysis/source_fastest_vs_fusion_winner.md
```

分析命令：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/market_data/analyze_book_ticker_fusion_latency.py \
  --source-bin 0=/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/bin/source_0.bin \
  --source-bin 1=/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/bin/source_1.bin \
  --source-bin 2=/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/bin/source_2.bin \
  --source-bin 3=/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/bin/source_3.bin \
  --fusion-bin /home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/bin/fusion.bin \
  --metadata-bin /home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/bin/fusion_metadata.bin \
  --output-dir /home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/analysis \
  --top-n 20
```

### 记录数量和对齐

| stream | count |
| --- | ---: |
| source_0 recorder | 57,260 |
| source_1 recorder | 57,354 |
| source_2 recorder | 57,406 |
| source_3 recorder | 57,170 |
| fusion recorder | 57,656 |
| fusion metadata | 57,803 |
| fusion process published | 57,803 |

按 `(symbol_id, id, exchange_ns, fusion_publish_ns)` 做 identity-set 对齐：

| check | count |
| --- | ---: |
| fusion records matched metadata | 57,656 / 57,656 |
| fusion without metadata | 0 |
| metadata without fusion recorder record | 147 |
| BTC metadata without fusion recorder record | 87 |
| ETH metadata without fusion recorder record | 60 |

`analyze_book_ticker_fusion_latency.py` 当前 JSON 中的 `fusion_metadata_alignment` 是按文件位置逐条比较。
本次 metadata writer 比 fusion recorder 更早开始，metadata 多出开头 147 条，因此位置比较会整体错位；
判断实际内容对齐应使用上面的 identity-set 口径。

### Latency 口径

```text
source_latency_ns = source.local_ns - source.exchange_ns
fusion_latency_ns = fusion.local_ns - fusion.exchange_ns
fusion_hop_ns     = fusion_publish_ns - source_local_ns
```

在 fusion 输出中，`fusion.local_ns = fusion_publish_ns`，所以 `fusion_latency_ns`
是 Gate BBO exchange timestamp 到 canonical SHM publish 的总延迟。

### Overall latency

单位：ns。

| stream | p50 | p95 | p99 | p99.9 | max | mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| canonical fusion latency | 190,598 | 296,827 | 379,303 | 686,858 | 7,071,206 | 200,671 |
| fusion hop | 484 | 785 | 1,029 | 1,312 | 4,371,768 | 596 |

本次 `fusion_hop_ns` p99 约 `1.029us`，p99.9 约 `1.312us`；有一条
`4.371768ms` outlier。除该 outlier 外，fusion hop 主体很小。

### Per-source latency

单位：ns。

| source | count | p50 | p95 | p99 | p99.9 | max | mean |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 57,260 | 234,970 | 465,719 | 2,045,999 | 14,853,771 | 131,884,100 | 340,707 |
| 1 | 57,354 | 341,552 | 763,578 | 5,047,139 | 27,463,312 | 138,573,725 | 536,158 |
| 2 | 57,406 | 409,613 | 890,819 | 4,183,991 | 11,446,142 | 175,593,884 | 555,971 |
| 3 | 57,170 | 209,570 | 370,879 | 1,497,621 | 10,656,569 | 125,426,992 | 286,794 |

对比单路 source，本次 canonical fusion 的 p50 / p95 / p99 / p99.9 都更低；
尤其 p99 / p99.9 tail 改善明显。

### Winner ratio

| source | winner count | ratio |
| --- | ---: | ---: |
| 0 | 26,409 | 45.6879% |
| 1 | 1,903 | 3.2922% |
| 2 | 845 | 1.4619% |
| 3 | 28,646 | 49.5580% |

BTC winner ratio:

| source | ratio |
| --- | ---: |
| 0 | 42.7881% |
| 1 | 2.9202% |
| 2 | 1.2272% |
| 3 | 53.0645% |

ETH winner ratio:

| source | ratio |
| --- | ---: |
| 0 | 48.5605% |
| 1 | 3.6607% |
| 2 | 1.6943% |
| 3 | 46.0844% |

本次 source 0 和 source 3 是主要 winner，source 1 / 2 仍偶尔赢。
这说明多路连接不是简单 primary / standby：即使某一路整体更快，其它 source 仍会在部分 update 上先到。

### BTC / ETH 分 symbol latency

单位：ns。

BTC (`symbol_id=92`):

| metric | fusion latency | fusion hop |
| --- | ---: | ---: |
| count | 28,678 | 28,765 |
| p50 | 198,090 | 484 |
| p90 | 276,304 | 668 |
| p95 | 303,711 | 784 |
| p99 | 381,075 | 1,025 |
| p99.9 | 662,507 | 1,319 |
| max | 4,649,973 | 4,371,768 |
| mean | 206,931 | 678 |

ETH (`symbol_id=162`):

| metric | fusion latency | fusion hop |
| --- | ---: | ---: |
| count | 28,978 | 29,038 |
| p50 | 183,266 | 483 |
| p90 | 261,045 | 673 |
| p95 | 288,231 | 785 |
| p99 | 377,775 | 1,035 |
| p99.9 | 773,695 | 1,307 |
| max | 7,071,206 | 10,006 |
| mean | 194,477 | 515 |

### Fusion winner 与 source-bin fastest 对比

比较口径：

- `fusion winner`: `fusion_metadata.bin` 中记录的 `source_id`。
- `source-bin fastest`: 对同一 `(symbol_id, BookTicker.id)`，在四个 source bin 中取
  `BookTicker.local_ns` 最小的 source。

覆盖：

| item | count |
| --- | ---: |
| metadata records | 57,803 |
| records with at least one matching source-bin record | 57,656 |
| records missing all source-bin records | 147 |
| missing fusion winner source record | 0 |
| source-bin duplicate `(symbol_id, id)` | 0 on all four source bins |
| exchange_ns mismatch for fusion winner source | 0 |
| tie count for minimum source `local_ns` | 1 |

可用 source 数量分布：

| available sources | records |
| ---: | ---: |
| 1 | 6 |
| 2 | 97 |
| 3 | 1,222 |
| 4 | 56,331 |

结果：

| scope | same | different | difference ratio |
| --- | ---: | ---: | ---: |
| at least one source available | 57,591 | 65 | 0.1127% |
| all four sources available | 56,267 | 64 | 0.1136% |

差异方向：

| fusion source -> source-bin fastest | count |
| --- | ---: |
| 0 -> 1 | 4 |
| 0 -> 2 | 1 |
| 0 -> 3 | 52 |
| 1 -> 3 | 5 |
| 2 -> 3 | 2 |
| 3 -> 0 | 1 |

差异大小定义：

```text
delta_ns = fusion_winner_source_local_ns - source_bin_fastest_local_ns
```

| metric | ns |
| --- | ---: |
| min | 2 |
| p50 | 93 |
| p90 | 269 |
| p95 | 342 |
| p99 | 745 |
| p99.9 | 1,292 |
| max | 1,353 |
| mean | 136 |

结论：fusion winner 与离线 source-bin fastest 基本一致。少量 `0.11%` 差异的幅度主要是几十到几百 ns，
符合 V1 语义：fusion 发布的是 fusion process 实际先处理到、且能推进 `last_published_id` 的 source；
它不会等待四路都到达后再按 `source.local_ns` 全局排序。

## 结论

1. 本次 2-symbol / 30m / 4-source shadow 显示，canonical fusion 对单路 source 的 p99 / p99.9
   tail 有明显改善。
2. `source SHM -> fusion -> canonical SHM` 的常态 hop 很小，p99 约 `1.029us`，
   p99.9 约 `1.312us`。但存在单个 ms 级 outlier，需要后续更长时间和更多 symbol 继续观察。
3. source 0 / 3 是主要 winner，但 source 1 / 2 也会赢一部分 update，说明 fastest-route
   fusion 有别于固定 primary。
4. fusion winner 与离线 source-bin fastest 的差异只有约 `0.11%`，且差异幅度很小；
   这支持当前“不等待、不比较、first processed wins”的 V1 设计。
5. 本次结果不能单独作为切策略依据。接入策略前还需要至少做更多 symbol、更长时间、
   明确 recorder affinity、并和 LeadLag `lag_freshness_ns` / `stale_lag_quote` reject 对齐验证。
