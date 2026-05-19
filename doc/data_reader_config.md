# Data Reader 配置说明

## 范围

`data_reader` 描述 strategy 侧行情输入。当前支持两类 `book_ticker` source：

- `type = "shm"`
- `type = "binary_file"`
- `feed = "book_ticker"`

`RealtimeDataReader` / `HistoricalDataReader` 不创建线程，由 strategy loop 主动调用 `Poll(handler)` 或 `Drain(handler, budget)`。
SHM live reader 在 `StrategyRuntime` 中走 `Poll()`；binary replay / finite reader 走 `Drain()`。SHM reader attach
data session 创建的 SHM channel，不负责 create / remove SHM；binary file reader 顺序读取已落盘的 `BookTicker`
二进制文件，适合 replay / 对账。

`RealtimeDataReader` 要求至少配置一个实时 source；空 source 配置属于启动期配置错误，会在构造 reader 时直接失败。

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

ORDI_USDT 三天 Tardis replay binary 当前默认放在：

```text
/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/20260415.bin
/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/20260416.bin
/home/liuxiang/tardis/merged_book_ticker/ORDI_USDT/20260417.bin
```

HDF / xex_mars `bbo` 数据也可以先转换成同一个 `aquila::BookTicker` binary ABI，再用同类
`binary_file` 配置回放。转换入口是：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/hdf_book_ticker_to_binary.py \
  --start-date 20260415 \
  --end-date 20260417
