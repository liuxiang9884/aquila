# Bitget UTA OrderGateway 与 LeadLag 接入设计

## 状态

- 日期：2026-07-11
- 范围：Bitget UTA v3 `OrderGatewayWorker`、`bitget_order_gateway` 进程，以及 LeadLag 对现有
  `order_gateway` backend 的最小接入
- 设计审查：已完成 `grill-me-enhanced`
- 当前阶段：设计已批准，尚未实现

## 目的

本文定义 Bitget `OrderSession` 如何接入现有通用 OrderGateway SHM 链路，并让 LeadLag 能通过已有
`order_gateway` backend 向 Bitget 发送订单。

本版本的硬约束是：Bitget 组件与现有 Gate 组件保持一一对应。除 Bitget 协议和已有类型差异外，不新增 Gate
没有的生产组件、配置字段、校验规则、限流器、恢复控制器或通用抽象。

## 事实源与对照入口

实现时以以下 Gate 代码为结构和行为模板：

- `exchange/gate/trading/order_gateway_worker.h`
- `tools/gate/gate_order_gateway.cpp`
- `test/exchange/gate/trading/order_gateway_worker_test.cpp`
- `test/exchange/gate/trading/multi_order_session_gateway_test.cpp`
- `core/trading/order_gateway_client.h`
- `core/trading/order_gateway_shm.h`
- `core/trading/order_gateway_shm_types.h`

Bitget 已有能力入口：

- `exchange/bitget/trading/order_session.h`
- `exchange/bitget/trading/order_session_runtime_adapter.h`
- `exchange/bitget/trading/order_types.h`
- `exchange/bitget/trading/order_feedback_session.h`
- `docs/superpowers/specs/2026-07-10-bitget-order-feedback-session-design.md`

LeadLag 入口：

- `tools/lead_lag/live_strategy.h`
- `tools/lead_lag/live_strategy.cpp`
- `strategy/lead_lag/config.h`
- `strategy/lead_lag/config.cpp`

## 已锁定边界

### 当前范围

- 新增与 Gate 同构的 Bitget `OrderGatewayWorkerPublisher`、
  `OrderGatewaySessionEventHandler` 和 `OrderGatewayCommandWorker<SessionT>`。
- 新增与 Gate 同构的 `bitget_order_gateway` 可执行程序。
- 一个 gateway 进程支持 N 个 route；每个 route 独占一个 worker thread、一个 Bitget
  `OrderSession` 和一对 SPSC command/event queue。
- 每个 route 引用独立的 Bitget `OrderSession` config，可由配置选择 HA 或高速 private endpoint。
- 复用现有 OrderGateway SHM、command/event ABI、client 和 reject reason。
- LeadLag 使用现有 pair 的 lag exchange 与 lag instrument 构造下单请求，使已有 `order_gateway`
  backend 可以发送 Bitget 订单。
- 补充 gtest、真实 SHM 集成测试、CLI dry-run/validate-only 验证、Debug/Release 回归和 Release benchmark。

### 非目标

- 不修改 OrderGateway SHM ABI。
- 不重构或泛化 Gate gateway 实现，不抽取 Gate/Bitget 公共 worker 基类或模板层。
- 不新增 UID/account rate limiter、预算队列或本地 rate-limit reject。
- 不修复 `OrderPool` 跨进程重启后复用低 56-bit counter 导致的 `clientOid` 重用问题。
- 不加入 REST reconcile、LeadLag 自动恢复或 gateway 内恢复状态机。
- 不改变 direct `strategy.order_session` 的 Gate session 装配。
- 不自动启动实盘、不发送真实订单。
- 在唯一 ID 问题处理前，不做可重复的 live LeadLag 测试。

## 组件与进程结构

```text
LeadLag process
  -> TradingRuntime / OrderManager
  -> OrderGatewayClient
  -> existing OrderGateway SHM
       command queue[route]
       event queue[route]
  -> bitget_order_gateway process
       BitgetOrderGatewayRouteWorker[route]
         OrderGatewayCommandWorker<BitgetOrderSession>
         Bitget OrderSession
  -> Bitget UTA private WebSocket

Bitget OrderFeedbackSession process
  -> existing OrderFeedback SHM
  -> LeadLag TradingRuntime / OrderManager
```

