# Data Reader 配置说明

## 范围

`data_reader` 描述 strategy 侧行情输入。当前支持：

- realtime SHM source：`feed = "book_ticker"` 或 `feed = "trade"`
- historical / replay binary source：仅 `feed = "book_ticker"`

`RealtimeDataReader` / `HistoricalDataReader` 不创建线程，由 strategy loop 主动调用 `Poll(handler)` 或 `Drain(handler, budget)`。
SHM live reader 在 `TradingRuntime` 中走 `Poll()`；binary replay / finite reader 走 `Drain()`。SHM reader attach
data session 创建的 SHM channel，不负责 create / remove SHM；binary file reader 顺序读取已落盘的 `BookTicker`
二进制文件，适合 replay / 对账。`HistoricalDataReader` 尚未支持 `Trade` binary。

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
max_events_per_drain = 64

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
shm_name = "aquila_binance_market_data_combined"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "latest"
required = true
```

Trade SHM source 示例：

```toml
[[data_reader.sources]]
name = "gate_trade"
type = "shm"
exchange = "gate"
feed = "trade"
shm_name = "aquila_gate_market_data"
channel_name = "trade_channel"
start_position = "latest"
read_mode = "drain"
required = true
```

Binary replay 示例：

```text
config/data_readers/lead_lag_ordi_binary_replay.toml
```

```toml
[data_reader]
name = "lead_lag_ordi_binary_replay"
max_events_per_drain = 4096

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
`docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md`。

live Gate / Binance canonical fusion recorder 与 Tardis `book_ticker` CSV 的离线对账入口是
`scripts/market_data/compare_fusion_tardis_book_ticker.py`。该脚本按 `BookTicker` binary ABI 读取
fusion 落盘文件，使用 instrument catalog 的 `price_tick` / `quantity_step` 将 price / quantity
转成整数 units，再与 Tardis `timestamp` 毫秒口径做 multiset 对账；Gate timestamp 语义差异需结合
`--near-ms` 分类解释。20260627 30-symbol 对账结果和命令见
`docs/fusion_tardis_bbo_comparison.md`。

## 字段

| 字段 | 默认值 | 含义 |
| --- | --- | --- |
| `instrument_catalog.file` | 无，必须显式配置 | instrument CSV 路径。相对路径会在加载配置文件时解析到仓库路径。 |
| `instrument_catalog.schema` | 无，必须显式配置 | CSV schema，当前固定 `aquila.instrument.v1`。 |
| `data_reader.name` | 无，必须显式配置 | reader 实例名。 |
| `data_reader.max_events_per_drain` | `64` | 外层调用 `Drain(handler, max_events)` 时使用的默认批量预算，必须是正的 `uint32` 范围整数；`Poll()` 本身始终是单事件接口。`TradingRuntime` 只在 finite / replay reader 上使用该预算。旧字段 `max_events_per_source` 已删除，配置中出现会直接报错。 |
| `data_reader.execution_policy.bind_cpu_id` | `-1` | 预留给 strategy / probe 绑核使用；第一版 parser 只保留配置值。 |
| `data_reader.execution_policy.idle_policy` | `spin` | 预留给外层 loop 选择 idle 行为；第一版 `RealtimeDataReader` 不自己执行 idle。 |
| `data_reader.sources.name` | 无，必须显式配置 | source 名称，必须唯一。 |
| `data_reader.sources.type` | `shm` | source 实现类型，支持 `shm` 和 `binary_file`。 |
| `data_reader.sources.exchange` | SHM 无默认值，必须显式配置；binary file 可省略 | `gate` 或 `binance`。binary file 中每条 `BookTicker` 自带 exchange。 |
| `data_reader.sources.feed` | `book_ticker` | 行情类型。SHM source 支持 `book_ticker` 和 `trade`；`binary_file` 当前只支持 `book_ticker`。 |
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
- 适合 book ticker / BBO 这类策略只关心最新状态的行情；用于 trade 时表示采样最新成交，不保留完整逐笔流。

`drain`：

- 每个 source 每次 `Poll()` 最多产出一条。
- 调用方如果需要批量消费，应调用 `Drain(handler, max_events)`，由 reader 循环执行 `Poll()`，最多输出 `max_events` 条。
- 不主动跳到最后一条；除非 reader 已经落后超过 SHM ring capacity，底层 reader 会记录 overrun 并拉回可见窗口。
- 适合需要完整事件流的验证、probe、benchmark、binary replay，以及 trade 这类不能丢中间事件的 feed。

