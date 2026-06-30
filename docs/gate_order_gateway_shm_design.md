# Gate 多路 Order Gateway SHM 设计

## 目的

本文记录 Gate 多路 `OrderSession` 的下一版生产设计。目标是在 `strategy-process`
和独立 `order-gateway-process` 之间使用共享内存 SPSC queue，把同一个策略下单意图
fanout 到 N 条 Gate trading WebSocket connection，以降低单连接延迟波动和交易所内部排队不确定性对成交率的影响。

本文只锁定架构、状态语义、queue payload 和实现边界；不宣称成交率或延迟收益。收益必须由 live smoke、probe 或 report 证据验证。

## 当前实现状态

2026-06-30 已落地本文的 V1 实现入口：

- SHM POD types / queue runtime：`core/trading/order_gateway_shm_types.h`、`core/trading/order_gateway_shm.h`。
- strategy-side gateway：`core/trading/order_gateway_client.h`，供 `OrderManager` 作为 gateway contract 使用。
- worker / process：`exchange/gate/trading/order_gateway_worker.h`、`tools/gate/gate_order_gateway.cpp`。
- config parser：`core/config/order_gateway_config.*`、`core/config/strategy_config.*` 的 `[strategy.order_gateway]`。
- LeadLag fanout：`strategy/lead_lag/config.*`、`strategy/lead_lag/execution_state.h`、`strategy/lead_lag/strategy.h`。
- live strategy 接入：`tools/lead_lag/live_strategy.cpp` 可在 live-orders 模式选择 `OrderGatewayClient`。
- 30-symbol validate-only 配置：`config/order_gateways/gate_order_gateway_30symbols_private_plain_20260627.toml` 和 `config/strategies/lead_lag_30symbols_fusion_order_gateway_live_strategy_20260627.toml`。

上述实现已通过单元 / smoke / validate-only 测试，但尚未做真实 `order-gateway-process` + strategy live smoke；不能据此宣称成交率或延迟收益。

## 已锁定决策

- 进程模型：2 个进程，`strategy-process` 和 `order-gateway-process`。
- 线程模型：`strategy-process` 内 1 个 strategy owner thread；`order-gateway-process` 内 N 个 `OrderSessionWorker` thread。不计 log backend thread。
- N 是运行时参数，编译期最大值 `16`。
- 跨进程通信使用一个 POSIX shared memory 对象，内部包含 N 路 `command_queue` 和 N 路 `event_queue`。
- `command_queue[i]` 是 `strategy -> OrderSessionWorker[i]`；`event_queue[i]` 是 `OrderSessionWorker[i] -> strategy`。
- queue payload 是固定大小 POD struct，不包含 `std::string`、`std::string_view`、指针、虚函数或动态分配。
- field 命名使用 `parent_id`，不修改 `LocalOrderIdCodec`。
- 每个 child order 必须有唯一 `local_order_id`；同一个策略意图 / fanout batch 的 child 共享同一个 `parent_id`。
- feedback 路径保持不变：单账户级 `OrderFeedbackSession -> OrderFeedback SHM -> strategy-process`。
- write-path、TCP_INFO、socket timestamping 等详细诊断留在 `order-gateway-process` / `OrderSession` 日志中，不放进主 `event_queue`。

## 进程和线程图

```text
strategy-process
  StrategyOrderOwnerThread
    TradingRuntime
    LeadLag Strategy / ExecutionState
    OrderManager
    OrderGatewayClient
      RouteTable(local_order_id -> route_id)
      route_ready[0..N-1]
    OrderFeedbackShmReader

order-gateway-process
  OrderSessionWorker[0]
    command_queue[0] consumer
    event_queue[0] producer
    Gate OrderSession[0]

  OrderSessionWorker[1]
    command_queue[1] consumer
    event_queue[1] producer
    Gate OrderSession[1]

  ...

  OrderSessionWorker[N-1]
    command_queue[N-1] consumer
    event_queue[N-1] producer
    Gate OrderSession[N-1]

gate-feedback-process
  Gate OrderFeedbackSession -> OrderFeedback SHM
```

