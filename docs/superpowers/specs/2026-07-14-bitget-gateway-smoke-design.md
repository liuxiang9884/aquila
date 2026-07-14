# Bitget Gateway 单笔实盘 Smoke 设计

## 背景与目标

Bitget V1 已具备 strict stop-and-flat、fresh-run isolation、manifest v2、REST 保守 snapshot、进程 quiescence，以及 emergency helper 的 dry-run、空账户和最小仓位平仓证据；目前仍缺少 gateway 与 LeadLag 的 live 证据。

本设计只补齐 gateway 第一笔 live smoke。它对照 Gate `fill_probe_strategy` 的生产链路形状，使用 `OrderGatewayClient`、order feedback SHM、显式状态机和结构化事件记录，但把测试范围压缩为一次 fresh run 中的一条 route、一笔 BTCUSDT 最小数量被动 IOC。它不启动 LeadLag、不循环发单、不把 fresh run 解释为 resume，也不在同一 run 中重启任一交易进程。

## 范围与非目标

### 范围

- 合约：Bitget USDT-FUTURES `BTCUSDT`。
- 数量：交易所允许的最小开仓量，由配置显式给出并在启动前通过 instrument metadata 校验。
- route：`route_id = 0`，`fanout = 1`。
- entry：一笔被动限价 IOC，方向与价格由 smoke 配置和新鲜 BBO 决定。
- 证据：gateway response 中的 `Ack`、独立 order feedback 中的 terminal、事件 CSV、summary，以及停进程后的最终 REST snapshot。
- 成交处置：若 entry 部分或全部成交，通过同一 `OrderGatewayClient` 提交等量 `reduce_only` aggressive IOC，并等待 terminal。
- 异常处置：本地 runner 非零退出；外层 guard 停止 gateway 与 feedback 进程，等待 quiescence，再调用 Bitget emergency helper 执行保守 stop-and-flat。

### 非目标

- 不运行或修改 LeadLag 策略。
- 不移植 Gate fillability probe 的多 node、GTC/IOC 双 route 或长时间循环能力。
- 不提供 resume；每次执行必须重新生成 run directory、SHM 名称、配置和 manifest。
- 不把 gateway live smoke 的成功当作 LeadLag live 授权或证据。

## 方案选择

采用独立的 Bitget one-shot runner 与专用 guarded orchestration。runner 复用 Gate probe 已验证的订单客户端、feedback reader、local order id 关联和有界状态机模式；外层 orchestration 复用现有 Bitget fresh-run、PID/config/credentials 绑定、REST preflight/final snapshot 和 quiescence 逻辑。

不选择把 Gate `fill_probe_strategy` 泛化为跨交易所工具，因为其多 node、双 route 和 fillability 语义会扩大首笔 live smoke 的改动与风险。不选择借用 LeadLag 策略发单，因为那会跨越尚未取得的 LeadLag live 证据门。

## 组件设计

### `bitget_gateway_smoke`

新增独立 C++20 executable，建议位于 `tools/bitget/gateway_smoke/`。它只负责一次有界交易状态机：

1. 加载 smoke 配置，连接 run-specific gateway command/response SHM 与 feedback SHM。
2. 等待 gateway ready、feedback continuity ready 和新鲜、非 crossed 的 Bitget BBO。
3. 根据 side、tick size 和 BBO 计算不会主动穿价的 entry IOC 价格。
4. 只提交一次 entry，记录本地发送时间、local order id 和 gateway response。
5. 只把 gateway `Ack` 计为直连证据；随后等待匹配 local order id 的独立 feedback terminal。
6. entry 零成交且 terminal 为 `Cancelled` 时正常完成交易阶段。
7. entry 有累计成交时，提交同量、反向、`reduce_only = true` 的 aggressive IOC close，并等待 Ack 与 terminal。
8. 将所有状态转换、gateway response 和 feedback 写入事件 CSV，并原子生成 summary。

runner 不负责在未知状态下盲目重试，也不负责进程级 emergency cleanup。任何 response/feedback timeout、continuity loss、非法状态转换、close 剩余量或状态不明均返回非零，由外层 guard 接管。

### Gateway smoke guard

新增 gateway-smoke 专用 prepare/guard 入口，不放宽现有 LeadLag guard 对 `lead_lag_strategy`、strategy config 和 manifest v2 的约束。两者可以抽取并复用无策略语义的内部 helper，但 gateway smoke 使用独立 schema，例如 `aquila.bitget_gateway_smoke_manifest.v1`。

