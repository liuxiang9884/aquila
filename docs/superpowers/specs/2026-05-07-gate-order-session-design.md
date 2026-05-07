# Gate OrderSession 设计

> 2026-05-07 更新：本设计中的“Strategy 缓存 Gate wire-ready fields”边界已被 `docs/superpowers/plans/2026-05-07-order-session-struct-flow-implementation-plan.md` 调整。当前事实源以 `doc/project_onboarding_guide.md`、`doc/agent-handoff-gate-trade-architecture.md` 和当前代码为准：Strategy 直接发送订单 struct，Gate `OrderSession` 在发送路径完成 JSON 序列化。

## 背景

`aquila` 下一步需要把 Gate futures 的上行交易指令接入 C++ 主路径。当前已经有：

- Gate futures market data / data session / SBE BBO 行情链路；
- Gate submit WS response parser：`exchange/gate/trading/submit_response_parser.h`；
- Gate REST 账户、查询、下单、撤单测试脚本；
- Sirius 旧实现作为参考，但旧实现绑定 Drogon `WebSocketClient`，且把交易 WS login、下单、撤单和私有订单回报放在同一个 `TradeEngine` 中。

本设计只覆盖第一版 Gate submit/cancel `OrderSession`。它不把 Sirius 的重 `OrderStruct` 和 Drogon 继承模型搬进 Aquila。

## 目标

第一版目标是实现一个低延迟、边界清晰、可测试的 `aquila::gate::OrderSession`：

- 支持 Gate WebSocket `futures.login`；
- 支持 `futures.order_place`；
- 支持 `futures.order_cancel`；
- 解析 `ack=true` 的轻量 API ack；
- 解析 `ack=false` 的最终 result / error；
- 用本地 `local_order_id` 关联 Gate `request_id`、Gate order `text` 和交易所 `exchange_order_id`；
- 对 Strategy 输出同步 `OrderResponse`，让 Strategy 建立本地订单和交易所订单的映射。

本设计不声明性能收益。后续进入实现后，任何低延迟结论都需要 benchmark 或 live 运行证据。

## 非目标

第一版不做：

- 风控、仓位限制、订单状态机、订单执行策略；
- 私有订单 / 成交 / 仓位回报流；
- REST reconcile；
- batch place、amend、cancel all、cancel ids、cancel cp；
- Gate price order；
- 多线程队列化的 submit response 分发；
- 把 Sirius 的完整 `OrderStruct` 作为 `OrderSession` 输入。

其中私有回报流属于后续 `OrderFeedbackSession`，不进入本次 submit/cancel 主路径。

## 线程和模块边界

当前推荐线程结构保持：

```text
StrategyThread
  - Strategy
  - aquila::gate::OrderSession

GateOrderFeedbackThread
  - aquila::gate::OrderFeedbackSession

OrderFeedbackSession -> SPSC -> StrategyThread
```

`OrderSession` 与 Strategy 位于同一个线程。调用 `PlaceOrder()` / `CancelOrder()` 后，本地 send 结果立即返回；Gate 的 ack/result/error 在同一 session 回调路径中同步交给 Strategy。

边界如下：

| 模块 | 职责 |
| --- | --- |
| Strategy | 风控、订单状态、订单执行逻辑、symbol metadata 校验、价格和数量格式化、本地订单对象生命周期。 |
| OrderSession | Gate submit/cancel wire protocol、login、固定缓冲区编码、WebSocket 写入、`request_id` 关联、轻量 API response parse 和回调。 |
| OrderFeedbackSession | 后续私有 `orders` / `usertrades` / `positions` 回报 decode、SPSC 写回 Strategy。 |

## 命名

交易模块位于 `aquila::gate` namespace，因此类型名不再带 `Gate` 前缀：

- `OrderSession`
- `OrderWireFields`
- `PlaceOrderRequest`
- `CancelOrderRequest`
- `OrderSendResult`
- `OrderResponse`
- `RequestIdCodec`
- `OrderTextCodec`

已有 `GateSubmitResponse` 解析结构可以在实现时迁移或兼容保留；新增 submit/cancel 类型使用无前缀命名。

## Sirius 参考结论

Sirius 中值得保留的做法：

- `request_id` 的高位编码请求类型，低位保存递增序列；
- `text="t-<local_order_id>"` 用于把 Gate order result 反查回本地订单；
- 下单和撤单都由 trade/order session 发出；
- cancel 优先使用交易所 order id，不存在时使用 `text`；
- `request_sequence -> local_order_id` 是轻量关联表，不是订单 pending table；
- place ack 只表示 Gate 接收请求，不建立交易所 order id 映射；
- place final result 才在 `OrderSession` 内建立 `local_order_id -> exchange_order_id` 缓存，供后续 cancel 编码优先使用。

