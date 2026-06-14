# Gate 多路最快行情融合 Shadow 结果

本文记录 Gate `BookTicker` fastest-route fusion 的 shadow 实测结果。设计背景和算法语义见
`docs/gate_fastest_route_fusion_design.md`，实现计划见
`docs/gate_fastest_route_fusion_implementation_plan.md`。

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
