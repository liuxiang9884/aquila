# BookTicker Fusion Metadata Policy 设计

## 背景

当前 `BookTickerFusionRunner` 在每条 winner canonical `BookTicker` 发布后，总是构造
`FusionMetadataRecord` 并写入 `fusion_metadata.bin`。这对 shadow attribution 和离线
latency 分析有用，但在准备接策略或做低开销运行时，文件写入、metadata record 构造和
metadata write error 统计不应留在 hot path。

## 目标

新增编译期 metadata policy，使构建时可以决定是否启用 fusion sidecar metadata：

- metadata enabled：保持当前行为，写 `fusion_metadata.bin`，维护 metadata write error 统计。
- metadata disabled：不打开 metadata 文件，不构造 `FusionMetadataRecord`，不维护 metadata
  write error 统计；仍保留基础运行统计 `total_read_count` 和 `total_published_count`。

## 非目标

- 不改变 fusion first-arrival 语义：仍按 `(symbol_id, BookTicker.id)` 单调推进。
- 不改变 canonical `BookTicker` SHM ABI。
- 不新增 runtime TOML 开关；本次要求是编译期决定。
- 不记录 duplicate / stale / invalid symbol 的额外明细。

## 编译期开关

新增 CMake cache 变量：

```text
AQUILA_BOOK_TICKER_FUSION_METADATA_MODE=file|off
```

默认值为 `file`，保持现有 shadow 和 analyzer 工作流兼容。低开销构建可显式设置：

```bash
cmake -DAQUILA_BOOK_TICKER_FUSION_METADATA_MODE=off ...
```

`file` 表示启用 sidecar metadata；`off` 表示编译期关闭 metadata。

## 配置行为

`file` 模式：

- `fusion.output.metadata_bin` 仍然必填。
- runner 构造时打开 `FusionMetadataWriter`。

`off` 模式：

- `fusion.output.metadata_bin` 不再必填。
- 如果配置里仍保留 `metadata_bin`，parser 接受但 runner 忽略。
- dry-run / summary 日志应明确输出 `metadata_enabled=false`，避免误以为写了 sidecar 文件。

## 代码结构

新增编译期模式头文件：

```text
core/common/book_ticker_fusion_metadata_mode.h
```

新增 policy 头文件：

```text
core/market_data/book_ticker_fusion_metadata_policy.h
```

policy 提供两个实现：

- `FileBookTickerFusionMetadataPolicy`：持有 `FusionMetadataWriter`，写 `FusionMetadataRecord`。
- `NoopBookTickerFusionMetadataPolicy`：不持有 writer，`Write(...)` 编译为空操作，`Flush()` 返回
  true，metadata write error count 固定为 0。

`BookTickerFusionRunner` 使用默认 policy alias。metadata disabled build 中，publish 分支只更新时间、
发布 canonical SHM 和基础计数，不构造 sidecar record。

## 日志和统计

保留：

- `total_read_count`
- `total_published_count`
- `flush_ok`
- `ok`
- `error`

metadata enabled 时保留：

- `total_metadata_write_errors`
- `metadata_output`

metadata disabled 时：

- 不维护 metadata write error 统计，外部展示为 0 或省略。
- 日志包含 `metadata_enabled=false`。

## 验证

至少运行：

```bash
cmake --build build/debug --target core_market_data_book_ticker_fusion_runner_test -j8
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_runner_test
cmake --build build/debug --target core_market_data_book_ticker_fusion_thread_test -j8
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_thread_test
cmake --build build/debug --target book_ticker_fusion_config_test -j8
./build/debug/test/config/book_ticker_fusion_config_test
```

并额外用 `AQUILA_BOOK_TICKER_FUSION_METADATA_MODE=off` 构建相关目标，验证：

- `metadata_bin` 可省略。
- canonical SHM 仍发布。
- 不生成 metadata 文件。
- metadata write error 统计为 0。
