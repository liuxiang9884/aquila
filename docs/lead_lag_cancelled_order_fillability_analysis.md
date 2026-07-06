# LeadLag Cancelled Order Fillability 分析方案

## 目的

本文定义如何结合 LeadLag 实盘订单日志和 recorder 落地的 typed BookTicker binary v1 数据，复查某个开仓 IOC order 为什么没有成交。目标是回答两个问题：

1. 从策略实际看到的 lag fusion BBO 看，该 order 在 signal、Ack、cancel feedback 之间是否曾经达到可成交价格。
2. 如果 BBO 视角显示可成交但最终仍 `kCancelled`，下一步应从 fusion/source 差异、Gate Ack / lifecycle tail、数量和撮合边界继续定位。

本文只定义分析方案和字段口径，不把 public / private BBO 结果解释为撮合引擎的确定事实。BBO 不是 order book depth，也不是 matching engine 队列。

## 输入

### 订单输入

优先使用正式 report 目录里的 `order_detail.csv`：

```text
reports/<run_id>/order_detail.csv
```

如果 run 仍在运行，可以先对当前 strategy log 生成快照 report：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/generate_live_report.py \
  --run-id <run_id> \
  --log /home/liuxiang/tmp/<run_id>/logs/<lead_lag_strategy_log>.log \
  --config /home/liuxiang/tmp/<run_id>/configs/<lead_lag_pair_config>.toml \
  --instrument-catalog config/instruments/usdt_futures_common_gate_binance_20260602.csv \
  --guard-stdout /home/liuxiang/tmp/<run_id>/guarded_live.stdout.log \
  --output-root /home/liuxiang/tmp/<run_id>/status_reports_<snapshot_ts>
```

需要保留的订单字段：

| 字段 | 用途 |
| --- | --- |
| `local_order_id` | 本地订单主键，用于关联日志和 report。 |
| `symbol` / `symbol_id` | 定位 symbol bin。 |
| `order_role` / `reduce_only` | 筛选开仓单。 |
| `side` | 判断买单看 ask，卖单看 bid。 |
| `quantity` / `quantity_text` | 检查 BBO 数量或后续 depth 边界时使用。 |
| `raw_price` | signal 时 lag 对手价，买单为 lag ask，卖单为 lag bid。 |
| `order_price` / `price_text` | 实际 IOC limit 价格；fillability 判断使用该价格。 |
| `status` / `cumulative_filled_quantity` / `cancelled_quantity` | 区分完全未成交、partial cancel 和 filled。 |
| `signal_lead_id` / `signal_lag_id` | signal 触发时策略看到的最新 lead / lag BBO id。 |
| `ack_lead_id` / `ack_lag_id` | 处理 Gate Ack response 时策略看到的最新 lead / lag BBO id。 |
| `accepted_lead_id` / `accepted_lag_id` | 处理 Gate accepted/result response 时策略看到的最新 lead / lag BBO id。 |
| `cancelled_lead_id` / `cancelled_lag_id` | 处理 cancel feedback 时策略看到的最新 lead / lag BBO id。当前字段名使用英式 `cancelled`。 |
| `filled_lead_id` / `filled_lag_id` | 处理 fill feedback 时策略看到的最新 lead / lag BBO id，用于成交样本对照。 |
| `request_send_local_ns` / `ack_local_receive_ns` / `order_finished_local_ns` | 本地时间区间，用于辅助定位发送、Ack、终态闭环。 |
| `ack_exchange_request_ingress_ns` / `ack_exchange_response_egress_ns` / `finish_exchange_ns` | Gate response / feedback 时间戳，用于辅助判断交易所侧 lifecycle。 |

### 行情输入

策略使用 fusion 行情时，lag Gate canonical recorder 是首选输入：

```text
/home/liuxiang/tmp/<run_id>/bin/gate_live_fusion_canonical.bin
```

如果本轮同时录了 Gate source recorder，后续可以把 source0-source3 作为对照：

```text
/home/liuxiang/tmp/<run_id>/bin/gate_live_fusion_source0.bin
/home/liuxiang/tmp/<run_id>/bin/gate_live_fusion_source1.bin
/home/liuxiang/tmp/<run_id>/bin/gate_live_fusion_source2.bin
/home/liuxiang/tmp/<run_id>/bin/gate_live_fusion_source3.bin
```

对于多个连续 recorder bin，可以向拆分脚本重复传 `--input`。输入和输出都使用 typed BookTicker binary v1；拆分后的每个
`symbol.bin` 也包含一个 16-byte header。多个输入写入同一个输出时，只在输出文件开头写一个 header，后续追加的是同一
symbol 的 payload records，不按时间再拆。

## 拆分 symbol 行情

使用 `scripts/market_data/split_book_ticker_by_symbol.py`：

```bash
scripts/market_data/split_book_ticker_by_symbol.py \
  --input /home/liuxiang/tmp/<run_id>/bin/gate_live_fusion_canonical.bin \
  --instrument-catalog config/instruments/usdt_futures_common_gate_binance_20260602.csv \
  --run-id <run_id> \
  --output-root /home/liuxiang/tmp/book_ticker_symbol_splits \
  --symbol <SYMBOL> \
  --json-output /home/liuxiang/tmp/book_ticker_symbol_splits/<run_id>_<SYMBOL>_summary.json