不保留的做法：

- 不继承 Drogon `WebSocketClient`；
- 不把私有订单回报和上行 submit/cancel 放在同一个对象里；
- 不让 `OrderSession` 持有 Strategy 的完整订单状态；
- 不把 Strategy 的 `OrderStruct` 直接暴露给 `OrderSession`。

## 输入结构

当前实现已改为 `OrderSession` 直接接收 Strategy 创建好的订单 struct，并在发送路径完成 Gate JSON 编码。Strategy 不缓存 Gate wire fields，不维护 exchange order id 索引；risk、symbol metadata 和订单状态仍属于 Strategy。

```cpp
struct PlaceOrderStructFields {
  std::int64_t local_order_id;
  std::string_view symbol;
  OrderSide side;
  std::int64_t quantity;
  std::string_view price_text;
  TimeInForce time_in_force;
  bool reduce_only;
};
```

约束：

- Gate signed size 由 `OrderSession` 根据 `side + quantity` 现场派生；
- market order 使用 `price_text="0"` 且 `time_in_force=ImmediateOrCancel`；
- limit order 使用 Strategy 预格式化后的 `price_text`；
- `text` 第一版使用 `t-<local_order_id>`；
- place final result 成功后，`OrderSession` 内部缓存 `local_order_id -> exchange_order_id`，缓存最多保留 `request_map_capacity` 条；
- cancel 优先使用 `OrderSession` 内部缓存的 exchange order id；没有缓存时按 `text="t-<local_order_id>"` 撤单；
- `forget_exchange_order_id_for_local_order()` 是后续成交终态 / reconcile 清理缓存的显式入口。

## RequestIdCodec

`RequestIdCodec` 用于把请求类型和递增序列编码进 Gate WS `request_id`：

```text
encoded_request_id = (request_type << 56) | (sequence & ((1 << 56) - 1))
```

请求类型第一版包含：

| 类型 | 含义 |
| --- | --- |
| `kLogin` | `futures.login` |
| `kPlaceOrder` | `futures.order_place` |
| `kCancelOrder` | `futures.order_cancel` |

实现要求：

- 解码失败时 response 归类为 malformed/unexpected，不调用 Strategy 的业务状态更新；
- 关联表 key 使用低 56 位 raw sequence；
- `request_sequence` 在冷路径或 session 构造后递增使用，不允许每次随机生成；
- 保留类型位，避免未来 place/cancel/login response 共用一个递增空间时混淆。

## OrderTextCodec

第一版只支持 `t-<local_order_id>`：

```text
t-12345 -> local_order_id = 12345
```

`OrderTextCodec` 负责：

- 构造 `t-<local_order_id>`；
- 从 Gate result 的 `text` 中解析本地订单 id；
- 在 cancel fallback 中构造 `text` order id。

Sirius 的 `ao-<price_order_id>` 属于 price order / auto order 场景，本次不实现，但保留在文档中作为后续扩展方向。

## 本地 send 结果

`PlaceOrder()` / `CancelOrder()` 的返回值表示本地编码和写入状态，不表示交易所结果：

```cpp
enum class OrderSendStatus : std::uint8_t {
  kOk,
  kNotLoggedIn,
  kNotActive,
  kInflightFull,
  kEncodeBufferTooSmall,
  kNoPreparedWriteSlot,
  kWriteUnavailable,
};

struct OrderSendResult {
  OrderSendStatus status;
  std::uint64_t request_sequence;
  std::uint64_t encoded_request_id;
};
```

规则：

- `kOk` 表示本地已把请求交给 WebSocket 写路径；
- `kNotLoggedIn` / `kNotActive` 不生成假 Gate response；
- `kEncodeBufferTooSmall` 表示固定缓冲区不足，必须由调用方按本地失败处理；
- `kNoPreparedWriteSlot` 表示 WebSocket prepared write arena 没有可用 slot；
- `kWriteUnavailable` 表示 WebSocket pending write 队列暂时不可写；
- local send 失败时不插入 `request_sequence -> local_order_id` 关联。

## OrderResponse

`OrderResponse` 是 Gate 轻量 API response 到 Strategy 的同步回调对象。`exchange_order_id` 会继续透传给 Strategy 事件，但当前 Strategy 不维护 exchange order id 索引；OrderSession 内部缓存负责 cancel 编码所需的本地/交易所 id 对应。

