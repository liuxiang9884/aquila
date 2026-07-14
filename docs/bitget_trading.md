# Bitget UTA Trading

本文是 Bitget UTA v3 交易链路的当前事实源，覆盖 `OrderSession`、`OrderFeedbackSession`、RTT probe、
`OrderGateway` 与 LeadLag 接入。历史 implementation plan 和完成态 spec 不再作为运行或设计入口。

## 当前范围与证据边界

截至 2026-07-14，仓库已实现：

- `OrderSession`：private WebSocket login、limit GTC/IOC place、single cancel、request correlation 和直接 operation response。
- `OrderFeedbackSession`：独立 private connection、account-wide `order` topic、累计订单生命周期事实、feedback SHM 路由和 continuity lost。
- RTT probe：单路/多路 run plan、passive IOC、Ack 与 terminal feedback 对账、CSV/metadata 输出和 safety close 状态流。
- `bitget_order_gateway`：N route worker、真实 OrderGateway SHM command/event queue、route state 和 CLI dry-run/validate-only。
- LeadLag：可以用现有 lag metadata 构造 Bitget gateway command；只接入 `order_gateway` backend。
- REST stop-and-flat：提供 UTA market/reduce-only request builder、allowlist/dedicated-account emergency helper、
  REST preflight/final check 和 Gate/Bitget 共用的 LeadLag guard orchestration。
- Fresh-run isolation：为每轮 Bitget live 生成独立 gateway/feedback SHM、runtime config 和 manifest；
  `--execute` 会在读取凭据和访问 REST 前强制验证 manifest。

真实 passive IOC 已在 dedicated account 上分别验证官方 HA endpoint 与推断的高速 private endpoint。样本均取得 Ack 与
terminal feedback 双证据，且运行结束后通过 REST 确认无 open order、无 position。该证据只覆盖 probe；gateway/LeadLag
路径尚未发送真实订单。2026-07-14 已对 `BTCUSDT` 完成当次 read-only REST baseline、allowlist emergency dry-run 和
flat-account helper smoke：baseline 无 open order、无 position，dry-run plan 为空；flat-account smoke 的 symbol cancel 返回
HTTP 400/code `25204 Order does not exist`，helper 未重发，随后以 conservative REST snapshot 得到
`verified_flat_after_unknown`，未提交 close order，独立 post-check 仍为 flat。原始 JSON 位于
`/home/liuxiang/tmp/bitget_btcusdt_evidence_20260714T013021Z/` 和
`/home/liuxiang/tmp/bitget_btcusdt_flat_helper_20260714T013900Z/`。该次 live flat position 响应为 `data.list=null`，helper 已补充
兼容与自动测试。

2026-07-14 首次 `BTCUSDT` tiny-position 尝试以 `0.0001 BTC` market buy 成交，但 helper 在“有仓位、无 open order”时因
symbol cancel 返回 HTTP 400/code `25204` 而提前进入错误复核，没有自动提交 close；随后按当次授权直接提交
`sell 0.0001 / reduceOnly=yes`，并以独立 `open orders → positions → open orders` REST post-check 证明 flat。根因修复后，
flat-account live regression 已确认两次 `25204` 被作为幂等 no-op 处理并返回 `verified_flat`。原始证据位于
`/home/liuxiang/tmp/bitget_btcusdt_tiny_long_20260714T030554Z/`。该次尝试不算自动 tiny-position stop-and-flat 通过；必须重新取得
真实订单授权并复跑。修复后的同日重跑以 `0.0001 BTC` market buy 成交，helper 接受两次明确 `25204` no-op，自身提交
`sell 0.0001 / reduceOnly=yes`；开仓和平仓 order-info 均为 terminal `filled`，helper 返回 `verified_flat`、errors 为空，独立
`open orders → positions → open orders` post-check 仍为 flat。重跑证据位于
`/home/liuxiang/tmp/bitget_btcusdt_tiny_long_retry_20260714T033212Z/`。因此 tiny-position stop-and-flat 证据门已通过，下一门是
fanout=1 gateway passive IOC；尚无 gateway IOC 或 LeadLag 真实订单证据。Dedicated-account flat、余额、IP 白名单和 endpoint
可用性都是当次运行事实，不得外推为永久状态。

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

