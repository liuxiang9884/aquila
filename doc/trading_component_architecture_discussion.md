# Trading Component 架构讨论

本文记录当前围绕交易系统核心组件的架构讨论。它是持续讨论文档，不是最终实现计划；后续会按组件逐项细化，再对照当前实现拆分修改任务。

当前讨论范围：

```text
DataReader
OrderSession
OrderFeedbackSession
OrderManager
Strategy
```

如果组合这些组件时需要一个很薄的组装 / 事件分发层，可以增加，但它不应改变业务职责边界，也不应在热路径引入明显性能损耗。优先使用 template、inline direct call、function object 或编译期组合；不要默认引入虚函数或动态分发。

## 全局架构原则

- 先逐个讨论组件设计，再讨论组合方式。
- 每个组件都要明确 owns / consumes / emits / must not do。
- 组件之间通过窄接口协作，不依赖对方内部状态。
- `Strategy` 表达交易逻辑和策略级执行关系，不直接解析交易所协议，不直接维护通用订单池。
- `OrderManager` 是订单状态 owner，统一更新订单状态。
- `OrderSession` 是上行交易通道，负责下单 / 撤单和 ack / response。
- `OrderFeedbackSession` 是下行订单事实通道，负责订单回报。
- `DataReader` 是策略侧行情输入抽象，实时和历史只是不同行情 source，不影响 `Strategy` 接口。

推荐事件顺序：

```text
market data:
  DataReader
    -> Strategy::OnBookTicker(...)

order submit:
  Strategy
    -> OrderManager
    -> OrderSession
    -> exchange place / cancel

order ack / response:
  OrderSession
    -> OrderManager::OnOrderResponse(...)
    -> Strategy::OnOrderResponse(...)

order feedback:
  OrderFeedbackSession
    -> OrderManager::OnOrderFeedback(...)
    -> Strategy::OnOrderFeedback(...)
```

ack / response 和 feedback 都必须先进入 `OrderManager`，再通知 `Strategy`。这样 `Strategy` 收到事件时，通过查询接口看到的是已经更新后的订单状态。

## DataReader 架构

### 已确认约束

- `DataReader` 不负责 merge。
- 实时多路行情如果需要 merge，应由上游 `DataSession` 或行情生产层完成。
- 历史多路行情如果需要 merge，应在 load 数据前由离线数据程序完成。
- 历史数据第一版只支持已经处理好的 binary source；CSV / HDF / parquet 等 source 后续需要时再补。
- `Strategy` 不关心数据来自 SHM、SPSC、broadcast queue 还是 binary file。
- `Diagnostics` 是否启用由模板静态指定；`Diagnostics` 是记录器 / policy，`Stats` 是计数快照，不建议在组件内部命名为 `Metrics`。

### 职责边界

`DataReader` 负责：

- 读取已经标准化的 `BookTicker`。
- 支持多个实时 source 或历史 binary source。
- 按配置控制每次 poll 的事件预算。
- 输出给 handler 的 `OnBookTicker()`。
- 在 diagnostics policy 启用时记录 poll、empty poll、book ticker 数、skipped、overrun、文件完成等统计。

`DataReader` 不负责：

- 连接交易所 WebSocket。
- 解析交易所原始协议。
- 合并多路行情。
- 做策略过滤、风控、订单管理或持仓管理。
- 把 replay 文件从 CSV / HDF 等格式转换为 binary；这些属于离线数据准备程序。

### 推荐拆分

```text
DataReader concept
  - Strategy-facing reader abstraction

LiveDataReader
  - realtime source reader
  - source: SHM broadcast queue or SHM SPSC
  - read mode: latest / drain
  - no natural EOF

ReplayDataReader
  - historical replay reader
  - source: preprocessed binary BookTicker files
  - read mode: drain only
  - files are consumed in configured order
  - finished() indicates EOF
```

当前实现映射：

```text
core/market_data/data_reader.h
  ~= LiveDataReader first version

core/market_data/binary_data_reader.h
  ~= ReplayDataReader / BinaryBookTickerSource first version
```

### Strategy-facing concept

`Strategy` 侧只依赖这个能力：

```cpp
template <typename ReaderT, typename HandlerT>
concept DataReaderLike = requires(ReaderT reader, HandlerT handler) {
  { reader.Poll(handler) } -> std::convertible_to<std::uint64_t>;
};
```

