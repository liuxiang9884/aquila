# Gate / Bitget 交易线程与进程拓扑优化计划

## 背景与目标

本阶段从 numeric request / SHM V3 已接受 checkpoint `cd1163c` 派生，专用 worktree
为 `/home/liuxiang/tmp/aquila-gate-bitget-trading-topology`，branch 为
`perf/gate-bitget-trading-topology`。准确的 branch、ahead 和 dirty 状态只信
`git status --short --branch`。

前一阶段已经完成非拓扑 submit、ACK、feedback parser/runtime 和 Strategy 热点优化，并
接受 `bcdc358` 的 numeric `OrderPlaceRequest` / minimal `OrderCancelRequest` 与 SHM
V3；随后 local ID、final JSON writer 和 decimal writer 候选均因完整链或相邻路径回退
而撤销。本阶段不重新打开 request ABI 或 formatter 设计，只分析 Strategy、order
gateway worker、OrderSession、feedback consumer 之间的线程、进程、CPU 和 SHM 交接
成本。

目标：

- 量化 Strategy producer → order worker、order worker → Strategy consumer 的真实跨核
  SHM handoff 延迟；
- 量化 1 route / 4 route 空轮询、burst drain 和 command/event round trip 对 Strategy
  loop 的成本；
- 用 profile 区分 queue cache-line ownership、payload copy、poll cadence、socket owner
  loop、IRQ/softirq 抢占和 host jitter；
- 只接受同时通过正确性、并发、完整链和 tail 门禁的最小候选；
- 持续 profile，不能因单个 queue 或 affinity 候选完成而停止整个优化目标。

## 当前事实与边界

2026-07-20 的只读系统快照：

- 32 个 CPU，单 NUMA、单 socket、每 core 单线程；CPU `0-31` 共享同一 LLC。
- 没有 `isolcpus` / `nohz_full`；本阶段不修改 kernel command line、IRQ affinity、
  `irqbalance`、RPS/XPS、coalescing、ring 或其他系统设置。
- 当前 ENA Tx/Rx IRQ 分别落在 CPU `11/26/7/28/21/5/9/16`，management IRQ 在 CPU18。
- 现有 4-route Gate/Bitget gateway 配置使用 worker CPU `16-19`，因此 CPU16 和 CPU18
  当前与 ENA IRQ 冲突。
- 测试、build、benchmark 和分析只使用 CPU `28-31`；不占用 live `0-15`。CPU28 当前有
  ENA IRQ，正式无网络 microbenchmark 优先使用 CPU29-31，并记录实际 affinity。
- 当前机器没有相关 live trading / data session / gateway 进程运行。
- `kernel.perf_event_paranoid=4`；继续使用 gprof/gprofng 和分层 benchmark，不修改系统
  设置以启用 perf。
- 未获用户明确授权前不发送真实订单；因此 worker affinity/IRQ 候选只能形成离线证据和
  live A/B 方案，不能声明公网或交易所 RTT 收益。

## 2026-07-20 baseline measurement support

`order_gateway_shm_topology_benchmark` 已适配 SHM V3，并覆盖：

- CPU29 producer → CPU30/31 consumer 的 command one-way handoff；
- CPU29 owner → CPU30/31 worker → CPU29 owner 的 command/event round trip；
- command enqueue 同线程下界；
- 1/4 route empty poll 和最后一条 route 单 event scan；
- command/event burst `1/4/16/64` drain。

所有跨线程 case 都有 affinity 检查、start barrier、单 in-flight、sequence / ownership
断言、停止条件和运行期 timeout。Release 三轮 smoke 的代表性 p50 为：enqueue
`36 ns`、29→30/31 one-way `334/404 ns`、round trip `820/809 ns`、1/4 route empty
poll `39/69 ns`。这些数字只证明测量入口可用，不是正式 baseline，也不构成生产优化
收益。

正式 baseline 使用 `542ed4b`、5 组 × 10 repetitions。组中位数中 CPU29→30/31
one-way `p50` 为 `380/398.5 ns`，round trip `p50` 为 `823.5/869 ns`，1/4 route
empty poll `p50` 为 `39/69 ns`。gprofng 必须使用 `-F on` 跟随 target `execve`；
`-F off` 只会得到空 experiment。有效 profile 显示 round-trip worker 的主要 CPU 时间
位于 empty spin、command queue remote `tail` observation 和 event queue remote
`head` observation；slot 定位仍生成整数除法。