Outer guard process
  └─ REST preflight/final check ─ strategy child ─ emergency stop-and-flat
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
scripts/bitget/trading/place_futures_order.py
scripts/bitget/trading/emergency_flatten_futures.py
scripts/lead_lag/prepare_bitget_live_run.py
scripts/lead_lag/run_live_with_guard.py
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
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

所有真实订单都需要用户对当次运行单独授权；dry-run、login、subscription 或 validate-only 不构成下单授权。

## V1 stop-and-flat 与 fresh-run contract

V1 不修改 `OrderPool`，所以新进程仍可能复用低 56-bit counter，`clientOid` 仍是 `a-<local_order_id>`。该事实不再阻断
V1 的原因不是 ID 已经全局唯一，而是 V1 明确禁止跨进程恢复后继续交易：

- 任意 `UnknownResult`、`ContinuityLost`、strategy/gateway/feedback/guard 故障都终止整轮 run；
- 停止旧 strategy、gateway 和 feedback producer 后，按合约范围撤销 open orders、提交 reduce-only market close，
  并只用后续 REST snapshot 证明 flat；成功或失败都保持停机；
- 下一轮必须使用新的 `run_id`、gateway SHM、feedback SHM 和 runtime config，不允许 strategy-only restart；
- 跨 run 审计身份使用 `(run_id, clientOid)`，不按历史 `clientOid` 自动重建订单并恢复开仓；
- 只有未来要支持 overlap、read-only reconcile 后 resume 或 crash-state recovery 时，persistent `local_order_id/clientOid`
  才重新成为前置条件。

`scripts/lead_lag/prepare_bitget_live_run.py prepare` 只生成 `/home/liuxiang/tmp/<run_id>/configs/` 下的三个 overlay
和 `bitget_live_manifest.json`，并把 gateway 的 `order_session_config` 与 strategy 的 LeadLag config 改写为绝对路径；该命令不连接
交易所、不读取账户、不创建 SHM。Operator 确认 feedback 与 fanout=1 gateway
确实使用 manifest 中配置后，以两个实际 PID 执行 `mark-applied --gateway-pid <pid> --feedback-pid <pid>`。manifest schema v2
会重新读取三个 TOML，并复核路径、SHM、`route_count=1`、三字段凭据、category、position/margin mode 和 private WebSocket
endpoint 一致性；同时从 `/proc` 绑定 PID、start time、executable、`--connect` 与实际绝对 `--config`。进程 credential 值只在内存中
比较，不写入 manifest、summary 或 log。Guard 的 Bitget `--execute` 还会再次验证：

- manifest schema、run directory、strategy command 的 `--config` 和 `run_id`；
- gateway/feedback SHM 都包含本轮 `run_id`，strategy/gateway/feedback 指向完全一致；
- `external_configs_applied=true`、`route_count=1`，gateway/feedback 的 PID/start time/executable/config 未变化，且 guard
  当前 credential 值与两个进程一致；PID reuse、`/proc` 不可读或 credential 无法比较时 fail closed；
- strategy feedback 必须启用且 `poll_budget > 0`，不能以真实订单模式绕过 feedback SHM；
- strategy overlay 指向的 LeadLag 配置中，所有 `lag_exchange = "bitget"` 的 symbol 都必须包含在 guard
  `--contract` 范围内；guard 范围可以更大，但不能遗漏策略可能交易的 Bitget symbol；
- strategy command 必须直接执行 basename 为 `lead_lag_strategy` 的二进制，不接受 `bash -c`、`env`、`taskset`
  等 wrapper；否则 guard 无法从 argv 证明 `--execute` 和 `--config`；
- 真实 `--execute` 只允许默认生产 REST base URL，不接受测试、代理或任意覆盖 endpoint；
- summary 的 `runtime_isolation` 记录实际 manifest/config/SHM、`strategy_lag_symbols` 与 `validated=true`，
  不记录 secret/passphrase 内容。

## REST emergency helper 与 guard 语义

`scripts/bitget/trading/emergency_flatten_futures.py` 支持：

- `allowlist`：按 category 查询全量后在本地只保留显式 `--symbol`，使用 per-symbol cancel 撤单和平仓；用于共享账户或限定合约；
- `dedicated-account`：必须显式 `--confirm-dedicated-account`，扫描整个 category，并在首次查询与撤单后的 position snapshot
  都受 `--max-position-count` 保护；
