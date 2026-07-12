# Data Session SHM 通讯设计

## 目的

Gate / Binance / Bitget data session 通过共享内存把标准化 `aquila::BookTicker` 发布给
strategy、recorder、TUI 和 probe 进程。Gate SBE data session、Binance raw trade data session 和
Bitget UTA SBE `publicTrade` data session 还可以在同一个 SHM object 中发布标准化
`aquila::Trade`。当前只支持固定大小 typed record，不引入通用 event ring 或 payload pool。

本文只定义 typed SHM ABI、producer/reader protocol 与恢复边界；TOML 字段见 `docs/data_session_config.md`。

## 当前方案

核心类型在 `core/market_data/data_shm.h`：

```text
DataShmPublisher
BookTickerShmReader
BookTickerShmManager
BookTickerShmChannel
TradeShmReader
TradeShmManager
TradeShmChannel
DataShmManager
```

共享队列：

```cpp
inline constexpr std::uint64_t kBookTickerShmCapacity = 65536;
inline constexpr std::uint64_t kTradeShmCapacity = 65536;

using BookTickerQueue =
    nova::static_impl::SPBroadcastQueue<aquila::BookTicker,
                                        kBookTickerShmCapacity>;
using TradeQueue =
    nova::static_impl::SPBroadcastQueue<aquila::Trade, kTradeShmCapacity>;
```

设计约束：

- 单个 channel 只有一个 producer。
- producer 不感知 reader 数量。
- producer 不等待 reader，不接受 reader backpressure。
- reader 本地维护 cursor，cursor 不写入 SHM。
- 慢 reader 可能 overrun；reader 会显式计数并拉回当前 ring 窗口。
- 第一版不保证慢 reader 收到所有历史 `BookTicker` 或 `Trade`。

## SHM Header

`BookTickerShmHeader` / `TradeShmHeader` 用于 attach 校验和运维诊断：

```text
magic
version
abi_size
capacity
producer_pid
created_ns
published_count
heartbeat_ns
```

attach 必须校验：

- magic。
- version。
- `sizeof(BookTicker)` 或 `sizeof(Trade)`。
- capacity。

`published_count` 和 `heartbeat_ns` 只用于诊断，不作为 reader 判断消息可见性的依据。核心发布顺序以 Nova queue `Current()` 为准。

## Producer 协议

data session 的标准 sink 命名为 `DataSink`，SHM 写端是 `DataShmPublisher`。

发布路径：

```text
exchange data session
  -> market data client / decoder
  -> DataShmPublisher
  -> BookTickerQueue / TradeQueue
  -> BookTicker / Trade SHM channel
```

接口：

```cpp
void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
void OnTrade(const aquila::Trade& trade) noexcept;

template <typename Writer>
void EmplaceBookTickerWith(Writer&& writer) noexcept;

template <typename Writer>
void EmplaceTradeWith(Writer&& writer) noexcept;

void FlushPublishedCount() noexcept;
void UpdateHeartbeatNs(std::uint64_t heartbeat_ns) noexcept;
```

热路径约束：

- 只做 fixed-size `BookTicker` / `Trade` 写入和轻量本地计数。
- 不扫描 reader。
- 不更新时间源。
- 不写 heartbeat。
- 不因 reader 慢阻塞 WebSocket read path。

Gate / Binance / Bitget data session 可用 slot-writer fast path，直接在 queue producer slot 中解码
`BookTicker` 或 `Trade`。Gate / Bitget `Trade` 来自 SBE `publicTrade` entry，生产路径不把 SBE
repeating group 先 materialize 成 `std::array`、`std::vector` 或 batch container；Binance raw trade
是一条 JSON 对应一笔 `Trade`。

## Reader 协议

reader attach 后本地保存：

```text
read_pos
overrun_count
skipped_count
```

start position：

- `latest`：从 `queue.Current()` 开始，只读新数据。
- `earliest_visible`：从当前 ring 可见窗口最早位置开始，适合 dump / recorder。

读取模式：

- `latest`：每次只保留 source 最新一条。
- `drain`：尽量按 reader cursor 读完可见窗口内事件。

overrun：

```text
if current - read_pos > capacity:
  skipped += current - read_pos - capacity
  read_pos = current - capacity
  overrun_count += 1
```

