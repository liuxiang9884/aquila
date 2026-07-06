# Recorder Typed Binary Header 设计

## 目标

把 `data_reader_recorder`、`HistoricalDataReader` 和 Python 行情分析脚本从裸 `BookTicker` / `Trade`
结构体流迁移到自描述 typed binary 格式。新文件必须在文件头里声明 magic、version、feed/type 和 record
ABI size，使 reader 能拒绝把 `BookTicker` 文件当 `Trade` 读、ABI size 不匹配或格式不匹配的输入。

## 当前事实

- recorder 单文件模式和 rotation segment 当前都写裸结构体流，不写 header。
- `BookTicker` 和 `Trade` 当前都是 64 bytes，仅靠 `file_size % sizeof(T)` 不能识别 feed 配错。
- rotation manifest 当前只是 JSONL segment 索引；`manifest_to_data_reader_config.py` 只读取 `file` 字段生成
  `files = [...]`。
- `HistoricalDataReader` 当前按 TOML `feed` 选择 record size 和 handler dispatch。
- 多个 Python 脚本直接用 `np.fromfile`、`np.memmap`、`np.frombuffer` 或 `tofile` 读写裸 `BookTicker`：
  - `scripts/market_data/analyze_book_ticker_latency.py`
  - `scripts/market_data/analyze_book_ticker_fusion_latency.py`
  - `scripts/market_data/compare_fusion_tardis_book_ticker.py`
  - `scripts/market_data/split_book_ticker_by_symbol.py`
  - `scripts/hdf_book_ticker_to_binary.py`
  - `scripts/lead_lag/generate_preflight_config_params.py`

## Locked Decisions

- 文件必须自描述；每个 recorder binary 文件写 typed header。
- manifest 继续作为 rotation segment 索引，可以增加 typed-header metadata 用于脚本预检，但不是事实源。
- recorder 新输出默认直接写 typed-header format v1。
- 不兼容 legacy raw：C++ reader、probe、LeadLag replay 输入和 Python 脚本都迁到 typed header 格式；旧裸文件需要重录。
- header 使用最小固定布局，不包含 `record_count`、`payload_bytes` 或 header CRC。
- realtime 和 historical reader 继续共用 `DataReaderConfig` TOML schema，但按 `source.type` 做不同字段校验。
- historical 读取时 TOML `feed` 必须和文件 header `feed/type` 一致，否则 fail fast。

## File Header ABI

第一版使用 16-byte little-endian header：

```cpp
struct MarketDataBinaryHeader {
  std::uint32_t magic;       // bytes "AQMD"
  std::uint16_t version;     // 1
  std::uint16_t header_size; // sizeof(MarketDataBinaryHeader) == 16
  std::uint16_t feed_type;   // 1 = book_ticker, 2 = trade
  std::uint16_t record_size; // sizeof(BookTicker) or sizeof(Trade)
  std::uint32_t flags;       // 0 for v1
};
```

规则：

- `magic` 的文件字节固定为 `41 51 4d 44`（ASCII `AQMD`）。
- 所有整数按 little-endian 编码。
- `version = 1`。
- `header_size = 16`。
- `flags = 0`；reader 看到未知 flags 必须 fail fast。
- `feed_type = 1` 对应 `feed = "book_ticker"`，`record_size` 必须等于 `sizeof(aquila::BookTicker)`。
- `feed_type = 2` 对应 `feed = "trade"`，`record_size` 必须等于 `sizeof(aquila::Trade)`。
- 文件大小必须满足 `file_size >= header_size` 且 `(file_size - header_size) % record_size == 0`。
- header-only 文件合法，表示 0 records；0-byte 文件非法。

## Recorder 写入

单文件模式：

- `--mode truncate`：创建/截断文件后先写 header，再写 records。
- `--mode append`：如果文件不存在或大小为 0，先写 header；如果文件已存在，必须先读取并校验 header 与当前 feed/type、version、record_size 完全匹配，再 append records。
- append 到旧裸文件必须失败，不做 raw 自动迁移。

