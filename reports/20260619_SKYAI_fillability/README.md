# SKYAI_USDT Fillability Analysis

本目录归档 `20260619_095317_28symbols_no_h_30d_fusion_off_l0_live` 中 `SKYAI_USDT` 的 IOC 未成交订单可成交性分析。归档只保留该 symbol 相关的订单、信号、精简日志、symbol-level BookTicker、分析中间产物和最终结论。

## 分析对象

- Run id: `20260619_095317_28symbols_no_h_30d_fusion_off_l0_live`
- Symbol: `SKYAI_USDT`
- Report snapshot: `/home/liuxiang/tmp/20260619_095317_28symbols_no_h_30d_fusion_off_l0_live/status_reports_20260620_012105/20260619_095317_28symbols_no_h_30d_fusion_off_l0_live`
- 主样本：开仓、完全未成交、`kCancelled` IOC，共 `93` 笔
- 对照样本：filled 或 partially-filled 订单，共 `17` 笔
- `accepted_lag_id` 覆盖：`67/93`；另外 `26/93` 的事件顺序是 `Ack -> Cancelled -> Finished -> late Accepted(accepted_lag_id=0)`，因此 accepted 相关口径只作为辅助。

## 快速使用

仓库中的压缩包为 `reports/20260619_SKYAI_fillability.zip`，解压后目录名仍为 `20260619_SKYAI_fillability/`。

建议阅读顺序：

1. 先看本文档的“核心结果”和“结论”。
2. 需要逐笔复核时，看 `analysis/ack_full_candidates.csv` 和 `analysis/cancel_point_candidates.csv`。
3. 需要理解 CSV 字段时，看 `FIELD_SCHEMA.md`。
4. 需要追溯原始来源时，看 `context/source_paths.txt` 和 `logs/extraction_notes.md`。
5. 需要验证归档完整性时，在本目录执行 `sha256sum -c checksums.sha256`。

## 文件说明

- `FIELD_SCHEMA.md`：本归档内 CSV、JSON summary、BookTicker bin 和日志的字段级说明。
- `manifest.json`：归档文件清单，包含每个文件的大小、sha256，CSV 文件还包含行数。
- `checksums.sha256`：归档完整性校验文件；不包含压缩包本身。

### `inputs/`

- `orders.csv`：从 `order_detail.csv` 过滤出的全部 `SKYAI_USDT` 订单，`113` 行。
- `signals.csv`：从 `signal.csv` 过滤出的 `SKYAI_USDT` signal，`237` 行。
- `positions.csv`：从 `position.csv` 过滤出的 `SKYAI_USDT` position 行，`9` 行。
- `latency.csv`：从 `latency.csv` 过滤出的 `SKYAI_USDT` 订单 latency，`113` 行。
- `instrument.csv`：instrument catalog 中 `SKYAI_USDT` 相关行，`2` 行。

### `logs/`

- `strategy.log`：strategy log 中包含 `SKYAI_USDT`、相关 `local_order_id`、非零 `exchange_order_id` 或 `text_order_id` 的行。
- `feedback.log`：Gate feedback session log 中相同规则过滤出的行。
- `extraction_notes.md`：日志过滤规则、id 集合规模和过滤后行数。

### `market_data/`

- `canonical.bin.zst`：Gate fusion canonical 的 `SKYAI_USDT` BookTicker split。
- `source0.bin.zst` 到 `source3.bin.zst`：Gate fusion source0..3 的 `SKYAI_USDT` BookTicker split。
- `split_summaries/*.json`：每个 split 的记录数和输入统计。

这些 `.bin.zst` 是 symbol-level 数据，不包含其他 symbol。解压后记录 schema 与 `scripts/market_data/analyze_book_ticker_latency.py::book_ticker_dtype()` 一致。

### `analysis/`