reader 不做多 source 全局时间排序。`RealtimeDataReader` 多 source round-robin 只提供公平轮询，不按
`exchange_ns` / `local_ns` merge。当前 `RealtimeDataReader` 已支持按 source `feed` 读取 `BookTicker` 或
`Trade` typed SHM channel，并分别调用 handler 的 `OnBookTicker()` / `OnTrade()`；`HistoricalDataReader`
已支持单 source `book_ticker` 或 `trade` binary，但不做 mixed feed ordering。LeadLag 策略主路径目前不订阅
trade source，LeadLag replay 仍只消费 `book_ticker` binary source。

## 配置

data session 配置指定 SHM 名称和创建方式，容量不在 TOML 暴露：

- Gate：`exchange/gate/market_data/data_session_config.*`
- Binance：`exchange/binance/market_data/data_session_config.*`
- Bitget：`exchange/bitget/market_data/data_session_config.*`
- 通用 SHM config：`core/market_data/data_shm_config.h`

Gate / Binance / Bitget combined SHM 配置使用：

```toml
[data_shm_sink]
enabled = true
shm_name = "aquila_gate_market_data"
book_ticker_channel_name = "book_ticker_channel"
trade_channel_name = "trade_channel"
create = true
remove_existing = false
```

`book_ticker_channel_name` 和 `trade_channel_name` 是同一个 SHM object 内的两个独立 typed
`SPBroadcastQueue` channel。Bitget 可按 `feeds` 启用 `book_ticker`、`trade` 或两者。旧
`channel_name` 仍作为 Gate / Binance / Bitget `book_ticker_channel_name` 的
legacy alias；如果同名 SHM object 由旧单 channel 版本创建，需要在确认 reader 已停止后临时显式设置
`remove_existing=true`，或使用新的 `shm_name` 重建 combined layout。

reader 配置见 `docs/data_reader_config.md`：

- source name。
- exchange / feed。
- `start_position`。
- `read_mode`。
- optional / required。
- diagnostics policy。

## 已落地入口

| 文件 | 职责 |
| --- | --- |
| `core/market_data/data_shm.h` | SHM channel、publisher、reader。 |
| `core/market_data/data_shm_config.h` | `BookTickerShmConfig`、`TradeShmConfig`、combined `DataShmConfig` 和 capacity。 |
| `core/market_data/realtime_data_reader.h` | strategy / tool realtime `BookTicker` SHM reader。 |
| `core/market_data/historical_data_reader.h` | binary replay reader。 |
| `tools/gate/data_session.cpp` | Gate data session publisher。 |
| `tools/binance/data_session.cpp` | Binance data session publisher。 |
| `tools/bitget/bitget_data_session.cpp` | Bitget `books1` / `publicTrade` data session publisher。 |
| `tools/market_data/data_reader_recorder.cpp` | SHM 到 `BookTicker` / `Trade` 独立 replay binary recorder。 |
| `monitor/market_data/market_data_thread.*` | TUI 专用 market data reader。 |

## 运行边界

- `DataShmPublisher` 是 single producer。
- 多 reader 可以并行 attach 同一 channel。
- recorder 是只读 SHM consumer，可以和 LeadLag / demo / TUI 并行。
- 完整 replay dump 使用临时 `drain` reader config，不要把仓库默认 strategy reader 改成 drain。
- Gate / Binance / Bitget live data session 默认使用 `CLOCK_REALTIME` 记录 `BookTicker.local_ns`，语义是 data session 接入 WebSocket frame 后、进入交易所 parser / decoder 前的本机 Unix epoch ns。
- 比较不同 Gate private IP 行情延迟时，Gate `exchange_ns` 是 SBE `bbo.time` 的 WebSocket server send timestamp，`event_ns` 是 `bbo.t` 的 orderbook engine update timestamp；需要按 data session 连接记录 endpoint / owner CPU，再统计 `exchange_ns -> local_ns`、SHM publish / reader 侧时间、`skipped` / `overruns`。该差值仍受 Gate / 本机时钟偏移和交易所 timestamp 语义影响，只作路径诊断，不单独证明真实单程网络延迟。
- Bitget `books1` 中 `exchange_ns = sts * 1000`，`event_ns = ts * 1000`，`id = seq`；历史 probe /
  fixture 缺少 `sts` 时 decoder 写 `exchange_ns = event_ns`。Bitget `publicTrade` 中
  `exchange_ns = sts * 1000`，`event_ns = ts * 1000`，`id = execId`，group index/count 写入
  `batch_index` / `batch_count`。

