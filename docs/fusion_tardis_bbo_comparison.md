# Fusion 与 Tardis BBO 对账

本文记录 live Gate / Binance canonical fusion `BookTicker` recorder 与 Tardis
`book_ticker` CSV 的离线对账入口和 20260627 30-symbol 对账结果。

## 数据范围

- live fusion recorder 运行目录：
  `/home/liuxiang/tmp/20260627_062142_30symbols_fusion_md_live/`
- Tardis 下载目录：`/home/liuxiang/tardis`
- instrument catalog：
  `config/instruments/usdt_futures_common_gate_binance_20260627.csv`
- 对账日期：`20260627`
- symbol 集合：
  `BTC_USDT`、`ETH_USDT`、`SOL_USDT`、`DOGE_USDT`、`XRP_USDT`、`LAB_USDT`、
  `HYPE_USDT`、`XAUT_USDT`、`BEAT_USDT`、`ZEC_USDT`、`H_USDT`、`SLX_USDT`、
  `WLD_USDT`、`BTW_USDT`、`AAVE_USDT`、`SUI_USDT`、`AVAX_USDT`、`M_USDT`、
  `PAXG_USDT`、`LINK_USDT`、`UB_USDT`、`BCH_USDT`、`BAS_USDT`、`UNI_USDT`、
  `ENA_USDT`、`ESPORTS_USDT`、`ONDO_USDT`、`NEAR_USDT`、`HEI_USDT`、
  `SAHARA_USDT`

每个 `exchange + symbol` 的对账窗口使用该 symbol 在 fusion 数据中 `20260627` 内的
第一条 `exchange_ns // 1_000_000` 作为 `window_start_ms`，使用 `20260627` 内最后一条
fusion 数据的 `exchange_ns // 1_000_000` 作为 `window_end_ms`。Tardis CSV 只取这个闭区间。

## 下载

使用 `third_party/crux/crux/tardis/download.py` 下载 Tardis `book_ticker`。该脚本的
`end_date` 传给 Tardis `to_date`，语义为非包含；下载 20260627 时使用 `2026-06-27` 到
`2026-06-28`。

当前机器上 `~/pyenv/lx/bin/python` 不存在；本仓库脚本使用：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python
```

示例：

```bash
PYTHONPATH=third_party/crux \
/home/liuxiang/dev/pyenv/lx/bin/python third_party/crux/crux/tardis/download.py \
  --exchange_id gate-io-futures \
  --symbols LAB_USDT \
  --data_types book_ticker \
  --start_date 2026-06-27 \
  --end_date 2026-06-28 \
  --download_dir /home/liuxiang/tardis
