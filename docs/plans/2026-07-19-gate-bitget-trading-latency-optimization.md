# Gate / Bitget 交易链路极限延迟优化计划

## 目标

以已合并的 `main@f5958186197019c4153368a767dad2013f39055a` 为唯一代码基线，在交易行为、
状态转换、安全语义和可观测性结果保持等价的前提下，反复使用真实历史 workload 做
profile、benchmark、优化和回归，降低 Gate 与 Bitget 交易链路的 `p50`、`p99`、`p99.9`
和 `cycles/op`。

本计划覆盖从行情进入策略到订单写入，以及从交易所回报进入本机到策略订单状态更新的
Aquila 软件链路。涉及的生产范围包括：

- `core/websocket/` 中交易链路实际使用的读写、dispatch 和 runtime；
- `core/market_data/` 中策略实际使用的 SHM / DataReader 交接；
- `core/trading/` 的订单池、OrderManager、gateway/feedback SHM、runtime 和公共类型；
- `strategy/lead_lag/` 的信号、下单、feedback 和 execution-group 主路径；
- `exchange/gate/trading/`、`exchange/bitget/trading/`；
- `tools/gate/gate_order_gateway.cpp`、`tools/bitget/bitget_order_gateway.cpp` 等生产装配入口。

## 非目标

- 不发送真实订单，不以本轮结果声明 fillability、成交率、PnL、公网或交易所撮合改善。
- 不增加交易规则、风控规则、订单类型、恢复能力、订阅或其他生产功能。
- 不把未合并 branch 的实现纳入基线；历史实盘产物只作为 workload/profile 证据。
- 不重做现有日志延迟格式化、固定结构 ring buffer 或后台编码机制。
- 不在主性能 branch 提前修改线程 ownership 或进程拓扑；该项只在最后阶段使用独立
  branch/worktree 评估。
- 不优化与交易主路径无关的 monitor、TUI、REST 慢路径或 report 生成。

## 已锁定决策

### 等价性

- 相同输入必须产生相同数量、价格文本、方向、类型、TIF、`reduce_only`、symbol、route、
  决策顺序、feedback 事实和订单状态转换。
- 交易所请求 method/path/body 保持一致；比较时只规范化鉴权时间戳、signature、运行时 ID
  和明确登记的非确定性诊断字段。
- 不使用浮点容差掩盖交易字段差异。
- 当前没有生产读写用途的字段可在全仓 producer/consumer/parser/report/doc/script 审计后，
  由 benchmark 决定保留、重排或删除；ABI/version 和测试必须同步更新。
- `UnknownResult`、`ContinuityLost`、queue-full、ready/not-ready、reconcile、stop-and-flat
  和断线语义不得弱化。

### 性能验收

- 采用 Pareto 验收：`p50`、`p99`、`p99.9` 或 `cycles/op` 至少一个可重复改善，其他核心
  指标不得超出各自噪声带退化；同时改善时优先 `p99.9 → p99 → p50`。
- 相同 Release 配置、固定 CPU/频率，并把测试核 IRQ 迁走。至少运行 5 组交替 A/B，每组
  至少 10 repetitions；至少 4/5 组方向一致。
- 收益必须超过 `2 × baseline MAD`，并满足 `≥2%` 或 `≥5 cycles` 中至少一个门槛。
- 只简化代码但未超过性能门槛的改动可标为“等价简化”，不得计入延迟收益。
- 每个优化提交都同时保留修改前后 benchmark 原始结果和环境快照；临时产物写入
  `/home/liuxiang/tmp`，不写入仓库。

### 日志与依赖

- 日志只审计重复字段、无消费者字段及重复采集/计算；error、continuity、reconcile 和订单
  lifecycle 证据不能通过采样或删除来制造收益。
- 新依赖必须沿用现有 CMake/vcpkg 集成，完成 license/维护审查，并在真实 workload 的
  端到端测试中证明收益；不得新增热路径分配、锁、异常或不可控线程。

