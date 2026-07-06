# Gate / Binance Trade 多路最快行情融合设计

本文记录 Trade fastest-route fusion 的当前设计结论。目标是把 Gate / Binance 多路 `Trade`
source 统一融合成一条 canonical `Trade` SHM，供后续 reader、recorder、replay 和策略链路消费。

该方案沿用现有 BBO fastest-route fusion 的低延迟模型：不等待、不回看、不补洞、不比较 payload，
只按 fusion runner 实际读到的 first arrival 推进 canonical stream。

## 已确认结论

- Trade fusion V1 的 identity 是 `(symbol_id, Trade.id)`。
- `batch_index` / `batch_count` 只作为 `Trade` payload 和诊断字段保留，不参与去重。
- 每个 symbol 只发布严格递增的 `Trade.id`；`id <= last_published_id` 的样本直接丢弃。
- 输出到 canonical SHM 的 `Trade.local_ns` 改写为 `fusion_publish_ns`，和现有 fused `BookTicker`
  语义一致。
- 原始 source `local_ns` 通过 sidecar metadata 保留，用于离线分析 source ingress latency 和 fusion hop。
- V1 不按 `exchange_ns` / `trade_ns` / `source_local_ns` 做跨 source 时间排序，不引入等待窗口或 watermark。
- data fusion bundle 启动 source data session 时只启用 `feed.trade = true`，并强制关闭
  `feed.book_ticker = false`。

## 泛化边界

实现上采用内部 C++ 泛化，但外部仍保持 feed-specific facade：

- 内部新增 shared fastest-route fusion core / runner / thread 模板或 traits 层。
- 现有 `BookTickerFusion*` 对外类型、TOML schema、CLI 和 config 文件保持兼容。
- 新增 `TradeFusion*` 对外类型、TOML schema、CLI 和 config 文件。
- 不把外部配置立即合并成 `feed = "book_ticker" | "trade"` 的统一 schema。

这样可以复用核心算法和 runner 结构，同时避免迁移已有 BBO 实盘入口和分析脚本。

## 主算法

Trade fusion 的 hot path 和 BBO fusion 等价：

```text
state[symbol_id]:
  last_published_id
  last_published_source

on Trade(trade, source_id):
  if trade.id > state.last_published_id:
      fusion_publish_ns = realtime clock now
      metadata.source_local_ns = trade.local_ns
      trade.local_ns = fusion_publish_ns
      publish trade immediately to canonical Trade SHM
      state.last_published_id = trade.id
      state.last_published_source = source_id
      write one sidecar metadata record
  else:
      drop
```

该 stream 是 fusion 观察到的最快可发布 trade stream，不保证连续，也不尝试补齐被更快 source 推进后
落后的 source 样本。

## Metadata 格式

外部 sidecar metadata 保持 feed-specific：

- 现有 BBO `FusionMetadataRecord` raw binary ABI 不变。
- Trade 新增独立 `TradeFusionMetadataRecord` raw binary ABI。
- 内部 writer / policy 可以泛化，但不迁移现有 BBO metadata 到 recorder typed-header 格式。

Trade metadata 至少记录：

```cpp
struct TradeFusionMetadataRecord {
  std::int32_t source_id;
  std::int32_t symbol_id;
  std::int64_t trade_id;
  std::int64_t exchange_ns;
  std::int64_t trade_ns;
  std::int64_t source_local_ns;
  std::int64_t fusion_publish_ns;
};
```

`batch_index` / `batch_count` 不作为 V1 metadata 必填字段；如果 replay 或 live smoke 证明 batch 语义需要
额外归因，再基于证据扩展 metadata ABI 和分析脚本。

## 配置方向

Trade fusion 使用与 BBO fusion 平行的 TOML：

```toml
[fusion]
name = "gate_trade_fusion_4sources"
max_events_per_source = 1
bind_cpu_id = 16
max_symbol_id = 4096

[fusion.output]
shm_name = "aquila_gate_trade_fusion"
channel_name = "trade_channel"
remove_existing = true
metadata_bin = "/home/liuxiang/tmp/gate_trade_fusion/metadata.bin"

[[fusion.sources]]
source_id = 0
name = "gate_trade_source0"
shm_name = "aquila_gate_trade_source0"
channel_name = "trade_channel"
```

Data fusion bundle 配置也保持 exchange-specific，并新增 trade source SHM 字段或 trade-specific source
config。启动 source data session 时覆盖：

```text
feeds.trade = true
feeds.book_ticker = false
trade_shm.enabled = true
trade_shm.shm_name = launch source trade SHM
trade_shm.channel_name = launch source trade channel
```

## 验证边界

实现完成后至少覆盖：

- generic core 对 `BookTicker` 既有行为的回归测试。
- `TradeFusionCore` 对 first-arrival、stale drop、invalid symbol 和 per-symbol 独立推进的测试。
- `TradeFusionRunner` 从多路 `TradeShmReader` 读取、写 canonical `TradeShmChannel`、改写 `local_ns`
  和写 metadata 的测试。
- `TradeFusionThread` stop / flush / stats 测试。
- Gate / Binance trade fusion config parser 和 data fusion dry-run 测试。
- `ctest` 中现有 BBO fusion 测试必须继续通过。

性能或延迟收益不能只凭设计宣称；Trade fusion 跑通后需要用 replay、dry-run、live smoke 或 benchmark
给出实际证据。
