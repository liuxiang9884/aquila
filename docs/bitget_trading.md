# Bitget UTA Trading

本文是 Bitget UTA v3 交易链路的当前事实源，覆盖 `OrderSession`、`OrderFeedbackSession`、RTT probe、
`OrderGateway` 与 LeadLag 接入。历史 implementation plan 和完成态 spec 不再作为运行或设计入口。

## 当前范围与证据边界

截至 2026-07-11，仓库已实现：

- `OrderSession`：private WebSocket login、limit GTC/IOC place、single cancel、request correlation 和直接 operation response。
- `OrderFeedbackSession`：独立 private connection、account-wide `order` topic、累计订单生命周期事实、feedback SHM 路由和 continuity lost。
- RTT probe：单路/多路 run plan、passive IOC、Ack 与 terminal feedback 对账、CSV/metadata 输出和 safety close 状态流。
- `bitget_order_gateway`：N route worker、真实 OrderGateway SHM command/event queue、route state 和 CLI dry-run/validate-only。
- LeadLag：可以用现有 lag metadata 构造 Bitget gateway command；只接入 `order_gateway` backend。

真实 passive IOC 已在 dedicated account 上分别验证官方 HA endpoint 与推断的高速 private endpoint。样本均取得 Ack 与
terminal feedback 双证据，且运行结束后通过 REST 确认无 open order、无 position。该证据只覆盖 probe；gateway/LeadLag
路径尚未发送真实订单。Dedicated-account flat、余额、IP 白名单和 endpoint 可用性都是当次运行事实，不得外推为永久状态。

`OrderSession` 的 direct operation response 只表示请求的直接响应：

- 成功 response 不是 accepted、filled 或 cancelled terminal。
- 明确 place reject 可以发布 rejected response。
- 连接、超时或协议状态无法确认结果时必须进入 `UnknownResult`/恢复语义，不得伪造 rejected 或 cancelled。
- accepted、partial fill、filled 和 cancelled 生命周期事实只信 `OrderFeedbackSession`。

## 组件与进程结构

```text
LeadLag strategy process
  └─ OrderGatewayClient
       ├─ route 0 command/event SHM ─ Bitget gateway worker 0 ─ OrderSession 0
       ├─ route 1 command/event SHM ─ Bitget gateway worker 1 ─ OrderSession 1
       └─ route N command/event SHM ─ Bitget gateway worker N ─ OrderSession N

Independent feedback process
  └─ OrderFeedbackSession ─ account-wide order topic ─ feedback SHM lanes
```

每个 `OrderSession` 和 `OrderFeedbackSession` 都由单一 owner thread 驱动 WebSocket。Gateway worker 负责发送 command、
发布 Ack/response 和 route readiness，不解释 duplicate/split、winner、overfill、非赢家 cancel 或策略恢复。REST baseline、
reconcile 和 emergency cleanup 必须在 WebSocket owner thread 外运行，避免把阻塞、动态分配和慢路径工作放入下单热路径。

## OrderSession contract

### Endpoint、login 与 ready

Checked-in config 默认使用官方 high availability private endpoint：

```text
wss://vip-ws-uta.bitget.com/v3/ws/private
```

推断的高速 private endpoint 已完成 DNS/TLS/login/真实 IOC 验证，但缺少官方稳定能力确认，不是默认生产配置。
Numeric `connect_ip` 只覆盖 TCP destination；TLS SNI、Host 和 WebSocket target 仍来自 session config。

`Ready()` 只表示当前 private WebSocket 已 login，可以接收新 command；它不证明 feedback continuity、REST baseline、
账户 flat 或历史请求已 reconcile。连接 generation 变化后，旧 request correlation 不得静默解释为新 generation 的结果。

### Place 与 cancel

当前 adapter 只覆盖 USDT futures 的 limit GTC/IOC 和 single cancel。Market、FOK、post-only、RPI、STP、TP/SL、modify、
batch、cancel-all、hedge-mode `posSide`、spot/margin/coin/USDC futures 都不在当前 contract 中。

