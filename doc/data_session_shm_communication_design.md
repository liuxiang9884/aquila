# Data session SHM 通讯设计

本文记录 `aquila` data session 到策略进程的第一版共享内存行情通讯设计。目标是复用 Nova
已有 SHM 和 broadcast queue 基础设施，让 Gate / Binance data session 通过
`DataSink::OnBookTicker()` 或 slot-writer fast path 把 `aquila::BookTicker` 发布到跨进程共享内存。

## 设计目标

- 第一版只支持 `BookTicker`，不引入通用 event ring / payload pool。
- data session 作为 single producer，策略或 reader 进程作为多个 independent reader。
- producer 不感知 reader 数量，不等待 reader，不接受 reader backpressure。
- reader 自己维护本地 read position；reader cursor 不写入 SHM。
- 复用 Nova `ShmAllocator` 和 `SPBroadcastQueue`，避免在第一版引入新的 IPC 基础结构。
- 对 overrun 做显式计数，并把 reader 拉回当前 ring 窗口；第一版不保证慢 reader 收到所有历史
  `BookTicker`。

## Sirius 参考模型

Sirius 的 Gate data SHM 链路是：

```text
DataCenter / DataEngine
  -> DataEventChannelGroup[client_id]
  -> DataBufferGroup[client_id] 或 TradePool[symbol_id]
  -> DataReader / Strategy 轮询 event channel
```

Sirius 的 event channel 只保存 `DataEvent{data_type, symbol_id, offset}`，真实 payload 放在
`DataBuffer` 或 per-symbol pool 中。reader 为每个 channel 维护本地 read position，轮询
`Current()` 到当前位置之间的事件。该实现不对慢 reader 做显式 overrun 检测，也不阻塞
producer。

`aquila` 第一版不照搬两段式 event/payload 结构，因为当前只需要广播固定大小的
`aquila::BookTicker`。直接 typed broadcast ring 更短，也更贴合现有 data session `DataSink`
接口。

## 选定方案

第一版使用 typed broadcast queue：

```cpp
inline constexpr std::uint64_t kBookTickerShmCapacity = 65536;

using BookTickerQueue =
    nova::static_impl::SPBroadcastQueue<aquila::BookTicker,
                                        kBookTickerShmCapacity>;
```

这里的 capacity 是编译期常量，不由 TOML 在运行时决定。配置不暴露 `capacity` 或
`expected_capacity`；创建端把编译期容量写入 header，attach 端用二进制中的编译期常量校验
header。

共享内存对象可以按下面结构组织：

```cpp
#include <atomic>

struct BookTickerShmHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t abi_size;
  std::uint32_t capacity;
  std::uint64_t producer_pid;
  std::uint64_t created_ns;
  std::atomic<std::uint64_t> published_count;
  std::atomic<std::uint64_t> heartbeat_ns;
};

struct BookTickerShmChannel {
  BookTickerShmHeader header;
  BookTickerQueue queue;
};
```

`header` 用于 attach 校验、运行诊断和运维观测；核心发布顺序仍由 Nova queue 的 `Current()` 表示。
创建端写入 `capacity = kBookTickerShmCapacity`，attach 端必须校验 header capacity、二进制编译期
capacity、`BookTicker` ABI size 和版本一致。
`published_count` 和 `heartbeat_ns` 只用于诊断，不作为 reader 判断消息可见性的依据。
`published_count` 是 publisher 对象本地计数；不在 `OnBookTicker()` / `EmplaceBookTickerWith()` 热路径中
写 shared header。需要对外暴露 SHM header 计数时，调用 `FlushPublishedCount()`，或在
`UpdateHeartbeatNs()` 这种 data session 外层冷路径里顺带刷新。

## 命名和接口约定

data session 的第一个模板参数命名为 `DataSink`：

```cpp
template <typename DataSink,
          typename WebSocketPolicy = DefaultTlsWebSocketPolicy,
          typename DiagnosticsPolicy = NoopDataSessionDiagnosticsPolicy>
class DataSession;
```

