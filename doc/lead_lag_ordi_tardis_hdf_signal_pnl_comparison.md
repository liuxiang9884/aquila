# ORDI_USDT LeadLag Tardis / HDF Replay 对比

## 背景

本次对比使用 ORDI_USDT 永续合约 20260415～20260417 三天的 book ticker 数据，分别从两条链路生成 Aquila `BookTicker` binary 后回放 LeadLag 策略：

- Tardis binary：`/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/*.bin`
- HDF binary：`/home/liuxiang/tardis/merged_book_ticker_hdf/ORDI_USDT/*.bin`
- Tardis signal：`/tmp/lead_lag_compare/tardis_signal.csv`
- HDF signal：`/tmp/lead_lag_compare/hdf_signal.csv`

回放使用同一套策略配置：

```bash
./build/debug/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --signals-output /tmp/lead_lag_compare/tardis_signal.csv

./build/debug/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --data-reader-config /tmp/lead_lag_compare/lead_lag_ordi_hdf_binary_replay.toml \
  --signals-output /tmp/lead_lag_compare/hdf_signal.csv
```

PnL 计算口径：

- 每次开仓名义本金：`1000 USDT`
- 手续费：每次成交名义金额的 `0.0002`
- ORDI tick size：`0.001`
- 滑点：按成交方向恶化 `n` 个 tick，`n = 0..5`
- 开仓和平仓都按 signal CSV 中的价格成交；`stoploss` 作为平仓信号参与 PnL。

## 输入数据对比

两条链路生成的 binary 文件大小接近，但精确记录数不一致。每条 `BookTicker` 为 64 字节。

| 日期 | Tardis records | HDF records | HDF - Tardis |
| --- | ---: | ---: | ---: |
| 20260415 | 5,763,144 | 5,757,715 | -5,429 |
| 20260416 | 51,292,907 | 51,016,737 | -276,170 |
| 20260417 | 37,743,010 | 37,512,472 | -230,538 |
| 合计 | 94,799,061 | 94,286,924 | -512,137 |

按交易所拆分后，差异主要来自 Gate：

| 日期 | Tardis Binance | HDF Binance | Binance 差异 | Tardis Gate | HDF Gate | Gate 差异 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 20260415 | 3,302,121 | 3,302,224 | +103 | 2,461,023 | 2,455,491 | -5,532 |
| 20260416 | 27,175,814 | 27,178,705 | +2,891 | 24,117,093 | 23,838,032 | -279,061 |
| 20260417 | 19,068,970 | 19,071,111 | +2,141 | 18,674,040 | 18,441,361 | -232,679 |

## 信号对比

| 数据源 | book_tickers | signals | open | close | stoploss |
| --- | ---: | ---: | ---: | ---: | ---: |
| Tardis | 94,799,061 | 2,350 | 1,175 | 1,173 | 2 |
| HDF | 94,286,924 | 2,620 | 1,310 | 1,309 | 1 |

按天统计 signal 数量：

| 日期 | Tardis signals | HDF signals | HDF - Tardis |
| --- | ---: | ---: | ---: |
| 20260415 | 320 | 346 | +26 |
| 20260416 | 1,052 | 1,166 | +114 |
| 20260417 | 978 | 1,108 | +130 |

按 `(exchange_ns, action, side, price, reduce_only)` 作为 signal key 对比：

| 项目 | 数量 |
| --- | ---: |
| 两边共同 signal key | 1,867 |
| 仅 Tardis 有 | 483 |
| 仅 HDF 有 | 753 |

第一个按输出顺序不一致的信号出现在第 8 个信号：

| 数据源 | ticker_id | exchange_ns | action | side | price | reduce_only |
| --- | ---: | ---: | --- | --- | ---: | --- |
| Tardis | 153749 | 1776217722950000000 | `kCloseShort` | `kBuy` | 2.519 | true |
| HDF | 153740 | 1776217722949000000 | `kCloseShort` | `kBuy` | 2.519 | true |

## PnL 对比

### Tardis

| slippage ticks | 费前 PnL | 手续费 | 费后 PnL |
| ---: | ---: | ---: | ---: |
| 0 | 2641.54471278 | 469.95104563 | 2171.59366715 |
| 1 | 2228.76164711 | 469.95736266 | 1758.80428444 |
| 2 | 1815.96048125 | 469.96371384 | 1345.99676741 |
| 3 | 1403.14109050 | 469.97009917 | 933.17099133 |
| 4 | 990.30335010 | 469.97651866 | 520.32683145 |
| 5 | 577.44713530 | 469.98297230 | 107.46416300 |

### HDF

| slippage ticks | 费前 PnL | 手续费 | 费后 PnL |
| ---: | ---: | ---: | ---: |
| 0 | 2938.19270675 | 523.92318621 | 2414.26952054 |
| 1 | 2479.62053261 | 523.93190378 | 1955.68862882 |
| 2 | 2021.02554814 | 523.94065907 | 1497.08488907 |
| 3 | 1562.40761656 | 523.94945206 | 1038.45816450 |
| 4 | 1103.76660108 | 523.95828277 | 579.80831831 |
| 5 | 645.10236488 | 523.96715120 | 121.13521368 |

### 费后 PnL 差异

| slippage ticks | HDF net PnL - Tardis net PnL |
| ---: | ---: |
| 0 | 242.67585339 |
| 1 | 196.88434438 |
| 2 | 151.08812166 |
| 3 | 105.28717317 |
| 4 | 59.48148686 |
| 5 | 13.67105068 |

## 不一致原因

当前证据说明，两条链路的输入 tick 流不是完全一致的数据集，而不是单纯 replay 程序不确定。

1. **输入记录数不同。** HDF 三天总记录数比 Tardis 少 `512,137` 条。缺口主要集中在 Gate：三天 HDF Gate 比 Tardis Gate 少 `517,272` 条；Binance HDF 反而比 Tardis 多 `5,135` 条。

2. **HDF 当前使用普通 `bbo` 表，不是 `bbo_ns`。** 按 HDF 读取规则，普通表时间字段单位是 ms；Tardis CSV 转换链路使用 Tardis `timestamp/local_timestamp` 的 us 级字段再转 ns。HDF 普通 `bbo` 会把 sub-ms 时间信息折叠到 ms 边界，影响同一毫秒内的排序、窗口统计和本地时间诊断。第一个信号里也能看到同一 `exchange_ns` 附近的 `local_ns` 精度不同。

3. **数据语义不完全等价。** Tardis 输入是 `book_ticker` CSV；HDF 输入是 xex_mars / hf_data_store_op 的 `bbo` 表。即使都表示 best bid/ask，采集、去重、落盘和时间精度规则可能不同。当前记录数差异已经证明两者不是 byte-for-byte 等价源。

4. **LeadLag 策略是状态型策略。** drift、noise、spread、move quantile、开平仓状态都会随历史窗口累计。早期少量 tick、时间排序或同毫秒排序差异，会影响后续 threshold 和触发点，所以信号数量、开平仓匹配点和 PnL 都会放大差异。

因此，当前 HDF replay 的 PnL 可以作为“基于 HDF BBO 数据源”的策略结果，但不能直接当作 Tardis replay 的逐 tick 对账结果。若目标是逐条对齐，需要先确认 HDF 是否有 `bbo_ns` 数据，或进一步按交易所、日期、`tx_time/bid/ask/qty` 做原始 tick diff，定位 Gate 缺失记录的来源。
