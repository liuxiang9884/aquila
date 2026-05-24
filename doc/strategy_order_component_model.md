# Strategy / Order 组件解耦模型

本文记录当前讨论确认的策略、行情读取和订单组件模型。它不是最终详细实现计划，后续仍需要继续细化
接口命名、错误码、测试矩阵和 position book / REST reconcile 边界。

本文刻意只讨论下面几个业务组件：

```text
DataReader
OrderSession
OrderFeedbackSession
OrderManager
Strategy
```

实际运行时可以由任意编排层把这些组件组装到同一线程或跨进程链路里；组件自身不依赖具体编排层类型。

## 总体原则

- `Strategy` 表达策略意图和保存策略级执行关系，不直接管理底层订单池。
- `OrderManager` 是订单状态 owner，统一管理本地订单对象、状态推进、成交累计和终态。
- `OrderSession` 是上行交易指令通道，只负责 place / cancel 编码、发送、ack / response 解析和轻量 correlation。
- `OrderFeedbackSession` 是下行私有订单事实通道，只负责接收 / 解析交易所订单回报并产出 feedback event。
- `DataReader` 是 Strategy 侧行情 reader，只读取统一 `BookTicker`，不解析交易所协议。
- 所有订单状态输入必须先进入 `OrderManager`，再通知 `Strategy`。

推荐的数据流：

```text
DataSession
  -> BookTicker SHM
  -> DataReader
  -> Strategy::OnBookTicker(...)

Strategy
  -> OrderManager::PlaceLimitOrder(...) / CancelOrder(...)
  -> OrderSession::PlaceOrder(...) / CancelOrder(...)
  -> exchange place / cancel

OrderSession ack / response
  -> OrderManager::OnOrderResponse(...)
  -> Strategy::OnOrderResponse(...)

OrderFeedbackSession feedback
  -> feedback event transport
  -> OrderManager::OnOrderFeedback(...)
  -> Strategy::OnOrderFeedback(...)
```

## TradingRuntime 组装链路

当前 `TradingRuntime<StrategyT, OrderSessionT, DataReaderT>` 是交易链路组合器。它持有并组装：

```text
DataReader
OrderSession
OrderManager
StrategyContext
Strategy
OrderFeedbackShmReader(optional)
```

主要 adapter / 适配边界：

- `exchange/gate/trading/order_session_runtime_adapter.h` 是 Gate-specific runtime adapter，把 Gate `OrderSession` 包装成
  `TradingRuntime` 可用的 `OrderSessionT`，负责 Gate response kind 到 core response kind 的转换、
  `BindRuntime()` 回调接线和 `SetRuntimeHook()` 转发。
- `StrategyContext<OrderSessionT>` 是 strategy 到 `OrderManager` 的窄下单接口；strategy 不直接拿
  `OrderManager` 或 exchange session。
- `OrderFeedbackShmReader` 把 SHM 中的统一 `OrderFeedbackEvent` drain 到 `TradingRuntime::OnOrderFeedback()`；
  它是 feedback transport 到 runtime 的 reader 边界。
- 当前 order quantity contract 是 `double quantity` + `quantity_text`：strategy / risk 使用 `double`
  做数量和名义金额计算，exchange adapter 使用已按 instrument `quantity_decimal_places` 格式化的
  `quantity_text` 编码真实下单字段；feedback event / SHM / `OrderManager` 也用 `double`
  表示累计成交、剩余和撤单数量。

完整结构图：

```text
              market data session
                    |
                    v
              BookTicker SHM
                    |
                    v
DataReader --> TradingRuntime --> Strategy
                    |              ^
                    |              |
                    v              |
             StrategyContext ------+
                    |
                    v
              OrderManager
                    |
                    v
        OrderSessionRuntimeAdapter
                    |
                    v
          Gate OrderSession <---- Gate WS order response
                    |
                    v
              Gate WS send


Gate private feedback WS
          |
          v
OrderFeedbackSession
          |
          v
OrderFeedback SHM
          |
          v
TradingRuntime -> OrderManager -> Strategy
```