对于 `N=4`，交易主路径应用线程数是 `1 + N = 5`：

```text
1 x StrategyOrderOwnerThread
4 x OrderSessionWorker
```

一次下单经过两个应用线程：

```text
StrategyOrderOwnerThread -> OrderSessionWorker[i] -> socket
```

## SHM 形态

一个 SHM 对象包含全部 route 的 queue。概念结构如下：

```text
OrderGatewayShmHeader
  route_count <= 16
  command_queue_capacity
  event_queue_capacity
  startup_ready_timeout_s
  command_queue_descriptors[16]
  event_queue_descriptors[16]
  data offsets / sizes

command_queue[0]
event_queue[0]
command_queue[1]
event_queue[1]
...
command_queue[N-1]
event_queue[N-1]
```

`route_count` 是运行时配置；只使用 `[0, route_count)` 的 queue。`command_queue_capacity`
和 `event_queue_capacity` 也是运行时配置。第一版推荐默认：

```text
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30
```

queue full 是故障保护，不是常规背压机制。容量应足够大，正常实盘不应触发 full。

## Command Payload

```cpp
inline constexpr std::size_t kMaxOrderGatewayRoutes = 16;
inline constexpr std::size_t kOrderGatewaySymbolBytes = 32;
inline constexpr std::size_t kOrderGatewayQuantityTextBytes = 32;
inline constexpr std::size_t kOrderGatewayPriceTextBytes = 32;

enum class OrderGatewayCommandKind : std::uint8_t {
  kNone = 0,
  kPlace = 1,
  kCancel = 2,
  kCacheExchangeOrderId = 3,
  kForgetExchangeOrderId = 4,
  kStop = 5,
};

struct OrderGatewayCommand {
  std::uint64_t command_seq{0};
  std::uint64_t parent_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::int64_t owner_enqueue_ns{0};

  std::int32_t symbol_id{0};
  std::uint16_t route_id{0};
  std::uint16_t symbol_size{0};
  std::uint16_t quantity_text_size{0};
  std::uint16_t price_text_size{0};

  OrderGatewayCommandKind kind{OrderGatewayCommandKind::kNone};
  Exchange exchange{Exchange::kGate};
  OrderSide side{OrderSide::kBuy};
  OrderType order_type{OrderType::kLimit};
  TimeInForce time_in_force{TimeInForce::kGoodTillCancel};
  std::uint8_t reduce_only{0};

  double quantity{0.0};

  char symbol[kOrderGatewaySymbolBytes]{};
  char quantity_text[kOrderGatewayQuantityTextBytes]{};
  char price_text[kOrderGatewayPriceTextBytes]{};
};
```

字段语义：

- `command_seq`：每条 command 唯一，用于 command / event 对账。
- `parent_id`：同一个策略意图、fanout batch、close batch 共享的 parent id。
- `local_order_id`：每个 child order 唯一，Gate `text=t-<local_order_id>` 使用它。
- `exchange_order_id`：cancel / cache 使用；place 时为 0。
- `route_id`：目标 order session index，等于 queue index。worker 必须校验 `route_id == own_route_id`。
- `symbol_size` / `quantity_text_size` / `price_text_size`：对应固定数组的有效长度。
- `symbol[32]`、`quantity_text[32]`、`price_text[32]` 超长时 strategy 本地 reject，不写 queue。

`kPlace` 使用完整下单字段；`kCancel` 使用 `local_order_id` / `exchange_order_id`；`kCacheExchangeOrderId`
使用 `local_order_id` / `exchange_order_id`；`kForgetExchangeOrderId` 使用 `local_order_id`；`kStop` 只需要
`command_seq` 和 `kind`。

## Event Payload