```

该脚本按 HDF `config` 表读取 `bbo_ns` 或 `bbo`，其中 `bbo_ns` 时间字段按 ns 解释，普通 `bbo`
时间字段按 ms 解释；dataset 会先一次读取成 `pandas.DataFrame`，再映射到 `BookTicker` struct 写出。
当前 ORDI_USDT HDF binary 输出在：

```text
/home/liuxiang/tardis/merged_book_ticker_hdf/ORDI_USDT/20260415.bin
/home/liuxiang/tardis/merged_book_ticker_hdf/ORDI_USDT/20260416.bin
/home/liuxiang/tardis/merged_book_ticker_hdf/ORDI_USDT/20260417.bin
```

Tardis `book_ticker` 与 HDF `bbo` 是两条不同输入链路。ORDI_USDT 20260415～20260417 的对账显示，
HDF 三天比 Tardis 少 `512,137` 条 `BookTicker`，差异主要来自 Gate；因此 HDF replay 结果不能直接当作
Tardis replay 的逐 tick 对账结果。详细记录数、signal 和 PnL 对比见
`doc/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md`。

## 字段

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `instrument_catalog.file` | 无，必须显式配置 | instrument CSV 路径。相对路径会在加载配置文件时解析到仓库路径。 |
| `instrument_catalog.schema` | 无，必须显式配置 | CSV schema，当前固定 `aquila.instrument.v1`。 |
| `data_reader.name` | 无，必须显式配置 | reader 实例名。 |
| `data_reader.max_events_per_source` | `64` | 外层调用 `Drain(handler, max_events)` 时使用的默认批量预算；`Poll()` 本身始终是单事件接口。`StrategyRuntime` 只在 finite / replay reader 上使用该预算。 |
| `data_reader.execution_policy.bind_cpu_id` | `-1` | 预留给 strategy / probe 绑核使用；第一版 parser 只保留配置值。 |
| `data_reader.execution_policy.idle_policy` | `spin` | 预留给外层 loop 选择 idle 行为；第一版 `RealtimeDataReader` 不自己执行 idle。 |
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

- 每个 source 每次 `Poll()` 最多产出一条。
- 调用方如果需要批量消费，应调用 `Drain(handler, max_events)`，由 reader 循环执行 `Poll()`，最多输出 `max_events` 条。
- 不主动跳到最后一条；除非 reader 已经落后超过 SHM ring capacity，底层 reader 会记录 overrun 并拉回可见窗口。
- 适合需要完整事件流的验证、probe、benchmark、binary replay，以及未来不能丢中间事件的 feed。

`binary_file` source 必须使用 `start_position = "earliest_visible"` 和 `read_mode = "drain"`。`HistoricalDataReader`
会在构造时检查文件存在、文件大小是 `BookTicker` 大小的整数倍；空文件是合法的已完成输入，构造后如果所有文件为空则
`finished() == true`，最后一个有数据文件后的尾部空文件会在最后一条数据读完时一并计入完成。`Poll()` 每次最多输出一条，
文件读完后 `Poll()` 返回 0 且 `finished() == true`；外层 runtime 需要在 idle hook 中主动停止 replay loop。

## Poll 语义

`RealtimeDataReader::Poll(handler)` 是单事件接口：从 `next_source_index_` 开始 round-robin 扫描 sources，找到
第一个可读 source 后输出一条并返回 1；所有 source 当前都无数据时返回 0。按 source `read_mode` 分发：

```text
latest source -> TryReadLatest()
drain source  -> TryReadOne()
```

成功输出后，`RealtimeDataReader` 把下一次扫描起点移动到当前 source 的后一个位置，避免固定 source 长期先被处理。
构造期已经保证 sources 非空；如果配置没有任何实时 source，应在启动阶段失败，而不是进入运行循环后持续返回 0。当前实现不依赖 `Poll()` 的空 reader 分支表达语义。
多 source round-robin 当前使用构造期生成的双倍扫描表，避免在 `Poll()` 循环中反复执行 index wrap 分支。

`RealtimeDataReader::Drain(handler, max_events)` 是批量接口：循环调用 `Poll()`，最多输出 `max_events` 条；
`max_events = 0` 时不读取并返回 0。

`StrategyRuntime` 的调用规则按 reader 是否显式满足 `FiniteDataReader` 区分：live reader 不声明 `kFiniteDataReader`，每轮只调用
`Poll(runtime)`；finite / replay reader 声明 `kFiniteDataReader = true` 并提供 `finished()`，runtime 每轮调用 `Drain(runtime, data_reader.max_events_per_source)`。
不支持 `Drain()` 的兼容 reader 仍走 `Poll()` fallback。

handler 需要提供：

```cpp
void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
```

## Diagnostics

`RealtimeDataReader` 的统计通过编译期 diagnostics policy 开关：

- 生产默认 `RealtimeDataReader<>` 使用 `NoopRealtimeDataReaderDiagnostics`。
- probe / test 可以使用 `RealtimeDataReader<RealtimeDataReaderDiagnostics>`。

当前统计包括：

- `total_count`
- per-source `book_ticker_count`
- per-source `skipped`
- per-source `overruns`
- per-source `last_book_ticker_id`

`poll_calls` / `empty_polls` 属于外层 runtime / scheduler / probe 的循环诊断，不放在 reader 内部 stats。

统计不使用 atomic；第一版假设 `RealtimeDataReader` 被单个 strategy 线程拥有。

## Probe

`tools/market_data/data_reader_probe.cpp` 是 strategy reader 的独立验证入口。默认读取仓库配置：

```bash
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
```

参数：

- `--config`：data reader TOML 路径。
- `--max-polls`：最多执行多少次 probe loop；每轮按配置预算调用 `Drain()`；`0` 表示直到 SIGINT / SIGTERM。
- `--log-every`：打印首条 book ticker，然后每 N 条打印一次采样；`0` 表示关闭采样日志。

probe 使用 `RealtimeDataReader<RealtimeDataReaderDiagnostics>`，退出时会打印整体统计和每个 source 的：

- `book_ticker_count`
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

reader final summary（按当前字段名归一化）：

```text
result=ok polls=5236281282 handler_book_tickers=4635362 diagnostics_total_count=4635362
source index=0 name=gate_book_ticker exchange=kGate book_ticker_count=495255 skipped=0 overruns=0 last_book_ticker_id=111902051288
source index=1 name=binance_book_ticker exchange=kBinance book_ticker_count=4140107 skipped=0 overruns=0 last_book_ticker_id=10485460945723
```

结论：本次 `drain` reader 运行窗口内两个 source 均未检测到 SHM ring overrun；`drain` 模式不主动
skip，因此 `skipped=0`。producer 侧 `DataShmPublisher::published_count()` 会从 SHM header 读取初始值；
当 data session 配置 `remove_existing=false` 时，producer 的最终 `book_tickers` 不一定等于本次窗口内生产条数。
评估本次 reader 读取情况时以 reader per-source summary 为准。
