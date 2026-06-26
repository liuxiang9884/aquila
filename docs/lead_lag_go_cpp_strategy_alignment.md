# LeadLag Go / C++ 策略实现差异说明

本文单独记录 LeadLag Go reference 与当前 C++ 实现的行为差异，并说明当前 C++ 是否应按 Go 迁移。Go reference 指仓库内
`reference/leadlag-current-strategy-package.zip`，主要入口是压缩包内的
`leadlag-current-strategy-package/go/leadlag/algo/strategy.go` 和
`leadlag-current-strategy-package/go/leadlag/algo/guards.go`；C++ 当前实现以
`strategy/lead_lag/*` 和 checked-in 策略 TOML 为准。

本文关注实盘策略行为。讨论和修改先按下面 5 项逐项推进：

1. 把 `parallel-limit` 从 pre-signal 挪到 post-signal guard。
2. 把 `lag_vol_guard` 从 replay-only audit 变成 live hot path open-only guard。
3. 保持 `drift_guard` post-signal open-only，但补齐 Go 风格的 guard snapshot / outcome 语义。
4. freshness 仍是 post-signal open-only；若严格按 Go，需要支持策略内 auto warmup；若按本项目边界，则只保留 fixed / preflight。
5. 调整 report / log，让被 guard 拦截的 open signal 都显示为“signal triggered + intent rejected”，而不是 pre-signal reject 或 missing order。

之后补充第 6 项：不把 Go 的 `taker_buffer` auto warmup 放入 C++ live hot path，但用 C++ 已固化的 `open_slippage_ticks` /
`close_slippage_ticks` 进入 open signal 的成本筛选。

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

## 3. `drift_guard` 的 live enforce 与 snapshot 边界

### Go current

Go current 的 `drift_guard` 是 post-signal open guard，位于 `lag_vol_guard` 之后、freshness 之前。它维护独立的 lag / lead ratio 状态，并在 open signal
触发后检查三类异常：

- `drift_instant_ratio = lag_mid / lead_mid`，判断 `abs(drift_instant_ratio - 1) > drift_instant`。
- `ratio_std`，判断 ratio 窗口标准差是否超过 `ratio_std`。
- `drift_mean`，判断 ratio 窗口均值是否满足 `abs(drift_mean - 1) > drift_mean_threshold`。

Go 的 full attribution / signal decision 语义会携带一组 guard snapshot 字段，例如 `drift_guard_enabled`、`drift_guard_ready`、
`drift_instant_ratio`、`drift_instant_threshold`、`drift_instant_hit`、`ratio_std`、`ratio_std_threshold`、`ratio_std_hit`、
`drift_mean`、`drift_mean_threshold`、`drift_mean_hit` 和 `drift_guard_outcome`。这些字段用于解释某个 open signal 为什么被
`drift-guard` 阻断，或正常通过时距离阈值还有多远。

### C++ current

C++ 当前已经把 `drift_guard` 加入实盘策略，并按 Go reference 口径 enforce：

- checked-in 策略 TOML 使用 `[lead_lag.pairs.trigger.drift_guard] enabled / drift_instant / ratio_std / ratio_std_window / drift_mean /
  drift_mean_window`。
- parser 拒绝旧 `trigger.drift_limit` 和旧 `drift_guard.mode`，避免旧 pre-signal drift limit 与当前 guard 同时 enforce。
- `Strategy::FinalizeActiveSignal()` 会先记录 `lead_lag_signal_triggered`，再执行 `RejectOpenForDriftGuard()`。
- guard 只作用于 `kOpenLong` / `kOpenShort`，close / stoploss 不受影响。
- 命中时输出 `lead_lag_order_intent_rejected reason=drift_guard`，report 以 `source_schema=intent_rejected_v1` 关联到 signal，不把它当作
  missing order。
- replay audit 已输出 raw `drift_instant` / `ratio_std` / `drift_mean` 和 `drift_guard_outcome`，用于离线解释具体命中维度。

因此第 3 项的行为层面已经完成：C++ 与 Go 一样在 signal 之后、订单 intent / synthetic accounting 之前拦截异常 drift 的 open signal。

### 当前差异

当前剩余差异只在 live 诊断粒度：

| 维度 | Go current | C++ current |
| --- | --- | --- |
| 是否 live enforce | 是 | 是 |
| 执行位置 | post-signal open guard | post-signal open guard |
| 是否影响 close / stoploss | 否 | 否 |
| 拦截原因 | `drift-guard` | `drift_guard` |
| live rejected intent 是否携带 snapshot | 是，full attribution 字段包含 guard snapshot | 否，只输出 `reason=drift_guard` |
| 离线 snapshot / outcome | full attribution / signal decision | replay audit 输出 `drift_instant` / `ratio_std` / `drift_mean` / `drift_guard_outcome` |

