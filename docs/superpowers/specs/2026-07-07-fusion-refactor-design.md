# Market Data Fusion 去重设计

## 目标

整理 `core/market_data` 中 BookTicker / Trade fastest-route fusion 的重复代码，在不改变对外配置、
CLI、运行语义和主路径性能的前提下，把公共实现收敛到一层浅模板和 feed traits。

本轮同时把 BookTicker / Trade 的 fusion metadata 统一为一个 ABI v2 record，使后续分析脚本和报告字段
使用一致的 `record_id` / `event_ns` 命名。

## 当前事实

- `book_ticker_fusion_config.h` 和 `trade_fusion_config.h` 的 source/output config 字段基本一致。
- `book_ticker_fusion.h` 和 `trade_fusion.h` 都围绕 `BasicFastestRouteFusionCore` 做薄封装，区别主要是
  record 类型和字段名。
- `book_ticker_fusion_metadata_policy.h` 和 `trade_fusion_metadata_policy.h` 的写入流程一致，只是 metadata
  record 字段提取不同。
- `book_ticker_fusion_runner.h` 和 `trade_fusion_runner.h` 的 runner traits 结构一致，区别是 reader 类型、
  SHM config 类型、publish callback 和 metadata record 类型。
- `book_ticker_fusion_thread.h` 和 `trade_fusion_thread.h` 已经只是 `BasicFastestRouteFusionThread` 的别名。
- 当前 BookTicker metadata record 是 40 bytes，Trade metadata record 是 48 bytes；字段名分别使用
  `book_ticker_id` 和 `trade_id`。
- `scripts/market_data/analyze_book_ticker_fusion_latency.py` 直接声明了 BookTicker fusion metadata dtype，
  因此 metadata ABI v2 需要同步更新脚本和测试。

## Locked Decisions

- 这是第一阶段 `core/market_data` fusion 去重，不重构 `core/config/*_fusion_config.cpp` 和
  `tools/market_data/data_fusion_tool_support.h`。
- 保留现有外部类型名、header、TOML schema、CLI binary name 和工具使用方式。
- 保留 `fastest_route_fusion_runner.h` 的 polling、drop、publish、metadata flush 语义。
- 只引入一层浅模板；BookTicker / Trade 继续暴露 feed-specific 薄封装，便于错误定位和调试。
- metadata ABI 可以升级到 v2，并同步修改 C++ tests、Python analyzer dtype 和相关文档。
- 不在热路径中引入虚函数、动态分配、运行时类型分发或额外日志格式化。
- 性能验收必须基于同一机器、同一 release build、同一命令的改前/改后 benchmark 或最小压测证据；没有证据
  不宣称性能无损。

## Scope

本轮包含：

- 提取公共 fusion config struct，并让 BookTicker / Trade config 类型继续以原名称可用。
- 提取公共 fastest-route fusion facade，使 BookTicker / Trade core 只保留 feed traits 和薄类型别名。
- 提取公共 metadata record / writer / policy，保留 `BookTickerFusionMetadata*` 和
  `TradeFusionMetadata*` 对外入口。
- 提取公共 runner traits helper，BookTicker / Trade 只声明 record、reader、SHM config 和 publish 方法。
- 更新 metadata ABI v2 的 C++ 单元测试、Python analyzer dtype、脚本测试和相关文档。
- 新增至少一个共享层测试，直接覆盖通用 metadata builder 或通用 decision/traits 行为。

本轮不包含：

- 重构 config parser、CLI 参数解析和工具层通用化。
- 改变 source selection、latest-visible 读取、runner 退出条件、publish 条件或 drop 条件。
- 改变 `BookTicker` / `Trade` record ABI。
- 改变 live strategy、report 统计口径或 PnL 逻辑。
- 宣称 fusion 对订单 fillability 或 PnL 有收益。

## Metadata ABI v2

统一 record：

```cpp
struct FusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t record_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t event_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};
```

字段语义：

- `source_id`：winning source index。
- `symbol_id`：record symbol id。
- `record_id`：BookTicker 使用 `BookTicker::id`，Trade 使用 `Trade::id`。
- `exchange_ns`：交易所事件时间。
- `event_ns`：BookTicker 使用 `exchange_ns`；Trade 使用 `trade_ns`。
- `source_local_ns`：winning source 原始 `local_ns`。
- `fusion_publish_ns`：fusion 发布时刻。

兼容入口：

- `book_ticker_fusion_metadata.h` 继续导出 `FusionMetadataRecord` 和 BookTicker metadata writer。
- `trade_fusion_metadata.h` 继续导出 `TradeFusionMetadataRecord`，但实现为 `FusionMetadataRecord` 的别名。
- Python analyzer 从旧字段 `book_ticker_id` 迁移到 `record_id`，并新增 `event_ns`。

