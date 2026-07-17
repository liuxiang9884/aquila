# Bitget UTA Trading

本文是 Bitget UTA v3 交易链路的当前事实源，覆盖 `OrderSession`、`OrderFeedbackSession`、RTT probe、
`OrderGateway` 与 LeadLag 接入。历史 implementation plan 和完成态 spec 不再作为运行或设计入口。

## 当前范围与证据边界

截至 2026-07-16，仓库已实现：

- `OrderSession`：private WebSocket login、limit GTC/IOC place、single cancel、request correlation 和直接 operation response。
- `OrderFeedbackSession`：独立 private connection、account-wide `order` topic、累计订单生命周期事实、feedback SHM 路由和 continuity lost。
- RTT probe：单路/多路 run plan、passive IOC、Ack 与 terminal feedback 对账、CSV/metadata 输出和 safety close 状态流。
- `bitget_order_gateway`：N route worker、真实 OrderGateway SHM command/event queue、route state 和 CLI dry-run/validate-only。
- LeadLag：可以用现有 lag metadata 构造 Bitget gateway command；只接入 `order_gateway` backend。
- REST stop-and-flat：提供 UTA market/reduce-only request builder、allowlist/dedicated-account emergency helper、
  REST preflight/final check 和 Gate/Bitget 共用的 LeadLag guard orchestration。
- Fresh-run isolation：为每轮 Bitget live 生成独立 gateway/feedback SHM、runtime config 和 manifest；
  `--execute` 会在读取凭据和访问 REST 前强制验证 manifest。
- Gateway smoke：提供独立的 `bitget_gateway_smoke` one-shot runner 和外围 guard pipeline；固定
  `BTCUSDT`、`0.0001 BTC`、buy passive IOC、fanout/route count 为 `1`，并以 direct Ack、独立
  terminal feedback、必要时同 gateway reduce-only close 和最终 REST flat 组成证据闭环。
- 四路 LeadLag gateway：`config/order_gateways/bitget_order_gateway_4routes.toml` 启动四条独立 private
  OrderSession；Bitget live prepare 支持并只放行 `route_count=1/4`。四路配置要求每个 Bitget pair
  `order_session_fanout=4`，否则在生成 runtime manifest 时 fail closed。20-symbol 四路策略配置位于
  `config/strategies/lead_lag_bitget_top20_highspeed_fanout4_20260716.toml`，每个 pair 使用 lead/lag freshness
  `3ms/500ms`、`open_notional=10`。
- 用户指定的 30-symbol Binance-lead/Bitget-lag 准备配置位于
  `config/strategies/lead_lag_bitget_requested_top30_highspeed_fanout4_20260716.toml`，同样固定 freshness
  `3ms/500ms`、`open_notional=10`、`parallel=1` 和 `order_session_fanout=4`。2026-07-16 官方
  Binance USD-M `exchangeInfo` 与 Bitget UTA `instruments` 快照显示 30/30 双边存在且可交易；其中
  `SKHY/SNDK/SKHYNIX/SOXL/MU/KORU/SAMSUNG/DRAM/MRVL/EWY` 在 Binance 属于
  `TRADIFI_PERPETUAL`，其余 20 个属于普通 `PERPETUAL`。新增的 11 个 symbol 已写入
  `config/instruments/usdt_future_universe.csv`，已有 19 个 symbol 的 Binance/Bitget 基本字段也按同一快照刷新。
  原始快照、哈希、状态矩阵与 2bps/5bps ticks 派生记录位于
  `/home/liuxiang/tmp/bitget_binance_symbol_check_20260716T0544Z/`。两家 30-symbol data session 已完成短时只读连接，
  Bitget 使用 `vip-ws-uta-pub-a.bitget.com/v3/ws/public/sbe`；该证据只证明本次订阅与收包，不构成真实订单或
  30-symbol LeadLag live 证据。

真实 passive IOC 已在 dedicated account 上分别验证官方 HA endpoint 与推断的高速 private endpoint。样本均取得 Ack 与
terminal feedback 双证据，且运行结束后通过 REST 确认无 open order、无 position。该证据只覆盖 probe，不替代 gateway 或
LeadLag 证据。2026-07-14 已对 `BTCUSDT` 完成当次 read-only REST baseline、allowlist emergency dry-run 和
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
`/home/liuxiang/tmp/bitget_btcusdt_tiny_long_retry_20260714T033212Z/`。因此 tiny-position stop-and-flat 证据门已通过。

