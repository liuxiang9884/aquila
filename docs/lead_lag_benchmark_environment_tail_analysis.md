# LeadLag Benchmark Tail 环境归因

本文记录 2026-06-06 对 `OpenSignalSubmitPath` benchmark tail 的环境归因。结论只适用于本地 benchmark 环境，不等价于实盘 Gate Ack RTT 或交易所下单延迟。

## Benchmark 覆盖范围

`BM_LeadLagRuntimeOpenSignalSubmitPathLatency` 的计时窗口是一次 trigger `BookTicker` 进入 `TradingRuntime::HandleBookTickerForTest()` 到返回：

```cpp
start_ns = NowNs();
runtime.HandleBookTickerForTest(trigger_ticker);
elapsed_ns = NowNs() - start_ns;
```

它包括：

- `TradingRuntime::OnBookTicker()` 到 `Strategy::OnBookTicker()`。
- LeadLag market route / raw market update / alignment / recorder / threshold。
- `SignalEngine::OnLeadTick()` 的开仓信号计算。
- signal timing / diagnostics / strategy log 调用。
- `SubmitExternalSignal()` 中的 price rounding、decimal unit / text preparation、freshness / risk check。
- `StrategyContext::PlaceOrder()` / `OrderManager::PlaceOrder()` 的 order 校验、order pool create、字段复制。
- benchmark fake `OrderSession::PlaceOrder()`，该实现只记录 counters 并执行 `benchmark::ClobberMemory()`。
- accepted 后 execution state 更新、submitted log 和 open risk reservation。

它不包括：

- runtime 创建和 seed 行情。
- 真实网络 IO、TLS、WebSocket 编码、Gate 下单帧发送。
- Ack / response / feedback / order finished。

因此该 benchmark 是“本地策略到 `OrderManager` 提交完成”的路径，不是交易所下单延迟。

## Refactor 前后对比

对比口径为 2026-06-05 修好 benchmark fixture 后的 round1 baseline 与 round7 final，均为 release build，`taskset -c 16`。

| Benchmark | Baseline | Final | 变化 |
| --- | ---: | ---: | ---: |
| `OnBookTickerRealTrace` median | `85.5ns` | `85.9ns` | `+0.4ns`, `+0.5%` |
| `ActiveLeadTickNoSignal` median | `229ns` | `198ns` | `-31ns`, `-13.5%` |
| `ActiveLagTickNoSignal` median | `228ns` | `216ns` | `-12ns`, `-5.3%` |
| `OpenSignalSubmitPath` median | `1.832us` | `1.775us` | `-57ns`, `-3.1%` |
| `OpenSignalSubmitPath` p50 | `1.551us` | `1.611us` | `+60ns`, `+3.9%` |
| `OpenSignalSubmitPath` p99 | `11.453us` | `11.382us` | `-71ns`, `-0.6%` |
| `FeedbackParserToRuntimeFill` median | `1.161us` | `1.166us` | `+5ns`, `+0.4%` |
| `FeedbackParserToRuntimeFill` p50 | `985ns` | `1.022us` | `+37ns`, `+3.8%` |
| `FeedbackParserToRuntimeFill` p99 | `1.257us` | `1.272us` | `+15ns`, `+1.2%` |

当前证据没有显示 LeadLag refactor 引入性能衰减。短 ns 级 benchmark 对 CPU 频率、调度和虚拟化噪声敏感，不能把 active lead 的单次改善解释为确定优化收益；更稳妥的结论是没有观察到回归。

## Tail 环境证据

本机环境观察：

- 机器是 `KVM` 虚拟机，kernel 为 AWS kernel。
- `taskset -c 16` 只绑定 benchmark 用户态线程，不隔离 CPU16 上的 kernel interrupt、softirq、RCU 或 host vCPU 调度。
- `/sys/devices/system/cpu/isolated` 为空，`nohz_full` 为空 / none，当前没有 CPU isolation / tickless benchmark core。
- `irqbalance` 正在运行。
- CPU16 历史上有网卡 IRQ 命中，例如 `enp55s0-Tx-Rx-7`，softirq 中 `NET_RX`、`TIMER`、`SCHED`、`RCU` 也有大量累计。
- Google Benchmark header 中 CPU MHz 在不同轮次显示 `2400MHz` 到 `3200MHz`，说明当前环境存在频率 / 虚拟化观测波动。

2026-06-06 重新运行一次 `OpenSignalSubmitPath`，同时抓取 CPU16 运行前后的 interrupt / softirq delta：

```text
BM_LeadLagRuntimeOpenSignalSubmitPathLatency:
p50_ns=1.567k
p99_ns=11.265k
p999_ns=19.529k
max_ns=114.511M

CPU16 deltas during run:
LOC   +14673
TIMER +567
RCU   +6686
SCHED +4
NET_RX +9
```

同一次 run 中出现 `max_ns=114.511ms`，该数量级不可能由本地策略 submit path 的纯 CPU 计算稳定产生，基本属于 OS 调度、host vCPU deschedule、timer / RCU / softirq 或虚拟化噪声。p99 是 `4096` 个样本里最慢约 `41` 个样本，只要这些样本中部分撞上上述系统事件，就会从正常 `1.5us-1.8us` 被拉到 `10us+`。

## 结论

`OpenSignalSubmitPath` 的 p50 / median 更接近代码路径本身；当前环境下 p99 / p999 / max 混入明显系统噪声。refactor 前后 p99 基本持平，说明没有观察到新增 tail regression；但当前环境不适合用 p99 做精细代码级归因。

后续如果需要严肃分析 LeadLag submit path 的代码级 tail，应至少满足：

- 使用 dedicated bare-metal 或低噪声机器，避免共享 KVM host 调度。
- 为 benchmark CPU 配置 CPU isolation，并使用 `nohz_full` / `rcu_nocbs`。
- 关闭或固定 `irqbalance`，把网卡 IRQ 和高频 softirq 移出 benchmark CPU。
- 固定 CPU 频率或明确记录 governor / turbo 状态。
- 避免 VSCode、Codex、build、replay、pcap heavy capture 等后台任务并发。
- 将 benchmark 拆成 stage timer：signal calc、diagnostics / log、order preparation、`OrderManager::PlaceOrder()`、post-submit execution update。

在当前机器上汇报 LeadLag benchmark tail 时，应同时记录 CPU、是否 isolated、是否有 IRQ / softirq delta、CPU MHz header、load average 和是否有异常 max；没有这些环境信息时，不应把 `p99` 直接归因到策略代码。