## Data Session 延迟诊断分层

Data session 已采用类似 Gate `OrderSession` Ack latency 的编译期分层诊断，但采样点围绕接收路径设计。当前 `BookTicker` ABI 为 72 bytes，包含 `exchange_ns`、`event_ns` 和 `local_ns`；latency outlier log 只在超过阈值时输出结构化 Nova log，帮助区分交易所 timestamp 语义、WebSocket / TCP 接收、本机 userspace、SHM publisher 和 reader 侧丢点。

当前实现入口：

```text
core/common/data_session_diagnostic_level.h
core/market_data/data_session_diagnostics.h
core/websocket/message_view.h
core/websocket/critical_session.h
exchange/gate/market_data/client.h
exchange/binance/market_data/client.h
exchange/bitget/market_data/client.h
exchange/gate/market_data/data_session_config.cpp
exchange/binance/market_data/data_session_config.cpp
exchange/bitget/market_data/data_session_config.cpp
```

分层：

| Level | 关注点 | 当前实现 |
| --- | --- | --- |
| `L0` | 关闭诊断 | 默认值；不改变 `MessageView` 布局，不采集额外 timestamp，不输出 latency outlier log。 |
| `L1` | outlier correlation | 当 `BookTicker.local_ns - BookTicker.exchange_ns > threshold_ns` 时输出 `data_session_book_ticker_latency_outlier`，字段包括 `exchange`、`source_id`、`symbol_id`、`book_ticker_id`、`latency_ns`、`exchange_ns`、`book_ticker_event_ns`、`book_ticker_local_ns`。 |
| `L2` | 本机 userspace read / parse / publish | 在 L1 基础上记录 `drive_read_enter_ns`、`read_return_ns`、`handler_entry_ns`、`parse_done_ns`、`shm_publish_done_ns`，并派生 read / dispatch / parse / publish 分段。 |
| `L3` | Kernel / TCP 状态快照 | 当前只保留 level 名称和编译期边界；尚未实现 data session `TCP_INFO` / recv queue 采样。 |
| `L4` | RX software timestamping | 复用 WebSocket socket timestamping。plain transport 且 `rx_software=true` 时可填 `kernel_rx_ns`；TLS transport 当前不提供 RX software timestamp。 |

运行期配置位于：

```toml
[data_session.diagnostics.latency_outlier]
enabled = true
source_id = 0
threshold_ns = 5000000
max_logs_per_second = 1000

[data_session.diagnostics.timestamping]
enabled = true
rx_software = true
```

当前没有独立 CSV / sidecar binary，也不扩展 replay binary。原因是本阶段只需要判断 `>5ms` tail 主要来自
network / kernel / userspace 哪段；直接按阈值写 log 可以避免在每条行情热路径引入额外写文件或比对成本。
如果后续要对大量 outlier 做离线统计，再把同一组字段迁移成固定 schema CSV。

字段说明统一维护在 `docs/diagnostic_fields.md`。缺失分段使用 `-1`，缺失 timestamp 使用
`available=false`；不要把不可用字段的 `0` 当成真实时间。

后续可选扩展：

1. `L3`：outlier 触发时读取 `TCP_INFO` 和 socket receive queue，用于确认 kernel TCP 状态。
2. Gate / Bitget exchange event timestamp：当前已写入 `BookTicker.event_ns` 并在 L1 log 中输出 `book_ticker_event_ns`；如需进一步诊断，可再派生 `event_to_ws_ns = exchange_ns - event_ns`。
3. 外部证据：对关键 outlier 做 pcap、硬件 timestamp 或多 subscriber / 多 endpoint 对照，用于确认是否发生在本机 NIC、private link、Gate edge 或 Gate app / matching 路径。

## 验证

```bash
ctest --test-dir build/debug -R '(core_market_data|data_session_config|data_reader_config|data_reader_recorder)' --output-on-failure
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/bitget_data_session --config config/data_sessions/bitget_data_session.toml
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
./build/debug/tools/data_reader_recorder --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml --output /home/liuxiang/tmp/live_merged_book_ticker.bin --trade-output /home/liuxiang/tmp/live_merged_trade.bin --mode truncate
git diff --check
```