- `cancel_windows.csv`：最终主口径订单级表，覆盖 `93` 笔开仓未成交 cancel。
- `cancel_windows_summary.json`：`cancel_windows.csv` 的汇总。
- `cancel_fillability.csv`：早期 canonical stage/window 分析表。
- `cancel_fillability_summary.json`：`cancel_fillability.csv` 的汇总。
- `filled_control.csv`：filled/partial 对照样本订单级分析。
- `source_by_order.csv`：canonical 与 source0..3 的订单级 crosscheck 聚合。
- `source_long.csv`：canonical 与 source0..3 的长表 crosscheck。
- `source_summary.json`：source crosscheck 汇总。
- `volume_by_order.csv`：BBO 对手量是否覆盖整单的补充分析。
- `volume_summary.json`：volume 分析汇总。
- `ack_full_candidates.csv`：`(ack_lag_id, cancelled_lag_id]` 内 BBO 价格穿越且对手量覆盖整单的候选订单，`5` 行。
- `cancel_point_candidates.csv`：`cancelled_lag_id` 本点仍价格穿越的候选订单，`3` 行。

### `context/`

- `configs/`：本次 run 相关 runtime config 的短名副本。
- `source_paths.txt`：所有源文件和 scratch 输出的完整路径。
- `generation_commands.sh`：归档生成和验证命令记录。

## 统计口径

- `any`：BBO 价格穿越且对手一档量大于 0。买单用 `ask <= order_price`，卖单用 `bid >= order_price`。
- `full`：满足 `any`，且对手一档量 `>= order quantity`。
- `request_send -> finish stateful`：`request_send_local_ns` 时刻最新 BBO 状态，加上 `(request_send_local_ns, order_finished_local_ns]` 内的新 BookTicker 更新。
- `request_send -> finish update-only`：只看 `(request_send_local_ns, order_finished_local_ns]` 内的新 BookTicker 更新。
- `ack -> cancel`：BookTicker id 区间 `(ack_lag_id, cancelled_lag_id]`，排除 ack 本点，包含 cancelled 点。
- `cancel point`：只看 `cancelled_lag_id` 本点。

## 核心结果

| 口径 | 价格穿越 `any` | BBO 量覆盖整单 `full` |
|---|---:|---:|
| `request_send -> finish` stateful | `92/93` | `24/93` |
| `request_send -> finish` update-only | `20/93` | `10/93` |
| `(ack_lag_id, cancelled_lag_id]` | `12/93` | `5/93` |
| `cancelled_lag_id` 本点 | `3/93` | `1/93` |

## 结论

整体上，`SKYAI_USDT` 的未成交 IOC cancel 与实盘快速盘口行为基本一致：策略在 signal/request_send 时看到可打穿盘口，但订单生命周期内多数盘口很快变化，或 BBO 一档量不足以覆盖整笔订单。

最值得继续逐笔复核的是 `analysis/ack_full_candidates.csv` 中的 5 笔订单：

- `288230376151712112`
- `288230376151712152`
- `288230376151712212`
- `288230376151712360`
- `288230376151712725`

这些订单在 `ack -> cancel` 严格区间内，从本地 Gate BBO 视角看曾经价格穿越且一档量足以覆盖整单，但最终仍 IOC cancel。它们不能直接证明 Gate 撮合异常，因为 BookTicker 不是 matching engine 收到 IOC 瞬间的撮合簿快照；但它们是后续定位网络延迟、source 差异或撮合时序的优先样本。

`analysis/cancel_point_candidates.csv` 中的 3 笔是更尖锐但更容易误读的样本，因为 `cancelled_lag_id` 是策略处理 cancel feedback 时看到的 latest BBO，不等于 Gate 撮合/取消发生时的盘口。

## 验证

本归档生成后应满足：

- `inputs/orders.csv` 为 `113` 行。
- `analysis/cancel_windows.csv` 为 `93` 行。
- `analysis/ack_full_candidates.csv` 为 `5` 行。
- `analysis/cancel_point_candidates.csv` 为 `3` 行。
- `zstd -t market_data/*.zst` 通过。
- `sha256sum -c checksums.sha256` 通过。