```cpp
enum class OrderGatewayEventKind : std::uint8_t {
  kNone = 0,
  kOrderResponse = 1,
  kCommandRejected = 2,
  kReady = 3,
  kNotReady = 4,
  kStopped = 5,
};

enum class OrderGatewayCommandRejectReason : std::uint8_t {
  kNone = 0,
  kInvalidCommand = 1,
  kSessionNotReady = 2,
  kSessionNotActive = 3,
  kInflightFull = 4,
  kEncodeFailed = 5,
  kNoPreparedWriteSlot = 6,
  kWriteUnavailable = 7,
  kUnsupportedOrderType = 8,
};

struct OrderGatewayEvent {
  std::uint64_t event_seq{0};
  std::uint64_t command_seq{0};
  std::uint64_t parent_id{0};
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::uint64_t request_sequence{0};
  std::uint64_t encoded_request_id{0};

  std::int64_t worker_dequeue_ns{0};
  std::int64_t request_send_local_ns{0};
  std::int64_t local_receive_ns{0};
  std::int64_t exchange_ns{0};
  std::int64_t exchange_request_ingress_ns{0};
  std::int64_t exchange_response_egress_ns{0};
  std::int64_t exchange_process_ns{0};
  std::int64_t worker_event_enqueue_ns{0};

  std::uint16_t route_id{0};
  std::uint16_t http_status{0};

  OrderGatewayEventKind kind{OrderGatewayEventKind::kNone};
  OrderGatewayCommandKind command_kind{OrderGatewayCommandKind::kNone};
  core::OrderResponseKind response_kind{core::OrderResponseKind::kAck};
  OrderGatewayCommandRejectReason reject_reason{
      OrderGatewayCommandRejectReason::kNone};

  std::uint8_t ready{0};
};
```

`kOrderResponse` 由 worker 将 Gate response 转成 core response kind 后写入；strategy owner drain 后继续按现有顺序执行：

```text
OrderManager::OnOrderResponse()
Strategy::OnOrderResponse()
```

`kCommandRejected` 统一转换为 `core::OrderResponseKind::kRejected`。这是确定性本地拒绝，表示请求没有进入交易所不确定状态。
只有 Gate 5xx、timeout 或 ambiguous final response 才使用 `kUnknownResult`。

`kCommandRejected` timing 口径：

```text
request_send_local_ns = 0
worker_dequeue_ns != 0
worker_event_enqueue_ns != 0
request_sequence / encoded_request_id 可选填，用于关联 worker log
```

## Ready 状态

不使用额外 ready snapshot。strategy 通过 drain `event_queue[0..N-1]` 维护 route flag：

```text
route_ready[0..N-1] = false
ready_count = 0
```

启动阶段：

```text
strategy attach gateway SHM
只 drain event_queue，不产生策略信号
收到 kReady(route=i):
  如果 route_ready[i] == false:
    route_ready[i] = true
    ++ready_count
ready_count == N 后进入 trading
如果 startup_ready_timeout_s 内未全 ready，strategy fail fast
```

运行阶段：

```text
收到 kNotReady(route=i):
  如果 route_ready[i] == true:
    route_ready[i] = false
    --ready_count

收到 kReady(route=i):
  如果 route_ready[i] == false:
    route_ready[i] = true
    ++ready_count
```

`ready_count` 必须由 flag transition 更新，不能对重复 `kReady` / `kNotReady` 无条件加减。下单时逐 route 检查
`route_ready[i]`；false route 跳过并通过 gateway stats 计数。`ready_count == 0` 时本次 signal 本地 reject。

`order-gateway-process` 启动后即创建 / 初始化 SHM，并启动 N 个 worker。某路连接失败不导致 gateway 整体退出；
worker 持续重连，并在 ready / not-ready 变化时写 event。strategy 使用 `startup_ready_timeout_s` 决定是否等待成功。

## Strategy Fanout 语义

LeadLag 新增策略参数：

```toml
[lead_lag.pairs.execute]
order_session_fanout = 4
```

