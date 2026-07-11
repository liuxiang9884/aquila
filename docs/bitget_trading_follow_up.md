# Bitget 交易链路未覆盖边界与后续方向

## 文档定位

本文集中记录 Bitget `OrderSession`、`OrderFeedbackSession`、`OrderGateway` 和 LeadLag 接入当前没有覆盖的部分，以及此前讨论过、未来可能单独推进的方向。

这里的条目不是已批准的 implementation plan，也不表示已经排期。进入新的订单、恢复、限流、并发或低延迟设计前，仍需按
`AGENTS.md` 询问是否启用 Grill Me；这类关键交易链路默认建议 `grill-me-enhanced`。

对应当前设计：

- `docs/superpowers/specs/2026-07-10-bitget-order-session-design.md`
- `docs/superpowers/specs/2026-07-10-bitget-order-feedback-session-design.md`
- `docs/superpowers/specs/2026-07-10-bitget-order-session-rtt-probe-design.md`
- `docs/superpowers/specs/2026-07-11-bitget-order-gateway-design.md`

## 当前可用基线

截至 2026-07-11：

- Bitget UTA v3 `OrderSession` 已实现 login、limit GTC/IOC place、single cancel、request correlation 和直接 operation response。
- `OrderFeedbackSession` 已实现 account-wide `order` topic、现有 feedback SHM 路由、累计生命周期事实和 continuity lost。
- `bitget_order_gateway`、Bitget gateway worker、真实 OrderGateway SHM command/event queue 和 LeadLag lag metadata 接入已实现。
- `OrderSession` / `OrderFeedbackSession` RTT probe 已在 dedicated account 上发送真实 passive IOC；HA 与推断的高速 private endpoint 样本均为 zero-fill cancelled，运行前后 REST 证明无 open orders、无 position。
- 当前默认 checked-in session config 仍使用官方 HA endpoint。高速 private endpoint 只完成 DNS/TLS/login/真实订单验证，尚未作为官方稳定 endpoint 写入生产配置。
- gateway/LeadLag 路径尚未发送真实订单；目前的 gateway 证据来自 gtest、真实 SHM 集成、CLI dry-run/validate-only、Debug/Release 回归和通用 OrderGateway benchmark。

直接 operation Ack 仍不是 accepted、filled 或 cancelled terminal。订单生命周期事实只信 `OrderFeedbackSession`；明确 place reject 可以作为 rejected response，unknown result 必须进入未知/恢复语义。

## 实盘前置阻断

以下项目不是可选优化，而是重复执行 live LeadLag 或扩大订单规模前的阻断条件。

### 跨进程 `local_order_id` / `clientOid` 唯一性

当前 `OrderPool` 在新进程中从低 56-bit counter `1` 重新开始。相同 `strategy_id` 重启后可能复用
`local_order_id`，进而复用 Bitget `clientOid=a-<local_order_id>`。

当前 live probe 通过更换 strategy lane 临时规避；该方式不适合长期、自动或可重复实盘。未来需要单独选择并验证持久化 run epoch、外部 ID allocator、持久 counter 或其他不破坏现有 ABI 的唯一性方案。

### REST baseline、reconcile 与 unknown window

Bitget `order` topic 没有初始 open-order snapshot，也没有 sequence。WebSocket ready 不能证明账户订单状态连续。

未来恢复链路至少需要：

- 启动前 account / open orders / positions baseline。
- reconnect、operation unknown result 和 feedback continuity lost 后的 reconcile。
- reconcile 完成前暂停新开仓，且不伪造 rejected/cancelled。
- run-end flat 证明和 emergency cleanup 结果。
- crash/restart 后 producer run、local order 和 exchange order 的对账边界。

REST 查询和恢复控制器应位于 WebSocket owner thread 外，避免把慢路径、阻塞和动态工作引入下单热路径。

### 真实 gateway / LeadLag 分阶段验证

未来 live 顺序建议保持：

1. REST flat baseline。
2. 独立 `OrderFeedbackSession` ready。
3. `bitget_order_gateway` 单 route、单 symbol、最小 passive IOC。
4. Ack 与 terminal feedback 双证据。
5. REST run-end flat。
6. 再讨论 LeadLag signal-conditioned smoke 和多 route fanout。

每次 live 都需要用户单独授权；默认不自动执行。

## 已讨论、未来可能实现

### UID/account 级 limiter

本版本按用户决定不实现 UID/account limiter。未来若要扩大 gateway fanout 或长时间运行，应先确认 Bitget UTA WebSocket
下单的官方预算口径，再设计账户级共享 limiter，而不是直接把 REST `10 requests/s/UID` 当作 WebSocket 规则。

需要重新设计的内容包括：