client-level measurement 进一步覆盖了真实
`OrderGatewayClient::PollOrderResponses()`，并在计时前把 header 和 client 都预热到
4-route `kReady` 稳态；该 benchmark 不初始化业务 log，也不执行 log 格式化或后端输出。
5 组 × 10 repetitions 的组中位数为：

- 1-route empty raw/client `p50/p99/p99.9`：`38/39.5/40 ns` 与
  `46/47/54 ns`；
- 4-route empty raw/client：`68/70/70.5 ns` 与 `88/107/109 ns`；
- 1-route one-ready raw/client：`49/51/52 ns` 与 `54/70/73 ns`；
- 4-route one-ready raw/client：`78/81.5/82.5 ns` 与
  `98/102/117 ns`。

4-route client 相比 raw route scan 的 `p50` 增量在 empty/one-ready 场景均为
`20 ns`。gprofng 采样和反汇编确认 client 会在 event queue 扫描前后各执行一次 header
route-state 扫描；`kReady` / `kNotReady` 的 `ApplyRouteState()` 还会重复调用
`AllRoutesStopped()`。下一候选只允许消除能够从当前 state 直接证明冗余的本地扫描，不
改变 stopped、queued final response 或 unknown-result 的处理顺序。

把 `kReady` / `kNotReady` 的 `running_ = !AllRoutesStopped()` 等价替换为
`running_ = true` 后，正式 client poll A/B 中 4-route empty 的
`p50/p99/p99.9` 从 `88/107/112 ns` 变为 `88/91.5/94.5 ns`，one-ready 从
`98.5/103/120 ns` 变为 `97/100.5/117.5 ns`。但两组交替顺序的 Gate/Bitget 完整
SHM submit screening 都回退：

- 第 1 组 Gate `p50/p99` 从 `1664/2018 ns` 回退到 `1731/2128 ns`，Bitget 从
  `1594/1914 ns` 回退到 `1606/1988 ns`；
- 反转执行顺序后，Gate 从 `1612/2015 ns` 回退到 `1910/2315 ns`，Bitget 从
  `1521/1826 ns` 回退到 `1583/1999 ns`。

该候选只改善 component tail，收益没有传递到完整链主指标，已最小撤销，不继续消耗
正式 5 组 × 10 repetitions。

## 2026-07-20 queue 候选结果

以下生产候选都已最小撤销：

- producer/consumer 双向 cached remote index：one-way `p50` 约改善 `9%`，但 CPU31
  `p99` 回退 `12.3%`，round trip 也出现回退；
- 仅 producer cached `head`：one-way `p50` 约改善 `10%`，但两组 cross-core `p99`
  回退约 `9%–11%`，round trip `p50/p99` 同时回退；
- power-of-two capacity 条件 mask：component 多数改善，但 command burst-64 和 route
  scan tail 回退；
- 仅 producer 使用条件 mask：正式 round trip `p50` 回退 `3.3%/9.7%`；
- 强制 power-of-two capacity 并在 push/pop 无分支使用 mask：queue round trip
  `p50/p99` 改善约 `5.4%–7.9%/4.3%–4.4%`，但完整 Gate SHM submit
  `p50/p99` 回退 `2.37%/2.00%`，Bitget 回退 `2.14%/2.60%`，Bitget `p99.9`
  回退 `2.94%`；
- 已初始化 client/worker 使用 `TryPushAssumeValid()` / `TryPopAssumeValid()` 跳过
  queue view validity 检查：Release/Debug focused tests 均通过，但正式 round trip
  CPU30/31 `p50` 回退 `13.05%/9.55%`，`p99` 回退 `12.53%/8.99%`，同线程 enqueue
  `p50` 回退 `5.56%`。

最后一项证明 SHM handoff 局部改善约 `4.5%–6.9%`，但收益没有传递到完整 submit；
不得采用或把 component 数字声明为链路收益。正式原始结果位于
`direct-slot-mask-formal/` 和 `direct-slot-mask-submit-formal/`。

## 2026-07-20 CPU16–19 离线筛选

measurement harness 已覆盖 CPU29 owner 与当前四条 gateway worker CPU16–19。5 组 ×
10 repetitions 的短筛选曾显示 IRQ 冲突核 CPU16/18 round-trip `p99.9` 比 CPU17/19
高约 `37%–51%`；但扩展到 5 组 × 100 repetitions 后，CPU16/17/18/19 的
`p50/p99/p99.9` 分别为：