`local_order_id` 编码为：

```text
clientOid = a-<local_order_id>
```

Request id 用于 operation response correlation，`clientOid` 用于订单生命周期和 feedback routing。两者不能互相替代。
Place/cancel 的 encoder 使用固定容量 buffer；热路径不得为 JSON 格式化引入无界动态分配。

### Response 与错误

- 本地确定失败：编码、容量、状态或参数在发送前确定失败，可以立即返回本地错误。
- 交易所明确拒绝：response 明确表示请求被拒绝，可以映射为 rejected response。
- 未知结果：发送后断线、无法关联、response 缺失或无法确认交易所是否接受时，保留订单并触发恢复，不得重发同一意图来猜测结果。
- `OrderSession` 必须先发布当前 response/unknown event，再把 route 置为 not-ready，避免丢失导致降级的最后一个请求结果。

## OrderFeedbackSession contract

### Subscription 与 ready

V1 只订阅 account-wide `order` topic。`Ready()` 表示 login 和 subscription Ack 匹配，不表示存在初始 open-order snapshot、
sequence continuity、REST baseline 或 reconcile。V1 不订阅 `fill`/`fast-fill`，不提供逐笔 `execId`、maker/taker role、fee 或
逐笔成交事件。

### Lifecycle 映射

Parser 先识别属于 Aquila 的 `clientOid=a-...`，再解析必需字段和 scope。当前发布累计订单事实：

- `new`：accepted/current order fact。
- `partially_filled`：累计成交量增加但未 terminal。
- `filled`：累计成交量达到 terminal fill。
- `cancelled`/`canceled`：取消 terminal，保留已累计成交量。

同一订单的 quantity/price/status 必须按累计事实处理，不能把重复或乱序消息转换为额外成交。无法恢复的 Aquila order decode、
disconnect 和 producer restart 会广播 `kContinuityLost`。Continuity latch 只有在外部 reconcile 协议确认基线后才能复位。

### SHM 路由

`clientOid` 解出 `local_order_id` 后，按现有 strategy lane 发布到 feedback SHM。Account-wide session 与 gateway 使用的
credentials 必须属于同一账户；多账户或多 feedback route 尚未定义。

## OrderGateway 与 LeadLag 接入

`MultiOrderSessionGateway` 管理 route readiness 和 route selection；`OrderGatewayWorker` 独占一条 command/event queue 和一个
`OrderSession`。Gateway SHM 使用现有通用 command/event ABI，不包含 Bitget numeric error code、connection id 或额外诊断字段。

LeadLag 根据 lag instrument metadata 选择 exchange/symbol，当前 Bitget 只允许 `strategy.order_backend = "order_gateway"`。
Direct `strategy.order_session` 装配仍是 Gate-only。Gateway N route 已通过 gtest、真实 SHM integration、CLI dry-run/
validate-only、Debug/Release regression 和通用 OrderGateway benchmark；首个 live gateway 必须固定 fanout=1。

Route 断线后的未完成 place/cancel、cancel 是否必须回原 route、operation response timeout 的生成责任和 gateway 进程重启后的旧
SHM command/event 清理尚未形成完整恢复 contract。

## RTT probe 与 live 证据

Probe 使用被动 IOC，按最新 BBO 和 instrument metadata 构造最小订单，等待 direct Ack 与独立 terminal feedback。正常顺序为：

1. REST account/open-orders/positions flat baseline。
2. `OrderFeedbackSession` ready。
3. 单 route session login。
4. Passive IOC place。
5. Direct Ack 与匹配 `local_order_id` 的 terminal feedback。
6. 如存在持仓，派发 reduce-only safety close 并等待 Ack+terminal feedback。
7. REST run-end flat。

