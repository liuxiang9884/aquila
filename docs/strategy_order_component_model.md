# Strategy / Order 组件模型

## 定位

本文只记录跨交易所的策略、行情读取、下单和回报组件边界。Gate 专属协议、OrderGateway SHM 与 live 状态见
`docs/gate_trading.md`；Bitget 专属 contract 见 `docs/bitget_trading.md`。公共 order/runtime contract 位于
`core/trading/*` + `aquila::core`。

核心组件：

```text
DataReader
Strategy
OrderManager
OrderSession
OrderFeedbackSession
TradingRuntime
```

## 总体原则

- `Strategy` 只表达策略意图和策略级状态，不管理通用订单池。
- `OrderManager` 是订单状态 owner。
- `OrderSession` 是上行 place / cancel 通道。
- `OrderFeedbackSession` 是下行 private order facts 通道。
- `DataReader` 是策略侧统一行情入口。
- 所有 order response / feedback 必须先进入 `OrderManager`，再通知 `Strategy`。
- 下单路径保持短链路，不把策略订单意图跨进程排队。

## 数据流

```text
DataSession
  -> BookTicker SHM
  -> DataReader
  -> Strategy::OnBookTicker(...)

Strategy
  -> StrategyContext
  -> OrderManager
  -> OrderSessionRuntimeAdapter
  -> Gate OrderSession
  -> Gate WS

Gate order response
  -> OrderSessionRuntimeAdapter
  -> TradingRuntime::OnOrderResponse()
  -> OrderManager
  -> Strategy

Gate private futures.orders
  -> OrderFeedbackSession
  -> OrderFeedback SHM
  -> TradingRuntime::OnOrderFeedback()
  -> OrderManager
  -> Strategy
```

## TradingRuntime

`TradingRuntime<StrategyT, OrderSessionT, DataReaderT>` 组装：

- `DataReader`
- `OrderSession`
- `OrderManager`
- `StrategyContext`
- `Strategy`
- optional `OrderFeedbackShmReader`

关键规则：

- `TradingRuntime::Create()` 从已解析 config 构造组件。
- Gate adapter 负责 Gate response kind 到 core response kind 的转换、`BindRuntime()` 和 `SetRuntimeHook()` 接线。
- `OnOrderResponse()` 和 `OnOrderFeedback()` 顺序固定：先 `OrderManager`，后 `Strategy`。
- `OrderSession::Ready() == false` 是上行交易能力硬边界；它不阻止已有 response / feedback drain。
- feedback continuity lost 只是下行事实流连续性信号，不由 runtime 统一解释成禁止开仓。

## DataReader

职责：

- attach 一个或多个 `BookTicker` SHM source。
- 输出统一 `aquila::BookTicker`。
- 支持 `latest` / `drain` 读取模式。
- 记录 per-source skipped / overrun 统计。

边界：

- 不连接交易所 WebSocket。
- 不解析 Gate SBE / Binance JSON。
- 不做全局多 source 时间排序。
- 不判断订单或持仓。

接口：

```cpp
template <typename Handler>
std::uint64_t Poll(Handler& handler) noexcept;

template <typename Handler>
std::uint64_t Drain(Handler& handler, std::uint64_t max_events) noexcept;
```

handler：

```cpp
void OnBookTicker(const aquila::BookTicker& ticker) noexcept;
```

## Strategy

职责：

- 消费行情、order response、order feedback。
- 保存 signal、execution group、pair state、position state 等策略级信息。
- 通过 `StrategyContext` 发出 place / cancel 意图。
- 通过 `FindOrder()` 查询 `OrderManager` 中的订单状态。
- 根据 `feedback_continuity_lost_detected()` 决定暂停开仓、只减仓、stop-and-flat 或人工介入。

边界：

- 不分配 `local_order_id`。
- 不维护通用订单池。
- 不解析 exchange response / feedback。
- 不把 Ack / accepted 当成真实持仓变化。

典型接口：

```cpp
void OnBookTicker(const aquila::BookTicker& ticker,
                  StrategyContext& context) noexcept;
void OnOrderResponse(const OrderResponseEvent& event,
                     StrategyContext& context) noexcept;
void OnOrderFeedback(const OrderFeedbackEvent& event,
                     StrategyContext& context) noexcept;
bool ShouldStop() const noexcept;
```

`StrategyContext` 是窄接口：

```cpp
OrderPlaceResult PlaceLimitOrder(OrderPlaceRequest request) noexcept;
OrderCancelResult CancelOrder(const OrderCancelRequest& request) noexcept;
const StrategyOrder* FindOrder(std::uint64_t local_order_id) const noexcept;
```

## OrderManager

职责：

- 分配 `local_order_id`。
- 维护固定容量订单池。
- 调用 gateway place / cancel。
- 消费 `OrderResponseEvent`。
- 消费 `OrderFeedbackEvent`。
- 维护 exchange order id cache、累计成交、成交价、终态和 continuity lost flag。
- 暴露只读查询给 strategy。