## 测量口径

最终和中途报告统一拆分：

1. **Aquila userspace**：行情读取、策略决策、SHM enqueue/dequeue、订单编码、WebSocket
   write，以及回报 handler、解析、feedback 发布和状态更新。本轮只对这一层声明代码收益。
2. **本机 kernel/NIC**：socket queue、TX/RX timestamp、softirq、调度和 TCP 状态，独立报告。
3. **WAN/交易所**：完整 Ack RTT、terminal lifecycle 和同交易所时钟字段。未校准跨机器
   clock 时不计算单程网络延迟；Bitget 零成交 IOC 不推导 match-attempt 时间。

## 实施阶段

### 阶段 0：冻结环境与基线

1. 保存 commit、compiler、CMake、Release flags、CPU model/topology、governor/frequency、
   kernel、NUMA、IRQ/softirq、load、后台进程和 affinity 快照。
2. 检查是否存在实盘或重负载任务；benchmark 只使用测试区 CPU。
3. 在 `/home/liuxiang/tmp` 建立相互独立的 baseline/candidate build 和结果目录。
4. 完成 Debug/Release 构建、focused tests 和现有 benchmark inventory。

### 阶段 1：建立等价性与端到端基准

1. 复用现有 Gate/Bitget parser、encoder、OrderSession、gateway、feedback、LeadLag 和 core
   benchmark；补齐缺失的端到端 stage benchmark。
2. 以已录制的 `bin/log/CSV` 构造离线 workload。输入格式以当前 `main` 可读取的 contract
   为准，历史未合并 binary 只提供事件分布，不提供语义基线。
3. 保存逐事件 order intent、wire payload、gateway event、feedback event 和最终订单状态，
   建立规范化后的 golden comparison。
4. 建立两条端到端口径：
   - market event → signal decision → gateway command → encoder/write boundary；
   - response/feedback payload → parser → SHM/runtime → OrderManager/strategy state。

### 阶段 2：非并发结构的反复优化

按 `profile → hotspot → candidate → equivalence → A/B benchmark → review → commit` 循环：

1. common/core：WebSocket、DataReader、订单池、OrderManager、decimal、gateway/feedback SHM。
2. strategy：LeadLag market-state、signal、submit、group、feedback 主路径。
3. Gate：request encode/sign、submit/feedback parse、OrderSession 和 gateway worker。
4. Bitget：request encode/sign、operation/feedback/fast-fill parse、OrderSession 和 gateway worker。
5. 日志字段：只处理经全仓审计确认的重复或不需要字段。
6. 新算法、数据结构或依赖只在 profiler 指向明确热点后评估。

每个保留的优化形成独立原子 commit；性能倒退、行为差异或复杂度无法由收益证明的候选不保留。

### 阶段 3：整链回归

1. 对每个阶段重新运行 focused/full tests、golden replay 和完整 Gate/Bitget 端到端 benchmark。
2. 使用最初冻结的 `main@f595818` baseline 重新做交替 A/B，不累加 microbenchmark 收益。
3. 复查 `core/`、Gate、Bitget 和 LeadLag 的 profiler，记录所有 `≥5%` CPU samples 热点的
   处理结果。

### 阶段 4：线程 / 进程拓扑独立评估

只有前三阶段完成且 profiler 证明同步或交接为主要热点时，从主性能 branch 新建独立
branch/worktree。该阶段可评估 polling、queue layout、memory order、跨核通信、affinity、
thread ownership 和进程边界，但必须额外验证：

- SPSC producer/consumer ownership 和内存可见性；
- event/order 顺序；
- disconnect、queue-full、producer restart、route stopped 和 fail-closed；
- replay、并发 stress 和故障注入结果。

证据不足或收益不满足门槛时整体放弃该 branch。

## 2026-07-19 暂停点

