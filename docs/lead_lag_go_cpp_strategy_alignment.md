# LeadLag Go / C++ 策略实现差异说明

本文单独记录 LeadLag Go reference 与当前 C++ 实现的行为差异，并说明当前 C++ 是否应按 Go 迁移。Go reference 指仓库内
`reference/leadlag-current-strategy-package.zip`，主要入口是压缩包内的
`leadlag-current-strategy-package/go/leadlag/algo/strategy.go` 和
`leadlag-current-strategy-package/go/leadlag/algo/guards.go`；C++ 当前实现以
`strategy/lead_lag/*` 和 checked-in 策略 TOML 为准。

本文关注实盘策略行为。讨论和修改按下面 5 项逐项推进：

1. 把 `parallel-limit` 从 pre-signal 挪到 post-signal guard。
2. 把 `lag_vol_guard` 从 replay-only audit 变成 live hot path open-only guard。
3. 保持 `drift_guard` post-signal open-only，但补齐 Go 风格的 guard snapshot / outcome 语义。
4. freshness 仍是 post-signal open-only；若严格按 Go，需要支持策略内 auto warmup；若按本项目边界，则只保留 fixed / preflight。
5. 调整 report / log，让被 guard 拦截的 open signal 都显示为“signal triggered + intent rejected”，而不是 pre-signal reject 或 missing order。

每一项都会单独记录 Go / C++ 当前差异、迁移方案，以及修改前后的优缺点。

## 1. `parallel-limit` 的执行位置

### Go current

Go 当前实现把 `parallel-limit` 放在 post-signal open guard 中。策略先评估当前 tick 是否形成 open long / open short signal；如果形成
open signal，再进入 `openGuardBlockReason()`，并检查当前 active cache 数量是否已经达到 `execute.parallel`：

```go
if guard.ParallelUsed >= guard.ParallelLimit {
    guard.ParallelOutcome = "blocked"
    s.pr.ParallelBlocked++
    return "parallel-limit", true, guard
}
```

因此 Go 能记录“这个 tick 本来已经形成开仓机会，但因为并发持仓数达到 `parallel` 限制而被拦截”。这个口径有利于回测和归因：

- 可以区分“没有开仓信号”和“有开仓信号但被并发限制挡住”。
- 可以统计如果放宽 `parallel`，可能多出多少 open opportunity。
- full attribution / `signal_decision` 可以记录 blocked signal 的方向、价格、guard snapshot 和 block reason。

### C++ current

C++ 当前实现把 `parallel-limit` 放在 open signal 评估之前。`SignalEngine::OnLeadTick()` 会先处理已有持仓的 close；如果没有 close signal，再检查
`ExecutionState` 是否已经达到容量：

```cpp
if (execution.active_group_count() >= execution.capacity()) {
  return Reject(SignalRejectReason::kParallelLimit);
}
if (execution.new_entries_paused()) {
  return Reject(SignalRejectReason::kDegraded);
}
SignalDecision open_long = TryOpenLong(pair, market, threshold);
```

达到 `parallel` 上限时，C++ 不再调用 `TryOpenLong()` / `TryOpenShort()`，因此不会形成 `lead_lag_signal_triggered`，也不知道当前 tick 如果没有
`parallel` 限制会触发开多还是开空。

### 差异影响

| 维度 | Go current | C++ current |
| --- | --- | --- |
| `parallel-limit` 位置 | open signal 之后的 post-signal guard | open signal 之前的 pre-signal reject |
| 是否先形成 open signal | 是 | 否 |
| 是否知道被挡 signal 的方向和价格 | 知道 | 不知道 |
| 是否会记录 triggered signal / blocked signal attribution | 可以 | 不会进入 triggered open signal |
| 是否会越过 `parallel` 真实下单 | 不会 | 不会 |
| 主要差异性质 | 归因和统计口径 | 归因和统计口径 |

这个差异不会改变实盘硬风险结果：两边达到 `parallel` 上限后都不会继续开仓。真正不同的是统计语义。Go 会把被 `parallel-limit` 挡住的 tick 记为
“有机会但被 guard 拦截”；C++ 当前会把它提前作为 `kParallelLimit` reject，避免继续计算 open 条件。

### 修改前后的优缺点

修改前，即 C++ 当前 pre-signal `parallel-limit`：

