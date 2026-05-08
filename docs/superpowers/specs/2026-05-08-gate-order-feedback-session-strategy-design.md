# Gate OrderFeedbackSession 与 Strategy 状态机设计

## 背景

Task1 固定 `OrderFeedbackSession -> Strategy` 的 SHM transport。Task2 在这个 transport 之上实现 Gate
private `futures.orders` lifecycle feedback：Gate feedback session 只解析和转换 event，Strategy
消费 event 后更新本地订单状态。

本设计继承 Sirius 实盘长期运行经验中有价值的部分：以交易所私有订单回报作为生命周期事实源，按本地
order id 归属策略，Strategy 自己维护订单对象和状态。Aquila 不复制 Sirius `TradeEngine` 中混合下单、
状态推进、回报解析和策略对象访问的耦合结构。

## 目标

- Gate `OrderFeedbackSession` 登录并订阅 private `futures.orders`。
- 解析 Gate SBE orders message。
- 从 `text="t-<local_order_id>"` 解出 `local_order_id`。
- 使用 `local_order_id` 高 8 bit 路由到 Task1 SHM lane。
- Strategy reader drain event 并调用 `Strategy::OnOrderFeedback()`。
- Strategy 根据 event 更新 `StrategyOrder` 状态、成交累计和终态字段。
- accepted event 建立 `local_order_id -> exchange_order_id` 映射所需信息；Strategy 再通知自己线程内的 `OrderSession` 更新 cancel cache。
- terminal event 触发 Strategy 清理或通知 `OrderSession` forget exchange id cache。

## 非目标

- 不订阅 `futures.usertrades`。
- 不订阅 `futures.positions`。
- 不实现 REST reconcile，但需要保留 gap detected 状态。
- 不实现跨 strategy 风控。
- 不把 `OrderFeedbackSession` 设计成访问 Strategy `OrderPool` 的对象。
- 不在 Strategy 内缓存 Gate wire fields。

## 进程与线程模型

支持单进程开发工具和多进程生产部署，默认设计以多进程为边界：

```text
gate-feedback-process
  Gate OrderFeedbackSession
  Gate private futures.orders WebSocket
  parse + convert + publish to SHM lane

strategy-1-process
  StrategyThread
  Strategy(strategy_id=1)
  Gate OrderSession
  OrderFeedbackShmReader(strategy_id=1)

strategy-2-process
  StrategyThread
  Strategy(strategy_id=2)
  Gate OrderSession
  OrderFeedbackShmReader(strategy_id=2)
```

取舍：

- 多个 Strategy 可以各自拥有上行 `OrderSession`，共享一个账号级下行 `OrderFeedbackSession`。
- feedback session 独立进程可以隔离 private WS reconnect、SBE parse 异常和 burst。
- route key 是 `local_order_id` 高 8 bit，不是 `exchange_order_id`。
- 单进程 tool 可以把这些组件放在一起跑，但代码边界仍按多进程设计。

## Gate OrderFeedbackSession 边界

`OrderFeedbackSession` 负责：

- WebSocket login；
- subscribe `futures.orders`；
- JSON control response parse；
- binary SBE orders dispatch；
- 从 orders 字段生成 `OrderFeedbackEvent`；
- 调用 `OrderFeedbackShmPublisher::Publish(event)`；
- 在 disconnect / reconnect / parse 不可恢复场景标记 global gap。

它不负责：

- 查 Strategy order；
- 判断风险；
- 根据订单对象补 side；
- 维护持仓；
- 直接平仓或撤单；
- reconcile。

## Orders Event Mapping

字段与语义沿用 `docs/superpowers/specs/2026-05-08-gate-order-feedback-event-design.md`：

| Gate `finish_as` | Event | Strategy 状态 |
| --- | --- | --- |
| `_new` | `kAccepted` | `kAccepted` |
| `_update` 且 `left_quantity > 0` | `kPartialFilled` | `kPartialFilled` |
| `filled` 且 `left_quantity == 0` | `kFilled` | `kFilled` |
| `cancelled` | `kCancelled` with `kManualCancelled` | `kCancelled` 或 `kPartiallyCancelled` |
| `ioc` | `kCancelled` with `kImmediateOrCancel` | `kCancelled` 或 `kPartiallyCancelled` |
| `reduce_only` | `kCancelled` with `kReduceOnly` | `kCancelled` 或 `kPartiallyCancelled` |
| `reduce_out` | `kCancelled` with `kReduceOut` | `kCancelled` 或 `kPartiallyCancelled` |
| `stp` | `kCancelled` with `kSelfTradePrevention` | `kCancelled` 或 `kPartiallyCancelled` |
| `liquidated` | `kCancelled` with `kLiquidated` | `kCancelled` 或 `kPartiallyCancelled` |
| `auto_deleveraging` | `kCancelled` with `kAutoDeleveraging` | `kCancelled` 或 `kPartiallyCancelled` |
| `position_close` | `kCancelled` with `kPositionClose` | `kCancelled` 或 `kPartiallyCancelled` |

