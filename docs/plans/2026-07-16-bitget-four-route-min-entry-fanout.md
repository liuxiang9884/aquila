# Bitget 四路最小开仓量 fanout 实施计划

## 目标

在保留 Bitget 历史 `fanout=1` gateway smoke 入口的前提下，为 LeadLag guarded live 增加四条独立
OrderSession route。每次 entry signal 向四条 ready route 各提交一张 full-size child order，并在提交第一张
child 前证明该 child quantity 严格等于对应 Bitget instrument 的 `min_quantity`；无法证明时整组 fail closed。

## 非目标

- 本计划不启动真实订单，不复用或修改历史 run artifact，也不授权下一轮 live run。
- 不改变 `parallel`、signal、freshness、drift guard、open/close 价格或 IOC/GTC contract。
- 不把四路连接解释为 `parallel=4`；四张 child 仍属于同一个 execution group，只占一个 parallel slot。
- 不实现账号级 submit/cancel/pending limiter，不声明 fillability、PnL 或 latency 改善。
- 不改变 Gate 既有配置和行为。

## 已锁定决策

- 沿用 Gate fanout contract：entry child 数量相同；已知成交量在 execution group 内汇总。
- `order_session_fanout=4` 时，entry risk reservation 和 gross-notional 检查使用四张 child 的合计暴露。
- normal close、stoploss 和 retry 沿用 Gate 行为：每条 ready route 都发送当前 group 已知总仓位数量的
  `reduce-only` child，不强制为 `min_quantity`。
- 增加显式 `require_min_entry_quantity` 配置，默认 `false`；Bitget 四路 live params 必须设置为 `true`。
- 保留现有单路 `config/order_gateways/bitget_order_gateway.toml` 供历史 gateway smoke 使用；新增四路
  LeadLag gateway 配置，四条 route 使用同一 credential、trading contract 和 high-speed private endpoint。
- Bitget live manifest 记录实际 `route_count`，并校验 gateway route 数、每路 OrderSession contract、credential、
  strategy fanout 和最小 entry quantity contract 一致。旧 manifest 只作为历史 artifact，不允许新 run resume。
- 按用户此前明确指示复用当前专用 branch，不创建额外 worktree；已有 `core/market_data/types.h` 改动不属于
  本任务，不触碰、不暂存、不提交。

## 影响边界

- LeadLag config / submit gate：`strategy/lead_lag/config.*`、`strategy/lead_lag/signal.h`、
  `strategy/lead_lag/strategy.h` 和对应 tests。
- Bitget live isolation：`scripts/lead_lag/prepare_bitget_live_run.py` 及 Python tests。
- 四路 source config：`config/order_gateways/`；实际 run-specific config 仍由 prepare pipeline 写入
  `/home/liuxiang/tmp/<run_id>/configs/`。
- 文档：`docs/bitget_trading.md`、`docs/lead_lag_live_operations.md`、`docs/runtime_cpu_allocation.md`、
  `docs/diagnostic_fields.md` 和 onboarding 摘要。

## 实施步骤

1. 先增加失败测试，证明当前 LeadLag 无法表达“entry 必须等于 min quantity”，并证明 Bitget prepare
   当前拒绝四路 gateway。
2. 增加 `require_min_entry_quantity` parse 和 submit-time equality gate；在 route selection 和第一张 child
   提交之前拒绝非最小 entry，不影响 reduce-only 路径。
3. 将 Bitget live prepare 从硬编码 `route_count=1` 改为绑定 source gateway 的实际 route 数；逐 route
   固化绝对 OrderSession config path，并校验所有 route 与 feedback 使用相同 credential 和 trading contract。
4. 增加四路 high-speed gateway source config；prepare 时要求所有 Bitget pair 的
   `order_session_fanout=route_count` 且 `require_min_entry_quantity=true`。
5. 更新长期领域文档，完成独立 diff/error-path review，运行 focused C++/Python tests、Release build、相关
   regression、CLI validate-only、`git diff --check` 和 evaluation 边界检查。
6. 将已完成 contract 迁移到领域文档后删除本计划，按最小闭环原子提交，push 当前 branch 并创建中文 PR。

## 验证策略

- C++：LeadLag config 与 strategy interface tests 覆盖 flag 默认值、显式开启、四路最小量提交、非最小量
  整组拒绝、reduce-only 不受 entry gate 影响，以及四路 risk aggregation。
- Python：prepare tests 覆盖四路生成、manifest route count、逐 route 绝对路径、route count/array mismatch、
  credential/contract mismatch、fanout mismatch 和缺少 min-entry contract。
- CLI：四路 Bitget gateway `--validate-only`；LeadLag strategy config/mode validation，不连接交易所、不读取凭据。
- 回归：Bitget gateway/feedback、OrderGateway core、LeadLag feedback/strategy、guard/prepare fixture suites。
- 静态边界：`git diff --check`；evaluation include/link 检查期望无命中。

## 回滚方案

- 新行为由四路 gateway source config、`order_session_fanout=4` 和
  `require_min_entry_quantity=true` 共同启用；Gate 和未启用 flag 的旧策略保持原行为。
- 删除四路 source config并回滚 manifest/strategy commits即可恢复单路 prepare；历史 run 不 resume。
- 任一 live config、route readiness、feedback continuity、REST baseline 或 final flat 证据失败时保持停机，
  不通过降级 route 数在同一 run 继续。

## 未消除风险

- 四张 entry child 都可能成交，单 signal 最大持仓是四倍 `min_quantity`；`min_quantity` 只限制单 child，
  不限制长时间累计请求量。
- Bitget `one_way_mode` 按 symbol 维护净仓，策略 fanout group 按 child 汇总已知成交；unknown result、feedback
  continuity loss 或账户事实不一致仍必须 strict stop-and-flat / handoff。
- 四条 private WebSocket 共用同一 UID；账号级 limiter 当前仍未实现，扩大运行时长、signal 频率或
  `parallel` 前仍需重新评估。
- 四路 latency、fillability 和连接独立性只能由后续单独授权的 staged live evidence 证明。
