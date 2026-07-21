# LeadLag parallel fixed slot 与 SHM v4 实施计划

## 目标

- 基于当前 `main` 重建 `parallel > 1` 基础设施，不 cherry-pick 旧实现分支。
- 将每个 pair 的 execution group 容器迁移为 `FixedOrderedSlotPool<ExecutionGroup, 16>`。
- 使用 `(symbol_id, group_id)` 作为稳定 execution group identity；`group_index` 仅用于进程内 O(1) slot 定位。
- 将 order gateway SHM 升级为 v4，并让 Gate、Bitget 及相关工具端到端传播 `group_id`。
- 将当前 LeadLag 日志和 report schema 迁移到新 `group_id` 语义，不兼容旧 `parent_id` 证据。

## 非目标

- 不修改任何现有 live config 的 `parallel=1`。
- 不启动真实订单、replay 或 guarded live 测试。
- 不重新加入 account limiter，不修改 order fanout 语义。
- 不把 `group_index` 作为 SHM、日志或 report 的稳定 join key。
- 不复用或激活历史 Superpowers 流程。

## 已锁定决策

- 新 branch/worktree 以 `main@83c5e12` 为基线，旧 `feature/lead-lag-fixed-slot-group-metadata` 只作参考。
- `execute.parallel` 只允许 `1..16`；配置解析和 runtime 初始化都 fail fast，禁止静默 clamp。
- slot 复用后若 `group_index/group_id` 不匹配，禁止扫描 fallback 和错误应用；pair 进入 `needs_reconcile`，暂停新开仓并保留退出能力。
- SHM v4 同时覆盖 Gate、Bitget、gateway smoke/fill probe、测试和诊断消费者。
- `group_id` 跨 symbol 使用时必须与 `symbol_id` 组合；跨运行证据还需 run/session identity。
- report/parser 只支持新 schema；PR #11 后续基于新 contract 单独调整。

## 影响边界

- Core order contract、OrderManager 和 order gateway SHM command/event。
- Gate/Bitget order session、runtime adapter、gateway worker 与 smoke/fill-probe 工具。
- LeadLag config、execution state、signal/strategy order flow、recovery 与 test hooks。
- LeadLag 诊断日志、order detail/report 脚本、CSV schema 和字段文档。
- fixed-slot 与 submit/feedback 热路径 benchmark。

## 实施阶段

1. 以失败测试锁定 core `group_id` contract、SHM v4 layout，以及 Gate/Bitget 双交易所传播。
2. 实现 core/SHM v4 与所有生产消费者，运行双交易所 focused tests 后形成原子提交。
3. 以失败测试锁定 `parallel=16`、`parallel=17` 拒绝、slot reuse、multi-group terminal feedback 和 mismatch reconcile。
4. 将 `ExecutionState` 迁移到 fixed slot，并把本地 `group_id/group_index` 写入订单；验证 close、retry、unknown result 和 continuity-loss 行为后形成原子提交。
5. 迁移新日志/report schema 和诊断文档，不保留 `parent_id` parser 兼容；运行 Python tests 后形成原子提交。
6. 完成跨模块 review、全量 focused regression、`git diff --check` 与 evaluation 边界检查。
7. 运行 fresh group-container、submit 与 feedback benchmark；只按实测结果陈述性能。
8. 更新领域事实源，移除完成态计划，push branch 并创建中文 PR。

## 验证策略

- Core/SHM：layout/version、place/cancel、route propagation、Gate/Bitget worker/session/adapter tests。
- LeadLag：config、feedback state、signal、strategy interface、parallel × fanout、slot reuse、unknown/reconcile、stop-and-flat 相关回归。
- Scripts：order detail、live report、diagnostic field/schema tests。
- 性能：固定 CPU 运行 fixed-slot container、submit breakdown 和 terminal feedback benchmark；不使用不可用的 system `perf`。
- 边界：确认所有现有 live config 仍为 `parallel=1`，且没有真实订单操作。

## 回滚

- 每个阶段保持原子提交；若 SHM v4、状态机或 report 阶段失败，可在 branch 上逐提交回退。
- 合并/部署时 SHM producer 与 consumer 必须一起升级并重启；v3/v4 不允许混跑。
- main 在 PR 合并前不受影响；未通过完整验证时不创建合并结论。

## 未决风险

- SHM v4 layout 对 queue slot size 和 cache footprint 的影响需用 size assertions 与 benchmark 验证。
- `parallel > 1` 的收益、资金占用和尾部风险尚无 live 证据，本计划只交付基础设施。
- PR #11 与新 report schema 存在后续 rebase/适配工作，不纳入本实现闭环。
