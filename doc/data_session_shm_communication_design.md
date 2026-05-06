# Data session SHM 通讯设计

本文记录 `aquila` data session 到策略进程的第一版共享内存行情通讯设计。目标是复用 Nova
已有 SHM 和 broadcast queue 基础设施，让 Gate / Binance data session 通过现有
`DataSink::OnBookTicker()` 接口把 `aquila::BookTicker` 发布到跨进程共享内存。

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

这里的 capacity 是编译期常量，不由 TOML 在运行时决定。配置可以保留
`expected_capacity = 65536` 用于启动校验，但不能把它解释成动态容量参数。

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
`published_count` 和 `heartbeat_ns` 只用于诊断，producer 使用 relaxed store 更新，不作为 reader
判断消息可见性的依据。

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

SHM 写端实现命名为 `DataShmPublisher`。第一版只实现 `OnBookTicker()`，后续如果增加 trade、
orderbook 或其他行情类型，可以在同一个 publisher 上继续扩展对应的 `OnXxx()` 方法：

```cpp
class DataShmPublisher {
 public:
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
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
    header_->published_count.store(published_count_, std::memory_order_relaxed);
    header_->heartbeat_ns.store(NowNs(), std::memory_order_relaxed);
  }
};
```

现有 data session 不需要改变解码链路：

```text
DataSession
  -> FuturesMarketDataClient
  -> DataShmPublisher::OnBookTicker(BookTicker)
  -> BookTickerQueue::Push / Emplace
```

producer 约束：

- 单个 SHM channel 只有一个 producer。
- producer 不扫描 reader，不维护 reader 列表。
- producer 热路径只做 fixed-size `BookTicker` 写入和轻量统计更新。
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

## 后续实现建议

第一步只落地：

- `BookTickerShmChannel` / `BookTickerShmManager`。
- `DataShmPublisher`，适配现有 Gate / Binance `DataSession<DataSink>`。
- `BookTickerShmReader`，提供 `TryReadOne()`。
- data session tool 增加可选 `--shm-publish` 或 TOML 字段；容量由代码常量固定，配置只做
  `expected_capacity` 校验。
- standalone shm reader/probe，用于验证跨进程读取。

最小验证：

- 单进程 producer/reader 单元测试，覆盖 empty、正常读、`unread_count > capacity` overrun。
- 本地跨进程 smoke：data session producer 写 SHM，reader 进程读到 `BookTicker`。
- release microbenchmark：`OnBookTicker -> queue.Push` 和 `TryReadOne` 成本。

任何性能结论必须记录 benchmark 命令、CPU affinity、构建类型和原始输出。
