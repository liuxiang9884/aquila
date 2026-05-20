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
- 多 source `round-robin` 只用于实时 reader 的公平调度，避免固定 source 长期优先；它不是时间顺序语义，不按
  `exchange_ns` / `local_ns` 做全局排序。
- 对 LeadLag 这类对 lead / lag tick 顺序敏感的策略，live 模式可以使用 `round-robin` 作为低成本公平调度，但不能
  依赖它复现严格事件时间顺序；如果策略研发要求严格排序，应让上游 producer 产出已 merge 的统一流，或在 replay
  前离线生成目标顺序的 binary source。
- `Strategy` 不关心数据来自 SHM、SPSC、broadcast queue 还是 binary file。
- `Diagnostics` 是否启用由模板静态指定；`Diagnostics` 是记录器 / policy，`Stats` 是计数快照，不建议在组件内部命名为 `Metrics`。
- `poll_calls` / `empty_polls` 描述外层轮询循环行为，不属于 `DataReader` 内部数据流统计；如果需要，应放到 runtime / scheduler / 组装层 diagnostics。

### 职责边界

`DataReader` 负责：

- 读取已经标准化的 `BookTicker`。
- 支持多个实时 source，或一个历史 binary source 中的多个预处理文件。
- 按配置控制每次 poll 的事件预算。
- 输出给 handler 的 `OnBookTicker()`。
- 在 diagnostics policy 启用时记录 book ticker 数、skipped、overrun、最后行情 id、文件完成等数据流统计。

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

RealtimeDataReader
  - realtime source reader
  - source: SHM broadcast queue or SHM SPSC
  - read mode: latest / drain
  - no natural EOF

HistoricalDataReader
  - historical replay reader
  - source: preprocessed binary BookTicker files
  - read mode: drain only
  - files are consumed in configured order
  - finished() indicates EOF
```

当前实现映射：

```text
core/market_data/realtime_data_reader.h
  ~= RealtimeDataReader first version

core/market_data/historical_data_reader.h
  ~= HistoricalDataReader / BinaryBookTickerSource first version
```

关系定义：

```text
DataReader
  - concept / 接口约束 / Strategy-facing capability
  - 不要求是虚基类，也不要求存在一个运行时多态 base class

RealtimeDataReader
  - DataReader concept 的实时实现
  - 从 SHM broadcast queue / SHM SPSC 等标准化实时 source 读取

HistoricalDataReader
  - DataReader concept 的历史回放实现
  - 从预处理后的 binary BookTicker source 读取
```

`DataReader` 这个名字在架构层表示能力边界，而不是必须落成一个继承体系。关键路径优先使用 concept / template / inline direct call 保证接口一致，避免让 `Poll()` 和 handler dispatch 变成虚调用。

### Strategy-facing concept

`Strategy` 侧只依赖这个能力：

```cpp
template <typename ReaderT, typename HandlerT>
concept PollDataReaderLike = requires(ReaderT& reader, HandlerT& handler) {
  { reader.Poll(handler) } -> std::convertible_to<std::uint64_t>;
};

template <typename ReaderT, typename HandlerT>
concept DrainCapableDataReader = requires(ReaderT& reader, HandlerT& handler,
                                          std::uint64_t max_events) {
  { reader.Drain(handler, max_events) } -> std::convertible_to<std::uint64_t>;
};

template <typename ReaderT, typename HandlerT>
concept DataReaderLike =
    PollDataReaderLike<ReaderT, HandlerT> &&
    DrainCapableDataReader<ReaderT, HandlerT>;
```

handler 只需要实现：

```cpp
void OnBookTicker(const BookTicker& ticker) noexcept;
```

如果调用方需要判断 reader 是否天然有限，可以额外约束：

```cpp
template <typename ReaderT>
concept FiniteDataReader = requires(const ReaderT& reader) {
  { ReaderT::kFiniteDataReader } -> std::convertible_to<bool>;
  { reader.finished() } -> std::convertible_to<bool>;
} && ReaderT::kFiniteDataReader;
```

`RealtimeDataReader` 满足 `DataReaderLike`，但没有天然 EOF，不提供 `kFiniteDataReader` 和 `finished()`；`HistoricalDataReader` 显式声明 `static constexpr bool kFiniteDataReader = true`，并提供 `finished()`，因此满足 `DataReaderLike` 和 `FiniteDataReader`。单纯提供 `finished()` 不会让 live-like reader 被误判为 finite reader。

### Poll / Drain 统一语义

`RealtimeDataReader` 和 `HistoricalDataReader` 都提供 `Poll()` 和 `Drain()`：

```text
Poll(handler)
  - 单事件接口。
  - 每次最多输出 1 条事件。
  - 输出 1 条时返回 1。
  - 没有输出时返回 0。