- CPU16：`810/923/1757 ns`；
- CPU17：`842/931/3095 ns`；
- CPU18：`834/950.5/1787 ns`；
- CPU19：`840.5/948.5/1861 ns`。

长筛选期间 CPU16 收到约 804 次 ENA Tx/Rx IRQ、CPU18 收到 61 次 management IRQ，
但离线结果没有证明冲突核更慢。CPU id 排名受 host jitter 影响，不能据此修改 live
affinity；仍需真实网络负载下的授权 A/B。

## 当前所有权模型

- Strategy / `TradingRuntime` 是每条 command queue 的唯一 producer，也是每条 event queue
  的唯一 consumer。
- 每个 Gate/Bitget OrderSession route worker 是对应 command queue 的唯一 consumer、
  event queue 的唯一 producer；socket I/O、command drain 和 response publish 同线程。
- `OrderGatewayQueueHeader::head` 与 `tail` 已分离到不同 cache line；slot 紧随 queue
  header，command/event 使用 trivially-copyable 固定布局和 `memcpy`。
- producer publish 使用 slot copy 后 `tail.store(release)`；consumer acquire-load
  `tail` 后 copy slot，再 `head.store(release)`。当前 producer 每次 push 都 acquire-load
  remote `head`，consumer 每次 pop 都 acquire-load remote `tail`。
- OrderSession active-spin loop 每次迭代先执行 runtime hook；gateway hook 最多 drain 64
  个 command，再执行 websocket write/read。
- Strategy 每次 loop 在处理 market data 前轮询 order response 和 order feedback；
  `OrderGatewayClient::PollOrderResponses()` 会扫描每条 route 的 event queue，并同步 header
  route state。

## 阶段 1：冻结 topology baseline

### 测量支撑

在 `benchmark/core/trading` 增加只属于 benchmark 的跨线程测量入口，不改生产行为：

1. 单 queue producer-only enqueue，保留现有同线程下界。
2. CPU29 producer → CPU30 consumer 的 command one-way handoff：
   - consumer active-spin；
   - producer 每次只保留一个 in-flight command，避免 throughput batching 混入最低延迟；
   - 记录 enqueue → dequeue 的 `p50/p99/p99.9/max` 和 MAD。
3. CPU29 producer → CPU30 worker → CPU29 consumer 的 command/event round trip。
4. 1 route / 4 route event empty poll 和单 event drain。
5. burst `1/4/16/64` 的 command/event drain，区分最低延迟与吞吐。
6. CPU29↔30 与 CPU29↔31 各跑 screening；由于同 NUMA/LLC，只用于识别系统噪声，不据
   CPU id 差异直接修改 live affinity。

benchmark 必须有明确线程 start barrier、停止条件、超时和 ownership 断言；不把
Google Benchmark control thread 误算进 handoff latency。

### Baseline 与 profile

- measurement-support commit 的生产代码与 `cd1163c` 相同；正式 baseline binary 从该
  harness commit 构建，Release、GCC 13.3.0、固定相同 CPU 和参数。
- 每个正式 case 至少 5 组交替 baseline/candidate，每组至少 10 repetitions。
- 保存组中位数、MAD、方向一致组数、max/outlier count 和环境快照。
- gprofng 分别 profile producer enqueue、consumer empty spin、burst drain 和 round trip。
- 原始产物写入
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-topology-results/`；
  build 写入
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-topology-builds/`。

## 阶段 2：最小 queue ownership 候选

只有阶段 1 证明 remote index load 或 queue scan 是可行动热点时才实施。

优先候选：

1. producer 在进程内 queue view 缓存 consumer `head`，仅在 cached capacity 判断为 full
   时 acquire-refresh remote `head`；
2. consumer 缓存已观察到的 producer `tail`，burst drain 期间复用；queue empty 时仍
   acquire-refresh remote `tail`；
3. 如果 4-route empty poll 是 Strategy loop 的显著热点，再单独设计带 generation 的
   level-trigger notification；不得用可能丢 wakeup 的裸 flag 或只依赖 event queue 的
   edge-trigger hint。

不在同一候选中同时修改 queue layout、memory order、poll cadence、affinity 或 process
boundary。每个候选都先运行 SPSC stress / wraparound / full / empty / producer restart /
consumer restart / route stopped 测试，再进入完整 Gate/Bitget benchmark。

