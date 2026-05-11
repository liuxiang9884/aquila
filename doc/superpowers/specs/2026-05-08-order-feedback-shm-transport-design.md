# OrderFeedback SHM Transport 设计

## 背景

Gate `OrderSession` 第一版已经能通过 WebSocket 提交 place / cancel 请求；`OrderFeedbackSession`
的 event 语义也已经收敛到只以 Gate private `futures.orders` 为订单生命周期事实源。下一步需要先把
`OrderFeedbackSession` 到 Strategy 的跨线程 / 跨进程通信边界固定下来，再实现 Gate SBE private
orders parse 和 Strategy 状态机更新。

本设计对应 Task1：只实现订单 feedback SHM 通讯，不解析 Gate 报文，也不更新 Strategy 订单状态。当前 Task1
已按本设计的简化版落地：gap 通过 `OrderFeedbackEvent::kGap` control event 投递到 lane，不再使用 shared
gap epoch atomic。

## 目标

- 使用 Nova 的 SPSC queue 承载订单 feedback event。
- 一个 shared memory object 内固定 8 条 strategy lane。
- `local_order_id` 高 8 bit 作为 `strategy_id`，直接数组路由，不做 string channel map。
- producer 是共享的 `OrderFeedbackSession` 进程或线程。
- consumer 是对应 `strategy_id` 的 Strategy 进程或线程。
- 正常 event publish / poll 热路径不做 atomic 计数累加、不做动态分配、不阻塞。
- 队列满、WebSocket 断线和 duplicate consumer claim 走冷路径诊断。
- 第一版不做 producer / reader heartbeat，不做 stale owner 自动判断或 pid alive probe。

## 非目标

- 不实现 Gate `futures.orders` parser。
- 不实现 Strategy `OnOrderFeedback()`。
- 不实现 REST reconcile。
- 不支持动态 strategy lane 数量。
- 不支持 per-strategy `channel_name` 路由。
- 不做 overwrite queue；订单 feedback 不能用覆盖旧消息的语义。

## SHM 布局

第一版固定 8 lane：

```text
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"

OrderFeedbackShmChannel
  OrderFeedbackShmHeader
  OrderFeedbackLane lanes[8]
    OrderFeedbackLaneHeader
    nova::static_impl::SPSCQueue<OrderFeedbackEvent, 65536>
```

`channel_name` 只用于打开 SHM 内唯一订单 feedback channel；解析出 `strategy_id` 后直接访问
`lanes[strategy_id]`。固定 8 lane 会预分配所有 queue 的内存，第一版接受这个成本，以换取路由简单和运行期无 map 查找。

`nova::static_impl::SPSCQueue<T, Capacity>` 要求 `T` 是 trivial / standard-layout，`Capacity` 是 2 的幂。
`Capacity=65536` 时，queue 为区分空 / 满可能保留一个空槽，因此可同时承载的未消费 event 数量按
65535 理解。

## Event Carrier

Task1 选择宽结构 `OrderFeedbackEvent` 作为 SHM ABI。理由：

1. `SPSCQueue` 对 trivial / standard-layout 类型约束更直接。
2. 宽结构没有 union active member 生命周期问题。
3. parser 填字段、Strategy switch 和 benchmark 都更容易先验证正确性。
4. 单 event 体积增长在 8 lane、65536 容量下可接受。

第一版建议结构：