Drain(handler, max_events)
  - 批量接口。
  - 循环调用 Poll(handler)，最多输出 max_events 条事件。
  - 返回实际输出的事件数量。
```

二者的差异只在 EOF：

```text
RealtimeDataReader
  - Poll() == 0 表示当前没有实时数据，不代表结束。
  - 不提供 finished()。

HistoricalDataReader
  - Poll() == 0 表示 replay 已结束。
  - finished() 是显式 EOF 状态查询，应与 Poll() == 0 的 EOF 语义一致。
```

`Drain()` 的默认实现可以由 reader 直接内联提供：

```cpp
template <typename Handler>
std::uint64_t Drain(Handler& handler, std::uint64_t max_events) {
  std::uint64_t count = 0;
  while (count < max_events) {
    const std::uint64_t n = Poll(handler);
    if (n == 0) {
      break;
    }
    count += n;
  }
  return count;
}
```

第一版约束：

- 不在 `Poll()` 中表达批量预算。
- 不在 replay reader 中保留 `max_events_per_poll`。
- 批量消费预算由 `Drain(handler, max_events)` 的调用方传入。
- `Drain(handler, 0)` 返回 0，不读取数据。

### Diagnostics / Stats 模板

推荐命名：

```text
RealtimeDataReaderDiagnostics
NoopRealtimeDataReaderDiagnostics
RealtimeDataReaderStats
RealtimeDataReaderSourceStats

HistoricalDataReaderDiagnostics
NoopHistoricalDataReaderDiagnostics
HistoricalDataReaderStats
```

推荐形态：

```cpp
struct RealtimeDataReaderSourceStats {
  std::uint64_t book_ticker_count{0};
  std::uint64_t skipped{0};
  std::uint64_t overruns{0};
  std::int64_t last_book_ticker_id{0};
};

struct RealtimeDataReaderStats {
  std::uint64_t total_count{0};
  std::vector<RealtimeDataReaderSourceStats> sources;
};

struct HistoricalDataReaderStats {
  std::uint64_t total_count{0};
  std::uint64_t files_completed{0};
};

class NoopRealtimeDataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = false;

  explicit NoopRealtimeDataReaderDiagnostics(std::size_t) noexcept {}
  void RecordBookTicker(std::size_t, const BookTicker&) noexcept {}
  void RecordSkipped(std::size_t, std::uint64_t) noexcept {}
  void RecordOverrun(std::size_t, std::uint64_t) noexcept {}
};

class RealtimeDataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  explicit RealtimeDataReaderDiagnostics(std::size_t source_count);

  void RecordBookTicker(std::size_t source_index,
                        const BookTicker& ticker) noexcept;
  void RecordSkipped(std::size_t source_index,
                     std::uint64_t skipped) noexcept;
  void RecordOverrun(std::size_t source_index,
                     std::uint64_t overrun_delta) noexcept;

  [[nodiscard]] const RealtimeDataReaderStats& stats() const noexcept;
};

class NoopHistoricalDataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = false;

  void RecordBookTicker(const BookTicker&) noexcept {}
  void RecordFileCompleted() noexcept {}
};

class HistoricalDataReaderDiagnostics {
 public:
  static constexpr bool kEnabled = true;

  void RecordBookTicker(const BookTicker& ticker) noexcept;
  void RecordFileCompleted() noexcept;

  [[nodiscard]] const HistoricalDataReaderStats& stats() const noexcept;
};
```

`poll_calls` / `empty_polls` 不放在 `RealtimeDataReaderStats` 或 `HistoricalDataReaderStats` 中。它们回答的是“外层循环调用 reader 多少次”和“外层循环有多少次没有拿到任何事件”，更适合放在 `TradingRuntime`、scheduler 或未来的组装层 diagnostics。

第一版 stats 字段语义：

```text
RealtimeDataReaderStats::total_count
  - live reader 已经输出给 handler 的所有 source 数据总数。
  - 当前只实现 BookTicker，因此它等于所有 source 的 book_ticker_count 之和。