```

输出示例：

```text
/home/liuxiang/tmp/book_ticker_symbol_splits/<run_id>/<SYMBOL>.bin
```

对 live-growing bin 做快照分析时，先复制 recorder bin 到 run snapshot 目录再拆。直接读取仍在增长的 typed binary 可能因为尾部 payload record 未完整写入而失败，不是推荐路径。

## 目标订单集合

第一阶段只分析完全未成交的开仓 cancel：

```text
order_role == "entry"
reduce_only == "false"
status == "kCancelled"
cumulative_filled_quantity == 0
```

暂时排除：

- `kRejected`：没有进入同一类 IOC 可成交性判断。
- `kFilled`：作为成交对照样本单独分析。
- `kPartiallyCancelled` 或 `cumulative_filled_quantity > 0` 的 cancel：需要同时分析已成交数量和剩余数量，不和完全未成交 cancel 混在一起。

## BBO 对齐口径

对每个 cancelled order，从该 symbol 的 lag fusion `symbol.bin` 中定位以下 BookTicker record：

| 订单字段 | 行情含义 |
| --- | --- |
| `signal_lag_id` | signal 触发时策略看到的 lag BBO。 |
| `ack_lag_id` | 策略处理 Gate Ack 时看到的 lag BBO。 |
| `accepted_lag_id` | 策略处理 Gate accepted/result response 时看到的 lag BBO。 |
| `cancelled_lag_id` | 策略处理 cancel feedback 时看到的 lag BBO。 |

主分析区间：

```text
[signal_lag_id, cancelled_lag_id]
```

更严格的 live-order 区间：

```text
[ack_lag_id, cancelled_lag_id]
```

如果 `accepted_lag_id` 存在，也记录：

```text
[accepted_lag_id, cancelled_lag_id]
```

`BookTicker.id` 是交易所 update id，不要求连续。查找时应按 id 比较区间包含关系，而不是假设每个 id 都存在。若某个 stage id 在 symbol bin 中找不到，需要输出 `missing_bbo_id`，再检查 recorder 是否覆盖该时间段、symbol 是否拆错、是否用了错误 exchange/source 文件。

## 可成交性判定

以 `order_price` 为实际 IOC limit 价格。

买单：

```text
side == "kBuy"
如果区间内存在 ask_price <= order_price，则 BBO 视角下曾经可成交。
```

卖单：

```text
side == "kSell"
如果区间内存在 bid_price >= order_price，则 BBO 视角下曾经可成交。
```

需要分别计算：

- `would_cross_signal_to_cancel`：`signal_lag_id -> cancelled_lag_id` 是否触达。
- `would_cross_ack_to_cancel`：`ack_lag_id -> cancelled_lag_id` 是否触达。
- `would_cross_accepted_to_cancel`：`accepted_lag_id -> cancelled_lag_id` 是否触达，若没有 `accepted_lag_id` 则为空。
- `first_cross_lag_id` / `first_cross_exchange_ns` / `first_cross_local_ns`：第一次触达的 lag BBO。
- `best_bid_between` / `best_ask_between`：区间内最优 BBO。

辅助判断：

- 如果 signal 到 cancel 触达，但 ack 到 cancel 未触达，说明 signal 时机会已经消失，下单或 Gate Ack 前后延迟可能是关键。
- 如果 ack 到 cancel 触达，但仍 cancel，优先检查 Gate source recorder、fusion 选择、BBO quantity、Gate exchange lifecycle 和 matching/BBO 边界。
- 如果 accepted 到 cancel 触达但仍 cancel，是最高优先级复查样本。

## 建议输出 CSV

建议后续自动化脚本输出一行一个 order：

| 字段 | 说明 |
| --- | --- |
| `run_id` | run id。 |
| `local_order_id` | 本地订单 id。 |
| `symbol` / `symbol_id` | symbol 信息。 |
| `side` | `kBuy` / `kSell`。 |
| `quantity` | 下单数量。 |
| `raw_price` | signal 时 lag 对手价。 |
| `order_price` | 实际 IOC limit 价格。 |
| `status` | 订单终态。 |
| `signal_lag_id` / `ack_lag_id` / `accepted_lag_id` / `cancelled_lag_id` | 阶段 lag BBO id。 |
| `signal_lag_bid` / `signal_lag_ask` | signal 时 lag BBO。 |
| `ack_lag_bid` / `ack_lag_ask` | Ack 时 lag BBO。 |
| `accepted_lag_bid` / `accepted_lag_ask` | accepted/result 时 lag BBO。 |
| `cancelled_lag_bid` / `cancelled_lag_ask` | cancel feedback 时 lag BBO。 |
| `best_bid_signal_to_cancel` / `best_ask_signal_to_cancel` | signal 到 cancel 区间最优 BBO。 |
| `best_bid_ack_to_cancel` / `best_ask_ack_to_cancel` | Ack 到 cancel 区间最优 BBO。 |
| `would_cross_signal_to_cancel` | signal 到 cancel 是否触达 order price。 |
| `would_cross_ack_to_cancel` | Ack 到 cancel 是否触达 order price。 |
| `would_cross_accepted_to_cancel` | accepted 到 cancel 是否触达 order price。 |
| `first_cross_lag_id` / `first_cross_exchange_ns` / `first_cross_local_ns` | 第一次触达记录。 |
| `records_signal_to_cancel` / `records_ack_to_cancel` | 区间内 BBO record 数量。 |
| `reason` | 分类结论。 |

## 分类结论

建议 `reason` 先使用以下枚举：

| reason | 含义 |
| --- | --- |
| `no_cross` | signal 到 cancel 区间内 BBO 从未触达 `order_price`，BBO 视角下确实难成交。 |
| `cross_before_ack_only` | signal 到 Ack 前触达过，但 Ack 到 cancel 未触达；机会可能在订单到达前消失。 |
| `cross_after_ack_before_cancel` | Ack 到 cancel 之间触达过；需要进一步查 Gate / fusion / quantity / matching 边界。 |
| `cross_after_accepted_before_cancel` | accepted/result 到 cancel 之间触达过；最高优先级复查。 |
| `missing_signal_bbo` | 找不到 `signal_lag_id` 对应 BBO。 |
| `missing_ack_bbo` | 找不到 `ack_lag_id` 对应 BBO。 |
| `missing_cancelled_bbo` | 找不到 `cancelled_lag_id` 对应 BBO。 |
| `invalid_id_order` | stage id 顺序不满足 `signal <= ack <= cancelled` 等基本关系；需先复查日志或 symbol bin。 |
| `not_cancelled_entry` | 输入不是完全未成交的开仓 cancel，跳过。 |

## Source / Fusion 对照

如果 canonical fusion 显示没有可成交机会，可以再查 source0-source3：

1. 对 4 个 source bin 分别拆出同一个 `symbol.bin`。
2. 用同样的 stage id 或本地时间窗口定位 source BBO。
3. 判断是否存在某个 source 在 Ack 到 cancel 区间触达 `order_price`，但 canonical fusion 没有输出。

对照结论：

- `source_cross_canonical_no_cross`：可能是 fusion first-arrival、source update id、canonical 输出时序或 symbol 对齐需要复查。
- `canonical_cross_source_no_cross`：通常需要检查是否使用了同一 exchange/symbol 文件，或 source id 与 canonical id 语义是否一致。
- `all_no_cross`：更支持“订单价格在可观察 BBO 上确实没有成交机会”。

## 解释边界

该分析的结论应写成“BBO 视角下是否可成交”，不要写成“交易所一定应该成交”。主要边界：

- BookTicker 是 BBO，不包含完整 depth 和队列。
- BBO quantity 如果不可用或不足，无法证明大数量 IOC 能完全成交。
- Gate feedback 的 `exchange_update_ns`、Ack timestamp 和 BookTicker timestamp 字段语义不同，不能简单混成单程网络延迟。
- strategy 看到的是 fusion canonical；matching engine 实际状态可能与 public/private BBO 有细微差异。
- live-growing recorder 文件可能在读取时继续增长；需要先 snapshot，再对快照文件拆分。

## 后续自动化入口建议

后续可以新增脚本：

```text
scripts/lead_lag/analyze_cancelled_order_fillability.py
```

建议参数：

```bash
scripts/lead_lag/analyze_cancelled_order_fillability.py \
  --order-detail reports/<run_id>/order_detail.csv \
  --lag-book-ticker-bin /home/liuxiang/tmp/book_ticker_symbol_splits/<run_id>/<SYMBOL>.bin \
  --symbol <SYMBOL> \
  --output /home/liuxiang/tmp/<run_id>/fillability/<SYMBOL>_cancelled_orders.csv
```

如果要同时做 source 对照，可追加：

```bash
  --source-book-ticker-bin source0=/path/<SYMBOL>.bin \
  --source-book-ticker-bin source1=/path/<SYMBOL>.bin \
  --source-book-ticker-bin source2=/path/<SYMBOL>.bin \
  --source-book-ticker-bin source3=/path/<SYMBOL>.bin
```

脚本实现前，手工分析也应遵守本文的字段名、区间和 `reason` 分类，避免每次复查使用不同口径。