```

Binance 使用 catalog 中的 `exchange_symbol`，例如 `LAB_USDT` 对应 `LABUSDT`。

本次 30-symbol 下载结果：

| Exchange | 文件数 | 压缩大小 |
| --- | ---: | ---: |
| Gate | 30 | 223,501,958 bytes |
| Binance | 30 | 1,078,827,638 bytes |
| 合计 | 60 | 1,302,329,596 bytes |

## 对账脚本

入口：

```text
scripts/market_data/compare_fusion_tardis_book_ticker.py
scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py
```

脚本对 `BookTicker` binary 使用当前 64-byte ABI，读取字段：

```text
id, symbol_id, exchange, exchange_ns, local_ns,
bid_price, bid_volume, ask_price, ask_volume
```

Tardis `book_ticker` CSV 读取字段：

```text
timestamp, ask_amount, ask_price, bid_price, bid_amount
```

严格匹配 key：

```text
timestamp_ms = fusion.exchange_ns // 1_000_000
timestamp_ms = tardis.timestamp // 1_000
bid_price_units = round(bid_price / price_tick)
bid_volume_units = round(bid_volume / quantity_step)
ask_price_units = round(ask_price / price_tick)
ask_volume_units = round(ask_volume / quantity_step)
```

`--near-ms N` 会在 strict unmatched 后，对相同 price / quantity units 的两侧记录按
`timestamp_ms` 做 `±N ms` 贪心匹配。这个分类用于把 timestamp 语义或采集打点偏移从可能缺失中剥离。
`tardis_only_after_near_records` 是更接近“fusion 可能缺失 Tardis 数据”的候选数量。

## 20260627 结果

本次结果目录：

```text
/home/liuxiang/tmp/20260627_fusion_tardis_bbo_compare/
```

关键产物：

```text
analysis/fusion_tardis_bbo_summary_20260627.csv
analysis/fusion_tardis_bbo_summary_20260627.json
fusion_symbol_splits/gate_20260627_30symbols_summary.json
fusion_symbol_splits/binance_20260627_30symbols_summary.json
full_compare/<exchange>/<symbol>/*_summary.{csv,json}
full_compare/<exchange>/<symbol>/*_missing_samples.csv
```

总计结果，`near_ms=5`：

| Exchange | fusion_records | tardis_records | strict matched | strict fusion-only | strict Tardis-only | near matched | fusion-only after near | Tardis-only after near |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Gate | 25,113,984 | 14,209,628 | 2,789,325 | 22,324,659 | 11,420,303 | 11,410,218 | 10,914,441 | 10,085 |
| Binance | 92,207,967 | 92,148,236 | 92,148,236 | 59,731 | 0 | 0 | 59,731 | 0 |

Gate strict 对账差异很大，但 `±5ms` 同价量匹配可解释绝大多数 Tardis-only 差异。原因是当前 live
Gate fusion 的 `BookTicker.exchange_ns` 使用 Gate SBE `bbo.time`，即 WebSocket server send
timestamp；Tardis `book_ticker.timestamp` 的采集语义不保证与该字段逐毫秒一致。因此 Gate strict
结果不能直接当作缺失判断，优先看 `tardis_only_after_near_records`。

Binance 在本窗口内 `Tardis-only after near = 0`，即 Tardis 侧每条记录都能在 fusion canonical 数据中
找到严格匹配；fusion 额外 59,731 条，主要表现为 fusion-only。

Gate `tardis_only_after_near_records` 前 10：

| Symbol | fusion_records | tardis_records | strict matched | fusion-only after near | Tardis-only after near |
| --- | ---: | ---: | ---: | ---: | ---: |
| BTC_USDT | 1,366,171 | 651,050 | 132,729 | 716,498 | 1,377 |
| SOL_USDT | 852,059 | 544,332 | 105,648 | 308,834 | 1,107 |
| HYPE_USDT | 1,542,082 | 945,049 | 197,889 | 598,079 | 1,046 |
| SUI_USDT | 325,826 | 257,368 | 52,246 | 69,174 | 716 |
| NEAR_USDT | 427,349 | 295,692 | 55,923 | 132,327 | 670 |
| WLD_USDT | 296,269 | 225,590 | 49,038 | 71,334 | 655 |
| ETH_USDT | 1,533,641 | 736,619 | 141,203 | 797,562 | 540 |
| HEI_USDT | 1,203,460 | 757,835 | 146,439 | 446,121 | 496 |
| UNI_USDT | 108,418 | 89,231 | 16,765 | 19,628 | 441 |
| LAB_USDT | 5,717,128 | 2,578,701 | 465,726 | 3,138,738 | 311 |

Binance 前 10 均为 `Tardis-only after near = 0`；最大的 fusion-only after near 为：

| Symbol | fusion_records | tardis_records | strict matched | fusion-only after near | Tardis-only after near |
| --- | ---: | ---: | ---: | ---: | ---: |
| ETH_USDT | 13,458,793 | 13,436,028 | 13,436,028 | 22,765 | 0 |
| BTC_USDT | 12,059,987 | 12,053,637 | 12,053,637 | 6,350 | 0 |
| SOL_USDT | 6,363,571 | 6,358,529 | 6,358,529 | 5,042 | 0 |
| ZEC_USDT | 2,993,920 | 2,991,209 | 2,991,209 | 2,711 | 0 |
| DOGE_USDT | 2,545,647 | 2,543,137 | 2,543,137 | 2,510 | 0 |
| XRP_USDT | 3,177,282 | 3,174,989 | 3,174,989 | 2,293 | 0 |
| HYPE_USDT | 4,051,583 | 4,050,750 | 4,050,750 | 833 | 0 |
| LAB_USDT | 8,867,976 | 8,867,639 | 8,867,639 | 337 | 0 |
| BEAT_USDT | 3,537,321 | 3,537,189 | 3,537,189 | 132 | 0 |
| XAUT_USDT | 169,984 | 169,929 | 169,929 | 55 | 0 |

## 解释边界

- Tardis CSV 没有 live `BookTicker.id`，所以对账不能按 exchange update id 做 join，只能按时间和 BBO
  值做 multiset 比较。
- Gate live `exchange_ns` 使用 SBE `bbo.time`；Binance live `exchange_ns` 使用 JSON `E`。两者与 Tardis
  `timestamp` 的语义不完全等价。
- `near_ms` 匹配按同价量、时间邻近做贪心配对，只用于离线分类，不证明两条记录来自同一个 exchange
  update。
- `fusion-only after near` 不一定是 Tardis 缺失；也可能来自 fusion 的四路最快路由在同一 BBO 状态多次输出、
  Tardis 去重或时间戳口径差异。
- `Tardis-only after near` 是当前最值得继续抽样检查的“fusion 可能缺失”候选。
