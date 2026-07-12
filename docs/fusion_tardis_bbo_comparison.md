# Fusion 与 Tardis BBO 对账摘要

本文保存 2026-06-27 Gate/Binance 30-symbol canonical fusion 与 Tardis `book_ticker` 的离线对账方法和关键结果。
当前 fusion 架构见 `docs/market_data_fusion.md`。

## 输入与口径

- Live run：`/home/liuxiang/tmp/20260627_062142_30symbols_fusion_md_live/`
- Tardis root：`/home/liuxiang/tardis`
- Date：`20260627`
- Exchange/symbol：Gate 与 Binance 各 30 symbols
- Analyzer：`scripts/market_data/compare_fusion_tardis_book_ticker.py`
- Test：`scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py`

当前 catalog 已统一为 `config/instruments/usdt_future_universe.csv`；历史 run 使用的旧 catalog 名称只描述当次输入，
不得作为当前配置入口。

Fusion key 使用 72-byte `BookTicker` 的 timestamp/price/quantity；Tardis 使用 `timestamp, ask_amount, ask_price,
bid_price, bid_amount`。Strict matching 将 timestamp 归一到 ms，并按 price tick/quantity step 转整数单位。`--near-ms N`
只对 strict unmatched 中相同价量记录做 `±N ms` 贪心匹配，用于区分 timestamp 口径差异；它不证明同一 exchange update。

## 2026-06-27 结果

`near_ms=5`：

| Exchange | Fusion records | Tardis records | Strict matched | Near matched | Fusion-only after near | Tardis-only after near |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Gate | 25,113,984 | 14,209,628 | 2,789,325 | 11,410,218 | 10,914,441 | 10,085 |
| Binance | 92,207,967 | 92,148,236 | 92,148,236 | 0 | 59,731 | 0 |

Binance Tardis records 是 fusion 的 99.9352%；本窗口内 Tardis 每条记录都有 strict match，fusion 多 59,731 条
（0.0648%）。该结果未发现 Binance Tardis-only 缺口。

Gate strict 差异不能直接解释为缺失：live `exchange_ns` 使用 SBE `bbo.time`（WebSocket server send），`event_ns` 使用
`bbo.t`（engine update），Tardis `timestamp` 语义不保证逐 ms 相同。`±5ms` 同价量匹配解释了绝大多数 Tardis-only；
剩余 10,085 条是值得抽样的“可能缺失”候选，不是已确认丢包。

## Gate BTC 抽样

Gate BTC fusion 记录的 `id=bbo.u` 严格递增、无 duplicate id。观察到 66 个相邻 `exchange_ns` 回退点，幅度
0.006–19.719ms；按 timestamp 重排会导致 id 倒序。因此 live/replay/fusion canonical 顺序以 exchange update id/发布顺序为准，
只在 timestamp 统计或 Tardis 对齐时按 `exchange_ns` 排序。

## 复现命令

下载 Tardis 时 `to_date` 非包含；单日使用次日作为 end：

```bash
PYTHONPATH=third_party/crux \
/home/liuxiang/dev/pyenv/lx/bin/python third_party/crux/crux/tardis/download.py \
  --exchange_id gate-io-futures \
  --symbols BTC_USDT \
  --data_types book_ticker \
  --start_date 2026-06-27 \
  --end_date 2026-06-28 \
  --download_dir /home/liuxiang/tardis
```

对账必须按目标 symbol 的 fusion 首末 `exchange_ns` 取闭区间，记录 catalog、tick/step、ABI、near window 和输入文件 hash。

## 解释边界

- Tardis CSV 没有 live `BookTicker.id`，不能按 exchange update id join。
- Gate `bbo.time`、Gate `bbo.t`、Binance JSON `E` 与 Tardis `timestamp` 不是统一语义。
- `fusion-only` 可能来自多路最快路由重复 BBO、Tardis 去重或 timestamp 差异，不等于 Tardis 丢失。
- `Tardis-only after near` 是候选抽样集合，不是 fusion 已确认缺失。
- 该对账只验证行情数据覆盖与口径，不代表下单、fillability 或 PnL。