`binary_file` source 必须使用 `start_position = "earliest_visible"` 和 `read_mode = "drain"`。第一版
`HistoricalDataReader` 只接受一个 `binary_file` source；多日 / 分片 replay 用同一个 source 下的多个 `files`
表达，跨 source merge 必须在离线数据程序中预先完成。reader 会在构造时检查文件存在、文件大小是 `BookTicker`
大小的整数倍；空文件是合法的已完成输入，构造后如果所有文件为空则 `finished() == true`，最后一个有数据文件后的
尾部空文件会在最后一条数据读完时一并计入完成。非空文件在构造期完成 read-only mmap 和大小复核；`Poll()` /
`Drain()` 热路径只按 cursor 从已 mmap 区域拷贝 `BookTicker` 并推进状态，不打开文件、不抛出异常。文件读完后
`Poll()` 返回 0 且 `finished() == true`；外层 runtime 需要在 idle hook 中主动停止 replay loop。

## Poll 语义

`RealtimeDataReader::Poll(handler)` 是单事件接口：从 `next_source_index_` 开始 round-robin 扫描 sources，找到
第一个可读 source 后输出一条并返回 1；所有 source 当前都无数据时返回 0。按 source `read_mode` 分发：

```text
latest source -> TryReadLatest()
drain source  -> TryReadOne()
```

按 source `feed` 分发到对应 typed SHM reader：`book_ticker` 调用 `BookTickerShmReader` 并输出
`handler.OnBookTicker()`，`trade` 调用 `TradeShmReader` 并输出 `handler.OnTrade()`。同一 SHM object 中的
`book_ticker_channel` / `trade_channel` 是两个独立 broadcast queue；reader 保证各 channel 内按 cursor 读取，
不做跨 channel 全局时间排序。

成功输出后，`RealtimeDataReader` 把下一次扫描起点移动到当前 source 的后一个位置，避免固定 source 长期先被处理。
构造期已经保证 sources 非空；如果配置没有任何实时 source，应在启动阶段失败，而不是进入运行循环后持续返回 0。当前实现不依赖 `Poll()` 的空 reader 分支表达语义。
多 source round-robin 当前使用构造期生成的双倍扫描表，避免在 `Poll()` 循环中反复执行 index wrap 分支。

round-robin 只是实时多 source 的公平调度，不是 merge 或时间排序。它不比较 `BookTicker.exchange_ns` /
`BookTicker.local_ns`，也不保证 lead / lag 事件按全局时间顺序输出。LeadLag live 模式可以使用这个调度来避免
高频 source 长期压住低频 source；如果需要严格事件时间语义，应由上游 data session / producer 写出已 merge 的统一
SHM source，或在 replay 前离线生成目标顺序的 binary source。

`RealtimeDataReader::Drain(handler, max_events)` 是批量接口：循环调用 `Poll()`，最多输出 `max_events` 条；
`max_events = 0` 时不读取并返回 0。

`RealtimeDataReader` 和 `HistoricalDataReader` 的 `Poll()` / `Drain()` 当前都以 `noexcept` 暴露。配置校验、
SHM attach、binary 文件检查和 mmap 失败仍在构造 / 启动冷路径通过异常报告；handler 也应保持 `noexcept`，避免把
策略异常引入 reader 热路径。

`TradingRuntime` 的调用规则按 reader 是否显式满足 `FiniteDataReader` 区分：live reader 不声明 `kFiniteDataReader`，每轮只调用
`Poll(runtime)`；finite / replay reader 声明 `kFiniteDataReader = true` 并提供 `finished()`，runtime 每轮调用 `Drain(runtime, data_reader.max_events_per_drain)`。
不支持 `Drain()` 的兼容 reader 仍走 `Poll()` fallback。

handler 需要提供：

```cpp
void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
void OnTrade(const aquila::Trade& trade) noexcept;
```

不消费 trade 的 handler 也应显式提供空 `OnTrade()`，避免 mixed-feed 配置被静默丢弃。
`TradingRuntime::Create()` 会在启动冷路径检查 data reader config：如果配置包含 `feed = "trade"`，
但策略没有实现 `OnTrade(const aquila::Trade&, ContextT&)`，会直接返回配置错误。

## SHM 到 replay binary recorder