同日首次 gateway pipeline run `bitget_gateway_smoke_20260714T061416Z` 在发送订单前因 readiness waiter 读取错误的 Nova 文件名而
超时，runner 未启动、无 `order_event.csv`/`summary.json`，没有提交订单；子进程已实际退出，但未回收的 `Popen` zombie 又导致
quiescence 虚假失败。独立 PID 扫描和 production REST dry-run 证明账户 flat 后，已用自动测试覆盖并修复这两个根因。
新 fresh run `bitget_gateway_smoke_20260714T061702Z` 随后提交唯一一笔 `BTCUSDT buy 0.0001` passive IOC：feedback terminal
`cancelled/immediate_or_cancel` 先于 gateway direct Ack 到达，二者 exchange order id 一致，累计成交为 `0`，所以未触发 close；
三个绑定进程均证明停止，guard exit code 为 `0`，preflight、final 和独立 post-check REST 均为 flat。完整证据位于
`/home/liuxiang/tmp/bitget_gateway_smoke_20260714T061702Z/`。因此 fanout=1 gateway 证据门已通过。随后
`bitget_lead_lag_top20_highspeed_20260715T154837Z` 完成 20-symbol、`fanout=1/parallel=1`、10 小时
signal-conditioned LeadLag：644 个 signal、211 个 submitted order、21 个成交 entry 与 21 个完整 exit，
无 unknown/continuity/reconcile，quiescence 与 final REST flat 均通过。该 run 的实际净 PnL 为
`-0.03536520 USDT`，详细证据见
`reports/bitget_lead_lag_top20_highspeed_20260715T154837Z/analysis_report.md`。这些结果不能外推到四路
fanout、未来 fillability、长期稳定性或相对 endpoint 收益；四路当前只有代码、自动测试和 CLI validate-only 证据。

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

## 手续费配置与 PnL 证据

`lag_taker_fee` 是账户、product type 和 symbol 相关的运行配置，不属于 instrument metadata，不能从 catalog 或公共
contract 默认费率推断。2026-07-17 通过当前 UTA 账户的
`GET /api/v3/account/all-fee-rate?category=USDT-FUTURES` 更新
`config/strategies/lead_lag_bitget_requested_top30_highspeed_fanout4_20260716.toml`：

- 10 个 symbol：maker `-0.00005`、taker `0.000065`；
- 3 个 symbol：maker `-0.00005`、taker `0.00015`；
- 17 个 symbol：maker `-0.0001`、taker `0.0002`。

逐 symbol 映射以该 config 和 `lead_lag_config_test` 为可提交事实源。当前策略只消费 `lag_taker_fee`；maker rebate
未进入阈值或 PnL 模型。若未来订单可能作为 maker 成交，必须先扩展 fee contract，并按实际 `tradeScope` 选择费率。

本轮 14 小时 run 的冻结配置仍是当时的统一 `0.00015`，不得修改或用新配置重解释。对该 run 时间窗读取 UTA
`GET /api/v3/trade/fills` 得到 50 条逐笔 fill，全部为 `tradeScope=taker`；实际 `fee / execValue` 验证了成交
symbol 对应的 `0.000065` 或 `0.0002`。历史和最终 PnL 对账必须使用 REST fill 的实际 `feeDetail`，不能使用配置费率
替代。

每次新 live run 冻结输入前都必须重新读取账户 all-fee-rate，并逐 pair 对比 `lag_taker_fee`；任一缺失或不一致都应阻断
启动并先刷新配置。费率可能随账户等级、symbol 活动或产品属性变化，本次值不是永久 contract。

## OrderGateway 与 LeadLag 接入

`MultiOrderSessionGateway` 管理 route readiness 和 route selection；`OrderGatewayWorker` 独占一条 command/event queue 和一个
`OrderSession`。Gateway SHM 使用现有通用 command/event ABI，不包含 Bitget numeric error code、connection id 或额外诊断字段。

LeadLag 根据 lag instrument metadata 选择 exchange/symbol，当前 Bitget 只允许 `strategy.order_backend = "order_gateway"`。
Direct `strategy.order_session` 装配仍是 Gate-only。Gateway N route 已通过 gtest、真实 SHM integration、CLI dry-run/
validate-only、Debug/Release regression 和通用 OrderGateway benchmark；历史首个 live gateway 固定为 `fanout=1`。

