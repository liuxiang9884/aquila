# Bitget LeadLag V1 Stop-and-Flat 设计

## 目标

在不修改现有进程内 `local_order_id` allocator 的前提下，为 Bitget `order_gateway` backend
补齐与 Gate V1 同语义的 REST preflight、异常 stop-and-flat、run-end flat 和 fresh-run isolation，
从而允许后续按分阶段门槛执行首次 Bitget gateway 与 LeadLag guarded live smoke。

V1 的核心安全边界是“停止并清空”，不是“恢复并继续”：任何 `UnknownResult`、
`ContinuityLost`、strategy 非零退出、gateway 故障、guard 故障或无法解释的 REST 状态都会终止
整个 run。系统完成范围内撤单、reduce-only 平仓和 REST flat 验证后保持停机，不在同一 run
恢复交易，也不恢复 crash 前的策略或订单内存状态。

## 已锁定决策

- V1 不实现持久化跨进程订单 ID；未来若支持 overlapping runs、crash-state reconcile 或自动
  resume，必须重新评估该决策。
- 不允许 strategy-only restart。Strategy、Bitget gateway、feedback producer 和 run-specific SHM
  共享同一个 run boundary。
- 外围 live guard 使用显式 symbol allowlist；独立 emergency helper 同时支持 allowlist 与
  dedicated-account。Dedicated-account 模式必须显式确认并限制最大非 flat position 数量。
- 每个 run 使用新的 gateway/feedback SHM 名称和隔离配置。只有确认没有 live producer 或
  consumer 时，才允许销毁并重建固定名称 SHM。
- 跨 run 订单身份是 `(run_id, clientOid)`；V1 不允许仅按历史 `clientOid` 自动恢复状态。
- 首次 smoke 必须有人值守。Outer guard 被 `SIGKILL`、主机失效或网络完全不可用时，沿用 Gate
  V1 的人工 emergency handoff 边界，不宣称存在自动清理保证。

## 方案比较

### 方案 A：扩展现有 LeadLag guard，注入交易所 adapter

保留 `scripts/lead_lag/run_live_with_guard.py` 的 Gate orchestration 和默认行为，把 REST state
query、flat predicate、credential、client 和 emergency flatten 作为交易所 adapter 注入。Bitget
adapter 增加 passphrase、UTA category/symbol 归一化和 Bitget response validation。

优点是 Gate 与 Bitget 共用同一套 preflight、process supervision、final check、exit code 和 summary
语义，后续安全修复不会形成两套漂移实现。代价是需要对当前 Gate-oriented Python 类型和 CLI
做一次受测试保护的局部抽象。

### 方案 B：复制一份 Bitget guard

新建独立 `run_bitget_live_with_guard.py`，复制 Gate wrapper 后替换 REST 实现。短期改动直观，但
两个接近千行的 wrapper 会快速产生行为漂移，特别是 affinity overlay、credential validation、
exit code 和 report summary。该方案不采用。

### 方案 C：先实现持久化跨进程 ID

通过 run epoch 或持久 counter 保证 `clientOid` 永久唯一，再实现 reconnect reconcile/resume。
该方案适合未来 V2，但扩大了 ABI、状态持久化和 crash consistency 范围，不符合已经锁定的
V1 stop-and-flat 目标。该方案不采用。

结论：采用方案 A。

## 组件边界

### Bitget REST trading client

新增 Bitget trading-side REST client，复用现有签名规则，但不把写操作混入只读 CLI 的命令面。
Client 负责：

- 使用 `api_key`、`api_secret` 和 `api_passphrase` 生成 UTA REST headers；
- 对 GET/POST 请求统一处理 timestamp、query、JSON body、HTTP error 和 JSON decode；
- 验证顶层 `code == "00000"`，否则返回可分类的 REST failure；
- 只在结构化 summary 中保留非敏感 request/response 信息。

现有 `scripts/bitget/account/query_bitget_account.py` 继续作为只读 CLI；共享的签名与规范化 helper
可以复用，但其 CLI 不增加下单入口。

### Bitget emergency flatten helper