- `parallel` 是硬风险边界，不是策略质量过滤器；实盘达到并发持仓上限后，继续计算 open signal 不会改变交易行为。
- pre-signal reject 少走 open signal 计算、诊断构造和日志链路，hot path 更短。
- C++ 的 `new_entries_paused()` / `needs_reconcile` 也属于真实交易状态保护，和 `parallel` 一样适合放在新开仓评估之前。
- 缺点是满仓时不会形成 `lead_lag_signal_triggered`，因此无法从 live log / report 直接看到“本来会开仓，但被 parallel 挡住”的 signal。

修改后，即对齐 Go 的 post-signal `parallel-limit`：

- 优点是满仓时仍会先评估 open signal；如果本 tick 确实触发开仓，会输出 `lead_lag_signal_triggered`，再输出
  `lead_lag_order_intent_rejected reason=parallel_limit`。
- 优点是 report 可以把该行作为 `source_schema=intent_rejected_v1` 关联到 signal，避免把这类机会误判为 missing order。
- 优点是 live / replay / Go reference 的 signal 归因口径更接近，便于查看 signal 和复盘 `parallel` 参数是否过紧。
- 缺点是达到 `parallel` 上限后仍会多走 open signal 计算和一段日志路径，hot path 成本略高。
- 缺点是需要谨慎保证不会因为后置检查而绕过真实下单前的容量限制；synthetic accounting 和 external order 两条路径都必须在
  `parallel_limit` guard 后才能继续。

当前决策：为了方便查看 signal 和对齐 Go reference，C++ 第 1 项按 post-signal `parallel-limit` 迁移。`new_entries_paused()` /
`needs_reconcile` 仍保留为 C++ 真实交易状态保护，继续在 open signal 评估前拦截，不纳入本项迁移。

### 修改方案

1. 调整 `strategy/lead_lag/signal.h` 的 `SignalEngine::OnLeadTick()`：
   - 保留 close-first 行为。
   - 移除 open signal 前的 `execution.active_group_count() >= execution.capacity()` 直接 `kParallelLimit` 返回。
   - 保留 `execution.new_entries_paused()` 的 pre-signal `kDegraded` 返回，因为它表示 unknown-result reconcile / manual intervention 等真实交易状态，不是 Go 的普通 `parallel-limit` guard。
   - 然后继续调用 `TryOpenLong()` / `TryOpenShort()` 形成 open signal。
2. 在 `strategy/lead_lag/strategy.h` 增加 post-signal open-only guard，例如 `RejectOpenForParallelLimit()`：
   - 判断条件为 `OpenAction(last_signal_decision_.action)`、非 reduce-only，且 `runtime->execution.active_group_count() >= runtime->execution.capacity()`。
   - 放在 `FinalizeActiveSignal()` 中 `RecordTriggeredSignal()` 之后、`RejectOpenForDriftGuard()` 之前。
   - 命中时调用 `LogOrderIntentRejectedForSignal("parallel_limit", ...)`，再 `RejectSignal(SignalRejectReason::kParallelLimit)`。
   - 该 guard 必须同时覆盖 `PositionAccountingMode::kSyntheticSignals` 和真实 external order 路径，确保不会进入 `ApplySyntheticSignal()` 或 `SubmitExternalSignal()`。
3. 更新测试：
   - `lead_lag_signal_test` 或等价 signal 单测：满 `parallel` 时不再在 signal engine pre-signal 返回 `kParallelLimit`，而是允许 open signal 形成。
   - `lead_lag_strategy_interface_test`：构造 active group 已满且出现新 open signal 的场景，验证先有 triggered signal，再有
     `lead_lag_order_intent_rejected reason=parallel_limit`，且没有外部订单提交。
   - 如果 replay / signal-only 测试依赖旧 `kParallelLimit` 计数，需要同步改为 post-signal rejected intent 口径。
4. 更新 report / schema 文档：
   - `scripts/lead_lag/analyze_order_detail.py` 已支持通用 `lead_lag_order_intent_rejected`，通常只需新增测试覆盖 `reject_reason=parallel_limit`。
   - `docs/lead_lag_live_report_csv_schema.md` 和 `docs/diagnostic_fields.md` 需要把 `parallel_limit` 列入 open intent rejected reason。
5. 验证：
   - 运行 focused strategy / report 测试，至少覆盖 `lead_lag`、`signal_csv_writer`、`analyze_order_detail_test.py` 和
     `generate_live_report_test.py`。
   - 如果实现触碰热路径，需要补跑对应 LeadLag strategy benchmark；性能结论必须以 benchmark 输出为准。

## 2. `lag_vol_guard` 的实现口径与实盘边界

### Go current