关键运行规则：

- `TradingRuntime::Create()` 从已解析 config 构造 data reader、order session、`OrderManager`、context、strategy
  和可选 feedback reader，然后在 order session 支持时调用 `BindRuntime()`。
- 上行下单链路是 `Strategy -> StrategyContext -> OrderManager -> OrderSessionAdapter -> OrderSession`。
- order response 链路是 `OrderSession -> adapter -> TradingRuntime::OnOrderResponse() -> OrderManager -> Strategy`。
- private feedback 链路是 `OrderFeedbackSession -> SHM -> TradingRuntime::OnOrderFeedback() -> OrderManager -> Strategy`。
- `OrderSession::Ready() == false` 只 gate data reader / 行情驱动交易意图，不阻塞 order response 或 feedback drain。
- adapter 层应保持很薄，只做协议隔离、类型转换和 runtime 接线；不要承载业务状态机、错误决策、队列或长期诊断字段搬运。

## DataReader

### 功能表述

`DataReader` 是策略线程看到行情的唯一入口。它 attach 一个或多个 `BookTicker` SHM source，按配置读取统一的
`aquila::BookTicker` 并交给 handler。

它负责：

- 从已存在的 BookTicker SHM channel 读取数据。
- 支持 `latest` / `drain` 两种读取模式。
- 在 diagnostics policy 开启时记录 poll、skipped、overrun 和 per-source 统计。
- 保持交易所无关，只消费统一行情结构。

它不负责：

- 连接交易所 WebSocket。
- 解析 Gate SBE 或 Binance JSON。
- 做策略筛选、聚合订单或风控。
- 判断订单 / 仓位状态。

### 接口契约

建议保持当前 handler 形态：

```cpp
template <typename Handler>
std::uint64_t Poll(Handler& handler) noexcept;
```

`Handler` 只需要实现：

```cpp
void OnBookTicker(const aquila::BookTicker& ticker) noexcept;
```

`DataReader` 不需要知道 handler 是 `Strategy`、adapter、测试 fake，还是其他事件分发对象。

## Strategy

### 功能表述

`Strategy` 是策略逻辑 owner。它消费行情、订单 response 和订单 feedback，决定是否发出开仓、平仓、撤单或停止
运行等策略意图。

它负责：

- 保存策略级执行状态，例如 signal、execution group、pair 状态和自己关心的 `local_order_id`。
- 在行情事件中根据策略状态产生下单 / 撤单意图。
- 在订单 response / feedback 到达后更新策略级 execution state。
- 对 order response 只根据 `OrderResponseKind` 和订单 id 做状态推进；错误详情在 Gate / tool 日志处理，不进入
  strategy event。
- 通过只读查询查看订单最新状态，而不是复制 `OrderManager` 的订单状态机。
- 把 `OrderManager::feedback_continuity_lost_detected()` 作为策略 / 风控输入信号，决定是否暂停开仓、只允许减仓、
  触发 reconcile 或人工介入。

它不负责：

- 维护通用订单池。
- 分配 `local_order_id`。
- 解析 exchange ack / feedback 报文。
- 根据 feedback 推进通用订单状态。
- 直接推导真实账户持仓作为事实源。

### 接口契约

策略事件接口保持被动回调风格：

```cpp
void OnBookTicker(const aquila::BookTicker& ticker, StrategyContext& context) noexcept;
void OnOrderResponse(const OrderResponseEvent& event, StrategyContext& context) noexcept;
void OnOrderFeedback(const OrderFeedbackEvent& event, StrategyContext& context) noexcept;
```

可选 lifecycle：

```cpp
void OnStart(StrategyContext& context) noexcept;
void OnLoop(StrategyContext& context) noexcept;
void OnIdle(StrategyContext& context) noexcept;
void OnStop(StrategyContext& context) noexcept;
bool ShouldStop() const noexcept;
```

`StrategyContext` 是策略可见的窄接口，至少包含：