rotation 模式：

- 每个 `.tmp` segment 创建时先写 header。
- finalize 时按 `file_size >= header_size` 和 `(file_size - header_size) == records * record_size` 校验。
- 0-record segment 仍按当前行为删除 `.tmp`，不写 manifest。
- rename 成 `.bin` 后再 append manifest line。

## Manifest Metadata Extension

manifest 继续一行一个 JSON object。新增字段只用于脚本预检和人工排障：

```json
{
  "sequence": 1,
  "file": "/home/liuxiang/tmp/md/segments/book_ticker_000001.bin",
  "records": 12345,
  "bytes": 790096,
  "format": "aquila.market_data.binary",
  "version": 1,
  "feed": "book_ticker",
  "header_bytes": 16,
  "record_size": 64,
  "first_exchange_ns": 1770000000000000000,
  "last_exchange_ns": 1770000001000000000,
  "first_local_ns": 1770000000000100000,
  "last_local_ns": 1770000001000100000,
  "closed_reason": "rotation"
}
```

`bytes` 表示最终文件总字节数，包含 header。`records` 仍表示 payload record 数。

`manifest_to_data_reader_config.py` 应读取并校验这些字段：

- `format == "aquila.market_data.binary"`。
- `version == 1`。
- `feed` 必须等于 CLI `--feed`。
- `header_bytes == 16`。
- `record_size` 必须等于目标 feed 的当前 ABI size。
- `bytes == header_bytes + records * record_size`。

manifest 预检失败时拒绝生成 TOML；即使 manifest 预检通过，`HistoricalDataReader` 仍必须以文件 header 为准再次校验。

## DataReader TOML

不新增单独 historical schema；继续使用共享 `DataReaderConfig`。

Realtime source：

```toml
[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "latest"
required = true
```

Historical source：

```toml
[[data_reader.sources]]
name = "recorded_book_ticker"
type = "binary_file"
feed = "book_ticker"
files = [
  "/home/liuxiang/tmp/md/segments/book_ticker_000001.bin",
]
start_position = "earliest_visible"
read_mode = "drain"
required = true
```

校验规则：

- `binary_file` source 必须显式写 `feed`；不再依赖默认 `book_ticker`。
- `binary_file` source 仍必须使用 `start_position = "earliest_visible"` 和 `read_mode = "drain"`。
- `HistoricalDataReader` 仍只接受 exactly one `binary_file` source。
- 每个 input file 的 header feed/type 必须与 TOML `feed` 一致。
- 多文件 replay 要求所有文件 header 的 `feed_type`、`version`、`record_size` 一致。

## HistoricalDataReader

构造期对每个 file：

1. 检查 path 非空，文件存在且可 stat。
2. 检查 `file_size >= 16`。
3. mmap 文件。
4. 读取 header 并校验 magic/version/header_size/feed_type/record_size/flags。
5. 校验 header feed/type 与 TOML `feed` 一致。
6. 计算 `record_count = (file_size - header_size) / record_size`。
7. 保存 payload cursor 为 `mapping.data() + header_size`。

热路径 `Poll()` / `Drain()` 不解析 header，不打开文件，只按已保存的 payload cursor 读取 record 并分发 handler。

错误语义：

- 缺 header、旧裸文件、magic mismatch、version mismatch、feed mismatch、record size mismatch、unknown flags、trailing bytes 都在构造期抛出。
- header-only 文件合法，构造后该文件计为 completed。

## Python 读取与写入

新增共享 Python helper，例如 `scripts/market_data/typed_binary.py`：

- 定义 header parser / writer。
- 定义 `book_ticker_dtype()` 和后续 `trade_dtype()`，或复用现有 dtype 并集中出口。
- 提供：
  - `read_header(path)`;
  - `load_records(path, feed) -> np.ndarray`;
  - `memmap_records(path, feed) -> np.memmap`，offset 使用 `header_size`;
  - `iter_record_chunks(path, feed, chunk_records)`;
  - `write_header(handle, feed, record_size)`;
  - `write_records(path, feed, records)`.