四路 entry 沿用 Gate fanout contract：一个 signal 在同一 execution group 内向四条 ready route 各发送一张 full-size
child，四张各自拥有 `local_order_id/route_id`，整个 group 只占一个 `parallel` slot。Entry quantity 先按实际 order price、
`open_notional` 和 quantity step 计算；结果低于 instrument `min_quantity` 时直接使用最小量，高于最小量时保留计算结果，
不做最小量相等性拒绝。Risk reservation 按实际 ready route 数乘以单 child 暴露。Normal close、stoploss 和 retry 仍向
每条 ready route 发送当前 group 已知总仓位的 full-size `reduce-only` child，由交易所净仓限制防止反向开仓。

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
config/order_gateways/bitget_order_gateway_4routes.toml
config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml
config/data_sessions/bitget_gateway_smoke.toml
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
tools/bitget/gateway_smoke/
tools/bitget/order_session_rtt_probe/
tools/lead_lag/live_strategy.h
scripts/bitget/trading/place_futures_order.py
scripts/bitget/trading/emergency_flatten_futures.py
scripts/bitget/trading/prepare_gateway_smoke_run.py
scripts/bitget/trading/run_gateway_smoke_with_guard.py
scripts/lead_lag/prepare_bitget_live_run.py
scripts/lead_lag/run_live_with_guard.py
```

凭据默认从 `BITGET_TEST_KEY`、`BITGET_TEST_SECRET`、`BITGET_TEST_PASSPHRASE` 读取。CLI 参数传的是环境变量名，
不得把 secret 写入配置、命令历史、文档或 log。

常用只读验证：

```bash
./build/debug/tools/bitget_order_gateway --config config/order_gateways/bitget_order_gateway.toml
./build/debug/tools/bitget_order_gateway --config config/order_gateways/bitget_order_gateway.toml --validate-only
./build/debug/tools/bitget_order_gateway \
  --config config/order_gateways/bitget_order_gateway_4routes.toml --validate-only
./build/debug/tools/bitget_order_session_rtt_probe \
  --config config/order_session_rtt_probe/bitget_order_session_rtt_probe.toml
ctest --test-dir build/debug -R '^bitget_(order|operation)' --output-on-failure
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/prepare_gateway_smoke_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/run_gateway_smoke_with_guard_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

所有真实订单都需要用户对当次运行单独授权；dry-run、login、subscription 或 validate-only 不构成下单授权。

### Fanout=1 gateway passive IOC smoke

`run_gateway_smoke_with_guard.py` 是当前 gateway 证据门的唯一编排入口。它拒绝缺少 `--execute` 的调用，先扫描并拒绝
仍在运行的 LeadLag / Bitget trading 进程，再生成 `/home/liuxiang/tmp/<run_id>/configs/` 下的四份 overlay 和
`bitget_gateway_smoke_manifest.json`。每轮必须使用从未出现过的 `run_id`；目录已存在即 fail closed，不存在 resume、覆盖或
同一 run 重启 runner / data session / gateway / feedback 的语义。

流水线固定执行：生产 REST flat preflight → 启动并绑定单 symbol data session、account-wide feedback 和 fanout=1 gateway →
等待 feedback subscription Ack → one-shot runner 提交一次 `buy 0.0001 BTC` passive IOC → 等待 gateway direct Ack 与独立
terminal feedback → 若累计成交量非零，则通过同一 gateway 提交等量 reduce-only aggressive IOC close，并再次等待
Ack+terminal → 停止并证明三个 producer quiescent → 生产 REST final flat。任何 `UnknownResult`、`ContinuityLost`、明确拒绝、
超时、数量不变量失败、进程身份变化或无法证明 quiescent 都停止本轮；启动后的异常只允许外围 helper 做 stop-and-flat，不能在
同一 run 重发 entry。

Release binary 和凭据就绪、无冲突进程且已取得当次授权后，从仓库根目录运行：

```bash
run_id="bitget_gateway_smoke_$(date -u +%Y%m%dT%H%M%SZ)"
/home/liuxiang/dev/pyenv/lx/bin/python \
  scripts/bitget/trading/run_gateway_smoke_with_guard.py \
  --run-id "${run_id}" \
  --execute --pretty
```

若 binary 不在默认 `build/release/tools/`，必须通过 `--data-session-binary`、`--gateway-binary`、
`--feedback-binary` 和 `--smoke-binary` 显式传入四个绝对路径。完整证据位于 `/home/liuxiang/tmp/<run_id>/`：
`order_event.csv` 保存 gateway response 与 feedback 事件，runner 以原子替换写 `summary.json`，外围 guard 以原子替换写
`guard_summary.json`，另有 manifest、四份 runtime config 和各进程日志。Runner exit `0` 后，pipeline 还会复核 CSV/summary 的
`run_id`、唯一 entry、Ack、terminal、最小数量及可选 close 双证据；文件缺失或合同不一致按异常路径 quiesce 并 stop-and-flat。
`summary.json` 的 runner success 只证明订单状态机闭环；只有 `guard_summary.json` 同时记录正常 runner 退出、三个进程 quiescent
和 final REST flat，才算 gateway smoke 通过。