第一版放在每个 pair 的 `execute` 表中，默认 `1`。它表示每次 signal 最多向多少个 order session 发送 full-size duplicate child order。

启动时如果 `order_session_fanout > route_count`，strategy 打 warning，并按 `route_count` cap。运行时实际 fanout：

```text
target = min(order_session_fanout, route_count)
sent = 0

for route in 0..route_count-1:
  if sent == target:
    break
  if !route_ready[route]:
    record_route_not_ready(route)
    continue
  send child order to command_queue[route]
  ++sent

if sent == 0:
  reject this signal locally
```

第一版固定 route 顺序 `0..N-1`，跳过 not-ready route，不做 round-robin 起点漂移、不做 RTT 动态选路。

一个 signal 的 m 个 child order：

- 数量、价格、side、TIF、reduce-only 等订单字段一致。
- `parent_id` 相同。
- `local_order_id` 不同。
- `route_id` 不同。
- 一个 signal 只占用一个 `parallel` slot。

`parent_id` 由 strategy owner 在 fanout batch 开始时分配，推荐使用 batch 内第一个预分配 child
`local_order_id`。即使某条 route 在 enqueue 前本地 reject，同一 batch 的其它 child 仍沿用该 `parent_id`；
不能把 `parent_id` 绑定到“第一条成功进入交易所”的订单。

open 成交后，同一个 `parent_id` / execution group 下所有 child 的实际累计成交量合并成一个 position。close、stoploss 和 close retry
按该 position 总成交量，对 ready route 发同量 reduce-only close child。reduce-only close 中任一 child 成交后，其它 child 被交易所拒单或取消是可接受结果。

## PlaceOrder 语义

`OrderGatewayClient::PlaceOrder()` enqueue 成功就返回 `kOk`，语义是：

```text
command accepted by order-gateway command_queue
```

它不表示已经写入 socket。后续事实由 `event_queue` 回传：

```text
command_queue full before enqueue
  -> PlaceOrder() 返回 kSessionRejected
  -> 本地回滚，不进入 exchange uncertainty

enqueue ok, worker local reject before write
  -> event_queue: kCommandRejected
  -> owner 转成 core::OrderResponseKind::kRejected

enqueue ok, worker write path 结果未知 / Gate 5xx / timeout
  -> event_queue: kOrderResponse(kUnknownResult)
  -> strategy 进入 reconcile

enqueue ok, worker send ok
  -> 等 Gate Ack / final response 正常经 event_queue 回来
```

## Queue Full 语义

`command_queue` full：

- `PlaceOrder()` / `CancelOrder()` 返回本地 session rejected。
- 未成功写入 queue 的 place 不写 route table，或立即 rollback。
- 不阻塞等待。

`event_queue` full：

- worker 不能静默丢 `OrderResponseEvent`。
- worker 进入 fatal / degraded，停止对应 `OrderSession`。
- strategy 侧应通过可用控制路径观察到 `kStopped` 或进程异常，然后暂停新开仓并进入 reconcile / emergency handling。
- 不阻塞等待。

## 配置模型

新增 order gateway config：

```toml
[log]
log_level = "info"
file_sink_name = "/home/liuxiang/log/gate_order_gateway_30symbols_private_plain_20260627.log"
console_sink_name = "gate_order_gateway_30symbols_private_plain_20260627_console"
backend_thread_name = "gate_order_gateway_30symbols_private_plain_20260627_log"
backend_cpu_affinity = 0

[order_gateway]
name = "gate_order_gateway_30symbols_private_plain_20260627"
route_count = 4
command_queue_capacity = 4096
event_queue_capacity = 8192
startup_ready_timeout_s = 30

[[order_gateway.routes]]
name = "route0"
order_session_config = "config/order_sessions/gate_order_session_30symbols_private_plain_20260627.toml"
worker_cpu_id = 16

[[order_gateway.routes]]
name = "route1"
order_session_config = "config/order_sessions/gate_order_session_30symbols_private_plain_20260627.toml"
worker_cpu_id = 17
```

