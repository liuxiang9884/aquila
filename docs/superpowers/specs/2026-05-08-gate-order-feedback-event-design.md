# Gate OrderFeedback Event 设计

## 背景

Gate `OrderSession` 第一版已经覆盖 `futures.login`、`futures.order_place`、`futures.order_cancel`
和轻量 API response。实盘 WebSocket 下单测试证明 `OrderSession` 可以提交订单，但当前只依赖
place/cancel API ack/result 无法完整推进订单生命周期：Gate 可能先返回 API ack，而订单是否进入撮合、
成交或终止需要从私有 `futures.orders` 回报确认。

本设计只收敛第一版 `OrderFeedbackSession` 产生给 Strategy 的订单生命周期 event。它不实现账户、
仓位、REST reconcile，也不把 Sirius 的 `TradeEngine` 混合职责搬入 Aquila。

## 目标

第一版 `OrderFeedbackSession`：

- 订阅并解析 Gate private `futures.orders`；
- 从订单 `text="t-<local_order_id>"` 解出本地订单 id；
- 把 Gate `finish_as`、数量、价格、更新时间和可选 role 转成固定 Strategy feedback event；
- 不访问 Strategy 的订单对象和 `OrderPool`；
- 不直接修改订单状态，只把 event 传给 Strategy；
- 允许后续在架构讨论后选择 event 承载结构。

## 非目标

第一版不做：

- `futures.usertrades` 私有成交频道；
- `futures.positions` / account / balance 状态；
- REST reconcile；
- 跨 channel 成交幂等；
- fee、point fee、PnL、仓位归因；
- 下单、撤单或风控逻辑。

当前结论是第一版不引入 `futures.usertrades`：`orders` 与 `usertrades` 回报到达顺序可能随机，双 channel
会额外引入幂等和状态合并成本；`orders` 中已有第一版订单生命周期所需的主要信息。

## 事件来源

唯一生命周期事实源：

```text
Gate private futures.orders
```

`OrderSession` 的 API ack/result 只用于本地发送诊断和必要的 submit/cancel response，不作为订单最终生命周期事实源。

约束：

- `ack=true` 只表示 Gate 收到 API 请求，不产生 `OrderAcceptedFeedback`。
- `finish_as="_new"` 表示订单被交易所接受，产生 accepted event。
- `finish_as="_update"` 表示订单成交进度更新，通常产生 partial filled event。
- `finish_as="filled"` 表示订单终态完全成交，产生 filled event。
- 其他 terminal `finish_as` 统一产生 cancelled/finished 类 event，并用 `finish_reason` 表达终止原因。
- `OrderRejectedFeedback` 不来自 `futures.orders` 主回报流，只保留给本地发送失败或 API submit error。

## Local Order Id

`local_order_id` 是跨 strategy、跨进程路由订单回报的主身份，类型使用 `std::uint64_t`：

```text
local_order_id = (strategy_id << 56) | strategy_order_id
```

含义：

- 高 8 bit：`strategy_id`；
- 低 56 bit：strategy 内部单调递增的 `strategy_order_id`；
- `local_order_id == 0` 是无效值；
- Gate `text` 仍使用 `t-<local_order_id>`，不增加额外分隔字段；
- `OrderFeedbackSession` 从 `text` 解出 `local_order_id` 后，可直接用高 8 bit 路由到对应 strategy；
- `exchange_order_id` 不是 feedback 路由主键，只作为 accepted 后的辅助身份和 cancel 编码优化。

第一版实现入口：

```text
core/trading/order_id.h
```

## 数量和价格

Gate futures REST / WebSocket 文档中订单 `size` 和 `left` 的语义是 integer contract quantity；本地 Strategy
event 使用 `std::int64_t quantity` 表达非负累计数量，不用 `double`。

Gate SBE schema 中 orders message 有 `sizeExponent`、`sizeMantissa` 和 `leftMantissa`，但第一版按 Gate
futures 订单数量语义处理为整数合约张数：

- `cumulative_filled_quantity = abs(size) - abs(left)`；
- `left_quantity = abs(left)`；
- SBE `sizeExponent` 第一版要求为 `0`；
- 如果实盘遇到非零 `sizeExponent`，先记 unsupported parse error，不隐式转成 double。

价格使用 `double`：