新增与 Gate helper 同语义的 Bitget emergency helper，职责限定为冷路径账户安全处理：

1. 验证 allowlist 或显式确认的 dedicated-account scope；
2. 查询 `GET /api/v3/trade/unfilled-orders` 与
   `GET /api/v3/position/current-position`；
3. 对范围内 open order 调用 `POST /api/v3/trade/cancel-order`；
4. 重新查询 position，并按当前 `total` 的反方向提交 `orderType=market`、`reduceOnly=yes` 的
   `POST /api/v3/trade/place-order`；
5. 再次查询并撤销残留 open orders；
6. 轮询 REST，直到范围内 open orders 为空且所有 position 的 `total`、`available`、`frozen`
   都为 0，或到达 timeout；
7. 输出与 Gate 一致的结构化 plan、动作记录、final snapshot、result 和 exit code。

Bitget cancel/place 成功响应只表示交易所收到请求，不作为 terminal 或 flat 证据。最终结果只信
后续 REST open-order/position snapshot。官方接口依据：

- [Get Open Orders](https://www.bitget.com/api-doc/uta/trade/Get-Order-Pending)
- [Get Position Info](https://www.bitget.com/api-doc/uta/trade/Get-Position)
- [Cancel Order](https://www.bitget.com/api-doc/uta/trade/Cancel-Order)
- [Place Order](https://www.bitget.com/api-doc/uta/trade/Place-Order)

### LeadLag exchange guard adapter

`run_live_with_guard.py` 增加显式 exchange 选择，默认仍为 Gate，保证现有命令兼容。交易所 adapter
提供以下能力：

- 从 strategy 的 direct order-session 或 order-gateway route config 解析 credential env 名称；
- 校验 gateway 所有 route 属于同一账户；
- 创建交易所 REST requester；
- 归一化 allowlist symbol；
- 查询并解析 `GuardState`；
- 构造 exchange-specific flatten config 并执行 emergency flatten；
- 提供 exchange name、category/settle 和非敏感 credential source 给 summary。

Bitget credential contract 比 Gate 多 `api_passphrase_env`。显式 CLI override 必须三项同时提供，
并与可解析的 strategy/gateway config 完全一致；缺失、混用账户或 route credential 不一致都在
启动 REST preflight 前 fail closed。

Orchestration 本身保持 Gate V1 行为：

- preflight REST 失败：不启动 strategy；
- preflight 非 flat：不自动清理历史残留，拒绝启动；
- strategy 异常或非零退出：执行 emergency flatten；
- 正常退出：执行 final REST check；
- final check REST 失败或非 flat：执行 emergency flatten；
- flatten 可证明 flat：返回 emergency handoff exit code，仍保持停机；
- flatten 失败或无法证明 flat：返回 emergency failure，禁止自动重启。

### Fresh-run isolation

每次 live run 都使用 `/home/liuxiang/tmp/<run_id>/` 下的 config、log 和 snapshot。运行配置必须让：

- strategy 的 `[strategy.order_gateway].config` 指向本 run 的 Bitget gateway config；
- strategy feedback SHM 与本 run feedback producer 的 SHM 完全一致；
- gateway SHM 与 feedback SHM 名称都包含本次唯一 `run_id`；
- gateway 固定 `route_count=1`；
- gateway、feedback 和 strategy 的 credentials 指向同一 Bitget 账户。

实现沿用现有 TOML overlay 机制生成 scratch config，并新增 run-isolation validation。Guard summary
必须列出实际 strategy/gateway/feedback config、SHM name、run id 和验证结果。外部 feedback/gateway
进程只有在确实使用生成配置时才能标记 applied；未应用、名称不匹配或复用旧 SHM 时，真实订单
模式拒绝启动。

## 数据流与启动顺序

一次新的 V1 run 必须按以下顺序执行：

1. 停止旧 strategy、gateway 和 feedback producer；
2. 对旧 run 执行 REST stop-and-flat，并保存结构化证据；
3. 创建新的 run directory、runtime config 和 SHM 名称；
4. 启动新 feedback producer，等待 login/subscribed ready；
5. 启动 `fanout=1` gateway，等待 route ready；
6. Guard 使用与 gateway/feedback 相同的 credentials 做 fresh REST baseline；
7. Baseline 证明 allowlist 范围内无 open orders、position flat 后，才启动 strategy；
8. Strategy 退出后 drain 已到达的 response/feedback，执行 final REST check；
9. 任意异常进入 emergency flatten；最终无论成功或失败都保持停机。

V1 不使用 REST history 跨 run 恢复订单。Report 继续使用现有 `run_id:local_order_id` 和
`run_id` 参与的 position key，避免复用 `clientOid` 导致跨 run 混淆。

## REST flat 与错误语义

Flat snapshot 必须同时满足：

- REST 调用成功，顶层 code 和 data 结构可解释；
- allowlist 模式返回的 symbol 不得越界；
- open-order list 为空；
- 每个目标 symbol 不存在 position，或 `total == 0`、`available == 0`、`frozen == 0`；
- 不存在无法归属的额外 position/open order；dedicated-account 模式必须完整扫描 category；
- 数量全部用 `Decimal` 解析，不能用 binary floating-point 判断 flat 或生成 close quantity。

清理阶段与 Gate V1 一致，轮询到首个完整且可解释的 flat snapshot 即成功，不增加额外稳定时间
窗口。下一 run 的 fresh baseline 再次验证 flat，形成第二道边界。

以下情况全部 fail closed：HTTP timeout、rate limit、非 JSON、`code != "00000"`、字段缺失、
非法 Decimal、symbol/category mismatch、cancel/place 结果不明确、poll timeout，以及任何无法证明
open orders 为空或 position flat 的状态。Mutating response 不明确时不得盲目重发同一动作；helper
可以继续通过 read-only REST snapshot 判断是否已达成目标，无法证明时返回失败并等待人工处理。

## 安全边界与非目标

V1 不包含：

- 自动 reconnect 后继续交易；
- crash 前 `OrderManager` 或 execution group 重建；
- persistent `local_order_id` / `clientOid`；
- 多 route live、failover 或 account-level WebSocket limiter；
- `fill`/`fast-fill` 的逐笔 `execId` 重建；
- 无人值守长跑；
- outer guard 被 `SIGKILL`、主机失效或 REST 全面不可用时的自动恢复保证。

最后一类故障沿用 Gate V1：首次 smoke 有人值守，外部 supervisor/runbook 调用幂等 emergency
helper，任何无法证明 flat 的结果都禁止重启。

## 测试与证据门槛

### 自动测试

- Bitget REST 签名、GET/POST request body、顶层 code 和 malformed response；
- allowlist/dedicated-account scope、显式确认和最大 position 数量保护；
- open-order/position parser、Decimal、symbol/category mismatch 和 flat predicate；
- cancel-before-close、reduce-only market close、second cancel 和 poll timeout；
- idempotent flat account 不发送 mutating request；
- Gate guard regression，确保默认 exchange 和既有 exit code/summary 不变；
- Bitget credential/passphrase、route account consistency、preflight/final/flatten 分支；
- `UnknownResult`、`ContinuityLost`、stale SHM 与 run-isolation validation。

### 只读与 live smoke

1. Read-only account/open-order/position baseline；
2. emergency helper dry-run；
3. flat-account helper smoke，证明幂等且无 mutating request；
4. tiny-position stop-and-flat，证明 cancel、reduce-only close 和 REST final flat；
5. 注入 `ContinuityLost`/异常退出，证明 strategy 停止、guard handoff 和 final flat；
6. `fanout=1` gateway passive IOC，取得 direct Ack、matching terminal feedback 与 REST run-end flat；
7. 完成以上证据并提交后，才把 unique-ID P0 视为由 strict run isolation 替代；首个
   signal-conditioned LeadLag live smoke 仍需用户对当次 run 单独授权。

任何性能、延迟、稳定性或 fillability 结论必须来自对应 benchmark/profile/live evidence；本设计
不从组件测试或单次 login/Ack 外推这些结论。