`DataSink` 表示 data session 解码后的标准化行情数据出口。因为该类位于
`exchange/*/market_data/` 语境中，非 market data 不走这条链路，所以不再使用更长的
`MarketDataSink`。`Consumer` 这个名字不再用于该接口，避免和 SHM reader / consumer
进程概念混淆。

SHM 写端实现命名为 `DataShmPublisher`。第一版支持 `OnBookTicker()` 兼容接口，同时提供
`EmplaceBookTickerWith()` 让解码器可以直接写入 queue 当前 producer slot。后续如果增加 trade、
orderbook 或其他行情类型，可以在同一个 publisher 上继续扩展对应的 `OnXxx()` / `EmplaceXxxWith()`
方法：

```cpp
class DataShmPublisher {
 public:
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;

  template <typename Writer>
  void EmplaceBookTickerWith(Writer&& writer) noexcept;

  void FlushPublishedCount() noexcept;
  void UpdateHeartbeatNs(std::uint64_t heartbeat_ns) noexcept;
};
```

## Producer 协议

producer 是 data session 进程中的 SHM publisher，例如：

```cpp
class DataShmPublisher {
 public:
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    queue_.Push(book_ticker);
    ++published_count_;
  }

  template <typename Writer>
  void EmplaceBookTickerWith(Writer&& writer) noexcept {
    queue_.EmplaceWith(std::forward<Writer>(writer));
    ++published_count_;
  }

  void FlushPublishedCount() noexcept {
    header_->published_count.store(published_count_, std::memory_order_relaxed);
  }

  void UpdateHeartbeatNs(std::uint64_t heartbeat_ns) noexcept {
    FlushPublishedCount();
    header_->heartbeat_ns.store(heartbeat_ns, std::memory_order_relaxed);
  }
};
```

Gate data session 使用 slot-writer fast path 时，解码结果直接写入 shm queue slot，完成后再由
Nova queue release-store `Current()` 对 reader 可见：

```text
DataSession
  -> FuturesMarketDataClient
  -> DataShmPublisher::EmplaceBookTickerWith(writer)
  -> BookTickerQueue::EmplaceWith(writer)
  -> DecodeBookTickerWithHeader(..., BookTicker& slot)
  -> Current() release-store publish
```

不支持 `EmplaceBookTickerWith()` 的普通 `DataSink` 仍走兼容路径：先解码到栈上 `BookTicker`，再调用
`OnBookTicker(const BookTicker&)`。

producer 约束：

- 单个 SHM channel 只有一个 producer。
- producer 不扫描 reader，不维护 reader 列表。
- producer 热路径只做 fixed-size `BookTicker` 写入和轻量统计更新。
- producer 热路径不读取时间源，不更新 `heartbeat_ns`。
- producer 不因为 reader 慢而阻塞 WebSocket read path。

## Reader 协议

reader attach 同一个 SHM channel 后，本地保存：

```cpp
std::uint64_t read_pos_{0};
std::uint64_t overrun_count_{0};
```

启动时默认从最新位置开始：

```cpp
read_pos_ = queue_.Current();
```

调试或回放场景可以从当前 ring 可见窗口的最早位置开始：

```cpp
const auto current = queue_.Current();
read_pos_ = current > queue_.capacity() ? current - queue_.capacity() : 0;
```

第一版 reader 使用完整 `capacity` 窗口，只在 reader 已经落后超过 capacity 时判定 overrun：

```cpp
bool TryReadOne(aquila::BookTicker* out) {
  const auto current = queue_.Current();

  if (read_pos_ == current) {
    return false;
  }

  const auto capacity = queue_.capacity();
  const auto unread_count = current - read_pos_;

  if (unread_count > capacity) {
    read_pos_ = current - capacity;
    ++overrun_count_;
  }

  *out = queue_.Value(read_pos_);
  ++read_pos_;
  return true;
}
```

典型 reader loop：

```cpp
aquila::BookTicker book_ticker;
while (reader.TryReadOne(&book_ticker)) {
  consumer.OnBookTicker(book_ticker);
}
```

reader 约束：

