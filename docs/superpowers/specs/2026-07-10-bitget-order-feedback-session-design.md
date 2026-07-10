# Bitget UTA OrderFeedbackSession 设计

## 状态

- 日期：2026-07-10
- 范围：Bitget UTA v3 单路 `OrderFeedbackSession`
- endpoint：`wss://vip-ws-uta.bitget.com/v3/ws/private`
- topic：仅订阅 `order`
- 账户能力：仅接受 `usdt-futures`、`one_way_mode`、`crossed`
- 当前阶段不发送真实订单

## 目的

本文定义 Bitget UTA v3 `OrderFeedbackSession` 的协议边界、字段映射、订单归属、valid 检测、continuity 语义、
SHM 发布和验证要求。整体架构类比 Gate `OrderFeedbackSession`，但保留 Bitget JSON order push、无初始快照、无
sequence 和无 maker/taker role 等协议差异。

`OrderFeedbackSession` 是下行订单生命周期事实通道。它不发送 place/cancel，不解释策略意图，也不在 WebSocket 线程内执行
REST bootstrap/reconcile。

## 事实源

协议事实以 Bitget 官方文档为准：

- [UTA Order Channel](https://www.bitget.com/api-doc/uta/websocket/private/Order-Channel)
- [UTA Fill Channel](https://www.bitget.com/api-doc/uta/websocket/private/Fill-Channel)
- [UTA Fast Fill Channel](https://www.bitget.com/api-doc/uta/websocket/private/Fast-Fill-Channel)
- [UTA Best Practices](https://www.bitget.com/api-doc/uta/best-practices)
- [UTA Enumeration](https://www.bitget.com/api-doc/uta/enum)
- [UTA Change Log](https://www.bitget.com/api-doc/uta/changelog)
- [Bitget WebSocket 连接与登录](https://www.bitget.com/api-doc/classic/quickStart/websocket-intro)

仓库内对照入口：

- `exchange/gate/trading/order_feedback_parser.h`
- `exchange/gate/trading/order_feedback_session.h`
- `core/trading/order_feedback_event.h`
- `core/trading/order_feedback_shm.h`
- `core/trading/order_manager.h`
- `exchange/bitget/trading/order_codecs.h`
- `exchange/bitget/trading/order_signature.h`

## 已锁定边界

### 当前范围

- 独立 authenticated private WebSocket connection。
- login 后订阅 account-wide `order` topic。
- 解析 `new`、`partially_filled`、`filled`、`cancelled` / `canceled`。
- 使用 `clientOid=a-<local_order_id>` 识别 Aquila 订单并路由到现有 feedback SHM lane。
- 映射累计成交量、剩余量、撤销量、平均成交价和终态原因。
- 断线或不可恢复解码错误时发布全局 `OrderFeedbackKind::kContinuityLost`。
- TOML config、dry-run、login/subscribe-only probe、单元测试、SHM integration test 和 release benchmark。

### 非目标

- `fill`、`fast-fill`、`execId` 去重或逐笔成交事件。
- REST bootstrap、open-order snapshot、reconcile 或恢复状态机。
- account / position private topic。
- 修改 `OrderFeedbackEvent` 或 feedback SHM ABI。
- `hedge_mode`、isolated margin、spot、margin、coin futures 或 USDC futures。
- 多 feedback route、high-speed endpoint 或自动 failover。
- LeadLag 接线、真实订单闭环或 fill latency 结论。

## 方案选择

### V1 只使用 order topic

`order` topic 提供累计成交量、平均成交价和完整订单终态，可直接映射现有 `OrderFeedbackEvent`。`fast-fill` 是逐笔增量成交，
与 `order` 并用需要基于 `execId` 解决跨流重复、乱序和累计量重建；当前 core feedback contract 不携带 `execId`。

因此 V1 只订阅 `order`。`fast-fill` 延后到独立低延迟成交阶段，先完成 contract 设计和延迟 A/B，再决定是否接入。

### REST 恢复留在 session 外

Bitget 首次订阅 `order` 不补发当前订单，消息也没有 sequence。`OrderFeedbackSession` 无法仅凭订阅成功证明订单状态连续。
REST bootstrap/reconcile 由后续外部恢复控制器负责，不进入 WebSocket 线程。

`Ready()` 只表示已登录并成功订阅，不表示已经完成 REST baseline、reconcile 或账户状态恢复。

## 组件结构

```text
Bitget OrderSession connection
  -> place-order / cancel-order

Bitget OrderFeedbackSession connection
  -> login
  -> subscribe {"instType":"UTA","topic":"order"}
  -> JSON order push parser
  -> ownership / scope / semantic validation
  -> OrderFeedbackEvent
  -> OrderFeedbackShmPublisher
  -> strategy lane
  -> TradingRuntime::OnOrderFeedback()
  -> OrderManager
  -> Strategy
```

建议入口：

```text
exchange/bitget/trading/order_feedback_parser.h
exchange/bitget/trading/order_feedback_session.h
exchange/bitget/trading/order_feedback_session_config.h
exchange/bitget/trading/order_feedback_session_config.cpp
tools/bitget/bitget_order_feedback_session.cpp
config/order_feedback/bitget_order_feedback_session.toml
test/exchange/bitget/trading/order_feedback_parser_test.cpp
test/exchange/bitget/trading/order_feedback_session_test.cpp
benchmark/exchange/bitget/trading/order_feedback_parser_benchmark.cpp
```

## 连接和订阅状态

状态流：

```text
Disconnected
  -> Connecting
  -> Active
  -> LoginSent
  -> LoggedIn
  -> SubscribeSent
  -> Ready
```

规则：

- WebSocket active 后立即发送 login。
- 只有 `event=login` 且 `code=0` 时进入 `LoggedIn`。
- login 成功后发送单个 `order` subscription。
- 只有 subscription ACK 的 `event=subscribe`、`instType=UTA`、`topic=order` 匹配时进入 `Ready`。
- stale login/subscription ACK 不推进状态。
- login、subscribe error、heartbeat timeout、`30033` 或 connection phase 离开 active 时立即 not-ready。
- reconnect 后重新 login 和 subscribe，不复用旧认证或订阅状态。
- feedback 与 `OrderSession` 使用相同 high availability endpoint，但必须是独立 connection。
- login/subscription control request 的瞬时 write failure 请求重连；subscription ACK 或 error 都消费当前
  `SubscribeSent`，重复或矛盾的后续 ACK 不推进状态。
- 已登录连接的 subscribe 收到 `30004`、`30005`、`30007`、`30011`–`30015`、`30033` 时清除认证状态并请求重连。
  初始 login 的明确 credential reject 只保持 not-ready，不用相同 credential 盲目重连；`30004`、`30007`、`30033` 仍按
  connection error 重连。

首次成功连接不主动发布 continuity lost。首次 live 使用前，外部 preflight 必须证明没有遗留 open orders，或完成 REST baseline。

从 active 进入 disconnected、reconnect backoff、closing 或 closed 时，发布：

```text
kind = kContinuityLost
scope = kGlobal
reason = kSessionDisconnected
```

session 不合成 accepted、filled 或 cancelled，也不自行清除外部 recovery 状态。

## Subscription

固定 request：

```json
{
  "op": "subscribe",
  "args": [{
    "instType": "UTA",
    "topic": "order"
  }]
}
```

V1 不按 symbol 拆分订阅。account-wide subscription 允许使用 `clientOid` 在本地按 strategy id 路由，并避免 494 个 symbol
subscription 带来的控制面成本。

## Data envelope

V1 接受的 order push envelope：

```json
{
  "action": "snapshot",
  "arg": {
    "instType": "UTA",
    "topic": "order"
  },
  "data": [],
  "ts": 1742367838124
}
```

虽然字段名是 `snapshot`，官方明确说明首次订阅不推送当前订单；这里的 `action` 不能解释为可用于恢复的账户快照。

envelope 必须满足：

- root 是 object。
- `arg.instType == UTA`。
- `arg.topic == order`。
- `action == snapshot`。
- `data` 是 array。

无法读取上述 order data envelope 时，无法证明其中不包含 Aquila 订单，发布全局 `kDecodeUnrecoverable`。

session 的 text hot path 先由 order parser 检查 `action`；存在 top-level `event` 且没有 `action` 时返回 control-message
分类，再进入 login/subscription 冷路径 parser。order data 不允许先做一次完整 control parse，避免主路径重复 JSON stage-1 / envelope
扫描；control message 不计入 order parser 的 `messages_seen`。

## Valid 检测分层

检测拆成三层，不把全部条件堆在单个 parser 分支中。

### 第一层：订单归属

只读取 `clientOid`：

| 输入 | 处理 |
| --- | --- |
| `a-<valid uint64>` | 进入 Aquila 订单解析 |
| 以 `a-` 开头但 ID 非法 | 全局 `kDecodeUnrecoverable` |
| 缺失、null、非字符串、空值或其他 prefix | 记录 foreign/unroutable diagnostics 并忽略 |

复用 `bitget::ClientOidCodec::Parse()`，不新增第二套 ID parser。

### 第二层：必需字段解析

只有 Aquila 订单才解析以下必需字段：

```text
category
orderId
qty
holdMode
marginMode
cumExecQty
avgPrice
orderStatus
updatedTime
```

`cancelReason` 是 optional；缺失或未知值映射 `OrderFinishReason::kUnknown`。`symbol`、`side`、`timeInForce`、`reduceOnly`、
`createdTime`、`feeDetail`、`totalProfit` 和 `ts` 可用于 diagnostics，但不是构造 core feedback event 的必需字段。

必需字段缺失、类型错误、数值溢出或 timestamp 无法从 millisecond 转为 nanosecond 时，发布全局
`kDecodeUnrecoverable`。

### 第三层：scope 和语义校验

Aquila 订单必须满足：

```text
category == usdt-futures
holdMode == one_way_mode
marginMode == crossed
orderId > 0
qty > 0
0 <= cumExecQty <= qty
cumExecQty > 0 时 avgPrice > 0
updatedTime > 0
```

数值必须 finite。数量比较使用明确 epsilon，避免 double 表示误差导致合法 `filled` 被拒绝。

scope 不匹配表示账户模式、下单配置或交易所行为偏离当前 adapter contract。该订单不发布普通 feedback，而是发布全局
`kDecodeUnrecoverable`，不能静默忽略。

## Status 映射

2026-04-09 起 Bitget UTA private `order` topic 停止推送旧 `live` 状态；V1 使用 `new` 作为 matching system accepted
事实。

| Bitget `orderStatus` | core kind | 数量规则 |
| --- | --- | --- |
| `new` | `kAccepted` | `cumExecQty == 0` |
| `partially_filled` | `kPartialFilled` | `0 < cumExecQty < qty` |
| `filled` | `kFilled` | `cumExecQty` 与 `qty` 在 epsilon 内相等 |
| `cancelled` | `kCancelled` | `0 <= cumExecQty <= qty` |
| `canceled` | `kCancelled` | 兼容官方文档冲突并单独计数 |

属于 Aquila 的未知 `orderStatus` 不能猜测映射，发布全局 `kDecodeUnrecoverable`。

Bitget 明确业务 reject 在 place operation response 中返回，官方说明不会再通过 `order` topic 推送。因此 V1 parser 不从
`order` topic 合成 `OrderFeedbackKind::kRejected`。

## Quantity 和 price 映射

```text
cumulative_filled_quantity = cumExecQty
left_quantity = max(0, qty - cumExecQty)
cancelled_quantity = orderStatus 为 cancelled/canceled 时的 left_quantity，否则为 0
fill_price = avgPrice
```

现有 `OrderManager::ApplyCumulativeFill()` 使用累计成交量拒绝重复或倒退 feedback，并用累计量乘平均价更新累计成交额，因此
`avgPrice` 与当前 core contract 匹配。

`order` topic 不提供 `tradeScope`，所以：

```text
role = OrderRole::kNone
```

maker/taker 只能由未来 `fill` / `fast-fill` 逐笔成交 contract 提供，V1 不猜测。

## 无法直接映射的字段

| Bitget 字段或缺失能力 | V1 处理 | 原因 |
| --- | --- | --- |
| `tradeScope` | `OrderRole::kNone` | `order` topic 不提供该字段 |
| `execId` / `execLinkId` | 不映射 | 只存在于逐笔 `fill`，现有 core event 无对应字段 |
| `feeDetail` | 不进入 core event | 当前 `OrderFeedbackEvent` 不承载 fee，且 order topic 是累计订单事实 |
| `totalProfit` | 不进入 core event | 策略订单状态不应被解释为账户 PnL 事实源 |
| `createdTime` | 仅 diagnostics | 它不是 accepted 或本次状态更新时间 |
| envelope `ts` | 仅 diagnostics | 它是 gateway push time，不是订单生命周期更新时间 |
| order sequence | 无法映射 | Bitget `order` topic 不提供 sequence，不能据此检测 gap |
| 初始 open-order snapshot | 无法映射 | 首次订阅不补发当前订单，必须由外部 REST baseline 解决 |
| feedback reject | 不产生 | 明确入场 reject 只在 operation response 中出现 |

V1 不为上述缺失字段扩展 core ABI。只有未来逐笔成交、fee/PnL 或恢复 contract 明确后，才单独评估 ABI 变更。

## Cancel reason 映射

采用保守 allowlist：

| Bitget `cancelReason` | `OrderFinishReason` |
| --- | --- |
| `normal_cancel` | `kManualCancelled` |
| `ioc_not_full_cancel` | `kImmediateOrCancel` |
| `self_trade_cancel` / `stp_cancel` | `kSelfTradePrevention` |
| `adl_cancel` | `kAutoDeleveraging` |
| `burst_cancel` / `penetrate_cancel` | `kLiquidated` |
| 其他或空值 | `kUnknown` |

`delegated_risk_order_cancel`、`slippage_cancel`、`trade_count_too_small`、`limit_price_exceed_cancel` 等没有准确 core
枚举，不做宽泛归类。原始 `cancelReason` 的 hash 或 bounded text 进入 exchange-specific diagnostics。

未知 `cancelReason` 不影响 `kCancelled` 终态，也不触发 continuity lost。

## Timestamp 映射

| Bitget 字段 | 本地字段 | 说明 |
| --- | --- | --- |
| 本机 message ingress | `local_receive_ns` | Unix epoch ns |
| record `updatedTime` | `exchange_update_ns` | millisecond 乘 `1'000'000` |
| envelope `ts` | diagnostics | WebSocket gateway push time |
| record `createdTime` | diagnostics | OMS risk check 后创建时间 |

`updatedTime` 是当前 order lifecycle event 的交易所更新时间。不得使用 envelope `ts` 替代订单更新时间，也不得用跨机器 timestamp
直接推导单程网络延迟。

## Decode continuity

`kDecodeUnrecoverable` 表示 session 收到可能包含 Aquila 订单事实的 order data，但无法安全转换为 `OrderFeedbackEvent`，而
Bitget 当前 WebSocket 不提供 sequence 或 replay，外部必须进入 reconcile 边界。

触发条件：

- 整个 order data envelope 损坏。
- `clientOid` 以 `a-` 开头但无法解析本地 ID。
- Aquila 订单缺失必需字段或关键数值非法。
- Aquila 订单 scope 与当前 adapter contract 不匹配。
- Aquila 订单出现未知或数量不一致的 `orderStatus`。

不触发条件：

- 明确非 Aquila 的订单。
- 非必需 diagnostics 字段缺失。
- 未知 `cancelReason`。
- login/subscribe control error；该情况由连接状态机处理。

同一 connection generation 首次出现 decode continuity loss 时发布一次全局 event，后续同类错误只增加 diagnostics，避免重复灌入
SHM。reconnect 后新 generation 可以再次发布。V1 不实现同一 connection 内的 reconcile 后 latch reset；该 contract 与外部
恢复控制器一起设计。

## SHM 发布

复用现有 `OrderFeedbackShmPublisher`：

- 普通 event 由 `LocalOrderIdCodec::StrategyId(local_order_id)` 路由到固定 lane。
- lane queue full 由 publisher 记录并安排 `kLaneQueueFull` continuity event。
- session disconnect 和 decode unrecoverable 使用 `PublishGlobalContinuityLost()` 广播到全部 lane。
- 普通 event publish failure 不阻塞 WebSocket thread，不重试原订单事件。
- queue-full 产生的 pending continuity 由 session 在非阻塞的 bounded interval 上重试 flush；不能依赖同一 lane
  的下一条普通订单事件来触发告警投递。

V1 不修改 `OrderFeedbackEvent`、`kOrderFeedbackShmVersion` 或 SHM layout。

## Config

feedback TOML 只配置一条 high availability endpoint：

```toml
[order_feedback_session]
name = "bitget_order_feedback_high_availability"
category = "usdt-futures"
position_mode = "one_way_mode"
margin_mode = "crossed"

[order_feedback_session.credentials]
api_key_env = "BITGET_TEST_KEY"
api_secret_env = "BITGET_TEST_SECRET"
api_passphrase_env = "BITGET_TEST_PASSPHRASE"

[order_feedback_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"
port = 443
target = "/v3/ws/private"
enable_tls = true
```

SHM config 复用现有 feedback SHM contract。config parser 必须 fail-fast：

- credential env name 缺失。
- category、position mode 或 margin mode 超出 V1 scope。
- endpoint 不是 private v3 path。
- TLS/port 配置自相矛盾。
- SHM lane count 或 ABI 与现有 contract 不匹配。

## Diagnostics

至少记录：

```text
bitget_order_feedback_session_phase
bitget_order_feedback_login
bitget_order_feedback_subscribe
bitget_order_feedback_raw_update
bitget_order_feedback_validation_error
bitget_order_feedback_continuity_lost
bitget_order_feedback_session_summary
```

关键计数：

```text
text_messages
login_sent
login_accepted
login_rejected
subscribe_sent
subscribe_acks
subscribe_errors
order_envelopes
orders_seen
events_published
foreign_orders_ignored
unroutable_orders_ignored
legacy_canceled_statuses
validation_errors
decode_continuity_lost_events
publish_failures
disconnect_continuity_lost_events
```

不得记录 API key、secret、passphrase、signature 或完整 login payload。新增 log key、stats 字段或 report 字段时同步更新
`docs/diagnostic_fields.md`。

## 测试要求

### Parser

- envelope、topic、instType、action 和 data array。
- 单条和 batch order records。
- foreign `clientOid`、合法 Aquila ID、malformed `a-` ID 和最大 `uint64_t`。
- `new`、`partially_filled`、`filled`、`cancelled`、`canceled`。
- quantity、cumulative quantity、average price 和 timestamp 边界。
- scope mismatch、必需字段缺失、未知 status 和 malformed JSON。
- `cancelReason` allowlist 与 unknown fallback。
- `OrderRole::kNone` 和 `OrderFeedbackKind::kRejected` 不从 order topic 产生。

### Session

- active 后 login，login success 后 subscribe，ACK 后 ready。
- stale login/subscribe ACK 不推进状态。
- login/subscribe failure、heartbeat timeout、`30033`、disconnect 和 reconnect。
- disconnect 发布全局 `kSessionDisconnected`。
- Aquila data decode failure 发布一次 `kDecodeUnrecoverable`，同 generation 重复错误只计数。
- foreign order 和非必需字段缺失不发布 continuity lost。
- publish failure 不阻塞 message handling。

### SHM integration

- `clientOid` 中 strategy id 路由到正确 lane。
- 多 strategy lane 广播全局 continuity event。
- lane queue full 的 pending continuity 行为。
- producer restart 和旧 SHM ABI 检查继续使用现有 contract。

## Benchmark

实现阶段必须新增 release microbenchmark，至少覆盖：

```text
single accepted order parse + validate + map
single partial-filled order parse + validate + map
single terminal order parse + validate + map
foreign order ownership fast path
malformed Aquila order continuity path
typical batch parse + validate + publish
parser -> OrderFeedbackShmPublisher -> lane drain
session classification -> single accepted order parse
```

benchmark 记录命令、build type、样本数和 percentile。未固定 CPU、未控制机器负载或没有重复证据时只称为本机基线，不宣称
低延迟收益。

## Live 验证边界

V1 live probe 只允许：

```text
connect
-> login success
-> subscribe order success
-> Ready()
-> ping/pong
-> controlled reconnect
-> re-login/re-subscribe
-> kContinuityLost
-> clean stop
```

当前阶段不调用 place/cancel，也不等待真实 `new`、fill 或 cancel push。login/subscribe success 不能证明订单字段映射、trade
permission、账户模式、订单状态连续性或 feedback latency。
probe 只有在由配置的 duration timer 控制停止且曾完成 login/subscription 时才能返回成功；summary 必须明确输出
`completed_requested_duration` 和 `ever_ready`，连接提前退出不能沿用历史 ACK 误报成功。

真实订单闭环延后到以下条件全部满足后：

- 账户已切换并确认 `one_way_mode`。
- account-level rate limiter 已落地。
- REST baseline/reconcile 和 reconnect recovery contract 已完成。
- emergency cleanup / flat check 已准备。
- OrderSession 与 OrderFeedbackSession 均通过独立验证。

## 后续阶段入口

1. 外部 REST bootstrap/reconcile 和启动/重连时间窗口对账。
2. `fast-fill` 的 `execId` 去重、乱序、累计量重建和独立 `FillEvent` contract。
3. account / position private feed 与订单恢复的关系。
4. Bitget order gateway、account-level rate limit 和 LeadLag wiring。
5. guarded real-order feedback smoke 与 private endpoint latency A/B。