```cpp
OrderPlaceResult PlaceLimitOrder(OrderCreateRequest request) noexcept;
OrderCancelResult CancelOrder(std::uint64_t local_order_id) noexcept;
const StrategyOrder* FindOrder(std::uint64_t local_order_id) const noexcept;
```

策略发单后必须保存返回的 `local_order_id`，用它关联自己的 signal / execution group；需要底层订单状态时通过
`FindOrder()` 查询。

持仓模型后续应拆为独立 `PositionBook` / `PositionManager`，由成交 feedback 更新。`Strategy` 不应把 ack 或
accepted 当成持仓变化。

## OrderManager

### 功能表述

`OrderManager` 是订单状态 owner。它把策略下单意图转换为本地订单对象，并统一消费 `OrderSession` 的 response
和 `OrderFeedbackSession` 的 feedback。

它负责：

- 创建本地订单对象并分配 `local_order_id`。
- 维护固定容量订单池和订单状态。
- 调用 `OrderSession` 发送 place / cancel。
- 处理 ack / accepted / rejected / cancel accepted / cancel rejected 等 response。
- 处理 accepted / partial filled / filled / cancelled / rejected / continuity lost 等 feedback。
- 维护累计成交、成交价格、终态、error / reject reason。
- 在 accepted feedback 到达后通知 `OrderSession` 缓存 `exchange_order_id`，终态后通知其清理 cache。
- 暴露只读订单查询给 `Strategy`。

它不负责：

- WebSocket 连接和 exchange wire encoding。
- 私有订单回报 WebSocket 订阅。
- 策略 signal 逻辑。
- 完整账户持仓事实源；持仓聚合应由后续独立模块承担。

### 接口契约

下单 / 撤单入口：

```cpp
OrderPlaceResult PlaceLimitOrder(OrderCreateRequest request) noexcept;
OrderCancelResult CancelOrder(std::uint64_t local_order_id) noexcept;
```

订单事件入口：

```cpp
void OnOrderResponse(const OrderResponseEvent& event) noexcept;
void OnOrderFeedback(const OrderFeedbackEvent& event) noexcept;
```

查询入口：

```cpp
const StrategyOrder* FindOrder(std::uint64_t local_order_id) const noexcept;
bool RetireFinishedOrder(std::uint64_t local_order_id) noexcept;
bool feedback_continuity_lost_detected() const noexcept;
```

`OrderManager` 依赖的是一个窄的 order gateway 能力，而不是某个具体交易所 session：

```cpp
OrderSendResult PlaceOrder(const StrategyOrder& order) noexcept;
OrderSendResult CancelOrder(const StrategyOrder& order) noexcept;
```

如果 gateway 支持 exchange id cache，可选暴露：

```cpp
void CacheExchangeOrderId(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept;
void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept;
```

## OrderSession

### 功能表述

`OrderSession` 是交易所上行交易指令通道。Gate 当前实现覆盖 login、place、cancel、ack / final response 解析和
correlation。

它负责：

- 维护私有交易 WebSocket 连接和 login ready 状态。
- 把 `StrategyOrder` 编码成交易所 place / cancel 请求。
- 发送请求并维护 request sequence 到 `local_order_id` 的轻量 correlation。
- 解析 exchange ack / final result / error。
- 产出统一 `OrderResponseEvent` 或可转换为该事件的 exchange response；strategy 层 response 保留
  `kind` / `local_order_id` / `exchange_order_id`，并携带 submit response latency 所需的
  `local_receive_ns` / `exchange_ns`。
- 在 Gate / tool 层记录 rejected / cancel-rejected 的 request sequence、HTTP status 和 `error_label_hash` 等诊断字段。
- 根据 `OrderManager` 通知维护本地 `local_order_id -> exchange_order_id` cancel cache。

它不负责：

- 创建订单对象。
- 判断策略是否应该下单。
- 管理完整订单生命周期。
- 消费 private `futures.orders` 订单事实回报。
- REST reconcile 或账户 / 仓位恢复。

### 接口契约

session 生命周期：