- reader 必须 copy `Value()`，不要持有 queue slot 的 `Ref()`。
- reader 本地 cursor 不写 SHM。
- `unread_count > capacity` 时，reader 状态已经落后当前 ring 窗口；必须把 `read_pos_` 拉回
  `current - capacity`，否则会继续按旧 sequence 读取已经被覆盖的 slot。

## 当前方案的已知边界

当前选定方案暂不处理：

```text
current - read_pos_ == capacity
```

此时 reader 读取的是当前 ring 中最老的一条已发布数据。该数据按完整 capacity 语义仍在 ring
里，但 producer 下一条消息会覆盖同一个 slot。若 producer 已经开始写下一条，但还没发布新的
`Current()`，reader 只看全局 `Current()` 无法判断该 slot 是否正在被覆盖。

因此在极端边界上，reader 可能读到：

- 旧 `BookTicker`。
- 新 `BookTicker`。
- 半旧半新的 torn payload。

第一版接受这个边界风险，原因是：

- 第一版只承载 latest-BBO 语义的 `BookTicker`，不是 trade、orderbook diff 或 order feedback。
- producer 不被 reader 背压是 data session 主路径更重要的约束。
- 后续可以通过更严格的 slot sequence 方案升级，而不改变上层 `DataSink::OnBookTicker()` 抽象。

## 讨论过的边界处理方案

### 完整 capacity + `Current()`，只处理 `> capacity`

这是当前选定方案。

```text
current - read_pos_ < capacity   正常读
current - read_pos_ == capacity  仍尝试读边界数据
current - read_pos_ > capacity   overrun，跳到 current - capacity
```

优点：

- reader 不主动遗漏仍在 ring 里的边界数据。
- 完全复用 Nova 当前 `SPBroadcastQueue` 语义。
- 实现最小，producer 热路径不增加额外同步。

缺点：

- `== capacity` 的边界 slot 可能正在被下一条未发布消息覆盖。

### `capacity - 1` 保守窗口

```text
current - read_pos_ < capacity   正常读
current - read_pos_ >= capacity  overrun，跳到 current - (capacity - 1)
```

优点：

- 不读取即将被 producer 下一条消息覆盖的边界 slot。
- 不需要修改 Nova queue。

缺点：

- 会主动遗漏一条已经发布、但处在边界上的数据。
- 如果 reader 更慢，跳转时遗漏的不止一条。

该方案被放弃，因为当前希望尽量不主动丢已发布的 `BookTicker`。

### 遇到 `== capacity` 时等待下一轮

```text
current - read_pos_ == capacity 时不读，等待下一次 poll
```

优点：

- 避免立即读取边界 slot。

缺点：

- 如果 producer 暂停，reader 会卡在边界数据上。
- 如果 producer 继续写，下一轮通常会变成 `> capacity`，最终仍然跳过该边界数据。

该方案只是延迟丢弃，不能真正保留边界数据。

### Per-slot sequence

每个 slot 增加独立 sequence：

```cpp
struct Slot {
  std::atomic<std::uint64_t> sequence;
  aquila::BookTicker payload;
};
```

reader 通过读前/读后检查 slot sequence 判断 payload 是否对应目标 sequence，且是否在拷贝过程中被覆盖。

优点：

- 可使用完整 capacity。
- 不主动丢边界数据。
- 能明确判断 slot 当前装的是哪条消息。
- 更适合未来 trade、orderbook diff、order feedback 这类顺序敏感数据流。

缺点：

- producer 每条消息多 1 到 2 次 atomic store。
- 每个 slot 多 sequence 字段，增加内存和 cache footprint。
- 需要新增或扩展 queue 实现。

该方案作为后续升级方向保留。

### 全局 writing index

在 queue 层增加一个全局 atomic，记录 producer 当前正在写的 slot index：

```cpp
std::atomic<std::uint64_t> writing_index;
```

reader 读前/读后检查目标 index 是否等于 `writing_index`。

优点：

- 比 per-slot sequence 少占用 slot 内存。
- 能识别“目标 slot 正在被写”的情况。

缺点：