### 当前决策

当前决策：**不把 Go 风格 drift guard snapshot / outcome 扩进 live hot path 的 `lead_lag_order_intent_rejected` 或 report schema。**

理由：

- `drift_guard` 已经是 emergency sanity guard，实盘最关键的信息是 open signal 被 `drift_guard` 拦截；这一点当前日志和 report 已具备。
- live rejected intent 加入 `drift_instant_ratio`、阈值、hit bool 和 outcome 会扩大日志字段、parser、report CSV 和测试维护面。
- rejected path 虽然不是每个 tick 都走，但仍在实盘策略进程内；当前没有必要为了少量解释性字段增加 hot path 诊断负担。
- 具体命中维度可通过 `lead_lag_replay --lag-vol-guard-audit-output` 的 drift guard audit 字段离线解释；需要专项排查时再使用 replay / audit，而不是默认污染实盘日志。

因此第 3 项按文档收敛：C++ 保持当前 live enforce 行为，不新增代码。若未来需要完整 Go attribution，再单独评估日志字段、report schema 和热路径成本。

## 4. freshness guard 的 fixed / preflight 边界

### Go current

Go current 的 freshness 是 post-signal open guard，位于 `drift_guard` 之后。它只拦截 open signal，不影响 close / stoploss。Go 支持 fixed 和 auto 两种阈值来源：

- fixed：配置 `lead_fixed_threshold_ms` / `lag_fixed_threshold_ms` 后直接使用。
- auto：策略进程内记录 BBO freshness latency，口径是 `local_time_ms - server_time_ms`；warmup 到期后冻结阈值，使用类似
  `mean + 3 * std` 的统计结果。
- auto 阈值尚未可用时，会回退 fixed threshold；fixed threshold 不大于 0 时等价于该侧 freshness guard disabled。
- open signal 后分别检查 lead / lag 当前 freshness；Go 的 block reason 是 `freshness-lead` / `freshness-lag`。

Go 的优点是策略进程可以根据启动后的实时链路状况自动得到阈值。缺点是 live 策略内需要维护 warmup 样本、冻结时机、fallback 逻辑和诊断状态。

### C++ current

C++ 当前不在 live 策略内实现 `freshness_auto`，只执行固定阈值 freshness guard：

- 每个 `[[lead_lag.pairs]]` 直接配置整数毫秒字段 `max_lead_freshness_ms` / `max_lag_freshness_ms`。
- live 策略启动时把它们转换为 ns，后续只比较 `signal_decision_ns - lead_exchange_ns` 和
  `signal_decision_ns - lag_exchange_ns`。
- freshness guard 在 `lead_lag_signal_triggered` 之后、真实订单准备和提交之前执行；只作用于 `kOpenLong` / `kOpenShort`。
- 命中时输出 `lead_lag_order_intent_rejected`，reason 为 `stale_lead_quote` 或 `stale_lag_quote`，并携带
  `max_lead_freshness_ns` / `max_lag_freshness_ns`、`freshness_guard_pass=false` 和 `freshness_reject_reason`。
- `freshness_shadow` 已删除；候选 freshness 对照不在同一个 live 策略进程内运行。

启动前由 `lead_lag_freshness_preflight` 通过 live data reader / fusion canonical `BookTicker` 采样，再用
`scripts/lead_lag/apply_freshness_preflight_summary.py` 把结果写回策略 TOML。这个 preflight 负责把 Go-like auto 统计迁移到实盘启动前，而不是让策略热路径动态学习阈值。

### 当前差异

| 维度 | Go current | C++ current |
| --- | --- | --- |
| 执行位置 | post-signal open guard | post-signal open guard |
| 是否影响 close / stoploss | 否 | 否 |
| 阈值来源 | fixed 或策略内 auto warmup | 启动前 preflight 生成 fixed `max_*_freshness_ms` |
| live 策略是否保存 warmup 样本 | 是，auto 模式需要 | 否 |
| auto 未 ready 的 fallback | fixed threshold | 不适用，live 只读 fixed threshold |
| rejected reason | `freshness-lead` / `freshness-lag` | `stale_lead_quote` / `stale_lag_quote` |
| freshness shadow | Go full attribution 可记录 guard outcome | C++ 已删除策略内 shadow；对照实验用独立 replay / signal-only 进程 |