`data_reader_recorder` 通过 `RealtimeDataReader` 从现有 Gate / Binance `BookTicker` SHM 读取数据，并输出一个合并后的 replay binary 文件。输出格式保持和当前 replay binary 一致：文件内容是连续的 `aquila::BookTicker` 结构体记录，不增加额外 header 或文本索引，后续可直接作为 `binary_file` source 交给 `HistoricalDataReader` / `lead_lag_replay` 使用。当前 recorder 只支持 `BookTicker` binary；启动期会拒绝任何 `feed = "trade"` source，避免生成部分输出或把 `Trade` 静默写入 BookTicker 文件。

基础示例：

```bash
./build/debug/tools/data_reader_recorder \
  --config config/data_readers/strategy_data_reader.toml \
  --output /home/liuxiang/tmp/aquila_merged_book_ticker.bin \
  --mode truncate
```

参数：

- `--config`：data reader TOML 路径，默认 `config/data_readers/strategy_data_reader.toml`。
- `--output`：输出 `.bin` 路径，必填。
- `--mode`：写入模式，支持 `truncate` 和 `append`，默认 `truncate`。
- `--max-polls`：最多执行多少次 recorder loop；每轮按配置预算调用 `Drain()`；`0` 表示直到 SIGINT / SIGTERM。

可选 recorder 专用配置放在同一 TOML 的 `[recorder]` 表。未配置或
`rotation_enabled = false` 时保持单文件输出；启用 rotation 时，当前段先写 `.tmp`，关闭后
atomic rename 成 `.bin` 并追加 manifest JSONL。默认 rotation interval 为 1 小时：

```toml
[recorder]
rotation_enabled = true
rotation_interval_sec = 3600
output_dir = "/home/liuxiang/tmp/aquila_persistent_md/segments"
file_prefix = "book_ticker"
manifest_path = "/home/liuxiang/tmp/aquila_persistent_md/manifest.jsonl"
```

省略可选字段时：

- `rotation_interval_sec = 3600`
- `output_dir = parent(--output) / "segments"`
- `file_prefix = stem(--output)`
- `manifest_path = parent(--output) / (stem(--output) + "_manifest.jsonl")`

rotation 第一版只支持 `--mode truncate`；`--mode append` 会在启动期拒绝，避免追加模式下 sequence 和
manifest 恢复语义不清晰。

实盘交易并行录制：

- `data_reader_recorder` 是只读 SHM consumer，可以在 LeadLag / demo 策略实盘交易运行时并行启动；它不会消费掉 SHM 中的数据，也不触碰订单链路。
- 如果目标是完整 replay dump，不要直接使用仓库默认 `strategy_data_reader.toml`。默认 live strategy 配置通常使用 `read_mode = "latest"`，会主动跳过中间 BBO，只适合低频状态采样。
- 完整 dump 应使用临时 data reader 配置，把目标 source 设为 `read_mode = "drain"`。`start_position = "latest"` 表示从 recorder 启动后开始录；`start_position = "earliest_visible"` 表示先从当前 SHM 可见窗口开始补录。
- 录制输出和临时配置默认放到 `/home/liuxiang/tmp`。实盘交易同时录制时，应观察 recorder 退出统计里的 per-source `overruns` / `skipped`，并避免 recorder 抢占 strategy / order session 的关键 CPU。

完整 dump 示例：

```bash
./build/debug/tools/data_reader_recorder \
  --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml \
  --output /home/liuxiang/tmp/live_merged_book_ticker.bin \
  --mode truncate
```

当前边界：

- 使用一份 data reader TOML 作为输入，复用现有 SHM source 配置、`read_mode` 和 `max_events_per_drain`。
- 默认只输出一个 merged `.bin`；启用 `[recorder].rotation_enabled` 后输出多个 segment `.bin` 和一个
  manifest JSONL。记录顺序就是 `RealtimeDataReader` 实际交给 handler 的顺序。
- 如果输入配置使用 `drain`，recorder 会按 reader 可见事件流顺序连续写出；如果输入配置仍是 `latest`，则输出也继承 latest 的跳点语义，工具启动日志会显式打印 `latest_read_mode_source` warning。
- recorder 启动日志打印 `book_ticker_abi_size=sizeof(aquila::BookTicker)`；probe 会同时打印 `book_ticker_abi_size` 和 `trade_abi_size`。SHM attach 仍会校验 producer header 中的 ABI size。输出文件本身不写 header。
- 单文件写入路径使用二进制追加或截断模式，由 `--mode` 明确控制，默认截断；rotation 模式只支持截断。
- rotation manifest 每行一个 JSON object，记录 segment `sequence`、`file`、`records`、`bytes`、first / last
  `exchange_ns`、first / last `local_ns` 和 `closed_reason`。正在写的 `.tmp` 不写入 manifest，replay 只读取已关闭的 `.bin`。