- 多 reader 高频读取同一个全局 atomic，容易形成共享热点。
- 只能说明 slot 正在写，不能证明 slot 当前 payload 对应哪个 publish sequence。
- 仍需要依赖 `Current()` 判断 overrun。

该方案不作为第一版实现。

### 全局 seqlock

producer 写任意 slot 时更新全局 epoch，reader 拷贝前后检查 epoch 是否一致。

优点：

- 能防止读到任意写入中的 payload。

缺点：

- producer 写任意 slot 都会让所有 reader 对任意 slot 的读产生 retry。
- 粒度太粗，不适合低延迟行情 fanout。

该方案不建议采用。

### SPSC queue

SPSC queue 通过共享 `head/tail` 防止 producer 覆盖 consumer 尚未 pop 的 slot。

优点：

- 不存在 broadcast queue 的慢 reader 覆盖问题。

缺点：

- consumer 慢会让 producer spin 或 `TryPush` 失败。
- 一对一，不适合 data session 对多个策略/reader 的 fanout。
- 背压会污染 WebSocket read path。

该方案不适合 market data broadcast 主路径。

## 已落地实现

第一版代码入口：

- `core/market_data/data_shm_config.h`：固定 `kBookTickerShmCapacity = 65536` 和
  `BookTickerShmConfig`。
- `core/market_data/data_shm.h`：`BookTickerShmChannel`、`BookTickerShmManager`、
  `DataShmPublisher` 和 `BookTickerShmReader`。
- `exchange/gate/market_data/client.h`：当 `DataSink` 支持 `EmplaceBookTickerWith()` 时直接把
  Gate SBE `BookTicker` 解码到 sink-owned slot；否则保留 `OnBookTicker()` 兼容路径。
- `exchange/binance/market_data/client.h`：当 `DataSink` 支持 `EmplaceBookTickerWith()` 时直接把
  Binance JSON `BookTickerUpdate` 映射到 sink-owned slot；否则保留 `OnBookTicker()` 兼容路径。
- `exchange/gate/market_data/data_session_config.*` 和
  `exchange/binance/market_data/data_session_config.*`：解析 `[data_shm_sink]`。
- `tools/gate/data_session.cpp` 和 `tools/binance/data_session.cpp`：配置开启时使用
  `DataShmPublisher`，否则继续使用 `CountingDataSink`。
- `tools/market_data/book_ticker_shm_reader.cpp`：独立 SHM reader/probe。
- `test/core/market_data/data_shm_test.cpp`：producer / reader / direct emplace / overrun / heartbeat
  单元测试。
- `test/exchange/gate/market_data/futures_market_data_client_test.cpp`：Gate client slot-writer sink
  fast path 单元测试。
- `test/exchange/binance/market_data/futures_market_data_client_test.cpp`：Binance client slot-writer
  sink fast path 单元测试。
- `benchmark/core/market_data/data_shm_benchmark.cpp`：publisher temp+push、direct emplace 和 reader
  microbenchmark。
- `benchmark/exchange/gate/market_data/futures_market_data_benchmark.cpp`：Gate decode+push 与
  decode-into-slot benchmark。
- `benchmark/exchange/binance/market_data/futures_market_data_benchmark.cpp`：Binance parse+push 与
  parse-into-slot benchmark。

配置入口为 `[data_shm_sink]`：

```toml
[data_shm_sink]
enabled = true
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
create = true
remove_existing = false
```

`capacity` 和 `expected_capacity` 字段都不支持；如果配置中出现这些字段，解析会失败。容量只由
代码常量 `kBookTickerShmCapacity` 决定，并通过 SHM header 校验。

`enabled = true` 表示当前 data session 进程选择 `DataShmPublisher` 作为唯一 `DataSink`；未配置或
`enabled = false` 时选择 `CountingDataSink`。第一版不做 composite/fanout sink，同一进程内不会同时运行
`CountingDataSink` 和 `DataShmPublisher`。

## 验证记录

本轮实现后的最小验证命令：

