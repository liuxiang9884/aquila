# Bitget HS N=6 fanout=1 24 小时实盘计划

## 目标

基于 `origin/main@08e0fdf` 启动一轮新的 46-symbol Bitget LeadLag guarded live：Bitget BBO fusion
`N=6`（3 HA + 3 HS）、Binance fusion `N=4`、order `fanout=1`、`parallel=1`，order 与 feedback
private WebSocket 全部使用 HS endpoint，计划运行 24 小时并以 REST final flat 收口。

用户已在 2026-07-22 本轮对话中明确授权：“开始，ok后启动实盘”。该授权只适用于本计划定义的 fresh run。

## 非目标

- 不启用 `parallel > 1` / fixed-slot 分支。
- 不启用 `fanout=4`。
- 不加入本地 account limiter。
- 不修改协议、订单状态机、reconcile、formatter、CPU/IRQ 系统设置或生产代码。
- 不在异常后 resume；任意 continuity/unknown/process failure 都执行 strict stop-and-flat 并保持停机。

## 冻结决策

- Commit：`08e0fdf18512ab0a710acd56da7a4280def9d8a7`。
- Instrument：冻结当前 `config/instruments/usdt_future_universe.csv`，交易范围为上一轮验证过的 46 symbols。
- Market data：Bitget 6 source（HA 3 + HS 3）与 Binance 4 source，各自只向 strategy 发布 canonical fusion。
- Trading：单 route Bitget gateway，`order_session_fanout=1`、`parallel=1`、limiter absent。
- Endpoint：order 与 feedback 均为 `vip-ws-uta-pri-a.bitget.com:443/v3/ws/private`。
- Risk：每 pair `open_notional=10 USDT`，全局 `max_gross_notional=1000 USDT`。
- Duration：`86400s`；feedback 至少覆盖 strategy duration + 300s。
- CPU：优先复用 2026-07-22 已验证的单路拓扑，但启动前重新核对实际 IRQ、空闲 core、同机进程与 affinity；不修改系统设置。

## 执行步骤

1. 在独立 worktree 完成 Release build、focused Python safety tests 和配置静态审计。
2. 使用新的 run id 与隔离目录，冻结 catalog、binary、配置、run definition 和 SHA-256。
3. 生成 46-symbol fanout=1 strategy、Bitget/Binance N=6/N=4 fusion、recorder、gateway 与 feedback 配置。
4. 验证 46/46 instrument、fee-rate、双边 BBO、freshness、source/fusion metadata、recorder 与 CPU/IRQ 状态。
5. 运行当次 read-only REST baseline 与 emergency dry-run，要求完整 allowlist flat。
6. 启动 run-specific feedback/gateway，确认 HS login/subscribed、route 0 ready，并用 `mark-applied` 绑定 PID/config/account。
7. 通过 `run_live_with_guard.py --exchange bitget` 启动 24 小时 strategy，立即检查 runtime marker、PID、CPU、endpoint 与错误关键字。
8. 默认每 10 分钟记录 PID、market freshness、route ready、signal/order/fill、错误关键字和 REST/SHM health。
9. 正常或异常结束时先证明 gateway/feedback quiescent，再以 REST 证明 final flat；随后生成 report 与证据包。

## 验证策略

- Release build 成功；本轮触及的 guard/prepare/emergency Python tests 全部通过。
- 所有生成 TOML 可解析，46-symbol/fanout/parallel/endpoint/SHM/CPU 合同一致。
- 行情与 fee preflight 使用当次查询结果，不复用历史 flat、fee、BBO 或进程状态。
- Guard 启动后必须看到 `lead_lag_live_orders_runtime_started`，且无 `ERROR`、`FATAL`、`ContinuityLost`、
  `UnknownResult`、`needs_reconcile` 或 `manual_intervention`。
- 完成条件是 guard 正常退出且 quiescence + REST final flat；异常 cleanup 成功只记为 stopped-and-flat，不记为正常完成。

## 回滚与应急

- Preflight 任一项失败：不启动 strategy；停止本轮已启动的 run-specific producer，并重新证明账户 flat。
- 运行中异常：outer guard 停止完整交易栈，按 46-symbol allowlist 幂等撤单并 reduce-only market flatten。
- 无法停止绑定进程或无法由 REST 证明 flat：返回 guard `11`，禁止下一轮，立即人工 handoff。
- 不热改 endpoint/config，不在同一 run 重启 strategy；修正后必须使用新 run id。

## 已知边界

- HS private endpoint 缺少官方稳定能力确认；HA 只保留为下一轮显式回滚入口。
- Outer guard 不能覆盖自身 `SIGKILL`、主机失效、网络完全隔离或 REST 全不可用，需要有人值守。
- 当前机器没有 full kernel isolation；性能结果必须记录实际 CPU/IRQ 与同机负载，不能外推长期稳定性或 PnL。