### 当前决策

当前决策：**不把 Go 的 freshness auto warmup 放回 C++ live 策略热路径，继续使用启动前 preflight 生成固定阈值。**

理由：

- freshness threshold 是启动环境和 data reader / fusion 链路属性，适合在真实下单前作为启动门禁采样并固化。
- live 策略只读取固定整数毫秒阈值，行为更确定，report 更容易复现和解释。
- 策略内 auto warmup 会增加动态状态、冻结时机、fallback 分支和诊断字段；这些状态不应该为每次实盘下单路径长期存在。
- 项目当前 live pipeline 已要求启动前串联 freshness preflight 和 taker-buffer slippage preflight；新实盘启动仍应按该 pipeline 生成最终策略 TOML。
- 需要评估候选 freshness 参数时，应另起 replay / signal-only 或独立 live 进程做对照，不恢复已删除的 `freshness_shadow`。

因此第 4 项按文档收敛：C++ 保持当前 fixed / preflight 边界，不新增代码。若未来要严格按 Go 恢复策略内 auto warmup，需要重新评估热路径状态、配置 schema、report 字段和实盘启动流程。

## 5. guard 拦截后的 log / report 语义

### 目标语义

第 5 项的目标是让被 guard 或本地下单准备阶段拦截的 open signal 都呈现为：

1. 先输出 `lead_lag_signal_triggered`，保留 signal 方向、价格、lead / lag BBO id 和 freshness。
2. 再输出 `lead_lag_order_intent_rejected`，说明该 open signal 没有进入真实订单提交，并给出 `reason`。
3. report 把 rejected intent 当作 `source_schema=intent_rejected_v1` 的伪订单行关联到 signal，而不是把该 signal 标成 `missing_order`。

这个语义的重点不是“所有 reject 都要伪装成真实订单”，而是区分两类情况：

- 没有形成 open signal：不应生成订单行。
- 已形成 open signal，但被 guard / 本地准备阶段挡住：应生成 rejected intent 行，便于统计 open opportunity 和拦截原因。

### C++ current

C++ 当前已按这个目标处理 post-signal open 拦截：

- `parallel_limit`：第 1 项已从 pre-signal reject 改为 post-signal guard，命中时输出 `reason=parallel_limit`。
- `drift_guard`：命中时输出 `reason=drift_guard`。
- freshness：命中时输出 `reason=stale_lead_quote` 或 `reason=stale_lag_quote`，并携带 `freshness_guard_pass=false` 与
  `freshness_reject_reason`。
- risk：命中时输出 `reason=risk_limit`。
- 本地下单准备失败：`invalid_price`、`zero_quantity`、`order_text_slot_full`、`place_local_rejected` 等也走
  `lead_lag_order_intent_rejected`。

`scripts/lead_lag/analyze_order_detail.py` 会把 `lead_lag_order_intent_rejected` 合并为 `intent_rejected_v1` 行，`local_order_id=0` 不会触发
`missing_submitted_log`。`scripts/lead_lag/generate_live_report.py` 用 signal timing / symbol / action / side / raw price 等字段把该 rejected intent
关联回 `signal.csv`，因此对应 signal 不会被标记为 `missing_order`。

### 当前边界

仍保留为 pre-signal reject 的路径不纳入第 5 项：

- `new_entries_paused()` / `needs_reconcile` 属于真实交易状态保护，当前仍在 open signal 评估前返回 `kDegraded`。
- 未满足 threshold、price diff、lag part 等条件时，本来就没有 open signal，不生成 rejected intent。
- `lag_vol_guard` 当前不进入 live hot path，因此不会产生 live rejected intent；需要分析其 would-block 结果时使用 replay-only audit。

### 当前决策

当前决策：**不再改策略代码，只补 report / parser 回归测试和文档。**

理由：

- 关键 live 路径已经满足“signal triggered + intent rejected”语义。
- 继续扩展 live log schema 或 reason 命名会增加 report / parser 维护面，当前收益有限。
- 第 5 项真正需要补强的是回归保护：避免未来改 report/parser 时把 rejected intent 重新误判成 `missing_order`。

新增回归覆盖：

- `scripts/test/lead_lag/analyze_order_detail_test.py` 覆盖 `stale_lag_quote`、`risk_limit`、`zero_quantity` 三类 rejected intent，确认它们生成
  `intent_rejected_v1` 且不会出现 `missing_submitted_log`。