handler 只需要实现：

```cpp
void OnBookTicker(const BookTicker& ticker) noexcept;
```

### Diagnostics / Stats 模板

推荐命名：

```text
DataReaderDiagnostics
NoopDataReaderDiagnostics
DataReaderStats
DataReaderSourceStats
```

推荐形态：

```cpp
struct DataReaderSourceStats {
  std::uint64_t book_tickers{0};
  std::uint64_t skipped{0};
  std::uint64_t overruns{0};
  std::int64_t last_book_ticker_id{0};
};

struct DataReaderStats {
  std::uint64_t poll_calls{0};
  std::uint64_t empty_polls{0};
  std::uint64_t book_tickers{0};
  std::vector<DataReaderSourceStats> sources;
};

class NoopDataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopDataReaderDiagnostics(std::size_t) noexcept {}
  void RecordPoll() noexcept {}
  void RecordEmptyPoll() noexcept {}
  void RecordBookTicker(std::size_t, const BookTicker&) noexcept {}
  void RecordSkipped(std::size_t, std::uint64_t) noexcept {}
  void RecordOverrun(std::size_t, std::uint64_t) noexcept {}
};

class DataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  explicit DataReaderDiagnostics(std::size_t source_count);

  void RecordPoll() noexcept;
  void RecordEmptyPoll() noexcept;
  void RecordBookTicker(std::size_t source_index,
                        const BookTicker& ticker) noexcept;
  void RecordSkipped(std::size_t source_index,
                     std::uint64_t skipped) noexcept;
  void RecordOverrun(std::size_t source_index,
                     std::uint64_t overrun_delta) noexcept;

  [[nodiscard]] const DataReaderStats& stats() const noexcept;
};
```

`DataReader` 内部使用：

```cpp
template <typename Diagnostics = NoopDataReaderDiagnostics>
class LiveDataReader {
 public:
  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept {
    if constexpr (Diagnostics::kEnabled) {
      diagnostics_.RecordPoll();
    }

    // Poll source and emit handler.OnBookTicker(...)
  }

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  [[no_unique_address]] Diagnostics diagnostics_;
};
```

关闭 diagnostics 时，记录逻辑应被编译器消除，不使用 runtime bool。

### LiveDataReader 模板

实时 reader 第一版支持 SHM broadcast queue / SPSC。多 source 轮询采用 round-robin 起点，避免每轮固定偏向第一个 source。

```cpp
enum class LiveReadMode : std::uint8_t {
  kLatest,
  kDrain,
};

struct LiveDataSourceConfig {
  std::string name;
  std::string shm_name;
  std::string channel_name;
  LiveReadMode read_mode{LiveReadMode::kLatest};
  bool required{true};
};

struct LiveDataReaderConfig {
  std::string name;
  std::uint32_t max_events_per_source{64};
  std::vector<LiveDataSourceConfig> sources;
};

template <typename SourceT,
          typename Diagnostics = NoopDataReaderDiagnostics>
class LiveDataReader {
 public:
  explicit LiveDataReader(LiveDataReaderConfig config);

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept;

 private:
  struct SourceState {
    std::string name;
    LiveReadMode read_mode{LiveReadMode::kLatest};
    SourceT source;
    std::uint64_t last_overrun_count{0};
  };

  template <typename Handler>
  std::uint64_t PollLatest(std::size_t source_index, SourceState& source,
                           Handler& handler) noexcept;

  template <typename Handler>
  std::uint64_t PollDrain(std::size_t source_index, SourceState& source,
                          Handler& handler) noexcept;

  std::uint32_t max_events_per_source_{64};
  std::size_t next_source_index_{0};
  std::vector<SourceState> sources_;
  [[no_unique_address]] Diagnostics diagnostics_;
};
```

source 需要提供的能力：

```cpp
struct SourcePollStats {
  std::uint64_t skipped{0};
  std::uint64_t overrun_delta{0};
};

class LiveBookTickerSource {
 public:
  bool TryReadOne(BookTicker* out, SourcePollStats* stats) noexcept;
  bool TryReadLatest(BookTicker* out, SourcePollStats* stats) noexcept;
};
```

语义：