- `fill_price` 表示 Gate orders 回报中的累计成交均价；
- Strategy 根据本地累计成交量和新的 `cumulative_filled_quantity` 计算本次成交增量。

## Event 类型

### Accepted

```cpp
struct OrderAcceptedFeedback {
  std::uint64_t local_order_id{0};
  std::uint64_t exchange_order_id{0};
  std::int64_t exchange_update_ns{0};
};
```

语义：

- 只由 `finish_as="_new"` 产生；
- `exchange_order_id` 在 accepted event 中建立本地订单和交易所订单 id 的对应关系；
- `exchange_update_ns` 来自 Gate update time，统一转换为 ns。

Strategy 处理：

- 找不到订单则忽略并计数；
- 已结束订单忽略，保证幂等；
- 设置 `status = kAccepted`；
- 保存 `exchange_order_id` 和 `exchange_update_ns`。

### PartialFilled

```cpp
struct OrderPartialFilledFeedback {
  std::uint64_t local_order_id{0};

  std::int64_t cumulative_filled_quantity{0};
  std::int64_t left_quantity{0};

  double fill_price{0.0};
  std::int64_t exchange_update_ns{0};
};
```

语义：

- 主要由 `finish_as="_update"` 且 `left_quantity > 0` 产生；
- quantity 为非负累计数量，不带 side 符号；
- Strategy 使用本地订单 side / quantity 判断方向；
- 不携带 `exchange_order_id`，成交状态只按 `local_order_id` 查本地订单。

Strategy 处理：

- 找不到订单则忽略并计数；
- 已结束订单忽略；
- `delta = cumulative_filled_quantity - order.cumulative_filled_quantity`；
- `delta <= 0` 视为重复或旧回报，忽略；
- 补累计成交数量、成交金额和均价；
- 设置 `status = kPartialFilled`；
- 保存 `exchange_update_ns`。

### Filled

```cpp
enum class OrderRole : std::uint8_t {
  kNone,
  kMaker,
  kTaker,
};

struct OrderFilledFeedback {
  std::uint64_t local_order_id{0};

  std::int64_t cumulative_filled_quantity{0};

  double fill_price{0.0};
  OrderRole role{OrderRole::kNone};
  std::int64_t exchange_update_ns{0};
};
```

语义：

- 由 `finish_as="filled"` 产生；
- filled event 不携带 `left_quantity`，因为终态剩余数量应为 0；
- 如果 parser 收到 `finish_as="filled"` 但 `left_quantity != 0`，第一版记异常并不产生 filled event；
- `role` 来自 Gate orders 回报中的 maker/taker 字段；如果字段缺失或为空，保持 `kNone`。

Strategy 处理：

- 找不到订单则忽略并计数；
- 已结束订单忽略；
- 先按累计成交量计算 delta 并补齐成交累计；
- 设置 `status = kFilled`；
- 设置 `is_finished = true`；
- 保存 `role` 和 `exchange_update_ns`。

### Cancelled / Terminal Finished

```cpp
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

struct OrderCancelledFeedback {
  std::uint64_t local_order_id{0};

  std::int64_t cumulative_filled_quantity{0};
  std::int64_t cancelled_quantity{0};

  double fill_price{0.0};
  OrderFinishReason finish_reason{OrderFinishReason::kUnknown};
  std::int64_t exchange_update_ns{0};
};
```

语义：

- 该 event 表示订单没有以完全成交终止，而不是只表示人工撤单；
- `cancelled_quantity = left_quantity`；
- 如果终止前有部分成交，`cumulative_filled_quantity` 和 `fill_price` 用于让 Strategy 补齐最后成交累计；
- `liquidated`、`auto_deleveraging`、`position_close` 不映射为 rejected，而是映射为 terminal finish reason。

`finish_as` 映射：

| Gate `finish_as` | `OrderFinishReason` |
| --- | --- |
| `cancelled` | `kManualCancelled` |
| `ioc` | `kImmediateOrCancel` |
| `reduce_only` | `kReduceOnly` |
| `reduce_out` | `kReduceOut` |
| `stp` | `kSelfTradePrevention` |
| `liquidated` | `kLiquidated` |
| `auto_deleveraging` | `kAutoDeleveraging` |
| `position_close` | `kPositionClose` |

Strategy 处理：