RealtimeDataReaderSourceStats::book_ticker_count
  - 单个 source 已经输出给 handler 的 BookTicker 数量。

RealtimeDataReaderSourceStats::skipped
  - 单个 source 在 latest 模式下为读取最新值而跳过的中间 BookTicker 数量。

RealtimeDataReaderSourceStats::overruns
  - 单个 source 底层 SHM / queue 发生 overrun 的累计增量。

RealtimeDataReaderSourceStats::last_book_ticker_id
  - 单个 source 最后一次成功输出的 BookTicker.id。

HistoricalDataReaderStats::total_count
  - replay reader 已经输出给 handler 的所有数据总数。
  - 当前只实现 binary BookTicker replay，因此它等于 replay 输出的 BookTicker 数量。

HistoricalDataReaderStats::files_completed
  - replay reader 已经完整读完的 binary 文件数。
```

未来扩展 `trade` / `order book` 时，顶层 `total_count` 仍表示 reader 输出给 handler 的所有事件总数，不按 feed 类型拆分；feed 类型相关统计放在 `SourceStats` 中，例如：

```cpp
struct RealtimeDataReaderSourceStats {
  std::uint64_t book_ticker_count{0};
  std::uint64_t trade_count{0};
  std::uint64_t order_book_count{0};

  std::uint64_t skipped{0};
  std::uint64_t overruns{0};

  std::int64_t last_book_ticker_id{0};
  std::int64_t last_trade_id{0};
  std::int64_t last_order_book_id{0};
};
```

`HistoricalDataReaderStats` 的顶层也保持 `total_count` / `files_completed`。如果未来 replay 同时支持多 feed 且需要区分 feed 数量，应优先增加 replay source / feed 维度的 stats，而不是把顶层拆成多个 `total_*_count`。

`RealtimeDataReader` 内部使用：

```cpp
template <typename Diagnostics = NoopRealtimeDataReaderDiagnostics>
class RealtimeDataReader {
 public:
  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept {
    // Poll source and emit handler.OnBookTicker(...)
  }

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    std::uint64_t count = 0;
    while (count < max_events) {
      const std::uint64_t n = Poll(handler);
      if (n == 0) {
        break;
      }
      count += n;
    }
    return count;
  }

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  [[no_unique_address]] Diagnostics diagnostics_;
};
```

关闭 diagnostics 时，记录逻辑应被编译器消除，不使用 runtime bool。

### RealtimeDataReader 模板

实时 reader 第一版支持 SHM broadcast queue / SPSC。多 source 轮询采用 round-robin 起点，避免每轮固定偏向第一个 source。

```cpp
enum class LiveReadMode : std::uint8_t {
  kLatest,
  kDrain,
};

struct RealtimeDataSourceConfig {
  std::string name;
  std::string shm_name;
  std::string channel_name;
  LiveReadMode read_mode{LiveReadMode::kLatest};
  bool required{true};
};

struct RealtimeDataReaderConfig {
  std::string name;
  std::vector<RealtimeDataSourceConfig> sources;
};

template <typename SourceT,
          typename Diagnostics = NoopRealtimeDataReaderDiagnostics>
class RealtimeDataReader {
 public:
  explicit RealtimeDataReader(RealtimeDataReaderConfig config);

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept;

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept {
    std::uint64_t count = 0;
    while (count < max_events) {
      const std::uint64_t n = Poll(handler);
      if (n == 0) {
        break;
      }
      count += n;
    }
    return count;
  }

 private:
  struct SourceState {
    std::string name;
    LiveReadMode read_mode{LiveReadMode::kLatest};
    SourceT source;
    std::uint64_t last_overrun_count{0};
  };

