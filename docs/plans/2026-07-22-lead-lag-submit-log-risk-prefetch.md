# LeadLag submit log 与 risk prefetch 优化计划

## 目标

在不改变订单、风控、recovery、reconcile 和 report 重建语义的前提下，降低 Bitget
46-symbol、`fanout=1` 场景的 `signal_decision_ns -> request timestamp` cold P50/P95。

本分支基于 PR #14 的 `perf/lead-lag-cold-submit-breakdown`，作为 stacked change 使用其
cold / warm / endpoint-only benchmark。最终只接受完整 endpoint 有稳定收益的候选。

## 非目标

- 不改变 signal、价格、数量、route、fixed risk slot、global risk limit 或订单状态机行为。
- 不引入周期性 dummy submit、人工生产预热或真实订单。
- 不删除 `lead_lag_signal_triggered`、`lead_lag_order_submitted` 或任何
  `lead_lag_order_intent_rejected` 事件。
- 不在本轮改为增量维护 global risk totals；不修改 gateway / OrderPool 结构。
- 不修改系统 `kernel.perf_event_paranoid` 或其他性能相关系统设置。

## 已锁定决策

- 成功提交路径删除重复的 `lead_lag_order_intent` INFO；当前 report parser 不消费该事件。
- 保留所有成功结果、拒绝、恢复和对账事实源。
- prefetch 只在真实 signal 已 triggered 后执行，只针对明确数据对象；每个候选独立 A/B。
- 先处理重复日志，再测试 global risk cold prefetch；fixed slot 和 gateway 不在本轮优化。
- 采用既有门槛：至少 4/5 paired groups 同向；收益超过 `2 × baseline MAD`，并满足
  `>=2%` 或 `>=5 cycles`；P95、行为和 report 不回归。

## 影响边界

- `strategy/lead_lag/strategy.h`：successful pre-submit telemetry 与可选 risk prefetch。
- `strategy/lead_lag/strategy_test_hooks.h`、strategy tests：同步 benchmark-only stage 语义。
- `benchmark/strategy/lead_lag_submit_breakdown_benchmark.cpp`：保持 counter 起止点准确。
- `docs/diagnostic_fields.md`、`docs/lead_lag_latency_analysis.md`：同步字段和 fresh 证据。

## 实施步骤

1. 先新增或调整测试，使“成功 submit 不输出 order-intent、submitted 仍输出、rejected 仍输出”
   在旧实现上失败。
2. 最小删除 successful `lead_lag_order_intent` 调用，同步 test-hook / benchmark stage 命名；跑
   strategy focused tests 和 report parser tests。
3. 在固定 CPU 16 上对 parent baseline 与 candidate 做五组 paired cold endpoint A/B，每组
   1,024 samples；不通过门槛则撤销日志候选。
4. 以通过后的日志版本为新 baseline，依次筛选：
   - triggered 后预取 `reserved_open_risk_slot_bits_` / `order_risk_slots_`；
   - `CurrentGlobalRiskTotals()` 内对后续 `initialized_pair_runtimes_` 做 bounded pipeline
     prefetch。
5. 每个 prefetch 候选单独构建、五组 paired cold endpoint / risk-stage A/B；只保留通过者，
   局部分段改善但 endpoint 不改善时撤销。
6. 做最终 diff / error-path review、focused build/test、report fixture 回归、格式与边界检查，
   迁移有效结论并删除本完成态计划。

## 验证

- Release build：`lead_lag_submit_breakdown_benchmark`、`lead_lag_strategy_interface_test`。
- Strategy：successful submit、rejected intent、fanout、global risk、fixed risk slot focused tests。
- Report：`generate_live_report` / `analyze_order_detail` 相关 tests，确认缺少 successful
  `lead_lag_order_intent` 不改变 CSV/report。
- Benchmark：五组 paired、固定 CPU、每组 1,024 samples；比较完整 endpoint P50/P95，stage
  counter 只用于归因。
- `clang-format --dry-run --Werror`、`git diff --check`、evaluation 边界检查。

## 回滚

日志删除和每个 prefetch 候选分别形成原子 commit。任一候选不满足行为或性能门槛时，只撤销
该候选，不保留无收益复杂度；PR #14 benchmark 不受影响。

## 风险

- `__builtin_prefetch` 是 hint，可能因额外指令、cache pollution 或 CPU 频率变化而回退。
- stage timestamp 会扰动总路径，性能接受只看 endpoint-only paired 结果。
- 本机无 PMU cache-miss 证据；只能声明 fresh A/B 的路径收益，不能声明具体 cache 层级。
- 删除 pre-submit intent 会失去一个 crash-window 路标；submitted / rejected / gateway / feedback
  事实源必须经回归确认足以重建运行结果。
