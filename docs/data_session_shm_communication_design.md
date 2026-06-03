# Data Session SHM 通讯设计

## 目的

Gate / Binance data session 通过共享内存把标准化 `aquila::BookTicker` 发布给 strategy、recorder、TUI 和 probe 进程。第一版只支持固定大小 `BookTicker`，不引入通用 event ring 或 payload pool。

## 当前方案

核心类型在 `core/market_data/data_shm.h`：

```text
DataShmPublisher
BookTickerShmReader
BookTickerShmManager
BookTickerShmChannel
```

共享队列：

```cpp
inline constexpr std::uint64_t kBookTickerShmCapacity = 65536;

using BookTickerQueue =
    nova::static_impl::SPBroadcastQueue<aquila::BookTicker,
                                        kBookTickerShmCapacity>;
```

设计约束：

- 单个 channel 只有一个 producer。
- producer 不感知 reader 数量。
- producer 不等待 reader，不接受 reader backpressure。
- reader 本地维护 cursor，cursor 不写入 SHM。
- 慢 reader 可能 overrun；reader 会显式计数并拉回当前 ring 窗口。
- 第一版不保证慢 reader 收到所有历史 `BookTicker`。

## SHM Header

`BookTickerShmHeader` 用于 attach 校验和运维诊断：

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
- `sizeof(BookTicker)`。
- capacity。

`published_count` 和 `heartbeat_ns` 只用于诊断，不作为 reader 判断消息可见性的依据。核心发布顺序以 Nova queue `Current()` 为准。

## Producer 协议

data session 的标准 sink 命名为 `DataSink`，SHM 写端是 `DataShmPublisher`。

发布路径：

```text
exchange data session
  -> market data client / decoder
  -> DataShmPublisher
  -> BookTickerQueue
  -> BookTicker SHM
```

接口：

```cpp
void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;

template <typename Writer>
void EmplaceBookTickerWith(Writer&& writer) noexcept;

void FlushPublishedCount() noexcept;
void UpdateHeartbeatNs(std::uint64_t heartbeat_ns) noexcept;
```

热路径约束：

- 只做 fixed-size `BookTicker` 写入和轻量本地计数。
- 不扫描 reader。
- 不更新时间源。
- 不写 heartbeat。
- 不因 reader 慢阻塞 WebSocket read path。

Gate data session 可用 slot-writer fast path，直接在 queue producer slot 中解码 `BookTicker`，减少一次栈上临时对象拷贝。

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

reader 不做多 source 全局时间排序。`RealtimeDataReader` 多 source round-robin 只提供公平轮询，不按 `exchange_ns` / `local_ns` merge。

## 配置

data session 配置指定 SHM 名称和创建方式，容量不在 TOML 暴露：

- Gate：`exchange/gate/market_data/data_session_config.*`
- Binance：`exchange/binance/market_data/data_session_config.*`
- 通用 SHM config：`core/market_data/data_shm_config.h`

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
| `core/market_data/data_shm_config.h` | `BookTickerShmConfig` 和 capacity。 |
| `core/market_data/realtime_data_reader.h` | strategy / tool realtime SHM reader。 |
| `core/market_data/historical_data_reader.h` | binary replay reader。 |
| `tools/gate/data_session.cpp` | Gate data session publisher。 |
| `tools/binance/data_session.cpp` | Binance data session publisher。 |
| `tools/market_data/data_reader_recorder.cpp` | SHM 到 replay binary recorder。 |
| `monitor/market_data/market_data_thread.*` | TUI 专用 market data reader。 |

## 运行边界

- `DataShmPublisher` 是 single producer。
- 多 reader 可以并行 attach 同一 channel。
- recorder 是只读 SHM consumer，可以和 LeadLag / demo / TUI 并行。
- 完整 replay dump 使用临时 `drain` reader config，不要把仓库默认 strategy reader 改成 drain。
- Gate / Binance live data session 默认使用 `CLOCK_REALTIME` 记录 `BookTicker.local_ns`，语义是 data session 接入 WebSocket frame 后、进入交易所 parser / decoder 前的本机 Unix epoch ns。
- 比较不同 Gate private IP 行情延迟时，Gate `exchange_ns` 是 SBE `bbo.time` 的 WebSocket server send timestamp；需要按 data session 连接记录 endpoint / owner CPU，再统计 `exchange_ns -> local_ns`、SHM publish / reader 侧时间、`skipped` / `overruns`。该差值仍受 Gate / 本机时钟偏移和交易所 timestamp 语义影响，只作路径诊断，不单独证明真实单程网络延迟。