```cpp
bool Start() noexcept;
void Stop() noexcept;
bool Ready() const noexcept;
```

`Ready()` 是 `OrderSession` 对外唯一交易可用性信号：

- `Ready() == true` 表示调用方可以尝试发送 place / cancel。
- `Ready() == false` 表示调用方不应发起新的上行交易指令。
- 外部组件不区分 disconnected、reconnect backoff、login rejected、closing、closed、not active 或 not logged in；这些原因只进入 `OrderSession` 内部 diagnostics / log。
- 断线或 not ready 不产生 `OrderFeedbackKind::kContinuityLost`，也不直接修改订单生命周期状态。
- `Ready() == false` 是上行交易能力的硬边界；组合层或 `StrategyContext` 可以用它阻止新的开仓指令。
- `Ready() == false` 不阻塞既有 order response 和 private feedback 的 drain；它只阻止新的行情驱动交易意图。

发送入口：

```cpp
OrderSendResult PlaceOrder(const StrategyOrder& order) noexcept;
OrderSendResult CancelOrder(const StrategyOrder& order) noexcept;
```

response 输出不应绑定具体 `OrderManager` 或 `Strategy`。推荐依赖事件接收能力：

```cpp
void OnOrderResponse(const OrderResponseEvent& event) noexcept;
```

实际组装时，接收方必须保证顺序：

```text
OnOrderResponse(event):
  OrderManager::OnOrderResponse(event)
  Strategy::OnOrderResponse(event, context)
```

注意语义区分：

- `ack` 只表示交易所 WS API 收到请求，不代表订单进入订单簿。
- `accepted / rejected` 来自 place final result，可推进本地订单提交状态。
- `rejected / cancel_rejected` 下游只按 `OrderResponseKind` 处理；错误标签和原始错误信息只在 Gate / tool 日志中定位。
- 最终生命周期仍以 private order feedback 为更高可信事实源。

### 下单延迟时间字段

`StrategyOrder` 记录精简后的下单 RTT 观测字段：

```cpp
std::int64_t request_send_local_ns;
std::int64_t ack_local_receive_ns;
std::int64_t response_local_receive_ns;
std::int64_t ack_exchange_ns;
std::int64_t response_exchange_ns;
std::int64_t accepted_exchange_ns;
std::int64_t finish_exchange_ns;
```

其中 `request_send_local_ns`、`ack_local_receive_ns` 和 `response_local_receive_ns` 是 `OrderSession`
在 submit/cancel WebSocket 通道上打的本机 Unix epoch ns，不使用 runtime 内部 monotonic clock。
`request_send_local_ns` 表示请求即将提交给 WebSocket 发送路径的本地时间点，不表示 TCP 帧已经完整写出；这样可以直接计算
`ack_local_receive_ns - request_send_local_ns` 和
`response_local_receive_ns - request_send_local_ns`，观测“本地提交发送路径 -> 收到 Gate response”的 RTT，并可在本机时钟同步较好时和 Gate header 时间做粗略对比。

`ack_exchange_ns` / `response_exchange_ns` 来自 Gate submit response header，优先使用 `x_out_time` 转 ns；缺失时使用
`response_time` 转 ns。它们分别表示 Gate 发出 ack / final result 的 exchange-side 时间。

`accepted_exchange_ns` 和 `finish_exchange_ns` 来自 private `futures.orders` feedback 的 `exchange_update_ns`：
accepted / `_new` 写入 `accepted_exchange_ns`，filled / cancelled / rejected 等终态写入 `finish_exchange_ns`。旧的
`exchange_update_ns` 字段仍保留为“最后一次已应用 feedback 的 exchange update time”兼容字段；需要区分阶段时优先使用
`accepted_exchange_ns` / `finish_exchange_ns`。

## OrderFeedbackSession

### 功能表述

`OrderFeedbackSession` 是交易所下行私有订单事实通道。Gate 当前实现订阅 private `futures.orders`，把 SBE
orders payload 转成统一 `OrderFeedbackEvent`。

