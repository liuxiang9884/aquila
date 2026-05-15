# Data Reader 配置说明

## 范围

`data_reader` 描述 strategy 侧行情输入。当前支持两类 `book_ticker` source：

- `type = "shm"`
- `type = "binary_file"`
- `feed = "book_ticker"`

`DataReader` / `BinaryDataReader` 不创建线程，由 strategy loop 主动调用 `Poll(handler)`。SHM reader attach
data session 创建的 SHM channel，不负责 create / remove SHM；binary file reader 顺序读取已落盘的
`BookTicker` 二进制文件，适合 replay / 对账。

## 示例

仓库内示例：

```text
config/data_readers/strategy_data_reader.toml
```

该配置同时读取 Gate 和 Binance 的 book ticker：

```toml
[data_reader]
name = "strategy_data_reader"
max_events_per_source = 64

[data_reader.execution_policy]
bind_cpu_id = 4
idle_policy = "spin"

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

[[data_reader.sources]]
name = "binance_book_ticker"
type = "shm"
exchange = "binance"
feed = "book_ticker"
shm_name = "aquila_binance_market_data"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "latest"
required = true
```

Binary replay 示例：

```text
config/data_readers/lead_lag_ordi_binary_replay.toml
```

```toml
[data_reader]
name = "lead_lag_ordi_binary_replay"
max_events_per_source = 4096

[[data_reader.sources]]
name = "ordi_merged_book_ticker"
type = "binary_file"
feed = "book_ticker"
files = [
  "/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/20260415.bin",
  "/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/20260416.bin",
  "/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/20260417.bin",
]
start_position = "earliest_visible"
read_mode = "drain"
required = true
```

## 字段

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `instrument_catalog.file` | 无，必须显式配置 | instrument CSV 路径。相对路径会在加载配置文件时解析到仓库路径。 |
| `instrument_catalog.schema` | 无，必须显式配置 | CSV schema，当前固定 `aquila.instrument.v1`。 |
| `data_reader.name` | 无，必须显式配置 | reader 实例名。 |
| `data_reader.max_events_per_source` | `64` | `drain` 模式下每个 source 单次 `Poll()` 最多产出的事件数；`latest` 模式忽略该值。 |
| `data_reader.execution_policy.bind_cpu_id` | `-1` | 预留给 strategy / probe 绑核使用；第一版 parser 只保留配置值。 |
| `data_reader.execution_policy.idle_policy` | `spin` | 预留给外层 loop 选择 idle 行为；第一版 `DataReader` 不自己执行 idle。 |
| `data_reader.sources.name` | 无，必须显式配置 | source 名称，必须唯一。 |
| `data_reader.sources.type` | `shm` | source 实现类型，支持 `shm` 和 `binary_file`。 |
| `data_reader.sources.exchange` | SHM 无默认值，必须显式配置；binary file 可省略 | `gate` 或 `binance`。binary file 中每条 `BookTicker` 自带 exchange。 |
| `data_reader.sources.feed` | `book_ticker` | 行情类型，第一版只支持 `book_ticker`。 |
| `data_reader.sources.shm_name` | 无，SHM 必须显式配置 | SHM segment 名称。binary file 不使用。 |
| `data_reader.sources.channel_name` | 无，SHM 必须显式配置 | SHM channel 名称。binary file 不使用。 |
| `data_reader.sources.files` | 无，binary file 必须显式配置 | `BookTicker` 二进制文件列表，按配置顺序读取。SHM 不使用。 |
| `data_reader.sources.start_position` | SHM 为 `latest`，binary file 为 `earliest_visible` | attach / replay 起点；binary file 只允许 `earliest_visible`。 |
| `data_reader.sources.read_mode` | SHM 为 `latest`，binary file 为 `drain` | 读取语义，支持 `latest` 和 `drain`；binary file 只允许 `drain`。 |
| `data_reader.sources.required` | `true` | 预留字段；第一版 source attach 失败会直接启动失败。 |

## read_mode

`latest`：

- 每个 source 每次 `Poll()` 最多产出一条。
- 如果 source 已经积累多条未读数据，只返回当前可见的最后一条。
- 中间未读数据计入 diagnostics 的 `skipped`，这是主动合并，不等同于 ring overrun。
- 适合 book ticker / BBO 这类策略只关心最新状态的行情。