```cpp
enum class OrderResponseKind : std::uint8_t {
  kAck,
  kAccepted,
  kRejected,
  kCancelAccepted,
  kCancelRejected,
};

struct OrderResponse {
  OrderResponseKind kind;
  std::int64_t local_order_id;
  std::uint64_t exchange_order_id;
  std::uint64_t request_sequence;
  std::uint16_t http_status;
  std::uint64_t error_label_hash;
};
```

第一版回调接口：

```cpp
struct OrderResponseHandler {
  void OnOrderResponse(const OrderResponse& response) noexcept;
};
```

语义：

- place `ack=true`：输出 `kAck`，`exchange_order_id=0`，不清理关联表；
- place final result：输出 `kAccepted`，带 `exchange_order_id`，清理关联表；
- place error：输出 `kRejected`，清理关联表；
- cancel final result：输出 `kCancelAccepted`，清理关联表；
- cancel error：输出 `kCancelRejected`，清理关联表；
- 未知 `request_id` 或无法关联本地订单时，只计入 diagnostics，不调用 Strategy 业务回调。

## 关联表

`OrderSession` 持有一个轻量关联表：

```cpp
absl::flat_hash_map<std::uint64_t, std::int64_t> request_id_to_local_order_id_;
```

其中 key 是 raw `request_sequence`，value 是 `local_order_id`。这不是 pending order table，不保存价格、数量、状态、风控信息或重试策略。

容量策略：

- session 初始化时 `reserve()` 一个较大的默认值，建议第一版使用 16384；
- 达到上限时 `PlaceOrder()` / `CancelOrder()` 返回 `kInflightFull`；
- disconnect / reconnect 时清空关联表，由 Strategy 通过 session state 和后续 reconcile 处理未知状态；
- 不从关联表推导真实订单状态。

## 编码缓冲区

第一版固定缓冲区大小：

| 请求 | 缓冲区 |
| --- | ---: |
| login | 1024 bytes |
| place order | 1024 bytes |
| cancel order | 512 bytes |

编码使用 `fmt::format_to_n` 或等价的固定缓冲写入方式。若输出被截断，返回 `kEncodeBufferTooSmall`，不发送半截 JSON。

## JSON response parse

`submit_response_parser.h` 已能解析 `ack`、`result`、`error`、`exchange_order_id` 等字段，并直接返回 decoded correlation 字段：

- decoded `request_type`；
- raw `request_sequence`；
- optional `req_id`；
- optional `exchange_order_id`；
- optional parsed `local_order_id` from `text`；
- optional `error_label` 稳定 hash。

parser 保留 full profile 以兼容测试、benchmark 和诊断中使用的 request / req_id / text hash；`OrderSession` 热路径使用 no-hash profile，成功路径只解析 correlation 所需字段，跳过 request id / req_id / text hash，错误路径仍保留 `error_label_hash`。

生产路径继续使用 `simdjson::ondemand`。如果 payload 没有 padding，沿用现有 fallback 策略，不为了 submit/cancel response 引入新的 JSON 库。

## Session 状态

`OrderSession` 至少需要区分：

- websocket active；
- login request sent；
- login ready；
- disconnected。

第一版规则：

- login ready 前的 place/cancel 返回 `kNotLoggedIn`；
- websocket inactive 时返回 `kNotActive`；
- disconnect 后清空 request correlation；
- reconnect 后重新 login；
- 不在断线时主动构造 rejected/cancelled response。

## 测试边界

第一版实现应优先补这些测试：

- `RequestIdCodec` 编解码；
- `OrderTextCodec` 构造和解析；
- place/cancel JSON encoder 固定输出；
- 编码缓冲区不足时不写入；
- session 未登录时拒绝 place/cancel；
- session 写路径不可用时不插入关联表；
- place ack 不清理关联表；
- place result 在 `OrderSession` 内缓存 `local_order_id -> exchange_order_id` 并清理关联表；
- place error 清理关联表并输出 rejected；
- cancel fallback 使用 `t-<local_order_id>`；
- cancel 优先使用 `OrderSession` 内部缓存的 exchange order id；
- disconnect 清空关联表。

## Benchmark 边界

实现完成后再补 benchmark，至少覆盖：

- place request encode；
- cancel request encode；
- submit ack minimal parse；
- submit result parse；
- `OrderSession::HandleText()` response dispatch。

benchmark 只用于比较本仓库实现的版本变化，不把 Sirius/Drogon 结果写成 Aquila 生产路径性能结论。

## 后续范围

本设计完成后，后续独立设计：

- `OrderFeedbackSession`：SBE private `orders`、`usertrades`、`positions` decode；
- feedback SPSC event 结构；
- Strategy 订单对象和 symbol metadata cache；
- REST reconcile；
- 多交易所 `OrderSession` 抽象是否需要共享薄接口。