- 退出统计包括 total records、per-exchange records、per-source skipped / overrun、first / last `exchange_ns` 和 `local_ns`，rotation 模式额外输出 `segments_completed`。
- 本地测试 `data_reader_recorder_test` 覆盖两个 `BookTicker` SHM source 经 `RealtimeDataReader::Drain()` 写入同一个裸 replay binary 的路径。

rotation replay 不需要修改 `HistoricalDataReader`。现有 `binary_file` source 已支持一个 source 下多个 `files`；
可用 manifest 生成 replay data reader TOML：

```bash
scripts/market_data/manifest_to_data_reader_config.py \
  --manifest /home/liuxiang/tmp/aquila_persistent_md/manifest.jsonl \
  --output /home/liuxiang/tmp/aquila_persistent_md/replay_data_reader.toml \
  --name persistent_md_replay \
  --catalog config/instruments/usdt_futures.csv
```

生成的 TOML 会把已关闭 segment `.bin` 按 manifest 顺序写入 `files = [...]`，随后可直接用于
`data_reader_probe`、`lead_lag_replay` 或其他 `HistoricalDataReader` 使用方。

当前 `BookTicker.local_ns` 不是 DataReader 或策略层打点，而是在 data session 收到 WebSocket frame 后、进入交易所 parser / decoder 前采集：

- Gate 在 `exchange/gate/market_data/data_session.h` 的 binary frame path 调用 `websocket::NowNs(kClockSource)`，随后传给 `DecodeBookTickerWithHeader()` 写入 `BookTicker.local_ns`。
- Binance 在 `exchange/binance/market_data/data_session.h` 的 text frame path 调用 `websocket::NowNs(kClockSource)`，随后由 `AssignBookTickerFromUpdate()` 写入 `BookTicker.local_ns`。
- Gate / Binance data session 默认 `kClockSource = ClockSource::kRealtime`，`local_ns` 使用 `CLOCK_REALTIME` / Unix epoch ns；测试仍可通过自定义 WebSocket policy 覆盖为 monotonic / coarse clock。

当前 `BookTicker.exchange_ns` 表示交易所侧行情时间戳，但不同交易所字段语义不同：

- Gate SBE `bbo` 使用 `time * 1000`，即 Gate WebSocket server send timestamp；同一消息里的 `t` 是 orderbook engine update timestamp，目前不写入 `BookTicker`。
- Binance book ticker 使用 `E * 1'000'000`，即 Binance event time。

## Diagnostics

`RealtimeDataReader` 的统计通过编译期 diagnostics policy 开关：

- 生产默认 `RealtimeDataReader<>` 使用 `NoopRealtimeDataReaderDiagnostics`。
- probe / test 可以使用 `RealtimeDataReader<RealtimeDataReaderDiagnostics>`。

当前统计包括：

- `total_count`
- per-source `book_ticker_count`
- per-source `trade_count`
- per-source `skipped`
- per-source `overruns`
- per-source `last_book_ticker_id`
- per-source `last_trade_id`

`poll_calls` / `empty_polls` 属于外层 runtime / scheduler / probe 的循环诊断，不放在 reader 内部 stats。
`TradingRuntime` 可通过编译期 diagnostics policy 记录外层 loop 维度的 poll / drain 调用、empty poll、idle loop 和处理事件数；生产默认使用 no-op diagnostics。

统计不使用 atomic；第一版假设 `RealtimeDataReader` 被单个 strategy 线程拥有。

## Probe

`tools/market_data/data_reader_probe.cpp` 是 strategy reader 的独立验证入口。默认读取仓库配置：

```bash
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
```

参数：

- `--config`：data reader TOML 路径。
- `--max-polls`：最多执行多少次 probe loop；每轮按配置预算调用 `Drain()`；`0` 表示直到 SIGINT / SIGTERM。
- `--log-every`：分别打印首条 book ticker / trade，然后每 N 条打印一次采样；`0` 表示关闭采样日志。

probe 使用 `RealtimeDataReader<RealtimeDataReaderDiagnostics>`，退出时会打印整体统计和每个 source 的：

- `book_ticker_count`
- `trade_count`
- `skipped`
- `overruns`
- `last_book_ticker_id`
- `last_trade_id`