`drain`：

- 每个 source 每次 `Poll()` 最多产出 `max_events_per_source` 条。
- 不主动跳到最后一条；除非 reader 已经落后超过 SHM ring capacity，底层 reader 会记录 overrun 并拉回可见窗口。
- 适合需要完整事件流的验证、probe、benchmark、binary replay，以及未来不能丢中间事件的 feed。

`binary_file` source 必须使用 `start_position = "earliest_visible"` 和 `read_mode = "drain"`。`BinaryDataReader`
会在构造时检查文件存在、文件大小是 `BookTicker` 大小的整数倍，并在历史文件读完后让后续 `Poll()` 返回
0；外层 runtime 需要在 idle hook 中主动停止 replay loop。

## Poll 语义

`DataReader::Poll(handler)` 会扫一遍 sources，并按每个 source 的 `read_mode` 直接分发：

```text
latest source -> TryReadLatest()
drain source  -> TryReadOne() 最多 max_events_per_source 次
```

`DataReader` 保存 `next_source_index_`，每次 `Poll()` 后把起始 source 向后移动一位，避免固定 source
长期先被处理。`Poll()` 返回本次交给 handler 的 book ticker 数量。

handler 需要提供：

```cpp
void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
```

## Diagnostics

`DataReader` 的统计通过编译期 diagnostics policy 开关：

- 生产默认 `DataReader<>` 使用 `NoopDataReaderDiagnostics`。
- probe / test 可以使用 `DataReader<DataReaderDiagnostics>`。

当前统计包括：

- `poll_calls`
- `empty_polls`
- `book_tickers`
- per-source `book_tickers`
- per-source `skipped`
- per-source `overruns`
- per-source `last_book_ticker_id`

统计不使用 atomic；第一版假设 `DataReader` 被单个 strategy 线程拥有。

## Probe

`tools/market_data/data_reader_probe.cpp` 是 strategy reader 的独立验证入口。默认读取仓库配置：

```bash
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
```

参数：

- `--config`：data reader TOML 路径。
- `--max-polls`：最多调用多少次 `Poll()`；`0` 表示直到 SIGINT / SIGTERM。
- `--log-every`：打印首条 book ticker，然后每 N 条打印一次采样；`0` 表示关闭采样日志。

probe 使用 `DataReader<DataReaderDiagnostics>`，退出时会打印整体统计和每个 source 的：

- `book_tickers`
- `skipped`
- `overruns`
- `last_book_ticker_id`

## Live Drain Evidence

2026-05-06 使用 Gate / Binance data session 实盘写 SHM，并用临时 drain 配置运行 reader 1800s。
正式仓库配置仍保持 `read_mode = "latest"`；临时配置只用于验证完整事件流。

运行方式：

```bash
/usr/bin/timeout --kill-after=10s 1860s ./build/debug/tools/gate_data_session --connect
/usr/bin/timeout --kill-after=10s 1860s ./build/debug/tools/binance_data_session --connect
/usr/bin/timeout --kill-after=10s 1800s ./build/debug/tools/data_reader_probe --config /tmp/aquila_strategy_data_reader_drain.toml --log-every 10000
```

reader log：

```text
/home/liuxiang/log/strategy_data_reader_drain_live_20260506_133639.log
```

reader final summary：

```text
result=ok polls=5236281282 handler_book_tickers=4635362 diagnostics_book_tickers=4635362 empty_polls=5231647606
source index=0 name=gate_book_ticker exchange=kGate book_tickers=495255 skipped=0 overruns=0 last_book_ticker_id=111902051288
source index=1 name=binance_book_ticker exchange=kBinance book_tickers=4140107 skipped=0 overruns=0 last_book_ticker_id=10485460945723
```

结论：本次 `drain` reader 运行窗口内两个 source 均未检测到 SHM ring overrun；`drain` 模式不主动
skip，因此 `skipped=0`。producer 侧 `DataShmPublisher::published_count()` 会从 SHM header 读取初始值；
当 data session 配置 `remove_existing=false` 时，producer 的最终 `book_tickers` 不一定等于本次窗口内生产条数。
评估本次 reader 读取情况时以 reader per-source summary 为准。
