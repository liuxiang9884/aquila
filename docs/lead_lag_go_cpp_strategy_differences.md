# LeadLag Go / C++ 策略实现差异说明

本文单独记录 LeadLag Go reference 与当前 C++ 实现的行为差异，并说明当前 C++ 是否应按 Go 迁移。Go reference 指仓库内
`reference/leadlag-current-strategy-package.zip`，主要入口是压缩包内的
`leadlag-current-strategy-package/go/leadlag/algo/strategy.go` 和
`leadlag-current-strategy-package/go/leadlag/algo/guards.go`；C++ 当前实现以
`strategy/lead_lag/*` 和 checked-in 策略 TOML 为准。

本文关注实盘策略行为。若某个 Go 口径更适合回测归因或离线 audit，但会增加 C++ live hot path 成本，默认不直接搬入实盘路径。

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

### 当前 C++ 取舍

实盘路径建议保留 C++ 当前 pre-signal `parallel-limit`，不直接按 Go 改成 post-signal guard。原因：

- `parallel` 是硬风险边界，不是策略质量过滤器；实盘达到并发持仓上限后，继续计算 open signal 不会改变交易行为。
- pre-signal reject 少走 open signal 计算、诊断构造和日志链路，hot path 更短。
- C++ 的 `new_entries_paused()` / `needs_reconcile` 也属于真实交易状态保护，和 `parallel` 一样适合放在新开仓评估之前。
- 高频实盘路径应优先保持确定性、低延迟和低诊断成本；Go 的 post-signal 口径更适合回测归因。

如果后续需要 Go-like 机会统计，建议在 replay / signal-only / audit 层新增离线归因：当 `parallel` 已满时仍可评估 open opportunity 并记录
would-block 结果，但不要改变 live hot path 的 pre-signal 风控顺序。