- `scripts/test/lead_lag/generate_live_report_test.py` 覆盖同三类 reason 与 `lead_lag_signal_triggered` 的 join，确认 `signal.csv` 不出现
  `missing_order`，`order_detail.csv` 保留 `status=kRejected` / `source_schema=intent_rejected_v1`。

因此第 5 项按测试和文档收敛：C++ 当前 report / log 语义保留，后续新增 post-signal open guard 时应复用
`lead_lag_order_intent_rejected`，并同步补 report/parser 回归。

## 6. slippage ticks 进入 open cost model / signal 筛选

### Go current

Go current 的 `cost.taker_buffer` 可以进入 open cost model。`EntryCostBreakdown.RequiredEdge()` 包含 fee、entry taker buffer、normal close taker
buffer、lead noise 和 lag noise；`exclude_from_cost_model=true` 时才把 taker buffer 从成本模型中排除。Go 还支持 `auto` 模式，通过 lag Depth
L2 warmup 生成有效 buffer，并可同时影响 order price 和 open threshold。

### C++ current

C++ 不把 Go 的 `taker_buffer` auto warmup 放入 live 策略热路径，也不在 signal 阶段读取 `execute.taker_buffer.entry_fixed_pct` /
`normal_close_fixed_pct` 做动态 pct 成本模型。C++ 的实盘执行成本来源是启动前已经写入策略 TOML 的 fixed ticks：

- `open_slippage_ticks`：open order 的基础 aggressive ticks。
- `close_slippage_ticks`：normal close order 的基础 aggressive ticks。
- `stoploss_slippage_ticks`：stoploss 专用，不进入普通 open signal 筛选。
- `close_retry_slippage_step_ticks`：normal close retry 专用，不进入普通 open signal 筛选。

当前 C++ 已把基础 open / normal close ticks 折算为当前 open signal 触发价上的比例成本：

```text
entry_slippage_buffer = open_slippage_ticks * lag_price_tick / trigger_price
normal_close_slippage_buffer = close_slippage_ticks * lag_price_tick / trigger_price
required_edge = fee + entry_slippage_buffer + normal_close_slippage_buffer + lead_noise + lag_noise + target_profit_rate
```

其中 `trigger_price` 是 open long 的 lag ask 或 open short 的 lag bid。若 `price_tick <= 0`、`trigger_price <= 0` 或对应 ticks 为 0，则该侧
slippage buffer 记为 0，保持 legacy / 无效元数据下的旧行为。

### 当前差异

| 维度 | Go current | C++ current |
| --- | --- | --- |
| open 成本来源 | `cost.taker_buffer` pct，可 fixed / auto | `open_slippage_ticks` / `close_slippage_ticks` 固定 ticks |
| 是否策略内 auto warmup | 是，可基于 lag Depth L2 | 否，启动前 preflight 固化配置 |
| 是否进入 open threshold / signal 筛选 | 是，除非 `exclude_from_cost_model=true` | 是，但只使用 fixed slippage ticks 折算 pct |
| 是否影响真实下单价 | pct buffer 可影响 order price | 真实下单价仍使用 slippage ticks |
| stoploss / retry 是否进入普通 open cost | 不作为普通 open buffer | 不进入，只建模 open + normal close 基础 ticks |
| `execute.taker_buffer` 在 C++ 的角色 | 不适用 | 仅 `lead_lag_signal_decision` 参考价诊断和 preflight trace，不直接筛 signal |

### 当前决策

当前决策：**C++ 用实际执行的 slippage ticks 对齐 Go 的“执行摩擦进入 open cost model”语义，但不迁入 Go 的 `taker_buffer` auto warmup。**

理由：

- open signal 筛选应该覆盖基础 open + normal close 执行摩擦，否则 configured slippage 越大，信号门槛却不变，会低估开仓所需 edge。
- C++ live 策略的真实下单路径已经以 ticks 为唯一执行 aggressiveness 来源；用同一组 ticks 做 signal cost model，避免同时维护 pct buffer 与 ticks buffer。
- `open_slippage_ticks` / `close_slippage_ticks` 是启动前 preflight 或人工配置后的固定值，进入 signal hot path 只增加常数次乘除法，不引入 warmup 状态、
  Depth L2 依赖或动态 fallback。
- stoploss slippage 和 normal close retry step 是异常 / 补救路径，若进入普通 open threshold 会过度抬高门槛；当前只建模基础 open 和基础 normal close。

因此第 6 项按“fixed ticks cost model”收敛：C++ 不直接按 Go `taker_buffer` pct 迁移，但把已固化的执行 ticks 纳入 open signal 的
`required_edge`，用于横截面信号筛选。
