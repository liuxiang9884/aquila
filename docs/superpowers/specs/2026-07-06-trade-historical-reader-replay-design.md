# Trade HistoricalDataReader / Replay 设计

## 目标

为 recorder 已落盘的裸 `aquila::Trade` binary 增加 historical 读取能力，使 `data_reader_probe`
可以用 `binary_file` 配置顺序读完 Trade 文件并统计 `OnTrade()` 事件。

第一版只实现 trade-only historical reader / probe 闭环，不实现 `BookTicker` 与 `Trade` 的 mixed
historical replay，也不让 `lead_lag_replay` 消费 trade source。

## 当前代码事实

- `Trade` live SHM、Gate / Binance data session 发布、`RealtimeDataReader` 的 `OnTrade()` 分发，以及
  `data_reader_recorder` 写 `Trade` 裸 binary 已经落地。
- `HistoricalDataReader` 当前只接受 exactly one `binary_file` source，并按 `sizeof(BookTicker)` 校验文件。
- `core/config/data_reader_config.cpp` 当前拒绝 `binary_file + feed = "trade"`。
- `data_reader_probe` 的 `ProbeHandler` 已经有 `OnTrade()` 和 trade 计数，但 historical summary 只打印
  `handler_book_tickers`。
- `lead_lag_replay` 通过 `TradingRuntime<ReplayStrategy, ..., HistoricalDataReader>` 运行；
  `ReplayStrategy` 没有 `OnTrade()`，`TradingRuntime::Create()` 会对 trade source fail fast。
- `scripts/market_data/manifest_to_data_reader_config.py` 当前硬编码生成 `feed = "book_ticker"`。

## Scope

本轮包含：

- `DataReaderConfig` 允许 exactly one `binary_file` source 使用 `feed = "trade"`。
- `HistoricalDataReader` 继续保持单 binary source 模型，但 source feed 可为 `book_ticker` 或 `trade`。
- `HistoricalDataReader` 对 `book_ticker` 文件按 `sizeof(BookTicker)` 校验并调用 `handler.OnBookTicker()`。
- `HistoricalDataReader` 对 `trade` 文件按 `sizeof(Trade)` 校验并调用 `handler.OnTrade()`。
- `data_reader_probe` historical summary 同时输出 `handler_book_tickers` 和 `handler_trades`。
- `manifest_to_data_reader_config.py` 增加 `--feed book_ticker|trade`，默认保持 `book_ticker`。
- 文档同步说明 binary trade replay 的范围和限制。

本轮不包含：

- mixed `BookTicker` + `Trade` historical replay。
- 多个 `binary_file` source 的跨 feed merge。
- 按 `exchange_ns`、`trade_ns` 或 `local_ns` 做跨 feed 排序。
- 修改 LeadLag 策略消费 trade 行情。
- 让 `lead_lag_replay` 接受但丢弃 trade source。

## HistoricalDataReader 设计

`HistoricalDataReader` 保持一个 public type 和现有 `DataReaderLike` contract。构造阶段读取唯一
`DataReaderSourceConfig` 的 `feed`，保存为 reader feed 类型。`Poll()` / `Drain()` 根据 feed 选择读取结构：

```text
feed = book_ticker
  file size multiple of sizeof(BookTicker)
  memcpy BookTicker
  diagnostics.RecordBookTicker(record)
  handler.OnBookTicker(record)

feed = trade
  file size multiple of sizeof(Trade)
  memcpy Trade
  diagnostics.RecordTrade(record)
  handler.OnTrade(record)
```

reader 仍然按配置 `files` 顺序读取，一个文件读完后进入下一个文件；空文件仍计为 completed file。

诊断结构新增 trade 计数：

```cpp
struct HistoricalDataReaderStats {
  std::uint64_t total_count;
  std::uint64_t book_ticker_count;
  std::uint64_t trade_count;
  std::uint64_t files_completed;
};
```

`total_count` 继续表示已交给 handler 的全部事件数。

## Config 设计

`binary_file` source 规则改为：

- `feed` 允许 `book_ticker` 或 `trade`。
- `files` 必须非空。
- `start_position` 必须是 `earliest_visible`。
- `read_mode` 必须是 `drain`。
- historical reader 仍要求 exactly one `binary_file` source。

示例：

```toml
[[data_reader.sources]]
name = "recorded_trade"
type = "binary_file"
feed = "trade"
files = ["/home/liuxiang/tmp/run/recorded_trade.bin"]
start_position = "earliest_visible"
read_mode = "drain"
required = true
```

## Probe 行为

`data_reader_probe` historical 模式继续用 `HistoricalDataReader<HistoricalDataReaderDiagnostics>`。
summary log 输出扩展为：

```text
result=ok mode=historical stop_reason=... polls=...
handler_book_tickers=... handler_trades=...
diagnostics_total_count=... files_completed=...
```

这使 trade-only binary probe 可以直接用日志确认 `OnTrade()` 事件数。

## Manifest 脚本

`scripts/market_data/manifest_to_data_reader_config.py` 新增参数：

```bash
--feed book_ticker|trade
```

默认值是 `book_ticker`，以保持现有 BookTicker replay 配置生成行为不变。`--feed trade` 时：

- source name 为 `<name>_trade`；
- TOML 写 `feed = "trade"`；
- 仍复用 manifest 中的 segment 顺序；
- 仍过滤 `.tmp` segment；
- 仍生成单个 `binary_file` source。

## LeadLag Replay 边界

本轮不修改 `ReplayStrategy`，不新增空 `OnTrade()`。因此 `lead_lag_replay` 使用 trade binary config 时，
`TradingRuntime::Create()` 仍应返回配置错误。

这个边界是刻意保留的：否则 replay 进程会接受 trade source 但实际丢弃事件，容易误导为 LeadLag 已经
支持 trade replay 信号。

## 测试和验证

必须覆盖：

- `data_reader_config_test`：
  - `binary_file + feed = "trade"` parse 成功。
  - `binary_file` 仍拒绝非法 start position / read mode / empty files。
- `core_market_data_historical_data_reader_test`：
  - trade binary `Poll()` / `Drain()` 顺序读取。
  - trade 文件 size 非 `sizeof(Trade)` 倍数时报错。
  - diagnostics 记录 `trade_count` 和 `total_count`。
  - book_ticker 现有行为保持不变。
- `data_reader_probe_mode_test`：
  - exactly one trade binary source 仍识别为 historical。
- `scripts/test/market_data/manifest_to_data_reader_config_test.py`：
  - 默认仍生成 `feed = "book_ticker"`。
  - `feed="trade"` 生成 `<name>_trade` source 和 `feed = "trade"`。
- 真实 probe dry run：
  - 构造临时 Trade binary 和 `feed = "trade"` TOML。
  - 运行 `./build/debug/tools/data_reader_probe --config <toml> --max-polls <n>`。
  - 日志中 `handler_trades` 与 Trade record 数一致。

收尾验证命令：

```bash
ctest --test-dir build/debug -R '(data_reader_config|core_market_data_historical_data_reader|data_reader_probe_mode)' --output-on-failure
python3 scripts/test/market_data/manifest_to_data_reader_config_test.py
```

probe dry run 使用 `/home/liuxiang/tmp` 下的临时 artifact，不写入仓库目录。