### 内存模型门禁

- slot 内容必须 happens-before consumer 读取；
- producer 不得覆盖 consumer 尚未释放的 slot；
- consumer 不得读取 producer 尚未完整发布的 slot；
- cached index 只能延迟观察远端进度，不能导致 false available 或 false free；
- false full / false empty 只允许在下一次 refresh 消失，且不能改变既有 queue-full、
  stop、restart 和 fail-closed 语义；
- queue view 的复制/移动、attach/open 与多 route 初始化不能共享错误的本地 cache state。

## 阶段 3：runtime poll cadence 与 process boundary

只有 queue ownership 候选完成且 profile 仍证明 poll/交接是主要热点时才进入：

- 比较每 loop 扫描 1/4 route 的空轮询成本；
- 比较 command hook 位于 websocket loop 的当前位置与其他等价 cadence；
- 比较同进程不同线程与独立进程 SHM，但不把 direct same-thread fake 当成生产替代；
- 保持每个 socket、其 prepared-write arena、request map 和 parser callback 的单 owner
  thread，不为减少一次 handoff 引入跨线程 socket ownership；
- 任何 notification/generation ABI 修改都单独提升 SHM version，并完成全仓 producer /
  consumer / restart 审计。

## 阶段 4：affinity / IRQ live 验证方案

read-only profile 已发现现有 worker CPU16/18 与 ENA IRQ 冲突，但本阶段不直接修改系统。

在用户明确授权 live smoke 或真实订单前，只准备候选配置和验证矩阵：

- baseline：现有 worker CPU `16-19`；
- candidate：选择不与当次 ENA IRQ 冲突、也不占用行情/fusion/strategy/feedback/log 的
  CPU；每次 run 前重新读取 `/proc/interrupts`，不能硬编码当前快照；
- 记录实际 thread affinity、IRQ affinity、ENA stats、Ack RTT
  `p50/p99/p99.9/max`、`x_in → x_out` 和 request send fanout spread；
- baseline/candidate 交替运行，保持 endpoint、TLS/plain、connect IP、symbol、order size、
  route count 和同机负载一致；
- 没有真实网络证据时只报告“发现冲突”和“候选方案”，不声明 latency 收益。

## 验证与采用门

功能与并发：

- Debug/Release build；
- queue layout、wraparound、full/empty、ordering、copy/move、attach/open tests；
- producer/consumer stress、TSAN 可运行范围、route stop/restart、queue-full、fail-closed；
- Gate/Bitget gateway worker、OrderSession、runtime adapter、multi-session tests；
- LeadLag replay 和 signal CSV byte equivalence；
- `git diff --check` 与 evaluation include/link 边界。

性能：

- component screening 只决定是否进入正式 A/B；
- 采用结论看 cross-core handoff、Gate/Bitget GatewayWorker 和完整 Strategy submit/feedback
  runtime，不累加 component 百分比；
- `p50/p99` 必须至少 4/5 同向且组中位数无显著回归；`p99.9` 回归或方向不稳定时默认拒绝，
  除非完整外层 tail 有更强证据证明中性；
- throughput 改善不能抵消最低延迟或尾延迟回归；
- fresh profile 必须能解释收益来源，不能只凭 code-layout 偶然变化采用。

## 原子提交、push 与 PR

1. topology benchmark / profile 支撑；
2. 每个独立 queue 或 poll 候选及其测试；
3. 最终正式 A/B、replay 和专题报告；
4. 更新当前事实源后 push `perf/gate-bitget-trading-topology`，创建或更新独立中文 PR。

不满足门禁的生产候选最小撤销；benchmark 支撑可在边界正确且可复用时独立保留。

## 停止条件

满足以下全部条件，或用户明确要求暂停：

- command/event cross-core one-way 与 round-trip 均有 baseline/final；
- 1/4 route empty poll 与 burst drain 有 profile；
- 所有 `≥5%` samples 的可行动 userspace hotspot 已优化或有两次最小失败候选；
- affinity/IRQ 已有用户授权的 live A/B，或明确记录“无授权、不能验证”的边界；
- Gate/Bitget 完整链、并发安全、replay 和 tail 门禁通过；
- 最终报告包含相对 `cd1163c` 的整体前后数据与未采用候选。