```bash
cmake --build build/debug --target core_market_data_shm_test data_session_config_test gate_data_session_test binance_data_session_test gate_data_session binance_data_session book_ticker_shm_reader
./build/debug/test/core/market_data/core_market_data_shm_test
./build/debug/test/exchange/gate/market_data/gate_futures_market_data_client_test
./build/debug/test/exchange/binance/market_data/binance_futures_market_data_client_test
./build/debug/test/config/data_session_config_test
./build/debug/test/exchange/gate/market_data/gate_data_session_test
./build/debug/test/exchange/binance/market_data/binance_data_session_test
cmake --build build/release --target data_shm_benchmark gate_futures_market_data_benchmark binance_futures_market_data_benchmark
./build/release/benchmark/core/market_data/data_shm_benchmark --benchmark_filter='data_shm/publisher_(temp_book_ticker_push|emplace_book_ticker_with)' --benchmark_min_time=0.2s --benchmark_repetitions=5
./build/release/benchmark/exchange/gate/market_data/gate_futures_market_data_benchmark --benchmark_filter='gate_market_data/decode_book_ticker_(then_shm_push|into_shm_slot)' --benchmark_min_time=0.2s --benchmark_repetitions=5
./build/release/benchmark/exchange/binance/market_data/binance_futures_market_data_benchmark --benchmark_filter='binance_market_data/parse_book_ticker_(then_shm_push|into_shm_slot)' --benchmark_min_time=0.2s --benchmark_repetitions=5
git diff --check
```

2026-05-19 将 shared header `published_count` 写入移出 publisher 热路径后，release benchmark 原始输出摘要：

```text
data_shm/publisher_temp_book_ticker_push_mean              7.50 ns CPU  7.50 ns
data_shm/publisher_emplace_book_ticker_with_mean           4.25 ns CPU  4.25 ns
gate_market_data/decode_book_ticker_then_shm_push_mean    12.7  ns CPU 12.7  ns
gate_market_data/decode_book_ticker_into_shm_slot_mean     4.26 ns CPU  4.26 ns
binance_market_data/parse_book_ticker_then_shm_push_mean   183   ns CPU 183   ns
binance_market_data/parse_book_ticker_into_shm_slot_mean   185   ns CPU 185   ns
```

2026-05-19 复核 `BookTickerShmReader` overrun 边界后，reader 恢复为完整 capacity 窗口：`unread_count == capacity`
仍读取当前 ring 中最老的一条已发布 `BookTicker`，只有 `unread_count > capacity` 才记录 overrun 并拉回
`current - capacity`。这次改动是语义恢复，不作为性能优化；若未来要严格避免 `== capacity` 边界读到正在被
producer 覆盖的 slot，应升级 per-slot sequence，而不是主动丢掉已发布 BBO。

本轮验证命令：

```bash
cmake --build build/debug --target core_market_data_shm_test -j8
./build/debug/test/core/market_data/core_market_data_shm_test
ctest --test-dir build/debug -R 'core_market_data_(shm|realtime_data_reader)' --output-on-failure
cmake --build build/release --target data_shm_benchmark data_reader_benchmark -j8
./build/release/benchmark/core/market_data/data_shm_benchmark --benchmark_filter=BM_BookTickerShmReaderTryReadOne --benchmark_min_time=0.1s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
./build/release/benchmark/core/market_data/data_reader_benchmark --benchmark_filter=BM_RealtimeDataReaderEmptyPoll --benchmark_min_time=0.1s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
git diff --check
```

benchmark 对比：

| case | 保守窗口记录 | 完整 capacity 恢复后 | 结论 |
| --- | ---: | ---: | --- |
| `BM_BookTickerShmReaderTryReadOne_mean` | 1.94ns | 1.95ns | 基本持平 |
| `BM_RealtimeDataReaderEmptyPoll/1_mean` | 1.52ns | 1.21ns | 不触发 overrun 边界，不能归因于本次改动 |
| `BM_RealtimeDataReaderEmptyPoll/2_mean` | 2.29ns | 2.29ns | 持平 |
| `BM_RealtimeDataReaderEmptyPoll/4_mean` | 3.97ns | 4.04ns | 基本持平 |

上述 benchmark 输出只作为本机本次运行记录，不单独推导跨机器性能结论。