prepare 阶段生成：

- 只订阅 BTCUSDT book ticker 的 run-specific data-session config；
- run-specific gateway config；
- run-specific feedback config；
- smoke runner config；
- 唯一 command/response/feedback/BBO SHM 名称；
- manifest，记录上述配置的规范路径与 digest、credentials identity、预期 executable 和 run id。

guard 阶段执行：

1. 拒绝已有 Bitget strategy/data-session/gateway/feedback 进程或非 flat REST baseline。
2. 启动只发布本次 BBO SHM 的 data session，以及 feedback 与 gateway；绑定各自 PID、进程 start time、可执行文件、配置和 credentials。
3. 等待 data session 发布新鲜 BBO、feedback 与 gateway ready 后，只启动 manifest 指定的 `bitget_gateway_smoke`。
4. runner 退出后，无论成功或失败均先终止 data session、gateway、feedback 并确认 quiescence。
5. 调用 emergency helper；正常路径要求 REST 最终确认 open orders、plan orders 和 position 都为零。
6. 将 manifest、进程日志、runner CSV/summary、helper 输出和 REST snapshot 保存在同一 run directory。

guard 不支持在同一 run 中重启 runner、data session、gateway 或 feedback。进程提前退出、身份漂移、配置 digest 漂移或证据文件缺失均使本次 run 失败。

## 状态机与证据合同

runner 的主要状态为：

`WaitReady -> WaitBbo -> EntrySent -> EntryAcked -> EntryTerminal -> CloseSent -> CloseAcked -> CloseTerminal -> Done`

`EntryTerminal` 在累计成交为零时可直接进入 `Done`。任何阶段都可进入 `Failed`，但 `Failed` 不意味着本地已经 flat；只有外层 guard 完成进程 quiescence 和最终 REST snapshot 后，整次运行才能宣告安全收尾。

成功必须同时满足：

- fresh manifest 与运行进程绑定一致；
- entry 只提交一次；
- 已收到 entry gateway `Ack`；
- 已收到独立 entry terminal feedback；
- 若有 entry fill，close 已收到 `Ack` 和 terminal，且累计 close 数量覆盖 entry 成交数量；
- data session、gateway、feedback 已停止并 quiescent；
- 最终 REST snapshot 为 open orders = 0、plan orders = 0、position = 0。

gateway `Ack` 不能替代 independent feedback terminal，runner 的 `Done` 也不能替代最终 REST flat 证明。

## 价格与数量边界

- entry quantity 必须等于配置的最小数量，且满足 instrument metadata 的 `minTradeNum`、quantity step 和精度约束。
- entry 使用新鲜 BBO；buy 价格不得高于当前 best bid，sell 价格不得低于当前 best ask。
- aggressive close 使用当时新鲜的对手价并按 tick 对齐；不得使用 market order，也不得扩大仓位。
- close quantity 仅来源于 independent feedback 的 entry cumulative fill，且必须设置 `reduce_only`。
- BBO 过期、缺失、crossed，或 metadata 与配置不一致时不得发单。

## 失败与恢复

- runner 内部不对 entry 或 close 自动重发，以免未知状态下重复订单。
- runner failure 立即阻止任何后续 entry；guard 进入 stop-and-flat。
- guard 先停止交易进程并确认 quiescence，再运行 REST helper，避免 cleanup 与存活进程竞争。
- REST snapshot 的网络错误、解析错误或非空结果都按不安全处理，本次 run 返回失败并保留全部证据。
- 如果 helper 无法证明 flat，最终报告必须明确为 unresolved，不能宣称 smoke 成功。

## 测试与验证

实现按测试先行推进：

- 配置解析和非法 fanout/route/quantity/timeout 拒绝测试；
- 被动 entry 与 aggressive reduce-only close 的价格/数量测试；
- Ack、terminal、零成交、部分成交、全成交、乱序、重复、超时和 continuity loss 状态机测试；
- fake gateway response 与 feedback SHM 的端到端 runner 测试，证明只发一笔 entry；
- prepare/manifest digest、basename、PID identity、fresh-run 和 guard cleanup 测试；
- emergency helper 正常、异常及最终非 flat 测试；
- release build、相关 ctest、dry-run，以及 `git diff --check`。

只有自动验证通过后，才消耗用户给出的当次 BTCUSDT 最小数量 gateway live smoke 授权。实盘执行仍按本设计的一次 fresh run 进行；其证据只解除 gateway smoke 这一门，不自动进入 LeadLag live。