边界：

- 不连接 WebSocket。
- 不解析交易所 wire message。
- 不做策略信号。
- 不作为完整账户持仓事实源。

接口：

```cpp
OrderPlaceResult PlaceLimitOrder(OrderPlaceRequest request) noexcept;
OrderCancelResult CancelOrder(OrderCancelRequest request) noexcept;
void OnOrderResponse(const OrderResponseEvent& event) noexcept;
void OnOrderFeedback(const OrderFeedbackEvent& event) noexcept;
const StrategyOrder* FindOrder(std::uint64_t local_order_id) const noexcept;
bool RetireFinishedOrder(std::uint64_t local_order_id) noexcept;
bool feedback_continuity_lost_detected() const noexcept;
```

gateway contract：

```cpp
OrderSendResult PlaceOrder(const OrderPlaceRequest& request) noexcept;
OrderSendResult CancelOrder(const OrderCancelRequest& request) noexcept;
void CacheExchangeOrderId(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept;
void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept;
```

### 多路 OrderSession 扩展边界

如果后续实现多路 Gate trading WebSocket，下层应优先做成 `OrderManager` 后面的 composite gateway，例如
`MultiOrderSessionGateway`，而不是让 `Strategy` 或 `StrategyContext` 直接感知 N 条 session。当前 fanout 目标是让
策略 / OMS 生成的多个 child order 低 skew 地写入多条 connection；duplicate / split、winner、overfill 和 cancel
触发点都属于策略 / OMS 语义，不属于多路 `OrderSession` 层。

2026-06-30 已落地两种 gateway 形态：`exchange/gate/trading/multi_order_session_gateway.h` 是单进程
`1 thread : n OrderSession` baseline；`core/trading/order_gateway_client.h` + `tools/gate/gate_order_gateway.cpp`
是独立 `order-gateway-process` / SHM V3。LeadLag live-orders 已能通过 `[strategy.order_gateway]` 选择
`OrderGatewayClient`，并通过 `order_session_fanout` 生成多个 child order。Bitget 已完成 fanout=1 gateway passive IOC live smoke；
四路 gateway 当前只有代码、自动测试和 validate-only 证据，仍不宣称四路成交率或延迟收益。

设计边界：

- `OrderManager` 仍通过同一组 gateway contract 调用 place / cancel / cache / forget。
- `MultiOrderSessionGateway` 内部负责 route policy、`local_order_id -> session_index` route table、per-session ready / health 和账号级 submit / cancel / pending budget。
- 每个 child order 必须有独立 `local_order_id`；full duplicate 也必须表现为多个独立 child order。拆单算法属于 strategy 或后续更高层执行模块，不属于 `OrderSession`。
- cancel 默认回原下单 session；跨 session cancel failover 不能作为 V1 默认行为。
- private `futures.orders` 仍由单个账号级 `OrderFeedbackSession` 接收并发布到 SHM；order feedback 不按 order session 拆分。
- `MultiOrderSessionGateway` 不解释 parent signal、child group、winner、fillability 或 overfill；它只负责 route、send、cancel 和 session health / budget。

最小 baseline 使用 `core::kAutoGatewayRoute` 表示 gateway 自选 route；显式 `gateway_route_id=0..n-1`
表示发往运行时配置的某条 Gate order session。`OrderManager` 只复制该字段，不解释策略 fanout 语义。Ack response
的业务入口仍是 `OrderManager::OnOrderResponse()`，route table 只服务 cancel / cache / forget 回原 session。

生产 SHM gateway contract 见 `docs/gate_trading.md`：strategy 与 order gateway 拆成 2 个进程，strategy 进程的
`StrategyOrderOwnerThread` 拥有 `OrderManager`、route table 和 ready flags；`order-gateway-process` 内
`OrderSessionWorker[i]` 独占 `OrderSession[i]` 和对应 WebSocket connection。跨进程使用一个 SHM 对象承载 N 路
`command_queue` 和 N 路 `event_queue`，`N` 是运行时参数且最大为 `16`。`PlaceOrder()` 的 `kOk` 只表示 command
已进入 gateway queue，不等价于已经写到 socket；真实 Ack / final response 仍先进入 `OrderManager`，再通知 `Strategy`。
当前 LeadLag V1 使用 execution group / position lifecycle 级 `parent_id` 聚合，open、close、stoploss 和 retry
child 可共享同一个 `parent_id`；child order 仍保留唯一 `local_order_id`，不修改 `LocalOrderIdCodec`。如需 fanout
batch 级诊断，应新增独立 batch id。

## OrderSession

职责：