该 smoke 只验证一个最小量 gateway order 的安全闭环，不是 signal-conditioned LeadLag 证据，也不能外推多 route、长期稳定性、
fillability 或 latency。即使 gateway 门通过，LeadLag live 仍需按下一证据门重新授权和执行。

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
和 `bitget_live_manifest.json`，并逐 route 把 gateway 的 `order_session_config` 与 strategy 的 LeadLag config 改写为绝对路径；
该命令不连接交易所、不读取账户、不创建 SHM。Operator 确认 feedback 与 manifest 对应 gateway
确实使用 manifest 中配置后，以两个实际 PID 执行 `mark-applied --gateway-pid <pid> --feedback-pid <pid>`。manifest schema v2
会重新读取三个 TOML，并复核路径、SHM、`route_count=1/4`、逐 route 三字段凭据、category、position/margin mode 和 private
WebSocket endpoint 一致性；四路时还要求所有 Bitget pair 的 `order_session_fanout=4`。同时从 `/proc` 绑定 PID、start time、
executable、`--connect` 与实际绝对 `--config`。
进程 credential 值只在内存中比较，不写入 manifest、summary 或 log。Guard 的 Bitget `--execute` 还会再次验证：

- manifest schema、run directory、strategy command 的 `--config` 和 `run_id`；
- gateway/feedback SHM 都包含本轮 `run_id`，strategy/gateway/feedback 指向完全一致；
- `external_configs_applied=true`，manifest `route_count` 与 gateway route table 一致；gateway/feedback 的
  PID/start time/executable/config 未变化，且 guard
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
→ fanout=1 signal-conditioned LeadLag
→ fanout=4 staged LeadLag
```

截至 2026-07-16，tiny-position stop-and-flat、fanout=1 gateway 和 fanout=1 signal-conditioned LeadLag 已有上述当次证据；
fanout=4 尚无真实订单证据。任一对应证据门缺失时，不得宣称其 live safety、fillability 或 latency 已验证；单路结果不能
替代四路证据。
Outer guard 被 `SIGKILL`、主机失效、网络隔离或 REST 全不可用时仍没有自动恢复保证；首次 smoke 必须有人值守，并由外部
supervisor/runbook 重复调用幂等 helper，任何无法证明 flat 的结果都禁止启动下一轮。

## 后续方向与优先级

- P1：基于官方 UTA WebSocket 预算设计 UID/account 级共享 limiter；不能直接套用 REST `10 requests/s/UID`。
- P1：官方 HA 与高速 endpoint 长期 A/B、endpoint failover 和切换 unknown window。
- P1：按新鲜授权执行 fanout=4 staged live，先证明四 route ready、每 child 最小量、Ack/terminal 归组、reduce-only
  收敛、quiescence 和 final flat，再讨论 route policy。使用 30-symbol 准备配置时，真实启动前还要重新确认上述 10 个
  `TRADIFI_PERPETUAL` 的当时交易时段、双边 BBO 与 freshness，不能把 catalog 的 `TRADING/online` 当成 24x7 可交易保证。
- P1：若目标改为不平仓恢复交易，再设计 persistent ID、REST history reconcile、unknown-window order reconstruction 和 resume gate。
- P2：`fast-fill`/`fill` 的 `execId` 去重、跨流乱序和累计 quantity 重建。
- P2：补齐交易所侧订单时间戳与延迟证据：保留 WebSocket place response 的 `args[].cTime` 和顶层 `ts`，保留
  private `order` push 的 `createdTime`、`updatedTime` 和顶层 `ts`，并在完成 `fast-fill`/`fill` 去重与乱序 contract 后记录
  `execTime`。Report 必须区分本地 Ack RTT、交易所订单创建/response、terminal update 和实际 fill 时间；本地 send/receive 与
  交易所时间跨时钟，未完成 clock-offset 校准时不得解释为单程网络时延。REST 的
  `X-BG-REQUEST-ACCEPT-TIME`/`X-BG-RESPONSE-COMPLETE-TIME` 只适用于 REST 链路，不能替代当前 WebSocket 下单证据；SBE BBO
  `ts`/`sts` 只描述行情链路。Bitget 当前没有为零成交 IOC 提供文档明确的 order-ingress 或 match-attempt 时间戳，因此即使补齐
  上述字段，也只能改善阶段边界和成交单分析，不能精确拆分未成交订单的撮合处理时间。
- P2：account/position private feed、通用 Gate/Bitget policy、direct backend 和更多协议能力。

扩大频率、fanout 或运行时长前必须先完成 account limiter。任何性能、稳定性或 fillability 结论都需要对应 benchmark、profile
或 live 证据，不能从组件 microbenchmark、login success 或单次 passive IOC 外推。