```cpp
enum class OrderFeedbackKind : std::uint8_t {
  kAccepted,
  kPartialFilled,
  kFilled,
  kCancelled,
  kRejected,
  kGap,
};

enum class OrderRole : std::uint8_t {
  kNone,
  kMaker,
  kTaker,
};

enum class OrderFinishReason : std::uint8_t {
  kUnknown,
  kManualCancelled,
  kImmediateOrCancel,
  kReduceOnly,
  kReduceOut,
  kSelfTradePrevention,
  kLiquidated,
  kAutoDeleveraging,
  kPositionClose,
};

enum class OrderRejectReason : std::uint8_t {
  kUnknown,
  kSessionRejected,
  kExchangeRejected,
};

enum class OrderFeedbackGapScope : std::uint8_t {
  kLane,
  kGlobal,
};

enum class OrderFeedbackGapReason : std::uint8_t {
  kUnknown,
  kLaneQueueFull,
  kSessionDisconnected,
  kReconnectUnknownWindow,
  kDecodeUnrecoverable,
  kProducerRestart,
};

struct OrderFeedbackEvent {
  OrderFeedbackKind kind{OrderFeedbackKind::kAccepted};

  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::int64_t cumulative_filled_quantity{0};
  std::int64_t left_quantity{0};
  std::int64_t cancelled_quantity{0};

  double fill_price{0.0};
  OrderRole role{OrderRole::kNone};
  OrderFinishReason finish_reason{OrderFinishReason::kUnknown};
  OrderRejectReason reject_reason{OrderRejectReason::kUnknown};

  OrderFeedbackGapScope gap_scope{OrderFeedbackGapScope::kLane};
  OrderFeedbackGapReason gap_reason{OrderFeedbackGapReason::kUnknown};
  std::uint64_t gap_sequence{0};

  std::int64_t exchange_update_ns{0};
  std::int64_t local_receive_ns{0};
};
```

字段语义沿用 `doc/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md`。
`local_receive_ns` 是 feedback 进程本地收到并准备发布该 event 的时间，用于链路延迟诊断；它不参与 Strategy
状态推进。

`kGap` 是 transport control event。Strategy reader 不读取 shared gap epoch；它只通过 `Poll()` 消费 event，
并把 `kGap` 和普通 order feedback 一样交给 handler。Gate `futures.orders` 主生命周期 parser 不产生 `kGap`。

实现时需要 `static_assert` 固定：

- `std::is_trivial_v<OrderFeedbackEvent>`
- `std::is_standard_layout_v<OrderFeedbackEvent>`
- `sizeof(OrderFeedbackEvent)` 在测试中记录，避免无意加入非平凡字段或膨胀 ABI。

## Header 与 Lane Metadata

建议结构：

```cpp
struct OrderFeedbackShmHeader {
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t abi_size;
  std::uint32_t max_strategy_count;
  std::uint32_t queue_capacity;
  std::uint32_t event_size;

  std::uint64_t producer_pid;
  std::uint64_t producer_run_id;
  std::atomic<std::uint64_t> invalid_route_count;
};

struct OrderFeedbackLaneHeader {
  std::uint8_t strategy_id;

  std::atomic<std::uint64_t> consumer_run_id;
  std::atomic<std::uint64_t> consumer_pid;

  std::atomic<std::uint64_t> queue_full_count;
  std::atomic<std::uint64_t> dropped_count;
};
```

这些 metadata 保持最小化：

- `producer_pid` / `producer_run_id` 只用于诊断和后续扩展。
- `invalid_route_count` 只在 `local_order_id` 无效或 route 越界时更新。
- `consumer_run_id` 是 reader ownership token，0 表示 unclaimed。
- `consumer_pid` 仅诊断，不参与 ownership 判断。
- `queue_full_count` / `dropped_count` 只在当前 lane `TryPush()` 失败时更新。
- successful `published_count` / `consumed_count` 不写 shared header，只保留在 publisher / reader 对象本地。

正常 publish 热路径只做：

```text
strategy_id = DecodeStrategyId(local_order_id)
lane = lanes[strategy_id]
lane.queue.TryPush(event)
```

## Manager 初始化错误模型

`OrderFeedbackShmManager` 是 cold init / attach 边界，不在 publish / poll 热路径中。第一版对上层暴露显式错误返回：

- `OrderFeedbackShmManager::Create(config)`
- `OrderFeedbackShmManager::Open(config)`
- `OrderFeedbackShmManager::OpenOrCreate(config)`

这三个接口返回 `Result<OrderFeedbackShmManager>`。config invalid、SHM 打不开、channel not found、header ABI
mismatch 都通过 `Result.error` 返回。Nova `ShmAllocator` 底层可能抛出的异常只在 factory 内部 catch 并转换为
`Result.error`，不向 session / tool / Strategy 上层暴露 throwing constructor。

## Consumer Claim

`strategy_id` 不能依靠君子约定。第一版做两层约束：