`bitget_order_gateway` 只负责 place/cancel command 的发送和 operation response 的返回。订单 accepted、partial
fill、filled 和 cancelled 等生命周期事实仍由独立的 Bitget `OrderFeedbackSession` 提供。

## 与 Gate 保持一致的规则

以下行为直接按 Gate 实现，不增加 Bitget 特例：

- gateway process、route worker、线程所有权和启动/停止顺序。
- route 数量、queue capacity、CPU affinity、TLS 组合限制和 SHM 创建方式。
- `--connect`、`--validate-only`、`--remove-existing-shm`、`--max-runtime-ms` CLI。
- 默认不带 `--connect` 时只做 dry-run；`--validate-only` 不连接网络。
- `kPlace`、`kCancel`、`kCacheExchangeOrderId`、`kForgetExchangeOrderId`、`kStop` command 分发。
- route mismatch、text field bounds、event queue full、ready/not-ready/stopped 状态处理。
- `OrderGatewayCommand` 到 `StrategyOrder` 的字段映射。
- 发送成功后按 `request_sequence` 保存 `command_seq`、`parent_id` 和本地时间戳。
- session not-ready 时清除待关联的 request metadata，不合成订单终态。
- event queue 无法发布时停止对应 route，避免继续发送无法回传结果的命令。

Bitget gateway 不增加 credential 环境变量一致性、pair exchange 一致性或其他 Gate 不具备的启动校验。

## Bitget 必需差异

### Session 与 credentials

route 加载 Bitget `OrderSessionConfig`，构造 Bitget `LoginCredentials`，包括 API key、secret 和 passphrase。
除此以外，route config 加载、CPU 覆盖和连接策略与 Gate 一致。

### Operation response 关联

Gate 同一请求可能先收到 Ack，再收到后续 operation response，因此 Gate 在 Ack 后保留 request metadata。

Bitget 当前 UTA v3 `OrderSession` contract 对每个 place/cancel 请求只产生一个直接 operation response：成功时为 Ack，
失败时为错误响应。Bitget publisher 因此在收到任意 response 后清除该 `request_sequence` 的 metadata，避免无后续响应的
条目长期保留。

Ack 只表示交易所直接接受该操作请求，不解释为订单 accepted、filled 或 cancelled，也不作为 terminal feedback。

### Send status 映射

复用现有 `OrderGatewayCommandRejectReason`，不扩展 ABI：

| Bitget `OrderSendStatus` | Gateway reject reason |
| --- | --- |
| `kOk` | `kNone` |
| `kNotLoggedIn` | `kSessionNotReady` |
| `kNotActive` | `kSessionNotActive` |
| `kInflightFull`、`kOrderIdCacheFull` | `kInflightFull` |
| encode buffer、local order ID、symbol、quantity、price、signature 等本地编码错误 | `kEncodeFailed` |
| `kInvalidRoute` | `kInvalidCommand` |
| `kUnsupportedOrderType` | `kUnsupportedOrderType` |
| `kNoPreparedWriteSlot` | `kNoPreparedWriteSlot` |
| `kWriteUnavailable` | `kWriteUnavailable` |

交易所返回的明确 error 仍通过 `kOrderResponse` event 传播。连接中断或结果未知时，不伪造 rejected、cancelled 或其他
terminal 状态。

Bitget response 没有的 Gate 专用时间字段保持零值，不增加新的 event 字段。

## LeadLag 最小接入

现有 LeadLag config 已能解析 pair 的 lead/lag exchange 和 instrument，不新增配置字段。

LeadLag 当前下单构造和 lag ticker 过滤存在 `Exchange::kGate` 与 `GateSymbol()` 硬编码。本版本将这些位置改为使用：

- `pair.lag_instrument.exchange`
- `pair.lag_instrument.exchange_symbol`，为空时沿用现有 symbol fallback

