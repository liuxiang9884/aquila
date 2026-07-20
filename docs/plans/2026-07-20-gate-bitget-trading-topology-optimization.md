# Gate / Bitget 交易线程与进程拓扑优化计划

## 背景与目标

本阶段从 `perf/gate-bitget-trading-latency` 的已接受 checkpoint `d829337` 派生，专用
worktree 为 `/home/liuxiang/tmp/aquila-gate-bitget-trading-topology`，branch 为
`perf/gate-bitget-trading-topology`。准确的 branch、ahead 和 dirty 状态只信
`git status --short --branch`。

前一阶段已经完成非拓扑 submit、ACK、feedback parser/runtime 和 Strategy 热点优化，并
拒绝了把订单 command 全面改为 `double + decimal places` 的候选。本阶段不重新打开该
request ABI 设计，只分析 Strategy、order gateway worker、OrderSession、feedback
consumer 之间的线程、进程、CPU 和 SHM 交接成本。

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

- baseline binary 从 `d829337` 构建，Release、GCC 13.3.0、固定相同 CPU 和参数。
- 每个正式 case 至少 5 组交替 baseline/candidate，每组至少 10 repetitions。
- 保存组中位数、MAD、方向一致组数、max/outlier count 和环境快照。
- gprofng 分别 profile producer enqueue、consumer empty spin、burst drain 和 round trip。
- 原始产物写入
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-results/trading-topology/`；
  build 写入
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-builds/trading-topology/`。

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
- 最终报告包含相对 `d829337` 的整体前后数据与未采用候选。