`OrderRejectedFeedback` 不来自 `futures.orders` 主生命周期流。它只用于 `OrderSession` 本地发送失败或
API submit error。

## Parser Diagnostics

第一版 parser 遇到下列情况应计数并丢弃该条 event，必要时标记 gap：

- 不能解析 `text`；
- `text` 不是 `t-<local_order_id>`；
- `local_order_id == 0`；
- `strategy_id >= 8`；
- `sizeExponent != 0`；
- `finish_as="filled"` 但 `left_quantity != 0`；
- 未支持的 `finish_as`；
- SBE template / schema mismatch；
- price mantissa / exponent 转换失败。

对于单条 malformed order update，第一版可以只计数并丢弃；对于 session 级 decode 不可恢复、断线、重连后未知缺口，必须调用 `MarkGlobalGap()`。

## Strategy 状态机

第一版状态：

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

状态推进：

- `kSent`：Strategy 成功把 place 请求交给 `OrderSession` 写路径。
- `kAccepted`：收到 `_new` event。
- `kPartialFilled`：收到非终态累计成交更新。
- `kFilled`：收到 filled terminal event。
- `kCancelSent`：Strategy 成功把 cancel 请求交给 `OrderSession` 写路径。
- `kCancelled`：terminal event 且累计成交量为 0。
- `kPartiallyCancelled`：terminal event 且累计成交量大于 0。
- `kRejected`：本地发送失败或 API submit error。

Strategy `OnOrderFeedback()` 基本规则：

- 找不到订单：忽略并计数；
- 订单已结束：忽略并计数，保证 terminal 幂等；
- cumulative quantity 小于本地累计：忽略并计数；
- cumulative quantity 等于本地累计：非终态重复回报忽略，终态仍可推进终态；
- cumulative quantity 大于本地累计：计算 delta，更新累计成交量和成交金额；
- terminal event 后设置 `is_finished = true`；
- accepted event 保存 `exchange_order_id`；
- terminal event 后通知 `OrderSession` forget 本地 cancel cache。

## OrderSession Cache 更新

`OrderFeedbackSession` 不访问 `OrderSession`。accepted event 到达 Strategy 后，Strategy 在自己的线程中调用
`OrderSession` 的 cache update 接口：

```text
accepted event -> Strategy order.exchange_order_id = event.exchange_order_id
               -> OrderSession cache local_order_id -> exchange_order_id
```

terminal event 后：

```text
filled / cancelled event -> Strategy order terminal
                         -> OrderSession forget local_order_id cache
```

如果 Strategy 与 `OrderSession` 后续拆到不同线程，cache update / forget 需要走该 `OrderSession` 自己的 command queue。
当前默认同线程，因此不引入额外 queue。

cancel 编码仍保留 `text=t-<local_order_id>` fallback。没有办法在 feedback 进程中直接维护
`exchange_order_id -> local_order_id`，因为 feedback route 必须先依赖 `text` 找到本地 id。

## Gap Handling

Strategy reader 通过 Task1 SHM reader `Poll()` 消费 `OrderFeedbackEvent`。当 handler 收到
`OrderFeedbackKind::kGap`：

- Strategy 设置 `feedback_gap_detected = true`；
- 暂停新开仓；
- 后续 REST reconcile 任务负责恢复本地订单状态；
- 现有订单不凭本地猜测强行推进终态。

Task2 只把 gap event 状态传进 Strategy 并暴露可测字段，不实现 REST reconcile。Gate `futures.orders` parser
不从主生命周期流产生 `kGap`；`kGap` 只来自 Task1 SHM transport control path。

## 验证边界

Task2 完成后必须能证明：

- fake Gate SBE orders payload 能被解析成正确 `OrderFeedbackEvent`；
- fake session 能把 event publish 到正确 SHM lane；
- Strategy 可以从 reader drain event 并推进订单状态；
- accepted 后 exchange order id 被保存并传给 `OrderSession` cache；
- filled / cancelled 后终态幂等并清理 cache；
- `OrderFeedbackKind::kGap` event 会让 Strategy 进入 gap detected 状态；
- live 下单闭环仍需要极小数量 smoke，并且 smoke 结果只能证明协议连通性，不能证明性能。