Gate 配置中的 lag exchange 仍为 Gate，因此原有 Gate 行为不变。Bitget 配置把 lag exchange 设置为 Bitget 后，同一策略逻辑
通过现有 `OrderGatewayClient` 把 `Exchange::kBitget` 和 Bitget exchange symbol 写入 command。

该修改只使用现有配置和值，不增加 exchange selector、gateway 类型字段或新的 runtime 抽象。direct
`strategy.order_session` 的 Gate session config、runtime adapter 和进程装配保持不变。

## 故障与恢复语义

- session 未登录或不 active：command 立即发布本地 reject event。
- 本地 encode/write 失败：发布对应 reject reason，不等待 feedback。
- operation response error：发布 `kOrderResponse`，保留交易所错误码和可用字段。
- disconnect：route 进入 not-ready 并清除 request metadata；不推断未知请求结果。
- reconnect 后 route 仅在 Bitget session login ready 后重新进入 ready。
- feedback continuity lost、REST baseline 和订单恢复继续由 gateway 外部处理。
- cache/forget exchange order ID 的行为与 Gate worker 一致。

## 测试设计

### TDD 单元测试

先仿照 `test/exchange/gate/trading/order_gateway_worker_test.cpp` 写失败测试，再实现 Bitget worker。覆盖：

- place/cancel 成功调用 session，发送成功时不产生本地 reject。
- route mismatch 和越界 text field 在 session 调用前被拒绝。
- Bitget 各 `OrderSendStatus` 映射到现有 reject reason。
- event queue full 后 route stopped，且不继续处理后续 command。
- stop、ready、not-ready、cache/forget 行为。
- response event 携带 command metadata。
- Bitget Ack 或 error 后均删除 request metadata。
- not-ready 清除待关联 metadata。

测试继续使用仓库现有 gtest。

### SHM 集成测试

仿照 Gate `multi_order_session_gateway_test.cpp`，使用真实 OrderGateway SHM 和 fake Bitget session 验证：

- N route command/event queue 独立。
- route owner thread 的启动、command drain、response publish 和停止。
- command/event sequence 与 metadata 关联。
- 不连接交易所、不读取真实 credentials、不发送订单。

### CLI 与配置验证

- `bitget_order_gateway --validate-only`：加载 gateway 和各 route session config，不连接网络。
- 默认 dry-run：行为与 Gate 相同。
- 无效 gateway/session config：返回非零并报告对应配置错误。
- `--connect` 不纳入自动测试，避免误启动外部连接。

### 回归与性能验证

- Debug 构建并运行新增 gtest、相关 OrderGateway/LeadLag 测试和完整 `ctest`。
- Release 构建并运行同一组回归。
- 运行现有 OrderGateway Release benchmark，记录 SHM command/event 主路径结果。
- 若为了覆盖 Bitget worker 必须新增 benchmark case，只增加 test/benchmark 代码，不引入生产抽象。
- 性能结论只基于实际 benchmark 输出；本设计不预设 Bitget 与 Gate 的延迟结果。

## 实盘边界

本轮实现与自动验证不启动实盘。后续如需测试，必须单独取得用户授权，并至少满足：

- REST 证明账户无 open orders、无 position。
- Bitget `OrderFeedbackSession` 已 ready。
- 使用 `one_way_mode`、crossed、USDT futures。
- gateway fanout 先设为 1。
- 最小数量、远离盘口的 IOC 分阶段执行。
- 测试结束后再次用 REST 证明账户 flat。

由于当前 `local_order_id/clientOid` 在进程重启后可能复用，在该问题解决或由操作流程保证唯一 lane 前，不执行可重复的
live LeadLag 测试。

## 实现顺序

1. 编写 Bitget gateway worker 的失败 gtest。
2. 实现与 Gate 同构的 Bitget worker，使测试通过。
3. 编写并实现 Bitget gateway process、配置与 CLI 验证。
4. 添加真实 SHM 多 route 集成测试。
5. 将 LeadLag 的 lag exchange/symbol 硬编码替换为已有 pair metadata，并补回归测试。
6. 运行 Debug/Release 回归与 Release benchmark。
7. 多轮 review `OrderSession`、`OrderFeedbackSession` 和新增 gateway/LeadLag 链路，直到既定范围内无待修问题。