- 维护 private trading WebSocket 和 login state。
- 编码 place / cancel。
- 发送请求并维护 request sequence -> local order id correlation。
- 解析 submit Ack / final response / error。
- 输出 `OrderResponseEvent`。
- 维护 cancel 需要的 local id -> exchange order id cache。
- 记录 Gate / socket / latency diagnostics。

边界：

- 不创建订单对象。
- 不管理完整订单生命周期。
- 不消费 private `futures.orders`。
- 不做 REST reconcile。

`Ready()` 语义：

- `true`：调用方可以尝试上行 place / cancel。
- `false`：调用方不应发起新的上行交易指令。
- 具体原因只进入 diagnostics / log。
- 不产生 `OrderFeedbackKind::kContinuityLost`。

Ack / response 语义：

- `ack` 只表示 Gate WS API 收到请求，不代表订单进入订单簿。
- Gate `5xx` submit / cancel error 只表示请求结果未知，adapter 转成 `OrderResponseKind::kUnknownResult`；`OrderManager` 记录 response timing 但不把订单标记为 terminal，策略进入 `needs_reconcile` 并暂停新开仓，等待 private feedback 或 REST reconcile。若对应 terminal feedback 精确解决该 symbol 的所有 pending unknown order，且没有 global continuity lost / manual intervention 等更高等级 degraded 状态，策略会自动清除该 symbol 的 unknown-result pause 并恢复新开仓。
- 明确业务拒单仍按 `OrderResponseKind::kRejected` / `kCancelRejected` 推进；原始错误信息留在 Gate/tool 日志。
- 最终生命周期以 private order feedback 为更高可信事实源。

## OrderFeedbackSession

职责：

- 维护 private feedback WebSocket。
- login 后订阅 Gate `futures.orders`。
- 解析 accepted / partial filled / filled / cancelled / rejected。
- 发布统一 `OrderFeedbackEvent`。
- 断线或 continuity lost 时发布 `OrderFeedbackKind::kContinuityLost` 控制事件。

边界：

- 不修改 `StrategyOrder`。
- 不解释 continuity lost 后如何恢复。
- 不发 place / cancel。

跨进程模型：

```text
OrderFeedbackSession
  -> OrderFeedbackShmPublisher
  -> per-strategy lane
  -> TradingRuntime feedback reader
```

## Order request numeric contract

当前 place contract：

```text
char symbol[32]
double price + price_decimal_places
double quantity + quantity_decimal_places
```

- Strategy / risk 使用 `double` 做价格、数量和 notional 计算。
- decimal places 从 lag instrument metadata 复制到 `OrderPlaceRequest`，不在下单热路径推导。
- Direct Gateway、SHM Gateway 与 `OrderSession` 共享同一个 `OrderPlaceRequest`；SHM envelope
  只增加 command sequence、enqueue timestamp 和 command kind。
- exchange encoder 在 `OrderSession` 内生成 fixed decimal text 并直接写最终 JSON buffer；
  Strategy、`StrategyOrder` 和 Gateway SHM 不保存 price/quantity text。
- decimal-size Gate contract 在 `OrderSession` 把最终 `size` text 编码为 JSON string。
- feedback event / SHM / `OrderManager` 使用 `double` 表示累计成交、剩余和撤单数量。

cancel contract 是只含 `local_order_id`、`parent_id` 和 route 的
`OrderCancelRequest`。Gate/Bitget `OrderSession` 使用自己的 local-to-exchange order id
cache；cache miss 分别沿用 Gate `t-<local_order_id>` 与 Bitget `clientOid` fallback。

## 延迟字段

`StrategyOrder` 保留：

```text
request_send_local_ns
ack_local_receive_ns
response_local_receive_ns
ack_exchange_ns
response_exchange_ns
accepted_exchange_ns
finish_exchange_ns
```

口径：

- `request_send_local_ns`、`ack_local_receive_ns`、`response_local_receive_ns` 是本机 Unix epoch ns。
- `ack_rtt_ns = ack_local_receive_ns - request_send_local_ns` 是 Ack path 主指标。
- `ack_exchange_ns` / `response_exchange_ns` 来自 Gate submit response header。
- `accepted_exchange_ns` / `finish_exchange_ns` 来自 private `futures.orders` feedback。
- 跨机器 clock 不用于推导单程网络延迟。

新增诊断字段或 report CSV 字段必须同步更新 `docs/diagnostic_fields.md`。

## 当前未覆盖边界

- `PositionBook` / `PositionManager`。
- REST reconcile / resume。
- account / position realtime feedback。
- 多交易所 common order gateway 收敛。
- 多路 `OrderSession` 的真实 live smoke / fillability 验证；Bitget fanout=1 gateway 证据不能替代四路验证。
- batch / amend / cancel-all。

## 验证命令

```bash
ctest --test-dir build/debug -R '(core_order_pool|strategy|gate_order|gate_submit|order_session_config|order_feedback|lead_lag)' --output-on-failure
git diff --check
```