- 找不到订单则忽略并计数；
- 已结束订单忽略；
- 先按 `cumulative_filled_quantity` 补齐成交 delta；
- `cumulative_filled_quantity == 0` 时设置 `status = kCancelled`；
- `cumulative_filled_quantity > 0` 时设置 `status = kPartiallyCancelled`；
- 设置 `is_finished = true`；
- 保存 `finish_reason` 和 `exchange_update_ns`。

### Rejected

```cpp
enum class OrderRejectReason : std::uint8_t {
  kUnknown,
  kSessionRejected,
  kExchangeRejected,
};

struct OrderRejectedFeedback {
  std::uint64_t local_order_id{0};

  OrderRejectReason reason{OrderRejectReason::kUnknown};
};
```

`Rejected` 不属于 `futures.orders` 主生命周期 event。它只用于：

- `OrderSession` 本地写 socket / 编码 / local send 失败；
- WebSocket API 下单最终响应 `data.errs`，且没有对应 `orders` lifecycle。

## Strategy 状态

第一版 Strategy 订单状态建议为：

```cpp
enum class OrderStatus : std::uint8_t {
  kCreated,
  kSent,
  kAccepted,
  kPartialFilled,
  kFilled,
  kCancelSent,
  kCancelled,
  kPartiallyCancelled,
  kRejected,
};
```

含义：

- `kSent`：本地已经把 place 请求交给 `OrderSession` 写路径；
- `kAccepted`：`futures.orders` 返回 `_new`；
- `kPartialFilled`：收到非终态累计成交更新；
- `kFilled`：完全成交终态；
- `kCancelSent`：本地已经把 cancel 请求交给 `OrderSession` 写路径；
- `kCancelled`：未成交或剩余被终止，且累计成交为 0；
- `kPartiallyCancelled`：部分成交后剩余被终止；
- `kRejected`：本地发送失败或 API submit error。

## Event 承载方式待选

当前保留两个方案，架构讨论后再决定。

### 方案 A：宽结构

```cpp
enum class OrderFeedbackKind : std::uint8_t {
  kAccepted,
  kPartialFilled,
  kFilled,
  kCancelled,
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
  std::int64_t exchange_update_ns{0};
};
```

优点：

- 简单、连续、无动态分配；
- parser 填字段和 Strategy switch 处理都直接；
- 没有 union active member 生命周期问题；
- 第一版更容易测试和 live 验证。

代价：

- 每个 event 携带少量无用字段；
- 如果未来 feedback 流量很高，SPSC cache footprint 会更大。

### 方案 B：tagged union

```cpp
struct OrderFeedbackEvent {
  OrderFeedbackKind kind{OrderFeedbackKind::kAccepted};

  union {
    OrderAcceptedFeedback accepted;
    OrderPartialFilledFeedback partial_filled;
    OrderFilledFeedback filled;
    OrderCancelledFeedback cancelled;
  };
};
```

优点：

- 单条 event payload 更小；
- 字段语义和具体 event struct 对齐更严格。

代价：

- 必须保证 `kind` 和 active member 一致；
- 未来 event struct 一旦加入非平凡成员，需要显式处理构造、析构、拷贝和移动；
- 默认初始化、测试和调试更容易误读 inactive member。

## 未决架构问题

后续架构讨论需要继续确认：

1. `OrderFeedbackSession` 与 StrategyThread 之间的队列类型、容量、溢出策略和消费预算。
2. 第一版 event 承载方式选择宽结构还是 tagged union。
3. `exchange_update_ns`、本地接收时间和 Strategy 消费时间是否都进入 event 或 diagnostics。
4. 断线 / 重连后未知订单状态如何通过 REST reconcile 恢复。
5. `OrderSession` 内部 `local_order_id -> exchange_order_id` cache 在订单终态后由谁触发清理。
6. 是否需要单独的 unsupported terminal / parse error diagnostics。

## 参考

- Gate Futures WebSocket v4 orders channel: https://www.gate.com/docs/developers/futures/ws/zh_CN/
- Gate Futures REST API order fields: https://www.gate.com/docs/futures/api/index.html
- Sirius reference: `third_party/sirius/exchange/gate/trade/trade_engine.cpp`
- Sirius parser: `third_party/sirius/exchange/gate/parser/trade_parser.cpp`
- Gate SBE schema: `exchange/gate/sbe/schema/gate_fex_ws_latest.xml`
