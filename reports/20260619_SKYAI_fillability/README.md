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
2. 需要理解 signal 到 `x_in_time` / `x_out_time` 的追加分析时，看 `SIGNAL_XTIME_ANALYSIS.md`。
3. 需要理解启动前生成 taker buffer / freshness threshold 后的 shadow 闭环验证时，看 `PREFLIGHT_SHADOW.md`。
4. 需要逐笔复核时，看 `analysis/ack_full_candidates.csv`、`analysis/cancel_point_candidates.csv` 和 `analysis/preflight_shadow_orders.csv`。
5. 需要理解 CSV 字段时，看 `FIELD_SCHEMA.md`。
6. 需要追溯原始来源时，看 `context/source_paths.txt` 和 `logs/extraction_notes.md`。
7. 需要验证归档完整性时，在本目录执行 `sha256sum -c checksums.sha256`。

## 文件说明

- `FIELD_SCHEMA.md`：本归档内 CSV、JSON summary、BookTicker bin 和日志的字段级说明。
- `SIGNAL_XTIME_ANALYSIS.md`：signal 后 raw/order price 可成交窗口、Gate `x_in_time` / `x_out_time` 假设分析、open 成交订单延迟对比和未成交原因判断。
- `PREFLIGHT_SHADOW.md`：启动前生成固定 taker buffer / freshness threshold 后，结合订单、signal 和 Gate canonical BBO 的 shadow 闭环验证。
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
- `preflight_shadow_params.json`：从本 report 可用数据生成的 taker buffer / freshness shadow 参数和两种 freshness proxy 统计。
- `preflight_shadow_quote_latency.toml`：quote latency proxy TOML patch；用于记录接近启动前 raw BookTicker latency 的生成口径。
- `preflight_shadow_runtime_freshness.toml`：runtime signal freshness proxy TOML patch；用于记录当前策略实际比较字段的生成口径。
- `preflight_shadow_orders.csv`：`93` 笔 open no-fill cancel 和 `8` 笔 open filled control 的订单级 shadow 闭环表。
- `preflight_shadow_sensitivity.csv`：p50/p95/p99/p100 taker buffer 对 BBO any 可成交性的 sensitivity 汇总。
- `preflight_shadow_summary.json`：preflight shadow 闭环验证的机器可读汇总。

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

追加的 preflight shadow 闭环验证显示：

| 样本 | price | signal any | x_in any | x_out any |
|---|---|---:|---:|---:|
| cancel `93` | raw price | `62/93` | `46/93` | `34/93` |
| cancel `93` | order price | `93/93` | `68/93` | `50/93` |
| cancel `93` | p100 reference price | `93/93` | `93/93` | `93/93` |
| filled control `8` | order price | `8/8` | `8/8` | `8/8` |

Gate canonical BBO spread proxy 的 p50/p95/p99/p100 分别为 `4.67/10.56/15.03/97.91 bps`。本轮显式使用的 p100 buffer 能让 reference price 在 cancel 样本上 `x_in/x_out` 都保持 `93/93` any 可成交，但 median offset 约 `348 ticks`，明显只适合作为上限诊断，不适合直接 enforce。

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

preflight shadow 追加结论：

- taker buffer 对 BBO any 可成交性有解释力，但 p100 生成值约 `98 bps`，过于激进；下一轮更适合用 p95/p99 做 shadow 对比，并同时评估成交质量和 overpay。
- freshness auto 暂时没有解释力。quote latency proxy 会 block `43/93` cancel 和 `5/8` filled control；runtime signal freshness proxy 则 block `0/93` cancel 和 `0/8` filled control。
- 当前 freshness 还有口径问题：启动前生成通常基于 `quote.local_ns - quote.exchange_ns`，而策略 `freshness_shadow` 实际比较 `signal_decision_ns - quote.exchange_ns`。在进入 enforce 前必须统一生成口径和策略比较口径。

## 验证

本归档生成后应满足：

- `inputs/orders.csv` 为 `113` 行。
- `analysis/cancel_windows.csv` 为 `93` 行。
- `analysis/ack_full_candidates.csv` 为 `5` 行。
- `analysis/cancel_point_candidates.csv` 为 `3` 行。
- `analysis/preflight_shadow_orders.csv` 为 `101` 行。
- `analysis/preflight_shadow_sensitivity.csv` 为 `8` 行。
- `zstd -t market_data/*.zst` 通过。
- `sha256sum -c checksums.sha256` 通过。