`worker_cpu_id` 是该 route 的 worker thread / WebSocket runtime CPU；gateway 加载 route 时会用它覆盖
`order_session_config` 中的 websocket `bind_cpu_id`，因此 4 条 route 可以暂时复用同一个 order session TOML。
当前 30-symbol fusion + order gateway smoke / validate 配置把 4 条 order route 放在 `16-19`，避免与已有 fusion /
strategy / feedback 的 `0-15` profile 同核；真实 live 前必须按 `docs/runtime_cpu_allocation.md` 记录实际 CPU 分配，
并确认 `16-19` 没有测试负载。

strategy live config 支持可选：

```toml
[strategy.order_gateway]
config = "config/order_gateways/gate_order_gateway_30symbols_private_plain_20260627.toml"
```

30-symbol order-gateway strategy config 使用独立 pair 参数文件：

```text
config/strategies/lead_lag_30symbols_fusion_2bps_5bps_order_gateway_20260627.toml
```

该文件在每个 pair 的 `[lead_lag.pairs.execute]` 中设置 `order_session_fanout = 4`。legacy single-session live config
仍指向不带 fanout 的原文件，避免单 session 路径把 duplicate child 发到同一连接。

兼容规则：

- 有 `[strategy.order_gateway]`：走多路 order gateway。
- 只有 `[strategy.order_session]`：走现有单 session。
- 两者同时存在：config fail fast，避免误启动。

## 与现有 probe 的关系

`gate_order_session_rtt_probe` 当前已验证 N 个 worker thread / N 条 `OrderSession` 的基础运行、feedback 路由、CSV 采集和 RTT 诊断。
但 probe 的 coordinator 不传订单内容；worker 在自己的 runtime hook 中构造 probe order。它不是 production gateway。

本设计需要新增 production `OrderGatewayClient` / `order-gateway-process`，由 strategy owner 通过 `command_queue` 发送完整
`OrderGatewayCommand`，worker 只负责消费 command、调用本线程拥有的 `OrderSession`，并通过 `event_queue` 回传事件。

## 非目标

- 第一版不做 RTT 智能路由。
- 第一版不做 worker-per-session 和单线程 backend 的性能收益声明。
- 第一版不修改 `LocalOrderIdCodec`。
- 第一版不把 write-path / socket timestamping 诊断放进主 event queue。
- 第一版不改变 `OrderFeedbackSession` 的单账户级反馈路径。
- 第一版不处理 winner cancel、overfill 自动处理或 duplicate / split 策略选择之外的 OMS 语义。

## 验证要求

实现时至少覆盖：

- SHM header / queue ABI 单元测试，包括 `route_count <= 16`、capacity、POD struct size 和 overflow reject。
- `command_queue` place / cancel / cache / forget enqueue / dequeue 测试。
- `event_queue` response / ready / not-ready / command-rejected drain 测试。
- `OrderGatewayClient` route table 测试：place 写 route table，cancel / cache / forget 回原 route。
- strategy ready gate 测试：启动等全 N ready、重复 ready 不重复计数、not-ready 降级、30s 超时 fail fast。
- LeadLag fanout 测试：一个 signal 生成 m 个 child、共享 `parent_id`、不同 `local_order_id` / `route_id`，只占一个 parallel slot。
- close / stoploss / close retry fanout 测试：按 position 总成交量生成 reduce-only child。
- live 前 dry-run / smoke：先跑 order gateway + strategy validate-only，再做小额 guarded live smoke。

当前 validate-only 命令：

```bash
./build/debug/tools/lead_lag_strategy \
  --config config/strategies/lead_lag_30symbols_fusion_order_gateway_live_strategy_20260627.toml
```

未传 `--execute` / `--connect-data` 时，该命令只验证 config / parser / mode selection，不打开 websocket 或
order gateway SHM。