- 多 route 共享账户预算的所有权。
- place/cancel 是否共享 budget。
- 本地 queue、立即 reject 或 backpressure 的语义。
- `429`、`30006`、`30007` 等错误后的降级和恢复。
- limiter 对主路径延迟、排队和尾延迟的实际 benchmark。

### `fast-fill` / `fill` 逐笔成交

V1 feedback 只使用累计 `order` topic。未来如接入 `fast-fill` 或 `fill`，需要先定义：

- `execId` 去重与跨流重复处理。
- order/fill 到达乱序和 cumulative quantity 重建。
- maker/taker role、fee 和逐笔成交事件 contract。
- 是否扩展 core event / SHM ABI。
- 相对 `order` topic 的真实 latency A/B，而不是仅比较 parser microbenchmark。

### account / position private feed

未来可以评估 account、position topic 是否用于风险和恢复，但不能把它们直接当作 order lifecycle 替代物。需要明确 snapshot/delta、sequence、重连 baseline、账户模式和持仓口径。

### endpoint 与 failover

未来可以继续评估：

- 官方 HA endpoint 与高速 private endpoint 的稳定性、授权范围和长期 RTT。
- 单 route 固定 endpoint、跨 route endpoint 分散或外部 failover controller。
- DNS、numeric `connect_ip`、TLS SNI 和 observed remote endpoint 的审计。
- endpoint 切换时的 unknown request window 和 feedback continuity。

高速 endpoint 当前属于实测可用但官方公开文档未确认的边界，不能仅凭一次低 RTT 就升级为默认生产配置。

### 多 route live 与路由策略

当前 gateway 已实现 N route，但 live gateway 首测仍限定 fanout=1。未来扩大 fanout 前需要量化：

- route readiness、Ack RTT 和 terminal feedback 的分布。
- round-robin、固定 route 或其他 route policy 对尾延迟的影响。
- route 断线时未完成请求、cancel 回原 route 和恢复策略。
- 多 route 是否共享同一个 Bitget account 和 limiter。

### Gate / Bitget 通用化

本版本明确采用 Bitget 专用实现并保持 Gate 结构。未来只有在两边 contract 稳定且能证明不会模糊 Ack、final response、错误分类和诊断字段时，才考虑提取公共装配或 policy；不为了减少文件重复而在订单热路径增加虚函数或动态分发。

### LeadLag direct `order_session`

当前 Bitget 只接入 `order_gateway` backend，direct `strategy.order_session` 装配仍是 Gate-only。未来若确有单进程 Bitget
LeadLag 需求，应单独处理 credentials、runtime adapter、config 校验和 feedback/recovery 前置条件；不能把现有 gateway 接入误读为 direct backend 已支持。

### 协议能力扩展

以下能力尚未纳入当前 adapter：

- market、FOK、post-only、RPI、STP、TP/SL、modify、batch、cancel-all。
- `hedge_mode` / `posSide`、isolated、spot、margin、coin futures、USDC futures。
- 多 feedback route、多账户 gateway 或账户级路由。

每项都需要重新定义 core contract、错误/终态语义、测试和 live 安全边界，不能只增加 encoder 字段。

## 尚未充分设计的部分

以下方向此前没有形成批准设计，未来推进时不能按现有实现自然外推：

- gateway SHM 是否需要携带 Bitget numeric `error_code`、connection id 或更多交易所诊断字段；当前共享 ABI 不包含这些字段。
- operation response 长时间缺失但连接未断时的 timeout/unknown-result 生成责任。
- 多账户 credentials 与单个 account-wide FeedbackSession 的一致性约束。
- gateway 进程异常退出后的自动重启、SHM ownership 和旧 command/event 清理。
- reconcile 完成后 continuity latch 的复位协议。
- 真实成交、部分成交和 safety-close 失败时的人工接管 runbook。
- strategy lane 耗尽后如何迁移唯一 ID，而不破坏历史 order join。

这些条目应先形成独立 spec 和测试计划，不应直接塞入当前 worker/session。

## 建议优先级

| 优先级 | 方向 | 进入条件 |
| --- | --- | --- |
| P0 | 跨进程唯一 ID | 重复 live gateway / LeadLag 前完成 |
| P0 | REST baseline、reconcile、run-end flat | 自动或长时间实盘前完成 |
| P0 | 单 route gateway guarded smoke | 唯一 ID 和恢复边界明确后执行 |
| P1 | UID/account limiter | 扩大频率、fanout 或运行时长前完成 |
| P1 | endpoint 长期 A/B 与 failover | 官方能力和 unknown window 设计明确后执行 |
| P1 | 多 route live | 单 route 闭环稳定后执行 |
| P2 | fast-fill / fill、account / position feed | 独立 contract 和去重设计批准后执行 |
| P2 | Gate / Bitget 通用化、direct backend | 有明确维护或性能收益并完成 benchmark 后执行 |

优先级只描述依赖关系，不代表自动排期或授权。