## Live Drain Evidence

2026-05-06 使用 Gate / Binance data session 实盘写 SHM，并用临时 drain 配置运行 reader 1800s。
正式仓库配置仍保持 `read_mode = "latest"`；临时配置只用于验证完整事件流。

运行方式：

```bash
/usr/bin/timeout --kill-after=10s 1860s ./build/debug/tools/gate_data_session --connect
/usr/bin/timeout --kill-after=10s 1860s ./build/debug/tools/binance_data_session --connect
/usr/bin/timeout --kill-after=10s 1800s ./build/debug/tools/data_reader_probe --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml --log-every 10000
```

reader log：

```text
/home/liuxiang/log/strategy_data_reader_drain_live_20260506_133639.log
```

reader final summary（按当前字段名归一化）：

```text
result=ok polls=5236281282 handler_book_tickers=4635362 handler_trades=0 diagnostics_total_count=4635362
source index=0 name=gate_book_ticker exchange=kGate book_ticker_count=495255 trade_count=0 skipped=0 overruns=0 last_book_ticker_id=111902051288 last_trade_id=0
source index=1 name=binance_book_ticker exchange=kBinance book_ticker_count=4140107 trade_count=0 skipped=0 overruns=0 last_book_ticker_id=10485460945723 last_trade_id=0
```

结论：本次 `drain` reader 运行窗口内两个 source 均未检测到 SHM ring overrun；`drain` 模式不主动
skip，因此 `skipped=0`。producer 侧 `DataShmPublisher::published_count()` 以 SHM queue 当前 producer position
初始化；当 data session 配置 `remove_existing=false` 时，producer 的最终 `book_tickers` 仍不一定等于本次窗口内
生产条数。评估本次 reader 读取情况时以 reader per-source summary 为准。

## Live Record Smoke Evidence

2026-05-24 使用 Gate / Binance data session 写 SHM，并用临时 `drain` 配置运行
`data_reader_recorder` 写出单个裸 `BookTicker` replay binary。临时配置、日志和输出均在：

```text
/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524/
```

录制命令：

```bash
/usr/bin/timeout --kill-after=5s 45s ./build/debug/tools/data_reader_recorder \
  --config /home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524/strategy_data_reader_drain.toml \
  --output /home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524/live_merged_book_ticker.bin \
  --mode truncate
```

recorder 由 `timeout` 发送 SIGTERM 后正常收尾，输出文件大小为 `1,003,840` bytes，即 `15,685`
条 `sizeof(aquila::BookTicker)=64` 的连续记录。recorder summary：

```text
result=ok stop_reason=signal polls=146025092 handler_book_tickers=15685 diagnostics_total_count=15685
recorder_stats total_records=15685 first_exchange_ns=1779591707743090000 first_local_ns=6790761886994204 last_exchange_ns=1779591752524000000 last_local_ns=6790806665512928
exchange_stats exchange=kBinance records=13860
exchange_stats exchange=kGate records=1825
source_stats index=0 name=gate_book_ticker exchange=kGate book_ticker_count=1825 trade_count=0 skipped=0 overruns=0 last_book_ticker_id=113244034670 last_trade_id=0
source_stats index=1 name=binance_book_ticker exchange=kBinance book_ticker_count=13860 trade_count=0 skipped=0 overruns=0 last_book_ticker_id=10617499964819 last_trade_id=0
```

这组 2026-05-24 样例生成于 data session `local_ns` 改为 `CLOCK_REALTIME` 之前，因此示例里的
`first_local_ns` / `last_local_ns` 是旧 steady-clock 数值。新录制的 Gate / Binance live data session
`local_ns` 应为 Unix epoch ns。

随后使用临时 `binary_file` data reader 配置验证 replay 可读性：

```bash
./build/debug/tools/data_reader_probe \
  --config /home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524/recorded_binary_reader.toml \
  --max-polls 0 \
  --log-every 100000
```

`data_reader_probe` 进入 `mode=historical`，通过 `HistoricalDataReader` 读完 recorder 输出：

```text
result=ok mode=historical stop_reason=finished polls=4 handler_book_tickers=15685 diagnostics_total_count=15685 files_completed=1
```

结论：本次 live record smoke 中 Gate / Binance 两个 source 都有记录，输出裸 binary 可被
`HistoricalDataReader` 顺序读完；两个 live SHM source 的 `skipped=0`、`overruns=0`。