它负责：

- 维护私有订单回报 WebSocket 连接。
- login 后订阅订单回报 channel。
- 解析 exchange feedback payload。
- 产出 accepted / partial filled / filled / cancelled / rejected / continuity lost 等统一 feedback event。
- 断线或 transport continuity lost 时发出 `OrderFeedbackKind::kContinuityLost` 控制事件。
- `OrderFeedbackKind::kContinuityLost` 只表达下行订单事实流连续性不可证明；它不由 `OrderFeedbackSession`、
  `OrderManager` 或 runtime 统一解释成“所有策略禁止开仓”。

它不负责：

- 维护 `StrategyOrder` 对象。
- 直接修改策略 execution state。
- 管理下单 / 撤单请求。
- 判断 feedback continuity lost 后如何恢复；恢复策略应由订单管理、持仓管理和 reconcile 设计共同决定。

### 接口契约

session 生命周期：

```cpp
bool Start() noexcept;
void Stop() noexcept;
bool ready() const noexcept;
```

feedback 输出推荐只依赖发布 / sink 能力：

```cpp
bool Publish(const OrderFeedbackEvent& event) noexcept;
bool PublishGlobalContinuityLost(OrderFeedbackContinuityReason reason,
                      std::int64_t local_receive_ns) noexcept;
```

跨进程生产模型使用 SHM transport：

```text
OrderFeedbackSession
  -> OrderFeedbackShmPublisher
  -> per-strategy lane
  -> strategy-side reader
```

同进程测试或后续直连模型也应保持同一语义顺序：

```text
OnOrderFeedback(event):
  OrderManager::OnOrderFeedback(event)
  Strategy::OnOrderFeedback(event, context)
```

`OrderFeedbackSession` 本身不需要知道这个顺序由哪个编排层执行。

上行可用性和下行可信性不合并成一个统一强制 gate：

```text
OrderSession::Ready() == false
  -> 上行交易指令不可用，外层可以阻止新的开仓指令。

OrderManager::feedback_continuity_lost_detected() == true
  -> 下行订单事实流连续性不可证明。
  -> 是否暂停开仓、只允许 reduce-only / hedge、触发 REST reconcile 或人工介入，由具体 Strategy / risk policy 决定。
```

## 进程 / 线程模型

当前推荐模型：

```text
GateDataSessionThread / Process
  - Gate DataSession
  - BookTicker SHM publisher

BinanceDataSessionThread / Process
  - Binance DataSession
  - BookTicker SHM publisher

StrategyThread / Process
  - DataReader
  - Strategy
  - OrderManager
  - OrderSession
  - OrderFeedback SHM reader

GateOrderFeedbackThread / Process
  - Gate OrderFeedbackSession
  - OrderFeedback SHM publisher
```

边界说明：

- 行情接入与策略解耦：`DataSession` 写 SHM，`DataReader` 读 SHM。
- 订单回报与策略解耦：`OrderFeedbackSession` 写 SHM，策略侧 reader 消费对应 lane。
- 下单路径保持短链路：`Strategy -> OrderManager -> OrderSession`，避免把下单意图跨进程排队。
- 订单事件回调顺序固定：先 `OrderManager`，后 `Strategy`。
- `Strategy` 保存策略级 `local_order_id` 关联关系，真实订单状态从 `OrderManager` 查询。

## 当前未覆盖边界

后续细化时需要单独设计：

- `PositionBook` / `PositionManager`：按 strategy / symbol 聚合真实持仓，并明确 partial fill、reduce-only、close
  order 和手续费字段。
- REST reconcile：feedback continuity lost、WS 断线、进程重启后如何恢复订单和持仓可信状态。
- account / position feedback：是否需要接入交易所账户 / 仓位私有事件。
- 多交易所 `OrderSession` interface：Gate / Binance / 其他交易所的 common order gateway 概念如何收敛。
- 事件 dispatcher 命名：当前只要求顺序语义，不强制引入虚基类；热路径优先 template / concept 或函数对象组合。