当前工作位于 worktree
`/home/liuxiang/tmp/aquila-gate-bitget-trading-latency`、branch
`perf/gate-bitget-trading-latency`；准确的 HEAD、ahead 和 dirty 状态仍只信
`git status --short --branch`。

本阶段已经接受并提交的生产优化包括：

- `9b09d73 Optimize LeadLag global risk scan`：缓存按 `symbol_id` 顺序初始化的 pair
  runtime，focused benchmark 的 `p50/p99/p99.9` 分别从
  `799/1283/5012.5 ns` 降至 `674.5/708.5/1702 ns`，5/5 组同向。
- `c48b41b Optimize LeadLag open risk reservation scan`：用固定 bitset 维护 reserved-open
  slot，focused benchmark 的 `p50/p99/p99.9` 分别从
  `674.5/708/1586 ns` 降至 `69/73/87 ns`，5/5 组同向；实际配置 paired
  benchmark 的总路径 `p50` 从 `8173 ns` 降至 `7088 ns`，risk stage `p50/p99`
  从 `1125/1880 ns` 降至 `336/664 ns`，相关 strategy tests 88/88 通过。
- `f99dfc0 Optimize LeadLag terminal feedback lookup`：复用 feedback 入口已经找到的
  `StrategyOrder*`，消除 terminal feedback 的重复 order lookup；Gate、Bitget 以及两条
  parser → SHM → runtime 正式 A/B、replay、测试和 review 均通过。
- `f1dc51c Optimize Gate order request tracking`、`788b6c4 Cache Gate order session owner
  thread ID`：分别减少 Gate request tracking 和热路径 thread-id 查询成本；对应 Gate
  submit/response 正式 A/B 均通过。
- `c9aa980 Compile Bitget order request formats`：预编译 Bitget place/cancel request format；
  Bitget submit component 与整链验证通过。
- `51bc4e7 Remove duplicate Bitget send log timestamp`：只删除 Bitget send log 中可由其他
  字段还原的重复 timestamp，保留既有 `NOVA_*` log wrapper。日志格式化和输出在 backend
  thread，本项按用户约束只审计字段必要性与重复性，不运行日志 latency benchmark。
- `d4102fc Skip unused Gate feedback strings`：Gate SBE feedback 对未消费的 var-string
  只验证长度和边界、不再写出 `string_view`。三组 component 中 parser 约改善 `23.9%`，
  session 约改善 `4.9%`，parser → SHM 约改善 `4.3%`；五组完整 Gate parser → SHM →
  LeadLag runtime 的 `p50/p99` 为 5/5 同向，组中位数约改善 `5.0%/4.3%`，
  `p99.9` 为 3/5 同向，因此只声明中心和 p99 稳定收益。

benchmark 和 profile 支撑已提交到 `428c751` 至 `3156ce7`，其中 `3156ce7` 增加 Bitget
order ACK parser → correlation → handler 的完整测量入口。当前证据把 submit、ACK 和
terminal feedback 路径拆到 Gate/Bitget parser、feedback SHM/runtime、OrderManager、
Strategy、ExecutionState 和订单 lifecycle 日志；原始 build/results 均保存在
`/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-builds` 与
`/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-results`。本机
`kernel.perf_event_paranoid=4`，所以没有 `perf` call graph 证据，也没有修改系统设置。

已经拒绝且未保留在生产代码的主要候选包括：