Go current 的 `lag_vol_guard` 是 open signal 之后的 guard，放在 `parallel-limit` 之后、`drift_guard` 之前。它不看成交、不看 PnL，也不看 lead / lag
ratio，只看 lag 交易所自身 BBO mid 的短时波动：

- 每个 lag BBO 计算 `lag_mid = (bid + ask) / 2`。
- 保存上一笔有效 lag mid，计算 `abs_return = abs(current_mid / previous_mid - 1)`。
- 在 `jump_window` 内统计 `abs_return >= jump_threshold` 的次数，默认参数为 `jump_threshold=0.005`、`jump_count=3`、`jump_window=5m`。
- 在 `amplitude_window` 内计算 `amplitude = max_mid / min_mid - 1`，默认参数为 `amplitude_threshold=0.025`、`amplitude_window=1s`。
- 若 `jump_count` 达标或 `amplitude` 超阈值，则认为 `lag_vol_hot=true`。
- open signal 时如果仍在 cooldown 内，返回 `lag-vol-guard-cooldown`；如果当前 hot，则返回 `lag-vol-guard-trigger` 并把
  `cooldown_until` 推进到 `signal_time + cooldown`，默认 `cooldown=15m`。

设计意图是避免 lag 侧 BBO 自身剧烈跳动时继续用该 quote 新开仓。它更像微观结构 emergency guard，而不是收益优化器：它可以降低极端 lag side
抖动时的开仓概率，但也可能错杀真实 lead-lag 传导行情。

### C++ current

C++ 当前没有把 `lag_vol_guard` 放入 live hot path。现有实现只在
`lead_lag_replay --lag-vol-guard-audit-output <path>` 下运行 replay-only audit：

- `tools/lead_lag/lag_vol_guard_audit.*` 维护独立 Go-like 状态。
- audit 只在 replay open signal 后输出 `would_block`、`would_block_reason`、jump / amplitude / cooldown snapshot。
- audit 不改变 replay synthetic accounting，也不改变 live 策略行为。
- `strategy/lead_lag/config.cpp` 当前仍只接受 `lag_vol_guard.mode=off`，非 off 会被拒绝。

因此当前 C++ live 策略不会因为 `lag_vol_guard` 阻断真实 open order；若 report 看到 open 被阻断，原因只会来自已实装的 post-signal guard，例如
`parallel_limit`、`drift_guard`、freshness 或 risk / order preparation。

### 计算复杂度

若直接按 replay audit / Go-like 简单实现，open signal 时需要扫描窗口：

| 计算量 | 朴素复杂度 | live 推荐复杂度 |
| --- | ---: | ---: |
| `lag_mid` | `O(1)` | `O(1)` |
| `abs_return` | `O(1)` | `O(1)` |
| 裁剪过期窗口样本 | 单次 `O(k)` | 摊还 `O(1)` / tick |
| `lag_vol_jump_count` | `O(J)` | 维护 rolling count，`O(1)` |
| `lag_vol_amplitude` | `O(A)` | 维护 min / max 单调队列，`O(1)` 读取 |
| cooldown 判断 / 更新 | `O(1)` | `O(1)` |
| open signal guard 判断 | `O(J + A)` | `O(1)` |

其中 `J` 是 `jump_window` 内 jump 样本数，`A` 是 `amplitude_window` 内 mid 样本数，`k` 是本次裁掉的过期样本数。若未来进入 live hot path，不能直接把
`std::deque + 扫描窗口` 的 audit 写法搬进策略；应使用预分配 ring/window、rolling count 和单调队列，避免 open signal 路径出现窗口扫描或容器扩容。

### 当前决策

当前决策：**现在实盘策略不加入 `lag_vol_guard`，继续只保留 replay-only audit。**

原因是现有 replay 证据不支持 live enforce。2026-06-25 ORDI_USDT 三天 Tardis replay sweep 显示：

- `cooldown=3m/5m/10m/15m` 都降低 signal-only PnL。
- 默认 `15m` 过滤 `62/1175` 个 open signal，主要由 cooldown 扩大过滤范围。
- 被过滤 trade 在 0 tick 和 5 ticks 滑点口径下整体仍为正贡献。

这说明 `lag_vol_guard` 在该样本上确实有机械过滤动作，但没有带来预期保护收益，反而错杀了正贡献 open signal。除非后续更宽 symbol / 更长区间 replay 或小额
live smoke 给出反证，否则 C++ 不把 `lag_vol_guard` 加入实盘策略 hot path；Go / C++ 在这一点保持刻意不同。