迁移范围：

- `analyze_book_ticker_latency.py`：`np.fromfile` 改为 helper 读取 typed-header。
- `analyze_book_ticker_fusion_latency.py`：BookTicker input 改为 helper 读取 typed-header；fusion metadata binary 不属于本格式。
- `compare_fusion_tardis_book_ticker.py`：`np.memmap` 改为带 offset 的 helper。
- `split_book_ticker_by_symbol.py`：input 按 header 分 chunk；输出 per-symbol `.bin` 也写 typed-header。
- `hdf_book_ticker_to_binary.py`：输出 typed-header BookTicker binary。
- `generate_preflight_config_params.py`：`.bin` 和 `.bin.zst` 解压后的内容都按 typed-header 解析；`.bin.zst` 表示整个 typed binary 文件压缩，而不是只压缩 payload。

测试里所有临时 `.bin` fixtures 都应写 typed-header，不保留 raw fixture 分支。

## LeadLag Replay 边界

`lead_lag_replay` 继续只消费 `feed = "book_ticker"` 的 historical source。使用 Trade typed-header file 时：

- DataReader TOML `feed = "trade"` 会先被 `TradingRuntime::Create()` 因 `ReplayStrategy` 无 `OnTrade()` 拒绝；
- 如果 TOML `feed = "book_ticker"` 但文件 header 是 `trade`，`HistoricalDataReader` 在构造期因 feed mismatch 拒绝。

这样避免 replay 进程接受 trade 文件但实际丢弃事件。

## Out of Scope

- 不做 raw legacy 自动识别或 `--legacy-raw` 开关。
- 不做 raw-to-typed-header 转换工具。
- 不做 `BookTicker` + `Trade` mixed historical ordering。
- 不做多 `binary_file` source merge。
- 不修改 LeadLag 策略消费 trade。
- 不修改 fusion metadata binary 格式。

## 测试和验证

C++：

- `data_reader_recorder_test`
  - 单文件 truncate 写 header。
  - 单文件 append 校验 header 后追加。
  - append 到旧 raw / wrong feed / wrong record size 文件失败。
  - rotation segment 写 header，manifest 写 typed-header metadata。
  - zero-record rotation segment 不写 manifest。
- `core_market_data_historical_data_reader_test`
  - 读取 BookTicker typed-header。
  - 读取 Trade typed-header。
  - header-only 文件作为 0 records 完成。
  - 缺 header / wrong magic / wrong version / wrong feed / wrong record size / trailing bytes 均失败。
- `data_reader_config_test`
  - `binary_file` source 必须显式 `feed`。
  - 既有 realtime source 默认行为不受影响。
- `data_reader_probe_cli_test`
  - 使用真实 `data_reader_probe` 读取 Trade typed-header binary 并断言 `handler_trades`。

Python：

- `typed_binary` helper 单测覆盖 header parse/write、dtype offset、wrong feed、trailing bytes。
- 更新所有 market_data / lead_lag Python 测试 fixtures 为 typed-header。
- `manifest_to_data_reader_config_test.py` 覆盖 typed-header manifest metadata 校验。

收尾验证建议：

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target \
  data_reader_recorder_test \
  core_market_data_historical_data_reader_test \
  data_reader_config_test \
  data_reader_probe_cli_test

ctest --test-dir build/debug -R \
  '(data_reader_recorder|core_market_data_historical_data_reader|data_reader_config|data_reader_probe_cli)' \
  --output-on-failure

python3 scripts/test/market_data/analyze_book_ticker_latency_test.py
python3 scripts/test/market_data/compare_fusion_tardis_book_ticker_test.py
python3 scripts/test/market_data/split_book_ticker_by_symbol_test.py
python3 scripts/test/market_data/manifest_to_data_reader_config_test.py
python3 scripts/test/lead_lag/generate_preflight_config_params_test.py
git diff --check
```
