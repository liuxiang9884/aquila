# Strategy / Order 组件模型

## 定位

本文记录当前策略、行情读取、下单和回报组件的边界。公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`；Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h`。

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
OrderPlaceResult PlaceLimitOrder(OrderCreateRequest request) noexcept;
OrderCancelResult CancelOrder(std::uint64_t local_order_id) noexcept;
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
OrderPlaceResult PlaceLimitOrder(OrderCreateRequest request) noexcept;
OrderCancelResult CancelOrder(std::uint64_t local_order_id) noexcept;
void OnOrderResponse(const OrderResponseEvent& event) noexcept;
void OnOrderFeedback(const OrderFeedbackEvent& event) noexcept;
const StrategyOrder* FindOrder(std::uint64_t local_order_id) const noexcept;
bool RetireFinishedOrder(std::uint64_t local_order_id) noexcept;
bool feedback_continuity_lost_detected() const noexcept;
```

gateway contract：

```cpp
OrderSendResult PlaceOrder(const StrategyOrder& order) noexcept;
OrderSendResult CancelOrder(const StrategyOrder& order) noexcept;
void CacheExchangeOrderId(std::uint64_t local_order_id,
                          std::uint64_t exchange_order_id) noexcept;
void ForgetExchangeOrderId(std::uint64_t local_order_id) noexcept;
```

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

## Quantity Contract

当前 order quantity contract：

```text
double quantity + quantity_text
```

- strategy / risk 使用 `double` 做数量和 notional 计算。
- adapter / exchange encoder 使用已按 instrument metadata 格式化的 `quantity_text`。
- decimal-size Gate contract 通过 `quantity_text` 编码真实下单字段。
- feedback event / SHM / `OrderManager` 使用 `double` 表示累计成交、剩余和撤单数量。

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
- batch / amend / cancel-all。

## 验证命令

```bash
ctest --test-dir build/debug -R '(core_order_pool|strategy|gate_order|gate_submit|order_session_config|order_feedback|lead_lag)' --output-on-failure
git diff --check
```