Sample flow 对 Ack/final response 的 `local_order_id` 做 stage correlation；同一 cycle 的 session interval 为 0 时等待下一条行情，
非 0 时到 deadline 使用当时最新 BBO，不通过 sleep/busy loop 阻塞 owner thread。Probe 证据不能替代 signal-conditioned LeadLag
fillability、gateway latency 或长期 endpoint 稳定性结论。

## 配置、代码与验证入口

主要配置：

```text
config/order_sessions/bitget_order_session.toml
config/order_feedback/bitget_order_feedback_session.toml
config/order_feedback/bitget_order_feedback_shm.toml
config/order_gateways/bitget_order_gateway.toml
config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml
config/order_session_rtt_probe/bitget_order_session_rtt_connections.csv
config/data_readers/bitget_order_session_rtt_probe.toml
```

主要代码：

```text
exchange/bitget/trading/order_session.h
exchange/bitget/trading/order_feedback_session.h
exchange/bitget/trading/order_feedback_parser.h
exchange/bitget/trading/multi_order_session_gateway.h
exchange/bitget/trading/order_gateway_worker.h
tools/bitget/bitget_order_gateway.cpp
tools/bitget/order_session_rtt_probe/
tools/lead_lag/live_strategy.h
```

凭据默认从 `BITGET_TEST_KEY`、`BITGET_TEST_SECRET`、`BITGET_TEST_PASSPHRASE` 读取。CLI 参数传的是环境变量名，
不得把 secret 写入配置、命令历史、文档或 log。

常用只读验证：

```bash
./build/debug/tools/bitget_order_gateway --config config/order_gateways/bitget_order_gateway.toml
./build/debug/tools/bitget_order_gateway --config config/order_gateways/bitget_order_gateway.toml --validate-only
./build/debug/tools/bitget_order_session_rtt_probe \
  --config config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml
ctest --test-dir build/debug -R '^bitget_(order|operation)' --output-on-failure
```

所有真实订单都需要用户对当次运行单独授权；dry-run、login、subscription 或 validate-only 不构成下单授权。

## 实盘前置阻断

### P0：跨进程唯一 ID

`OrderPool` 在新进程中从低 56-bit counter 1 重新开始。相同 `strategy_id` 重启可能复用 `local_order_id` 和
`clientOid`。更换 strategy lane 只能临时规避，不能用于可重复 live。重复 gateway/LeadLag live 前必须选择并验证持久 run epoch、
外部 allocator、持久 counter 或不破坏现有 ABI 的等价方案。

### P0：REST baseline、reconcile 与 unknown window

恢复链路至少需要启动前 account/open-orders/positions baseline；reconnect、unknown result 和 continuity lost 后 reconcile；
reconcile 完成前暂停新开仓；run-end flat 与 emergency cleanup；crash/restart 后 producer run、local order 和 exchange order 对账。

### P0：单 route guarded smoke

只有唯一 ID 和恢复边界明确后，才按 REST flat → feedback ready → fanout=1 gateway passive IOC → Ack+terminal feedback →
REST run-end flat 的顺序执行。之后才能讨论 signal-conditioned LeadLag smoke。

## 后续方向与优先级

- P1：基于官方 UTA WebSocket 预算设计 UID/account 级共享 limiter；不能直接套用 REST `10 requests/s/UID`。
- P1：官方 HA 与高速 endpoint 长期 A/B、endpoint failover 和切换 unknown window。
- P1：单 route 闭环稳定后的多 route live 与 route policy。
- P2：`fast-fill`/`fill` 的 `execId` 去重、跨流乱序和累计 quantity 重建。
- P2：account/position private feed、通用 Gate/Bitget policy、direct backend 和更多协议能力。

扩大频率、fanout 或运行时长前必须先完成 account limiter。任何性能、稳定性或 fillability 结论都需要对应 benchmark、profile
或 live 证据，不能从组件 microbenchmark、login success 或单次 passive IOC 外推。
