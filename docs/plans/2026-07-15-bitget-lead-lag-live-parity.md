# Bitget LeadLag Gate 等价实盘适配计划

## 目标

在不改变 Gate 现有行为的前提下，为首次 Bitget `BTCUSDT` LeadLag live smoke 补齐与 Gate 等价的策略限制、参数生成、4-source fastest-route 行情、fresh-run 隔离、证据校验和 report 能力。完成后只代表代码与非下单验证就绪；真实订单仍须按 runbook 对具体 run 单独授权。

## 非目标

- 不执行真实订单，不复用历史 run，也不实现同一 run 的 strategy resume/restart。
- 不扩大到多 symbol、`fanout>1`、无人值守长跑、账号级 limiter 或持仓恢复交易。
- 不声称 fillability、PnL 或 latency 收益；这些结论只接受对应 live evidence。
- 不修改 Gate 的协议、配置常量或既有 live 证据语义。

## 已锁定决策

- 首次范围固定为 `BTC_USDT`、Bitget lag、`fanout=1`、最长一小时且有人值守。
- Binance lead 与 Bitget lag 各使用 4-source fastest-route fusion；每轮使用独立 config、SHM、日志和 metadata 产物。
- `max_lead_freshness_ms=3`、`max_lag_freshness_ms=200`；preflight 只验证，不自动放宽。
- 全局 `max_entry_groups=1`。第一组 entry 进入提交阶段后，本轮永久拒绝新 entry；feedback、normal close、stoploss、close retry 和外围 stop-and-flat 继续工作。唯一 group 终结且本地 flat 后允许策略提前退出。
- Bitget smoke 显式配置 `require_min_entry_quantity=true`；entry 数量必须恰好等于当次 Bitget metadata 的最小合法 quantity，无法证明时 fail closed。该开关默认关闭，避免改变 Gate 和旧配置行为。
- fee、instrument metadata、价格和派生的 notional/slippage/risk 必须来自当次 read-only snapshot，不复制 Gate 的交易所相关值。
- manifest 绑定 gateway、feedback、Binance 4 个 source、Bitget 4 个 source和两侧 fusion 的 PID、启动时间、binary、绝对 config 与 config 摘要；结束后全部 quiescent 才允许 final REST flat 证明。
- report 显式记录 `order_exchange`，仅在 Gate 数据存在时输出 Gate 专属 `x_in/x_out` 分解；Bitget 使用自身 Ack/feedback lifecycle 字段。

## 影响边界

- 策略：`strategy/lead_lag/config.*`、`strategy/lead_lag/strategy.h` 及对应 C++ tests。
- Bitget read-only preflight：`scripts/bitget/account/query_bitget_account.py` 与测试。
- live 运行准备与隔离：`scripts/lead_lag/prepare_bitget_live_run.py`、`scripts/lead_lag/run_live_with_guard.py`、source config templates 与 Python tests。
- report：`scripts/lead_lag/analyze_order_detail.py`、`scripts/lead_lag/generate_live_report.py`、CSV fixture tests。
- 文档：`docs/bitget_trading.md`、`docs/lead_lag_live_operations.md`、`docs/diagnostic_fields.md`、report schema、runtime CPU allocation 和 onboarding 摘要。

## 实施步骤

1. 先增加失败测试，证明当前配置无法表达 `max_entry_groups` / `require_min_entry_quantity`，且第二个 entry group 或非最小 entry 仍会被提交；随后实现全局一次性 entry-group budget、最小量 submit-time 校验、稳定拒绝原因和本地 flat 提前停止。
2. 增加 Bitget fee-rate read-only request/response contract 与 fixture tests；增加当次 snapshot 校验和参数 overlay 生成，强制 freshness `3/200`、`fanout=1`、最小 quantity、风险上限及 snapshot 时效。
3. 从 BTC-only source templates 生成 run-specific Binance/Bitget 4 路 data session、两侧 fusion、strategy data reader、LeadLag params、strategy/gateway/feedback overlays；所有路径、SHM、metadata 和 log 都落入 `/home/liuxiang/tmp/<run_id>/`。
4. 将十个行情进程加入 manifest v3 的 mark/validate/quiescence contract；凭据一致性仅校验 gateway/feedback，所有进程统一校验身份、绝对 config 和 fresh-run 归属。
5. 先以 Bitget fixture 证明现有 analyzer/report 丢失 exchange 身份或错误输出 Gate 专属字段，再实现 exchange-aware CSV 与条件报告。
6. 完成独立阶段 review、Release build、focused C++/Python tests、相关完整回归、CLI validate-only、`git diff --check` 和 evaluation 边界检查。
7. 将长期 contract 迁移到领域文档，删除本完成态计划后提交最终文档；push branch 并创建中文 PR。

## 验证策略

- C++：LeadLag config、strategy interface、feedback/runtime 及相关 trading tests。
- Python：account query、prepare manifest、guard quiescence、order analyzer、live report 全部 fixture suites。
- CLI：fresh run prepare、manifest 未绑定/错 PID/错 config/旧 SHM/缺 snapshot/过期 snapshot/非最小量均应 fail closed；不带 `--execute` 的 validate-only 流程不得读取交易凭据或发送订单。
- Build：fresh Release 构建必须产出 `lead_lag_strategy`、`bitget_data_session`、`binance_data_session`、两侧 fusion、`bitget_order_gateway` 和 `bitget_order_feedback_session`。
- 静态边界：`git diff --check`；`rg '#include "evaluation/' core exchange tools` 与 `rg 'aquila_evaluation' core exchange tools` 期望无命中。

## 回滚方案

- 所有新增 live 行为由显式 Bitget source config、manifest v3、非零 `max_entry_groups` 和 `require_min_entry_quantity=true` 启用；Gate 与旧配置保持默认不限 group、无需最小量相等的兼容行为。
- 每个实现单元独立提交，可按提交回滚。任何 manifest、snapshot、进程身份或 final REST 证据不一致时保持停机，不尝试 resume。
- 若 live 后出现异常，先按既有 strict stop-and-flat 取得 flat 或人工 handoff；不得通过回滚代码在同一 run 恢复开仓。

## 未消除风险

- Outer guard 被 `SIGKILL`、主机失效、网络隔离或 REST 全不可用仍需要人工 handoff。
- 首次 run 可能没有合格 signal 或只有零成交 terminal；前者不通过 signal-conditioned 证据门，后者只证明 signal-to-order-to-feedback，不证明 fillability 或持仓闭环。
- 当前账号级 submit/cancel/pending limiter 尚未实现，因此本计划严格限制为单 symbol、单 route、单 entry group 和一小时；扩大范围前仍被 limiter 阻断。
- 运行时价格变化可能使基于 snapshot 的 notional 不再只对应最小 quantity；策略提交前必须再次用实际 order price 验证 quantity 恰为最小值，否则拒绝 entry。
