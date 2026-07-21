# Gate Trading

本文是 Gate futures 交易链路的当前事实源，覆盖协议事实、单路 `OrderSession`、账号级
`OrderFeedbackSession`、多路 OrderGateway SHM 和恢复边界。跨交易所公共职责见
`docs/strategy_order_component_model.md`；测量工具见 `docs/gate_order_session_rtt_probe_design.md`。

## 当前实现状态与证据边界

已落地能力：

- `OrderSession`：`futures.login`、limit place/cancel、固定缓冲 encoder、Ack/final/error parse、request correlation 和 cancel exchange id cache。
- Decimal-size contract：`OrderPlaceRequest` 携带 `double price/quantity` 和 instrument
  decimal places；`OrderSession` 在发送前生成 fixed decimal text。带
  `X-Gate-Size-Decimal: 1` 时把 JSON `size` 编码为 string。
- `OrderFeedbackSession`：账号级 private `futures.orders`、feedback SHM lane、累计生命周期事实和 `kContinuityLost`。
- `TradingRuntime`：owner thread 驱动 `OrderSession`，drain response/feedback，并在 ready 后 poll `DataReader`。
- 多路交易：单进程 `MultiOrderSessionGateway` baseline，以及独立 `order-gateway-process` + SHM v4 + N 个 worker。
- LeadLag：支持 `order_gateway` backend、运行时 fanout 和稳定的 execution `group_id`；`group_index` 只在策略进程内定位 fixed slot。

单路链路已有真实 open/close 与 unfilled-cancel smoke。多路 order-gateway-process 已通过 unit、SHM integration、
validate-only 和 benchmark，但尚未发送真实订单，不宣称 fillability 或 latency 收益。

## 协议事实

- `futures.order_place`/`futures.order_cancel` 可能先返回 `ack=true`，再返回 `ack=false` final response。
- `ack=true` 只表示 Gate 收到请求，不是 accepted、订单簿可见、filled 或 terminal 事实。
- Final response 可提供 `exchange_order_id`；accepted/partial/terminal 生命周期以 private `futures.orders` 为准。
- Gate `5xx`、timeout 或 ambiguous result 映射为 `OrderResponseKind::kUnknownResult`，不能当作确定 reject。
- `OrderManager` 对 unknown 保留订单；LeadLag 标记 symbol `needs_reconcile` 并暂停新开仓。
- 只有精确 terminal feedback 解决该 symbol 全部 pending unknown，且没有 global continuity lost/manual intervention，才能自动恢复。
- SBE endpoint 可以混合 JSON text control frame 与 SBE binary payload。
- Decimal size 未按 string 编码时会被 Gate `int64` schema 拒绝。

## 进程、线程与所有权

单路形态：

```text
strategy process / owner thread
  TradingRuntime -> OrderManager -> Gate OrderSession
  OrderFeedbackShmReader -> OrderManager -> Strategy

feedback process
  Gate OrderFeedbackSession -> account-wide feedback SHM
```

多路生产形态：

```text
strategy process
  StrategyOrderOwnerThread
    Strategy / OrderManager / OrderGatewayClient
    RouteTable(local_order_id -> route_id)
    OrderFeedbackShmReader

order-gateway process
  OrderSessionWorker[0] -> OrderSession[0]
  OrderSessionWorker[1] -> OrderSession[1]
  ...
  OrderSessionWorker[N-1] -> OrderSession[N-1]

feedback process
  OrderFeedbackSession -> feedback SHM lanes
```

Strategy owner 是订单、route table 和 execution group 的唯一 owner。每个 worker 独占一条 command/event queue、
一个 `OrderSession` 和一个 WebSocket connection。Feedback 保持账号级单事实源，不按 route 分裂。

## OrderSession 与 OrderFeedbackSession contract

### ID、quantity 与 cache

- `local_order_id`：高 8 bit `strategy_id` + 低 56 bit strategy order id。
- Gate `text=t-<local_order_id>`，feedback router 用高 8 bit 选择固定 SHM lane。
- `request_sequence -> local_order_id` 只做 response correlation，不是 pending-order table。
- Accepted feedback 后 `OrderManager` 保存 `exchange_order_id` 并更新原 route cancel cache；terminal 后清理 cache。
- 业务计算和 Strategy/Gateway SHM request 使用 `double price/quantity`；wire text 只在
  `OrderSession` 按 request 中的 decimal places 生成；feedback/SHM 使用累计 `double`
  quantity。

### Response、feedback 与 continuity

- `ack=true` 不产生 accepted event。
- `finish_as="_new"` 才映射 accepted；filled/cancelled/rejected terminal feedback 推进订单终态。
- 断线时 `OrderSession` 清空 generation-local correlation，不伪造 rejected/cancelled。
- Queue full、producer restart 或 feedback WS 断线发布 `kContinuityLost`；strategy 暂停新开仓并进入 handoff/reconcile。
- `OrderSession::Ready()==false` 只限制新上行请求；runtime 仍需 drain 已到达的 response 和 feedback。

## OrderGateway SHM contract

一个 POSIX SHM 对象包含最多 16 路 command/event SPSC queue：

```text
OrderGatewayShmHeader
  route_count <= 16
  command_queue_capacity
  event_queue_capacity
  startup_ready_timeout_s
  route_states[16]
  queue descriptors / offsets

command_queue[i]: strategy -> worker[i]
event_queue[i]: worker[i] -> strategy
```