## Data Session 延迟诊断分层建议

Data session 也建议采用类似 Gate `OrderSession` Ack latency 的分层诊断，但采样点应围绕接收路径设计。目标不是把所有字段写进 `BookTicker`，而是在保持 64-byte `BookTicker` ABI 不变的前提下，为 outlier 写出独立 diagnostic sample，帮助区分 Gate 时间戳语义、WebSocket / TCP 接收、本机 runtime loop、SHM publisher 和 reader 侧丢点。

建议分层：

| Level | 关注点 | 建议采样内容 |
| --- | --- | --- |
| `L0` | 基础统计 | WebSocket message count、decode count、parse drops、SHM published count、reader `skipped` / `overruns`。 |
| `L1` | 交易所时间戳语义 | Gate SBE `bbo.t` 作为 `gate_engine_ns` 诊断字段；`bbo.time` 继续作为 `BookTicker.exchange_ns` / `gate_ws_send_ns`；派生 `gate_engine_to_ws_ns = gate_ws_send_ns - gate_engine_ns` 和 `gate_ws_send_to_local_ns = local_ns - gate_ws_send_ns`。 |
| `L2` | 本机 userspace runtime / read path | `runtime_loop_enter_ns`、`drive_read_enter_ns` / `drive_read_exit_ns`、`message_dispatch_ns`、`book_ticker_handler_entry_ns`、`local_ns`、`loop_gap_ns`、`message_gap_local_ns`。 |
| `L3` | Kernel / TCP 状态快照 | outlier 触发时读取 `TCP_INFO`，记录 `tcp_info_rtt_us`、`tcp_info_rttvar_us`、`tcp_info_retrans`、`tcp_info_total_retrans`、`tcp_info_unacked` 和 socket receive queue bytes。 |
| `L4` | RX software timestamping | plain transport 成功启用 timestamping 后记录 `rx_software_ns`，派生 `exchange_to_rx_software_ns` 和 `rx_software_to_local_ns`，拆分网络 / kernel delivery 与 kernel-to-userspace 调度处理。 |
| `L5` | 外部证据 | 对关键 outlier 做 pcap、硬件 timestamp 或多 subscriber / 多 endpoint 对照；用于确认是否发生在本机 NIC、private link、Gate edge 或 Gate app / matching 路径。 |

建议独立写一个 data session diagnostic CSV，而不是扩展 replay binary。第一版可以只在阈值触发时写 top tail sample，避免把日志、系统调用或格式化成本带入每条行情热路径。建议配置项：

```text
latency_threshold_ns
max_samples_per_second
sample_every_n
tcp_info_on_outlier
rx_timestamping_enabled
```

建议 sample schema 先保持固定 superset，未启用的 level 字段置空或写 `available=false`，不要把缺失时间戳写成真实 `0`：

```text
sample_index
symbol_id
ticker_id
gate_engine_ns
exchange_ns
local_ns
latency_ns
gate_engine_to_ws_ns
gate_ws_send_to_local_ns
runtime_loop_enter_ns
drive_read_enter_ns
drive_read_exit_ns
message_dispatch_ns
book_ticker_handler_entry_ns
loop_gap_ns
message_gap_local_ns
rx_software_available
rx_software_ns
exchange_to_rx_software_ns
rx_software_to_local_ns
tcp_info_available
tcp_info_rtt_us
tcp_info_rttvar_us
tcp_info_retrans
tcp_info_total_retrans
tcp_info_unacked
recv_queue_bytes
reason
```

落地顺序建议：

1. 先做 `L1` + `L2`：成本低，能直接回答 `exchange_ns -> local_ns` outlier 是否伴随 data session loop gap、message dispatch gap 或 Gate engine-to-WS-send gap。
2. 再做 `L4`：复用 order session socket timestamping 的成功启用 / `available=false` 语义，但只在 plain transport 上记录 RX 侧 attribution。
3. 最后按问题补 `L3` / `L5`：`TCP_INFO`、pcap 和硬件 timestamp 只在 outlier 复现或需要定位 NIC / private link / Gate 侧边界时启用。

## 验证

```bash
ctest --test-dir build/debug -R '(core_market_data|data_session_config|data_reader_config|data_reader_recorder)' --output-on-failure
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
./build/debug/tools/data_reader_recorder --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml --output /home/liuxiang/tmp/live_merged_book_ticker.bin --mode truncate
git diff --check
```