  template <typename Handler>
  std::uint64_t PollSource(std::size_t source_index, SourceState& source,
                           Handler& handler) noexcept;

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

- 构造期要求至少一个 source；空 source 配置是启动期配置错误，直接拒绝，不进入运行循环。
- `Poll()`：从 `next_source_index_` 开始 round-robin 扫描 source；找到第一个可读 source 后输出 1 条并返回 1；所有 source 都没数据时返回 0。
- 多 source round-robin 可以在构造期生成扫描表，热路径按表线性扫描，避免每次递增 index 后判断是否 wrap 到 0。

#### 多 feed source 未来实现

当前实现只有 `BookTickerShmReader`，因此 `RealtimeDataReader` 可以用一个 owning `sources_` vector 保存同构 `Source`。未来加入
`TradeShmReader` 和 `OrderBookShmReader` 后，source 会变成异构对象；第一选择不是引入虚基类，也不是把
`std::variant` 放进热路径，而是采用 typed storage + unified scan table：

```cpp
struct BookTickerSource {
  BookTickerShmReader reader;
  std::uint64_t last_overrun_count{0};
};

struct TradeSource {
  TradeShmReader reader;
  std::uint64_t last_overrun_count{0};
};

struct OrderBookSource {
  OrderBookShmReader reader;
  std::uint64_t last_overrun_count{0};
};

enum class SourcePollKind : std::uint8_t {
  kBookTickerLatest,
  kBookTickerDrain,
  kTradeDrain,
  kOrderBookLatest,
  kOrderBookDrain,
};

struct SourceRef {
  SourcePollKind kind;
  std::size_t source_index;
  void* source;
};

class RealtimeDataReader {
 private:
  std::vector<std::unique_ptr<BookTickerSource>> book_ticker_sources_;
  std::vector<std::unique_ptr<TradeSource>> trade_sources_;
  std::vector<std::unique_ptr<OrderBookSource>> order_book_sources_;

  std::vector<SourceRef> scan_sources_;
  std::size_t next_source_index_{0};
  std::size_t source_count_{0};
};
```

这里的 owning storage 按 feed 类型分开，保持 reader 对象强类型；`scan_sources_` 只作为统一 round-robin 调度表。多 source
双表扫描优化仍可保留：构造期把 `SourceRef` 表重复一轮，`Poll()` 继续按 `[next_source_index_, next_source_index_ + source_count)`
线性扫描。

热路径 dispatch 使用 `SourcePollKind` 合并 feed 和 read mode，避免先 switch feed 再 switch read mode：

```cpp
template <typename Handler>
std::uint64_t Poll(Handler& handler) noexcept {
  const std::size_t scan_end = next_source_index_ + source_count_;
  for (std::size_t scan_position = next_source_index_;
       scan_position < scan_end; ++scan_position) {
    const SourceRef& ref = scan_sources_[scan_position];

    std::uint64_t handled = 0;
    switch (ref.kind) {
      case SourcePollKind::kBookTickerLatest:
        handled = PollBookTickerLatest(ref, handler);
        break;
      case SourcePollKind::kBookTickerDrain:
        handled = PollBookTickerDrain(ref, handler);
        break;
      case SourcePollKind::kTradeDrain:
        handled = PollTradeDrain(ref, handler);
        break;
      case SourcePollKind::kOrderBookLatest:
        handled = PollOrderBookLatest(ref, handler);
        break;
      case SourcePollKind::kOrderBookDrain:
        handled = PollOrderBookDrain(ref, handler);
        break;
    }

    if (handled != 0) {
      next_source_index_ = scan_sources_[scan_position + 1].source_index;
      return handled;
    }
  }
  return 0;
}
```

read mode 约束：

- `book_ticker` 可支持 `latest` / `drain`。
- `trade` 第一版只支持 `drain`；trade 是事件流，`latest` 会主动丢成交事件。
- `order_book` 如果是 data session 已处理好的固定深度快照 / state，可支持 `latest` / `drain`；如果是 delta，只允许 `drain`，且 gap / rebuild 仍归 data session 或专门 order book builder，不放进 `RealtimeDataReader`。
- source `latest`：被 `Poll()` 选中时调用 `TryReadLatest()`，最多输出最新一条；允许 skip 中间 tick，适合状态型低延迟策略。
- source `drain`：被 `Poll()` 选中时调用 `TryReadOne()`，最多输出下一条；不在单次 `Poll()` 内批量吞掉一个 source。
- reader `Drain(handler, max_events)`：在 reader 层循环调用 `Poll()`，最多输出 `max_events` 条；用于完整事件消费、验证或对账。
- overrun / skipped 只记录诊断，不在 `DataReader` 内部做策略决策。

### HistoricalDataReader 模板

历史 replay 第一版只读预处理后的 binary `BookTicker` 文件，不做 merge，不做 CSV。

```cpp
struct HistoricalDataReaderConfig {
  std::string name;
  std::vector<std::filesystem::path> files;
};

template <typename Diagnostics = NoopHistoricalDataReaderDiagnostics>
class HistoricalDataReader {
 public:
  explicit HistoricalDataReader(HistoricalDataReaderConfig config);

  template <typename Handler>
  std::uint64_t Poll(Handler& handler);

  template <typename Handler>
  std::uint64_t Drain(Handler& handler, std::uint64_t max_events) {
    std::uint64_t count = 0;
    while (count < max_events) {
      const std::uint64_t n = Poll(handler);
      if (n == 0) {
        break;
      }
      count += n;
    }
    return count;
  }

  [[nodiscard]] bool finished() const noexcept;

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  BinaryBookTickerSource source_;
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
  void PrepareCurrentFile();
  void CompleteCurrentFile();

  struct FileState {
    std::filesystem::path path;
    std::uint64_t record_count{0};
    MappedFile mapping;
  };

  std::vector<FileState> files_;
  std::size_t current_file_index_{0};
  std::uint64_t current_records_remaining_{0};
  const char* current_cursor_{nullptr};
};
```

语义：

- 文件按配置顺序读取。
- 输入文件必须已经按目标 replay 顺序预处理好。
- 第一版只接受一个 `binary_file` source；多日 / 分片 replay 用同一个 source 下的多个 `files` 表达。
- 如果需要多交易所 / 多 feed 历史 merge，必须在离线数据程序中先合成一个目标 replay binary source。
- `HistoricalDataReader` 不支持 `latest`。
- `Poll()` 每次最多输出 1 条事件；输出成功返回 1。
- `Poll() == 0` 表示 replay 已结束，且 `finished() == true`。
- `Drain(handler, max_events)` 在 reader 层循环调用 `Poll()`，最多输出 `max_events` 条。
- 文件大小必须是 `sizeof(BookTicker)` 的整数倍；否则启动期拒绝。
- 非空文件在构造期完成 read-only mmap 和大小复核，`Poll()` / `Drain()` 热路径从当前 cursor 拷贝
  `BookTicker` 并推进状态，不打开文件、不抛出异常；空文件不 mmap，只在构造期或文件边界计入完成。

### 当前实现对照

当前实现已完成目标语义改造，并已完成实现类命名整理：

```text
core/market_data/realtime_data_reader.h
  - 类名是 RealtimeDataReader，承担实时 SHM reader 第一版职责。
  - 构造期拒绝空 sources，并校验每个 source 必须是 shm / book_ticker，start_position 和 read_mode 必须是已知枚举值；Poll() 语义不依赖空 reader 分支，当前保留冗余空检查是基于本轮 A/B benchmark 的代码生成结果。
  - 已提供 Poll(handler) 单事件接口和 Drain(handler, max_events) 批量接口。
  - Poll() 从 next_source_index_ 开始 round-robin 扫描 source，最多输出 1 条；多 source 路径使用构造期双倍 scan table，消除循环内 wrap 分支。
  - source read_mode = latest 时调用 TryReadLatest()。
  - source read_mode = drain 时调用 TryReadOne()。
  - stats 已移除 poll_calls / empty_polls，使用 total_count 和 per-source book_ticker_count。

core/market_data/historical_data_reader.h
  - 类名是 HistoricalDataReader，承担历史 binary BookTicker reader 第一版职责。
  - 已提供 noexcept Poll(handler) 单事件 step、noexcept Drain(handler, max_events) 和 finished()。
  - Poll() 成功输出 1 条时返回 1，EOF 返回 0。
  - 已移除 replay 内部 max_events_per_poll_ 语义。
  - 非空 replay 文件在构造期完成 core/utils/mapped_file.h 的 read-only mmap，热路径只使用 cursor 读取。
  - stats 已移除 poll_calls / empty_polls，使用 total_count / files_completed。

core/market_data/data_reader_concepts.h
  - 已提供 PollDataReaderLike、DrainCapableDataReader、DataReaderLike 和 FiniteDataReader concept。

core/strategy/trading_runtime.h
  - 已在 Create() 中保存 data_reader.max_events_per_source 作为外层 data reader poll budget。
  - PollDataReader() 只对同时满足 FiniteDataReader 和 DrainCapableDataReader 的 replay / finite reader 使用 reader.Drain(runtime, budget)。
  - live reader 即使提供 Drain() helper，只要不显式声明 kFiniteDataReader，runtime 仍调用 reader.Poll(runtime)。
  - 不支持 Drain() 的兼容 reader 继续 fallback 到 reader.Poll(runtime)。
```

### 当前 reader benchmark 证据

2026-05-19 新增 `benchmark/core/market_data/data_reader_benchmark.cpp`，用于覆盖 reader 层热路径：

```bash
./build/release/benchmark/core/market_data/data_reader_benchmark --benchmark_min_time=0.2s --benchmark_repetitions=3
```

本轮优化前后对比：

| case | baseline mean | optimized mean |
| --- | ---: | ---: |
| `BM_RealtimeDataReaderEmptyPoll/1` | 3.08ns | 1.36ns |
| `BM_RealtimeDataReaderEmptyPoll/2` | 6.18ns | 2.63ns |
| `BM_RealtimeDataReaderEmptyPoll/4` | 12.3ns | 4.29ns |
| `BM_HistoricalDataReaderDrainSingleFile/1` | 27.6ns | 25.0ns |
| `BM_HistoricalDataReaderDrainSingleFile/64` | 1677ns | 1550ns |
| `BM_HistoricalDataReaderDrainSingleFile/4096` | 109459ns | 98639ns |

对应代码变化：

- `RealtimeDataReader::Poll()` 将运行期 `% source_count` 替换为分支 wrap，并为单 source 增加快路径。
- `RealtimeDataReader` hot `Source` 只保留 `read_mode`、SHM reader 和 overrun 基线，不再保存完整 `DataReaderSourceConfig`。
- `RealtimeDataReader` 构造期拒绝空 sources；本轮 A/B 显示强删 `Poll()` 空检查在当前编译结果下更慢，因此语义改为启动期失败，但实现暂保留该冗余分支。
- `RealtimeDataReader` 多 source round-robin 后续改为构造期双表扫描：`Source*` 表和 source index 表各重复一轮，`Poll()` 线性扫描 `[next_source_index_, next_source_index_ + source_count)`；index-only 表 A/B 明显更慢，未采用。
- `RealtimeDataReader` / `HistoricalDataReader` 读入 `BookTicker` 时不再先清零局部对象。
- `HistoricalDataReader` 将 empty-file skip、文件打开和文件完成状态维护放到构造 / 文件边界慢路径，单条 `Poll()` 不再每次进入完整文件状态机。
- `HistoricalDataReader` 后续使用 `MappedFile` 对当前 replay 文件做 read-only mmap，替代 `std::ifstream::read`。

2026-05-19 round-robin scan table A/B，命令为：

```bash
./build/release/benchmark/core/market_data/data_reader_benchmark \
  --benchmark_filter=BM_RealtimeDataReaderEmptyPoll \
  --benchmark_min_time=0.5s \
  --benchmark_repetitions=5
```

| case | wrap branch mean | double table mean | index-only table mean |
| --- | ---: | ---: | ---: |
| `BM_RealtimeDataReaderEmptyPoll/1` | 1.20ns | 1.18ns | 1.84ns |
| `BM_RealtimeDataReaderEmptyPoll/2` | 2.53ns | 2.32ns | 3.64ns |
| `BM_RealtimeDataReaderEmptyPoll/4` | 4.19ns | 4.02ns | 4.90ns |

1 source 不走多 source round-robin table，差异主要反映代码布局和运行波动；2 / 4 source 的结果支持采用双表实现，不支持 index-only 实现。

2026-05-19 新增 single-source realtime drain benchmark，命令为：

```bash
./build/release/benchmark/core/market_data/data_reader_benchmark \
  --benchmark_filter=BM_RealtimeDataReaderDrainSingleSource \
  --benchmark_min_time=0.5s \
  --benchmark_repetitions=5
```

当前 production 实现仍用 `Drain()` 循环调用 `Poll()`；新增 benchmark 的当前基线：

| case | current mean |
| --- | ---: |
| `BM_RealtimeDataReaderDrainSingleSource/1` | 2.75ns |
| `BM_RealtimeDataReaderDrainSingleSource/64` | 156ns |
| `BM_RealtimeDataReaderDrainSingleSource/4096` | 9966ns |

本轮试过 single-source `Drain()` fast path，drain 4096 可降到约 `8102ns`，但同一 benchmark binary 中
`BM_RealtimeDataReaderEmptyPoll/1` 从约 `1.52ns` 回退到约 `3.45ns`，因此没有保留该生产实现。后续若继续优化
live drain，需要先证明不会影响 live `Poll()` 主路径，或者把 drain 优化隔离到不改变 `Poll()` 代码生成的实现方式。

2026-05-19 historical mmap A/B，命令为：

```bash
./build/release/benchmark/core/market_data/data_reader_benchmark \
  --benchmark_filter=BM_HistoricalDataReaderDrainSingleFile \
  --benchmark_min_time=0.5s \
  --benchmark_repetitions=5
```

| case | ifstream path mean | mmap path mean |
| --- | ---: | ---: |
| `BM_HistoricalDataReaderDrainSingleFile/1` | 25.0ns | 6.37ns |
| `BM_HistoricalDataReaderDrainSingleFile/64` | 1550ns | 369ns |
| `BM_HistoricalDataReaderDrainSingleFile/4096` | 98639ns | 23821ns |

同一实现跑 ORDI_USDT 三天 LeadLag replay：

```bash
/usr/bin/time -f 'elapsed_sec=%e user_sec=%U sys_sec=%S max_rss_kb=%M' \
  ./build/release/tools/lead_lag_replay \
  --config /tmp/aquila_lead_lag_ordi_mmap_replay.toml \
  --signals-output /tmp/lead_lag_mmap_signal.csv
```

结果：`book_tickers=94799061`，`signals=2350`，`open=1175`，`close=1173`，`stoploss=2`，
`elapsed_sec=7.73`，`user_sec=7.68`，`sys_sec=0.38`，`max_rss_kb=3486796`。mmap 提升 replay reader
吞吐，但顺序读完整大文件时 peak RSS 会反映被 fault-in 的映射页；如果后续要控制 replay 内存峰值，需要再评估分段 mmap 或显式 page advice。

2026-05-19 historical `Drain()` batch cursor 优化，命令为：

```bash
./build/release/benchmark/core/market_data/data_reader_benchmark \
  --benchmark_filter=BM_HistoricalDataReaderDrainSingleFile \
  --benchmark_min_time=0.5s \
  --benchmark_repetitions=5
```

| case | mmap Poll-loop Drain mean | batch cursor Drain mean |
| --- | ---: | ---: |
| `BM_HistoricalDataReaderDrainSingleFile/1` | 6.49ns | 6.53ns |
| `BM_HistoricalDataReaderDrainSingleFile/64` | 377ns | 338ns |
| `BM_HistoricalDataReaderDrainSingleFile/4096` | 24178ns | 21784ns |

这轮改动让 `HistoricalDataReader::Drain()` 在当前 mmap 文件内直接按 cursor 批量读取，只有到达文件边界时才进入
`CompleteCurrentFile()`，避免每条记录重复调用 `Poll()` 并走 `finished()` / 文件边界分支。单事件 case 基本持平，批量
Drain 明显减少循环开销。

同一实现跑 ORDI_USDT 三天 LeadLag replay：

```bash
/usr/bin/time -f 'elapsed_sec=%e user_sec=%U sys_sec=%S max_rss_kb=%M' \
  ./build/release/tools/lead_lag_replay \
  --config /tmp/aquila_lead_lag_ordi_replay.toml \
  --signals-output /tmp/lead_lag_historical_drain_opt_signal.csv
```

结果：`book_tickers=94799061`，`signals=2350`，`open=1175`，`close=1173`，`stoploss=2`，
`elapsed_sec=5.57`，`user_sec=5.50`，`sys_sec=0.32`，`max_rss_kb=3486864`。

2026-05-19 `MappedFileAccessPattern::kSequential` / `MADV_SEQUENTIAL` 实验，命令同上：

| case | batch cursor Drain mean | sequential mmap advice mean |
| --- | ---: | ---: |
| `BM_HistoricalDataReaderDrainSingleFile/1` | 6.53ns | 6.58ns |
| `BM_HistoricalDataReaderDrainSingleFile/64` | 338ns | 335ns |
| `BM_HistoricalDataReaderDrainSingleFile/4096` | 21784ns | 21294ns |

同一实现跑 ORDI_USDT 三天 LeadLag replay：`book_tickers=94799061`，`signals=2350`，`open=1175`，
`close=1173`，`stoploss=2`，`elapsed_sec=5.61`，`user_sec=5.48`，`sys_sec=0.38`，
`max_rss_kb=3486756`。microbenchmark 对批量 drain 略有改善，完整 replay 耗时基本持平；由于当前机器
`perf_event_paranoid=4`，无法用 `perf stat` 读取 page-fault counters。

2026-05-19 `BookTickerShmReader` capacity 边界 review 后，恢复为完整 capacity 窗口：`unread_count == capacity`
时不主动丢当前 ring 中最老的一条已发布 `BookTicker`，只有 `unread_count > capacity` 才记录 overrun 并拉回
`current - capacity`。这次改动服务于 reader 语义一致性，不是性能优化；`BM_BookTickerShmReaderTryReadOne`
从保守窗口记录的 `1.94ns` 到恢复后的 `1.95ns`，基本持平。同期 `BM_RealtimeDataReaderEmptyPoll` 1/2/4 source
为 `1.21ns` / `2.29ns` / `4.04ns`，该 benchmark 不触发 SHM overrun 边界，不能把 1 source 数值变化归因于本次
capacity 边界恢复。

当前仍未做的整理：

- `DataReaderConfig::max_events_per_source` 字段名仍保留旧名字；当前语义已改为外层 `Drain()` budget。

### 下一步建议

后续建议拆成独立任务继续推进：

1. 配置整理：评估是否把 `DataReaderConfig::max_events_per_source` 改名为更准确的 `drain_budget` / `max_events_per_drain`；如改名，需要处理现有 TOML 兼容。
2. runtime diagnostics：如需要 `poll_calls` / `empty_polls`，在 `TradingRuntime` / probe / scheduler 维度新增 loop diagnostics，不放回 reader stats。
3. feed 扩展：新增 trade / order book 时，`RealtimeDataReader` 使用 typed storage + unified scan table，不在热路径引入虚基类或 `std::variant`；stats 在 source 维度增加 `trade_count` / `order_book_count` 和对应 last id，顶层继续使用 `total_count`。

## OrderSession 架构占位

待后续细化。

当前已确认方向：

- 负责下单、撤单、接收 ack / response。
- 不创建订单对象，不管理完整订单生命周期。
- 对外只暴露 `Ready()` 作为交易可用性信号；`Ready() == true` 表示可以尝试发送 place / cancel，`Ready() == false` 表示不应发起新的上行交易指令。
- 外部不区分 disconnected、reconnect backoff、login rejected、closing、closed、not active 或 not logged in；内部具体原因只进入 `OrderSession` diagnostics / log。
- ack / response 输出后，组合层应先调用 `OrderManager::OnOrderResponse()`，再调用 `Strategy::OnOrderResponse()`。
- ack 只表示交易所接口收到请求，不代表订单已经进入订单簿。
- 断线 / not ready 不产生 `OrderFeedbackKind::kContinuityLost`，也不直接改变订单状态；未知订单状态后续通过 feedback 或 REST reconcile 收口。

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
3. `OrderFeedbackSession`：定义 feedback event、continuity lost 语义、transport 和恢复边界。
4. `OrderManager`：定义订单状态机、查询接口、执行关系和 position book 分界。
5. `Strategy`：定义 strategy context、execution group、事件回调和状态查询方式。
6. 组合层：在不影响性能的前提下，定义事件顺序保证和实际组装方式。