Payload 是固定大小 POD，不包含 pointer、`std::string`、虚函数或动态分配。主要入口：

```text
core/trading/order_gateway_shm_types.h
core/trading/order_gateway_shm.h
core/trading/order_gateway_client.h
exchange/gate/trading/order_gateway_worker.h
tools/gate/gate_order_gateway.cpp
```

### Command

`OrderGatewayCommand` 携带 `command_seq`、`owner_enqueue_ns`、command kind 和固定大小 union
payload。place payload 是完整 `OrderPlaceRequest`，cancel payload 是最小
`OrderCancelRequest`，cache/forget payload 只携带 local/exchange order id 与 route。

Command kind 为 place、cancel、cache exchange id、forget exchange id 和 stop。每个 child 有唯一 `local_order_id`；同一
execution group/position lifecycle 的 open、close、stoploss、retry 和 fanout child 共享 `group_id`。SHM v4 在
place/cancel 和 response event 中传播该字段，供 strategy execution group 做稳定归因；`parent_id` 继续保留为通用 gateway
correlation 字段，不等价于 LeadLag 的稳定 group identity。

### Event

`OrderGatewayEvent` 携带 command/local/exchange/group id、request correlation、route、response/reject kind，以及 owner enqueue、
worker dequeue、send、receive、exchange ingress/egress/process 和 worker enqueue timestamps。

Event kind 为 order response、command rejected、ready、not-ready 和 stopped。`kCommandRejected` 表示请求确定未进入交易所
不确定状态；place/cancel 分别映射本地 rejected/cancel-rejected。发送后无法确认结果时必须发布 `kUnknownResult`。

## Route state、queue full 与 liveness

`route_states[16]` 的 `kUnknown/kNotReady/kReady/kStopped` 是 event 丢失或 queue fatal 时的兜底，不替代 event queue。
Strategy 以 flag transition 更新 `ready_count`，重复 ready/not-ready 不得重复计数。启动时等待所有配置 route ready；
超过 `startup_ready_timeout_s` fail fast。运行时跳过 not-ready route，ready 恢复后重新参与 fanout。

Command queue full：立即本地 reject，不阻塞、不写 route table。Event queue full：worker 将 route 置 stopped，停止该 session；
client 对 route table 中仍未解决订单合成 unknown result，触发 reconcile，不能静默丢 event。

SHM v4 没有 heartbeat/owner-death protocol。Gateway `SIGKILL` 或 crash 后 header 可能残留 stale ready；生产运行依赖外部 supervisor，
后续需增加 route heartbeat 或等价 liveness。

## Strategy fanout 与故障语义

`order_session_fanout` 表示一次 signal 最多向多少条 ready route 发送 full-size duplicate child。V1 固定 route 顺序，
跳过 not-ready route，不做 RTT 动态选路。每个 child 的 `group_id` 相同、`local_order_id/route_id` 不同；generic
`parent_id` 也可用于 gateway correlation，但不作为 LeadLag report identity；
整个 execution group 只占一个 parallel slot。

Gateway 不解释 duplicate/split、winner、overfill 或非赢家 cancel。Open 的已知累计成交合并成 position；close/stoploss/retry
按 position 总量生成 reduce-only child。仍 pending/unknown 的 open child 留在原 execution group 等 feedback/reconcile。

`OrderGatewayClient::PlaceOrder()` 返回 `kOk` 只表示 command enqueue 成功，不表示 socket write 或 Gate Ack。Cancel/cache/forget
必须按显式 route table 回原 session；不能用 `local_order_id % N` 推导生产 route。

多连接不是账户预算倍增。任何 fanout 都必须受账号级 submit/cancel/pending budget 约束；扩大前先设计并验证 limiter。

## 配置、代码与验证入口

主要配置：

```text
config/order_sessions/gate_order_session.toml
config/order_feedback/gate_order_feedback_session.toml
config/order_feedback/gate_order_feedback_shm.toml
config/order_gateways/gate_order_gateway_30symbols_private_plain_20260627.toml
config/strategies/lead_lag_30symbols_fusion_order_gateway_live_strategy_20260627.toml
```

有 `[strategy.order_gateway]` 时走多路 gateway；只有 `[strategy.order_session]` 时走单 session；两者同时存在必须 fail fast。
`worker_cpu_id` 覆盖 route session 的 WebSocket CPU。实际 CPU 分配必须遵守 `docs/runtime_cpu_allocation.md`。

Focused verification：

```bash
ctest --test-dir build/debug \
  -R '(gate_order|order_gateway|order_feedback|trading_runtime|strategy_order|order_latency)' \
  --output-on-failure

./build/debug/tools/lead_lag_strategy \
  --config config/strategies/lead_lag_30symbols_fusion_order_gateway_live_strategy_20260627.toml
```

未传 `--execute`/`--connect-data` 的命令只做 config/mode validation。Live probe/smoke 必须有当次明确授权。

## 未完成边界

- REST bootstrap/reconcile/resume、feedback unknown window 和 crash/restart 对账。
- Gateway route heartbeat、owner death、stale SHM command/event 清理。
- 账号级 submit/cancel/pending limiter。
- 真实 fanout=1 gateway smoke，以及后续 skew/Ack RTT/fillability 量化。
- Submit 前完整 metadata/risk：tick、quantity、notional、reduce-only、price band。
- Batch place、amend、cancel-all、order status/list 和 account/position realtime feed。

性能、成交率和恢复结论必须由对应 benchmark、profile、live probe 或 reconcile 证据支撑。