- order price text active-bitset/index 虽显著改善 focused sparse scan，但 Gate/Bitget
  完整 feedback runtime 的尾部回归，候选 diff 仅保存在
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-results/order-price-text-erase-scan/rejected-candidate.diff`。
- 删除 `lead_lag_order_finished` 重复/低价值字段只节省约 `5 ns`，Strategy terminal
  路径反而回归；已完整撤销，当前没有删除生产诊断字段。
- Bitget numeric parser、`code == 0` success-message skip、Gate/Bitget 固定 JSON/literal
  compare 等候选虽然改善 component，但完整 ACK 或 feedback runtime 回归。
- Gate fixed-block unchecked read 在 component 改善 `3%–11%`，Bitget `clientOid` 单次
  `get_string()` 改善 component `1.5%–2.5%`，Bitget order cursor reset 改善 component
  `2.7%–4.3%`；完整 runtime 分别为 `p99 0/2`、`p50/p99/p99.9 0/2`、
  正式 `p50/p99 3/5` 且 `p99.9 1/5`，均已撤销。
- Gate hot literal compare 的 component 改善约 `13%–16%`，但五组正式 runtime 的
  `p50/p99` 只有 2/5 同向，也已撤销。

最新 fresh gprof 位于
`gate-feedback-parser-gprof-after-d4102fc` 与
`bitget-feedback-parser-gprof-after-d4102fc`。Gate 的剩余 `ReadRawOrderFeedbackUpdate`
和必要 string compare、Bitget 的 field traversal / stage1 / numeric decode 均已做过至少
两个最小候选；连续完整 A/B 没有找到新的非拓扑可接受项。当前 worktree 应保持 clean，
准确状态仍只信 `git status --short --branch`。

下一阶段是线程 / 进程拓扑，必须从当前已接受 HEAD 新建独立 branch/worktree，不在
`perf/gate-bitget-trading-latency` 上直接修改。开始前先读取
`docs/runtime_cpu_allocation.md`，冻结当前 Gate/Bitget submit、ACK、feedback runtime
baseline，并明确 owner thread、SHM producer/consumer、CPU affinity、IRQ/softirq 和
memory visibility 边界；不得修改 `kernel.perf_event_paranoid` 或其他系统设置。

## 验证策略

- 修改前：冻结现有 benchmark/golden 输出，证明测量面能观察目标路径。
- 每项修改：focused unit/integration tests、规范化等价对比、5 组交替 A/B benchmark。
- `core/`/SHM/并发修改：增加 queue-full、ordering、owner、memory visibility 和 restart 测试。
- Gate/Bitget 协议修改：encoder payload、parser、correlation、unknown/reject/terminal 测试。
- 最终：Debug/Release build、相关 focused tests、完整 `ctest`、evaluation 边界检查、
  `git diff --check`、完整 replay 和端到端 A/B。

## 回滚

- 每个优化独立提交；不满足等价性或性能门槛时 revert 对应 commit。
- benchmark/evaluation 支撑与生产优化分成可独立审计的提交。
- 线程/进程拓扑使用独立 branch/worktree，可整体丢弃，不污染主性能 branch。
- 新依赖若收益不足，连同 CMake/vcpkg 改动一起回滚。

## 停止条件

满足以下全部条件，或用户明确要求暂时停止：

- Gate/Bitget 两条方向都有 component 和端到端 baseline/final；
- 所有交易主路径 `≥5%` CPU samples 热点已优化或有不可行动证据；
- 连续两轮完整 profile/benchmark 找不到满足验收门槛的候选；
- 拓扑 branch 已明确 merge 或放弃；
- replay、行为、并发和故障验证通过；
- 已相对最初 baseline 给出整体链路前后数据。

用户要求暂时停止时，立即停止新的优化循环，保存当前 branch/commit、已完成 A/B、当前
userspace/kernel/WAN 分层报告和剩余热点，保证后续可恢复。

## 已知风险

- 当前 AWS/KVM 未做完整 `isolcpus/nohz_full/rcu_nocbs`，tail 会混入 IRQ、softirq、RCU、
  timer 和 host deschedule；报告必须包含环境与 MAD，不能把 max 当稳定代码成本。
- 历史 live workload 与 `main@f595818` 可能存在配置或未合并代码差异，必须只复用兼容输入，
  不把历史 binary 当实现事实源。
- 端到端离线 loopback 无法重现真实 WAN/交易所行为，只能证明 Aquila 软件段。
- ABI/data layout 优化可能扩大兼容影响，必须显式 version、全仓 consumer 审计和回归。