- `--dry-run`：执行只读 REST 查询并输出 plan，不发送 cancel/place；
- flat snapshot：完整遍历 UTA open-orders cursor，按 `open orders → positions → open orders` 查询；两次订单都为空且 position
  全部为零才算 flat，cursor 循环、页数超限或响应不可解释均 fail closed；
- 实际清理：即使首次 snapshot 没有订单，也执行一次幂等范围撤单；allowlist 使用
  `/api/v3/trade/cancel-symbol-order`，dedicated account 使用 category-wide cancel；随后按 REST position 的
  `posSide/total/marginMode` 提交反向 reduce-only market close，再次范围撤单并轮询；
- Bitget 对没有可撤订单的 symbol cancel 可能返回 HTTP 400/code `25204`；helper 只在 cancel-symbol 路径把该明确 code 视为
  幂等 no-op，并继续 position close 与最终 REST 验证，其他 REST code 和不可解析错误仍 fail closed；
- cleanup pacing：多 symbol 撤单最多 5 requests/s，多 position close 最多 10 requests/s；这些等待只在 REST 慢路径；
- flat predicate：open orders 为空，且每个 position 的 `total/available/frozen` 都为 0；所有数量用 `Decimal`；
- mutating response 不明确时只做独立 REST 复核；能证明 flat 才返回 `verified_flat_after_unknown`，否则 fail closed，不盲目重发。

Helper exit code：`0` 表示 REST 已证明 flat，`2` 表示 timeout 后仍非 flat，`3` 表示 scope/config 拒绝，`4` 表示
REST 失败或响应不可解释。`run_live_with_guard.py` 正常退出且 final flat 返回 `0`；异常后 flatten 成功返回 `10` 并保持停机；
flatten 失败或无法证明 flat 返回 `11`。strategy 返回或抛异常后，guard 必须先等待 gateway 自行退出，再按需升级
`SIGTERM`/`SIGKILL`，并停止 feedback；只有两个绑定进程均已停止才允许 final REST 或 emergency flatten。quiescence 失败直接返回
`11`，不生成新的 flat 成功证明。guard 自身收到终止信号时对子 strategy 的等待也有界，并会从 `SIGTERM` 升级到 `SIGKILL`。
Preflight 非 flat 只拒绝启动，不自动清理历史残留。

## 真实订单证据门

当前代码与自动测试完成不等于已有 Bitget live 证据。每一步真实订单都需要用户对当次运行单独授权，顺序固定为：

```text
read-only REST baseline
→ emergency dry-run
→ flat-account helper smoke
→ tiny-position stop-and-flat smoke
→ fanout=1 gateway passive IOC
→ signal-conditioned LeadLag smoke
```

在 tiny-position、gateway 或 LeadLag 授权缺失时，不得宣称 stop-and-flat live safety、gateway fillability 或 LeadLag latency 已验证。
Outer guard 被 `SIGKILL`、主机失效、网络隔离或 REST 全不可用时仍没有自动恢复保证；首次 smoke 必须有人值守，并由外部
supervisor/runbook 重复调用幂等 helper，任何无法证明 flat 的结果都禁止启动下一轮。

## 后续方向与优先级

- P1：基于官方 UTA WebSocket 预算设计 UID/account 级共享 limiter；不能直接套用 REST `10 requests/s/UID`。
- P1：官方 HA 与高速 endpoint 长期 A/B、endpoint failover 和切换 unknown window。
- P1：单 route 闭环稳定后的多 route live 与 route policy。
- P1：若目标改为不平仓恢复交易，再设计 persistent ID、REST history reconcile、unknown-window order reconstruction 和 resume gate。
- P2：`fast-fill`/`fill` 的 `execId` 去重、跨流乱序和累计 quantity 重建。
- P2：account/position private feed、通用 Gate/Bitget policy、direct backend 和更多协议能力。

扩大频率、fanout 或运行时长前必须先完成 account limiter。任何性能、稳定性或 fillability 结论都需要对应 benchmark、profile
或 live 证据，不能从组件 microbenchmark、login success 或单次 passive IOC 外推。