1. 配置 parser 检查 `strategy_id < 8`。
2. Strategy reader attach 时 claim 对应 lane。

claim 规则：

- `consumer_run_id == 0` 表示 unclaimed。
- `consumer_run_id != 0` 表示 lane 已被 claim；普通 `Claim()` 拒绝 duplicate attach。
- `consumer_pid` 仅用于诊断，不作为 ownership token。
- `Claim(..., force_claim=true)` 是显式恢复动作，可覆盖旧 run id 和 pid。
- `Release()` 只有 CAS 当前 `consumer_run_id` 成功时才清 `consumer_pid`，避免误清新 owner。

这条规则避免两个 Strategy 进程同时消费同一 lane。第一版不做 stale owner 自动判断、不做 pid alive probe，也不做
reader heartbeat；需要恢复时由运维或上层显式调用 `force_claim=true`。

## Gap 语义

### Lane Gap

当某 lane 普通 order feedback `TryPush()` 失败：

- 不覆盖旧 event；
- 不阻塞 producer；
- 不影响其他 lane；
- 当前 lane `queue_full_count++`；
- 当前 lane `dropped_count++`；
- publisher 在本地保留 pending lane `kGap` event；
- 后续 publish 或 `FlushPendingGapEvents()` 会先重试该 pending gap。

Strategy reader poll 到 lane `kGap` 后，必须进入 reconcile 状态；在 reconcile 完成前不应继续开新仓。

### Global Gap

当 `OrderFeedbackSession` 的 WebSocket 断线、重连后有未知时间窗口、SBE decode 进入不可恢复状态，或 producer 判断无法证明没有丢失回报时：

- `PublishGlobalGap(reason, local_receive_ns)` 对 8 lane fanout `kGap`；
- lane queue full 时，不阻塞、不覆盖，publisher 保留该 lane pending global gap 并本地重试；
- 所有 Strategy 应进入 reconcile 状态。

Task1 只提供 `kGap` 投递和读取接口，不实现 reconcile。

## Reader 语义

订单 feedback reader 只支持 drain 模式，不支持 latest 模式：

```cpp
template <typename Handler>
std::size_t Poll(std::size_t max_events, Handler&& handler);
```

`Poll()` 最多消费 `max_events` 条，并按 FIFO 顺序调用 handler。Strategy 主循环按固定预算调用该接口，避免 feedback burst 长时间占用策略线程。

## 配置建议

Task1 已新增独立示例配置：

```toml
[order_feedback_shm]
shm_name = "aquila_gate_order_feedback"
channel_name = "orders"
max_strategy_count = 8
queue_capacity = 65536
create = true
remove_existing = false
```

实现时 `max_strategy_count` 和 `queue_capacity` 第一版只允许等于编译期常量；保留字段是为了把部署配置写清楚，并在未来需要 ABI 版本升级时有显式检查点。

## 测试边界

Task1 至少覆盖：

- event 类型 trivial / standard-layout / size ABI；
- `kGap` event 的 scope / reason / sequence 字段；
- SHM create / attach header 校验；
- `local_order_id` 高 8 bit 路由到正确 lane；
- `strategy_id >= 8` publish 失败并计数；
- duplicate live consumer claim 被拒绝；
- `force_claim=true` 可显式恢复已 claim lane；
- `Release()` 只有 run id 匹配才清 pid；
- `TryPush()` full 时只更新当前 lane异常计数，并保留 pending lane `kGap`，不影响其他 lane；
- `PublishGlobalGap()` 对 8 lane fanout `kGap`，full lane 只保留自身 pending gap；
- `Poll(max_events)` 遵守消费预算。

## Benchmark 边界

Task1 benchmark 只证明 SHM transport 本身：

- `PublishThenDrain`；
- `PollOneWithRefill`；
- `PublishPollLoop`；
- `PublishGlobalGapThenDrain`。

`PublishThenDrain` / `PollOneWithRefill` 包含 drain / refill 维护，不是纯 publish / poll latency。性能结论必须基于实际 benchmark 输出记录；不能把 Task1 transport benchmark 外推为 Gate parser、Strategy 状态机或端到端链路性能。