ABI 影响：

- Trade metadata record 维持 48 bytes。
- BookTicker metadata record 从 40 bytes 变为 48 bytes。
- 旧 metadata binary 与新脚本不兼容；相关文档必须说明字段和 record size。

## Core 结构

新增或调整公共层，命名以实现阶段贴合现有文件组织为准：

```text
BasicFusionSourceConfig
BasicFusionOutputConfig
BasicFusionConfig

BasicFusionDecision
BasicFusionCore<FeedTraits>

FusionMetadataRecord
BasicFusionMetadataPolicy<FeedTraits, Config>

BasicFusionRunnerTraits<FeedTraits>
```

BookTicker traits 提供：

- `Record = BookTicker`
- `Reader = BookTickerShmReader`
- `ShmConfig = BookTickerShmReaderConfig`
- `RecordId(record) = record.id`
- `EventNs(record) = record.exchange_ns`
- `Publish(publisher, record) = publisher.PublishBookTicker(record)`

Trade traits 提供：

- `Record = Trade`
- `Reader = TradeShmReader`
- `ShmConfig = TradeShmReaderConfig`
- `RecordId(record) = record.id`
- `EventNs(record) = record.trade_ns`
- `Publish(publisher, record) = publisher.PublishTrade(record)`

BookTicker / Trade 头文件只负责：

- 暴露原有 public type names。
- 绑定对应 feed traits。
- 保留现有 include 路径兼容性。
- 在必要处提供简短英文注释说明 alias 关系。

## Runner 行为

runner 内层循环不做语义修改：

```text
poll source reader
  if no record: continue
  ask fusion core / decision
  if decision says publish:
    preserve source_local_ns
    overwrite record.local_ns with fusion_publish_ns
    publish record to fusion output
    optionally write metadata
```

允许的实现变化：

- 把 feed-specific `MakeMetadataRecord()` 收进 traits 或 policy。
- 把 feed-specific publish callback 收进 traits。
- 把 duplicated runner trait type aliases 收进公共 helper。

不允许的变化：

- 不改变 source scan 顺序。
- 不改变 tie-break、timestamp comparison 和 stale/drop 逻辑。
- 不改变 metadata disabled 时的分支条件数量级。
- 不新增 heap allocation、mutex、condition variable、virtual call 或 type-erased callback。

## 性能验收

性能无损作为交付条件，而不是代码注释。实现完成前必须建立当前 `main` 的 baseline，再用相同环境跑改后版本。

最低验收：

- release build 下运行现有 fastest-route fusion 相关 benchmark 或最接近的 benchmark target。
- BookTicker 和 Trade 都要覆盖；至少覆盖 core decision 路径和 runner publish 路径。
- metadata disabled 和 metadata enabled 两类场景都要覆盖；BookTicker metadata v2 多 8 bytes，需要单独观察。
- 同一命令重复运行，避免单次噪声误判。
- 任一热路径 benchmark 出现可复现退化时，必须先优化或调整设计，不能把实现标记为完成。

判断口径：

- 不以 debug build、单次 wall-clock 或脚本启动耗时作为性能结论。
- 不把 trade fusion live smoke 的行情 latency 直接解释成订单 fillability 或 PnL 收益。
- 如果 benchmark 噪声大，需要报告原始结果和不确定性，而不是宣称性能无损。

## 测试和验证

C++ 单元测试至少覆盖：

- `book_ticker_fusion_metadata_test`
- `trade_fusion_metadata_test`
- `book_ticker_fusion_runner_test`
- `trade_fusion_runner_test`
- `book_ticker_fusion_thread_test`
- `trade_fusion_thread_test`
- `book_ticker_fusion_config_test`
- `trade_fusion_config_test`
- 新增共享层 metadata / traits 测试

脚本测试至少覆盖：

- `scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py`

文档同步至少覆盖：

- `docs/diagnostic_fields.md`
- `docs/gate_fastest_route_fusion_design.md`
- `docs/trade_fastest_route_fusion_design.md`
- 如字段说明集中在 CSV schema 或 handoff 文档，也同步更新引用。

收尾验证：

```bash
git diff --check
ctest --test-dir build/debug -R '(book_ticker_fusion|trade_fusion|fusion_config)' --output-on-failure
python3 scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
```

性能验证命令在实现阶段按现有 benchmark target 名称确认后写入实现计划；所有临时 benchmark 输出写入
`/home/liuxiang/tmp`。