- `latest`：每个 source 每轮最多输出最新一条；允许 skip 中间 tick，适合状态型低延迟策略。
- `drain`：每个 source 每轮最多输出 `max_events_per_source` 条；用于完整事件消费、验证或对账。
- overrun / skipped 只记录诊断，不在 `DataReader` 内部做策略决策。

### ReplayDataReader 模板

历史 replay 第一版只读预处理后的 binary `BookTicker` 文件，不做 merge，不做 CSV。

```cpp
struct ReplayDataReaderConfig {
  std::string name;
  std::uint32_t max_events_per_poll{4096};
  std::vector<std::filesystem::path> files;
};

template <typename Diagnostics = NoopReplayDataReaderDiagnostics>
class ReplayDataReader {
 public:
  explicit ReplayDataReader(ReplayDataReaderConfig config);

  template <typename Handler>
  std::uint64_t Poll(Handler& handler);

  [[nodiscard]] bool finished() const noexcept;

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  BinaryBookTickerSource source_;
  std::uint32_t max_events_per_poll_{4096};
  [[no_unique_address]] Diagnostics diagnostics_;
};
```

binary source：

```cpp
class BinaryBookTickerSource {
 public:
  explicit BinaryBookTickerSource(std::vector<std::filesystem::path> files);

  bool TryReadOne(BookTicker* out);

  [[nodiscard]] bool finished() const noexcept;

 private:
  bool EnsureReadableFile();
  void OpenCurrentFile();
  void CompleteCurrentFile();

  std::vector<std::filesystem::path> files_;
  std::size_t current_file_index_{0};
  std::uint64_t current_records_remaining_{0};
  std::ifstream input_;
};
```

语义：

- 文件按配置顺序读取。
- 输入文件必须已经按目标 replay 顺序预处理好。
- `ReplayDataReader` 不支持 `latest`。
- `Poll() == 0` 在 replay 中可能表示 EOF，因此需要 `finished()` 区分。
- 文件大小必须是 `sizeof(BookTicker)` 的整数倍；否则启动期或打开文件时拒绝。

## OrderSession 架构占位

待后续细化。

当前已确认方向：

- 负责下单、撤单、接收 ack / response。
- 不创建订单对象，不管理完整订单生命周期。
- ack / response 输出后，组合层应先调用 `OrderManager::OnOrderResponse()`，再调用 `Strategy::OnOrderResponse()`。
- ack 只表示交易所接口收到请求，不代表订单已经进入订单簿。

## OrderFeedbackSession 架构占位

待后续细化。

当前已确认方向：

- 负责接收订单回报。
- 不维护 `StrategyOrder`。
- 不直接修改策略 execution state。
- feedback 输出后，组合层应先调用 `OrderManager::OnOrderFeedback()`，再调用 `Strategy::OnOrderFeedback()`。

## OrderManager 架构占位

待后续细化。

当前已确认方向：

- 是订单状态 owner。
- 负责创建 / 查询 / 更新订单。
- 负责消费 ack / response 和 feedback。
- `Strategy` 通过返回的 `local_order_id` 维护策略级 execution group，需要订单状态时通过只读查询接口读取。

## Strategy 架构占位

待后续细化。

当前已确认方向：

- 负责交易逻辑和策略级 execution state。
- 通过 `DataReader` 接收行情。
- 通过 `OrderManager` 发起下单 / 撤单意图。
- 保存自己关心的 `local_order_id`，但不复制通用订单状态机。
- 不把 ack / accepted 当成持仓变化；持仓应由成交 feedback 或后续 position book / reconcile 产生。

## 后续讨论顺序

建议继续按下面顺序细化：

1. `DataReader`：确认 live / replay 类名、config 是否拆分、是否保留当前 `DataReaderConfig` 兼容层。
2. `OrderSession`：定义 place / cancel 输入、ack / response 输出、correlation 和 exchange id cache 边界。
3. `OrderFeedbackSession`：定义 feedback event、gap 语义、transport 和恢复边界。
4. `OrderManager`：定义订单状态机、查询接口、执行关系和 position book 分界。
5. `Strategy`：定义 strategy context、execution group、事件回调和状态查询方式。
6. 组合层：在不影响性能的前提下，定义事件顺序保证和实际组装方式。
