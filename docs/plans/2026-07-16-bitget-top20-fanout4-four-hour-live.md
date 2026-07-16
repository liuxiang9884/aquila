# Bitget Top20 Fanout=4 四小时实盘计划

## 目标

使用 20-symbol Bitget LeadLag 配置执行一次 4 小时真实订单运行：`max_lead_freshness_ms=3`、
`max_lag_freshness_ms=500`、`open_notional=10`、`order_session_fanout=4`，每张 child 使用策略计算量与
instrument `min_quantity` 的较大值。

## 非目标

- 不修改信号、`parallel`、订单状态机、恢复或 stop-and-flat 语义。
- 不在同一 `run_id` 重启 strategy、gateway 或 feedback。
- 不把本次结果外推为长期稳定性、收益或 endpoint 优势。

## 边界与授权

- 用户已明确授权本次 4 小时、20-symbol、fanout=4 真实订单运行。
- 用户此前明确要求本轮不因未实现的 account limiter 或 `parallel` 停止；该偏离当前 runbook 的风险由用户接受，
  但不移除 fresh-run isolation、四 route readiness、REST baseline、异常 stop-and-flat、quiescence 和 final flat。
- 这是首次 fanout=4 live 证据；任何 route failure、`UnknownResult`、`ContinuityLost`、unresolved order 或进程异常都终止本轮，
  不自动恢复。

## 执行步骤

1. 将 checked-in 20-symbol 配置的 lag freshness 从 200ms 改为 500ms，并更新配置测试。
2. 完成 focused test、Release build、live tooling Python 回归，原子提交并同步 feature/main。
3. 创建唯一 run directory，生成 run-specific strategy/gateway/feedback 配置和 manifest。
4. 启动 feedback 与四路 gateway，证明 subscription ready、route 0..3 ready，并执行 `mark-applied`。
5. 对 20 symbols 完成当次 production REST baseline 和 emergency dry-run，再启动 guarded 14400 秒 live。
6. 每 10 分钟检查进程、四 route、行情 freshness、signal/order/terminal 与错误；异常立即停止。
7. 正常或异常退出后核对 quiescence、final REST flat、未决订单和持仓，随后生成 report。

## 验证

- checked-in config：20 pairs 全部 `3ms/500ms`、`open_notional=10`、fanout=4。
- focused/full C++ tests 与 LeadLag/Bitget Python live tooling tests。
- runtime manifest schema v2、PID/start-time/config/account binding、四 route ready。
- guard result、quiescence 和完整分页 REST `open orders → positions → open orders` final flat。

## 回滚与剩余风险

- 配置提交可回滚至 lag freshness 200ms；运行一旦开始只允许停止并执行 stop-and-flat，不能回滚外部订单事实。
- account limiter 未实现，四路会把每个 execution group 的 submit/cancel 数量放大到四倍；信号量低不能证明账户级请求预算安全。
- Outer guard 被 `SIGKILL`、主机失效或 REST 全不可用时仍需人工 handoff。
