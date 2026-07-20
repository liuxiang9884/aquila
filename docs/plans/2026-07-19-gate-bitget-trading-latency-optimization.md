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

该暂停点的 Gate 第 5 组、Bitget、parser → SHM → runtime、测试、replay 和 review
已经完成。后续 numeric request 工作位于 worktree
`/home/liuxiang/tmp/aquila-gate-bitget-order-request-format-e2e`、branch
`perf/gate-bitget-order-request-format-e2e`；准确的 HEAD、ahead 和 dirty 状态仍只信
`git status --short --branch`。

本阶段已经接受并提交的生产优化包括：

逐项正式 A/B、链路累计口径和所有未采用候选的完整汇总见
`docs/gate_bitget_trading_latency_optimization_report.md`。

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
- `30e27fe Compile Gate order request formats`：在保留现有 text request contract 和
  byte-exact wire 的前提下预编译 Gate 固定 JSON format。五组正式 A/B 中
  GatewayWorker `p50/p99` 从 `867.5/1188 ns` 降至 `736.5/1046.5 ns`，改善
  `15.10%/11.91%`，均为 5/5 同向；`p99.9` 只有 3/5 同向，不声明稳定收益。
- `bcdc358 Move order decimal formatting into sessions`：Strategy、Gateway、SHM 和
  OrderSession 共享 numeric `OrderPlaceRequest`，price/quantity text 只在 session
  生成；五组 no-log 同起止点 A/B 中 Gate/Bitget SHM 整链 `p50` 分别改善
  `12.90%/14.33%`，direct 整链分别改善 `11.15%/11.73%`，四项均为 5/5 同向。

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

最新 fresh profile 除 feedback 与 Gate compiled-format 证据外，还包括 numeric request
完成态的 `final-order-session-profile`。其中 Gate/Bitget EncodePlace 约为
`121/113 ns`，剩余 `parse_format_string` 主要来自 local order ID formatter，下一步先在
当前非拓扑 branch 上独立 screening。当前状态仍只信 `git status --short --branch`。

非拓扑热点完成后，线程 / 进程拓扑必须从已接受 HEAD 新建独立 branch/worktree。开始前
先读取 `docs/runtime_cpu_allocation.md`，冻结 Gate/Bitget submit、ACK、feedback runtime
baseline，并明确 owner thread、SHM producer/consumer、CPU affinity、IRQ/softirq 和
memory visibility 边界；不得修改 `kernel.perf_event_paranoid` 或其他系统设置。

## 2026-07-20 数值订单 request 与 OrderSession 格式化

### 背景与目标

用户在进入线程 / 进程拓扑阶段前追加一个非拓扑结构优化：订单价格和数量在 Strategy、
OrderManager、Gateway、SHM 与 OrderSession 之间只保存 `double`，并携带来自 instrument
catalog 的 decimal places；价格和数量文本只在各交易所 OrderSession 使用 fmtlib 写入最终
JSON 固定缓冲区时生成。

本阶段以 `f302f6e` 的生产代码为 before 基线。计划文档提交不改变该代码基线。完成或撤销
本阶段后再进入独立拓扑 branch/worktree，不把结构变化和线程 / 进程变化放在同一组 A/B 中。

目标包括：

- 删除 Strategy 长生命周期 `price_text` / `quantity_text` 及其 buffer；
- Direct 与 SHM 使用完全相同的 `OrderPlaceRequest` / `OrderCancelRequest`；
- 删除 Gateway payload、OrderSession request 和临时 `StrategyOrder` 之间的重复转换；
- 保持 Gate / Bitget wire JSON、订单状态、route、correlation、feedback 和风险结果完全等价；
- 分别量化 Strategy submit、Gateway direct/SHM、Gate/Bitget OrderSession 及完整本机 submit
  链路的 before/after 延迟和 profile 变化。

### 已锁定 contract

1. `OrderPlaceRequest` 是唯一 place 业务 request，至少包含：
   - `local_order_id`、`parent_id`；
   - `double price`、`double quantity`；
   - `char symbol[32]` 与 `symbol_size`；
   - `price_decimal_places`、`quantity_decimal_places`；
   - `symbol_id`、`exchange`、`side`、`order_type`、`time_in_force`、`reduce_only`、
     `gateway_route_id`。
2. `OrderCancelRequest` 是唯一 cancel 业务 request，只携带 local identity、correlation 和
   route；不携带 symbol、price、quantity 或 decimal places。OrderSession 继续用
   `local_order_id` 查询自己的 exchange-order-id cache，cache miss 时沿用 Gate order text /
   Bitget `clientOid` fallback。
3. `StrategyOrder` 保存一份 immutable `OrderPlaceRequest` 和 lifecycle / fill / timing 状态，
   不重复保存 request 字段。
4. `price_decimal_places` / `quantity_decimal_places` 来自对应 `lag_exchange + symbol` 的
   instrument catalog row，经 `InstrumentInfo` 和 `PairConfig::lag_instrument` 复制到 place
   request；不得根据 `double`、`price_tick` 或 `quantity_step` 在下单热路径重新推导。
5. Strategy 和 request builder 保证 price、quantity、decimal places 及 scaled value 安全；
   本阶段不新增 `finite`、正数、decimal range 或 scaled-range 检查。现有 session readiness、
   inflight、状态机、queue、JSON buffer 和发送错误处理保持。
6. OrderSession 是唯一 price/quantity wire text 生成边界。实际 profile 证明动态
   `"{:.{}f}"` 明显慢于 integer-scaled fixed-decimal writer，因此当前实现先在栈上
   32-byte buffer 生成 decimal view，再用 `FMT_COMPILE` 的 `fmt::format_to` 直接写最终
   JSON buffer；不生成 `std::string`，也不让 text 生命周期离开单次 encoder 调用。
7. SHM 只增加 command sequence、enqueue timestamp 和 kind 等传输 envelope；place/cancel
   payload 是上述 exact request。`OrderGatewayShmVersion` 随 ABI 修改提升。
8. `StrategyOrder` 不放入 SHM，OrderSession 也不从 SHM 按 local ID 查询完整订单。

### 非目标

- 不改变 tick/step rounding、下单价格、数量、side、TIF、reduce-only、route 或订单状态机。
- 不把 `double` 扩散成 market-data、持仓或持久化格式的全仓迁移；范围只覆盖订单 command
  contract 及现有 fill/lifecycle 中已经使用 `double` 的字段。
- 不新增真实订单、拓扑、同步、affinity、日志字段或依赖。
- 不因 focused fmt benchmark 改善而直接声明整链收益。

### 修改前证据与 baseline

在修改生产 contract 前完成以下工作：

1. 从 `f302f6e` 建立只读 detached baseline worktree/build，candidate 使用当前专用 worktree；
   两边使用相同 compiler、Release flags、CPU、affinity、benchmark 参数和环境。
2. 保存当前 Gate/Bitget place/cancel wire golden，覆盖：
   - price/quantity decimal places 为 `0`、常用值和 catalog 最大值；
   - buy/sell、reduce-only、不同 TIF；
   - Gate integer/quoted size 两种 session 配置；
   - exchange-order-id cache hit/miss cancel。
3. 冻结下列现有测量面；如果现有入口未覆盖数值准备到 write boundary，则先在 benchmark
   target 中补齐共享测量入口：
   - `lead_lag_submit_breakdown_benchmark`：decimal prepare、OrderManager、Gateway client、
     actual-config risk-on submit；
   - `strategy_order_gateway_benchmark`：place 和 cancel；
   - `gate_order_session_benchmark`：encode、OrderSession、StrategyContext、GatewayWorker；
   - `bitget_order_session_benchmark`：encode、OrderSession、StrategyContext、GatewayWorker。
4. 至少保留三种口径：
   - Strategy 已得到 raw decision 后，到 request 被 Gateway 接收；
   - Gateway request 到 OrderSession write boundary，分别测 direct 和 SHM worker；
   - LeadLag 数值计算 / quantization 到 Gate/Bitget JSON write boundary的完整本机 submit 链。
5. `perf` 因 `kernel.perf_event_paranoid=4` 不可用且不修改系统设置；before/after profile 使用
   当前可用的 gprof/gprofng instrumentation，并保存 flat profile 和环境快照。

所有原始产物写入
`/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-results/order-request-double-format/`，
baseline/candidate build 写入
`/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-builds/order-request-double-format/`。

### Test-first 等价性证据

生产修改前先增加能表达目标 contract 的测试或最小复现，并记录其在 before 代码上因
`OrderPlaceRequest`、decimal places 或 exact-request SHM contract 缺失而失败。测试至少包括：

- `OrderPlaceRequest` / `OrderCancelRequest` standard-layout、trivially-copyable 和 SHM layout；
- instrument decimal places 进入 LeadLag place request；
- Gate/Bitget 从 double + decimal places 生成 byte-exact place JSON；
- Direct 与 SHM worker 对相同 request 生成相同 JSON；
- cancel 只依赖 local ID、parent/route correlation 和 session exchange-order-id cache；
- place/ack/reject/cancel/terminal feedback 后 `StrategyOrder` 状态与 before golden 一致；
- LeadLag open/close/retry/stoploss 的 price、quantity、risk reservation 和 lifecycle log
  事实不变。

### 七个实施部分

1. **OrderPlaceRequest**
   - 在 `core/trading/order_types.h` 定义 raw `char symbol[32]`、double price/quantity 和
     decimal places 的唯一 POD request；
   - 不引入 `OrderSymbol`、`OrderDecimal` 或 text/string price/quantity 字段。
2. **OrderCancelRequest**
   - 定义只含 cancel 必需 identity/correlation/route 的 POD request；
   - Gate/Bitget session 的 local-to-exchange ID cache 与 fallback 语义保持。
3. **StrategyOrder**
   - 组合一份 immutable `OrderPlaceRequest`；
   - 保留 exchange order ID、status、fill、timing、finish/reject 等 lifecycle 状态；
   - 调整 OrderPool 的 local ID 写入 / lookup 边界，不在 `StrategyOrder` 再复制 request 字段。
4. **Strategy 与 OrderManager**
   - Strategy 从 `lag_instrument` 复制 symbol、price/quantity 和 decimal places；
   - OrderManager 分配 local ID、保存 request、构造最小 cancel request；
   - 删除 text lifetime 校验与 `OrderPriceTextStorage` 依赖。
5. **Gateway 与 SHM**
   - Direct Gateway API 只接受两个 exact request；
   - SHM command 使用 trivially-copyable union payload，place/cancel 不再共享一个包含无用字段
     的大结构；
   - worker 直接把 payload 交给 OrderSession，删除 `FillOrderCommand`、`CopyText`、
     `TextFieldsInBounds` 和 `ToStrategyOrder`；
   - 更新 SHM version、layout tests、queue-full、ordering、route 和 restart 覆盖。
6. **Gate / Bitget OrderSession**
   - API 改为 exact `OrderPlaceRequest` / `OrderCancelRequest`；
   - encoder 直接接收 request 加 timestamp/request-ID/connection state 等 session scalar，
     删除重复的 `PlaceOrderEncodeFields` / `CancelOrderEncodeFields`；
   - Gate 保留 signed size 和 quote-size 分支，Bitget 保留 side token 和 quoted number；
   - 两边在 OrderSession encoder 内把 double 转成 integer-scaled fixed decimal view，
     再用 `FMT_COMPILE` 直接写最终 JSON buffer；动态 fmt precision 候选只保留 profile
     证据，不进入生产。
7. **LeadLag risk reservation**
   - 将 `OrderPriceTextStorage` 缩减并重命名为只保存 local ID、reserved quantity/notional 和
     active state 的 risk slot；
   - 保留已接受的 active bitset/risk scan 优化，不恢复全表扫描；
   - 删除所有 price/quantity text acquire/release 和生命周期绑定。

### 原子提交边界

计划按以下可独立审计的闭环提交，不把 baseline 原始输出提交进仓库：

1. 新 contract 的 golden/layout/benchmark 支撑；
2. core request、StrategyOrder/OrderManager 与 Gateway/SHM exact-request 迁移；
3. Gate/Bitget OrderSession 与 encoder double-format 迁移；
4. LeadLag request builder 和 risk reservation storage 迁移；
5. 最终等价性、性能结果和专题报告更新。

如果中间提交无法保持构建和行为闭环，则合并相邻实现提交，但测试必须与首次可运行实现
处于同一原子提交。

### 验证与性能比较

功能验证：

- Gate/Bitget encoder、OrderSession、gateway worker、runtime adapter 和 multi-session tests；
- core OrderPool、OrderManager、StrategyContext、Gateway client/SHM/layout tests；
- LeadLag config、strategy interface、feedback/runtime、replay 和 live-strategy 离线 tests；
- Debug/Release build、相关 focused `ctest`、完整 `ctest`、`git diff --check`；
- evaluation include/link 边界检查；
- 规范化 wire payload、order events、feedback events 和最终 StrategyOrder replay 对账。

性能验证：

- 先做 component screening，但采用结论只看 direct/SHM 和完整 submit 链；
- baseline/candidate 至少 5 组交替 A/B，每组至少 10 repetitions，固定测试 CPU；
- 分别报告 Gate、Bitget 和 LeadLag 的 `p50/p99/p99.9`、MAD、方向一致组数和 profile hotspot；
- 对 OrderManager/request copy、SHM enqueue/dequeue、worker dispatch、double formatting 和最终
  write boundary 单独归因，防止把成本从 Strategy 移到 OrderSession 后误报为收益；
- 最终累计比较使用 `f302f6e` before 与完成态 candidate，不累加各 component 百分比。

### 采用、撤销与剩余风险

- byte-exact wire、订单状态、risk、replay 或错误语义任何一项不等价，立即撤销对应最小提交；
- Gate 或 Bitget 任一完整 submit 链显著回归时，不保留两套不一致的 core request contract；
  先对该交易所 formatter 做一次最小、等价优化，仍不通过则整体撤销本阶段生产修改；
- 满足原计划性能门槛时登记为延迟优化；未超过门槛但所有核心指标位于噪声带、并显著删除
  text lifetime 和重复 payload 时，只能登记为“等价结构简化”，不得宣称延迟收益；
- `p99.9` 方向不稳定时单独报告，不用 p50 改善掩盖 tail；
- double 动态 precision、SHM ABI/version、OrderPool identity、session cancel cache 和现有
  tools/tests request builder 是主要 review 风险；
- 本阶段完成后继续 profile；不能因为 request 重构通过就停止整个 Gate/Bitget 优化目标。

### 2026-07-20 同起止点测量修正

首次 screening 的分段入口不能单独支持整体撤销结论：

- baseline `StrategyContext` benchmark 直接接收已经带有 price/quantity text 的 request，
  没有计入 LeadLag 的 `double → OrderDecimal/text`、text slot 和相关复制；
- `GatewayWorker` benchmark 在 SHM command 预填后开始计时，同样只覆盖 dequeue 到 write；
- LeadLag actual-config benchmark 覆盖计算、text 准备和 SHM enqueue，但不运行真实
  Gate/Bitget OrderSession formatter。

因此，OrderSession / StrategyContext / GatewayWorker 的局部回退只能用于归因，不能直接
否决把 text 格式化从 Strategy 移到 OrderSession 的整体方案。重新判定必须在 baseline 与
candidate 两侧使用相同起点和终点：

1. 起点为 LeadLag 已得到用于下单的 `double` price/quantity，且尚未生成 text；
2. 同一计时区间覆盖 request 构造、OrderManager、Gateway direct 或 SHM
   enqueue/dequeue、Gate/Bitget OrderSession、最终 JSON encode 和 counting transport
   write；
3. baseline 必须在计时区间内执行现有 `OrderDecimal/text` 准备，candidate 必须在同一位置
   只传递 double 与 decimal places；
4. Gate 与 Bitget 分开报告 direct 和 SHM 路径，采用结论只看这些同起止点结果；旧分段
   benchmark 继续用于解释成本移动；
5. 新一轮 baseline 使用当前已接受 Gate `FMT_COMPILE` 的 `d829337`，candidate 必须保留
   该优化，不再以 `f302f6e` 作为当前生产对照。

在新测量完成前，下面的首次 screening 结果只表示“分段方向相反、整体结果未证明”，不再
作为 numeric request contract 已经性能失败的最终证据。

### 执行结果（2026-07-20）

七个实施部分已完整实现并完成 component screening、focused/full tests、typed replay 和
gprofng profile。以下是首次分段 screening 的历史结果，已由同起止点测量取代：

- Gate 在对 formatter 增加 `FMT_COMPILE` 后，place encoder 从约 `268.8 ns` 降至
  `234.2 ns`；完整 OrderSession / StrategyContext / GatewayWorker 的 `p50` 仍分别回归
  `6.3%/4.1%/3.2%`。
- Bitget 原本已经预编译固定 JSON format；place encoder 从约 `125.3 ns` 回归至
  `210.2 ns`，完整三层 submit 的 `p50` 回归 `17.9%/15.9%/11.3%`。
- LeadLag actual-config submit `p50` 改善约 `6.0%`，但收益来自删除 text lifetime 和缩小
  SHM/client 部分，不能覆盖 Gate/Bitget OrderSession 的 `double → fixed decimal` 成本。
- 首版生产 contract 修改曾完整撤销；当时没有进入正式五组 A/B，也没有改变 SHM ABI。
  由于缺少同起止点整链证据，这次撤销不是最终性能判定。
- 首版候选 patch 和历史 baseline/candidate/profile/replay 证据保存在
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-results/order-request-double-format/`。

撤销后先接受独立 Gate fixed-format 优化 `30e27fe`，再以 `d829337` 为 baseline 建立
同起止点 E2E benchmark，并重新实现完成态：

- benchmark 从 Strategy 已得到 double、尚未生成 text 开始，覆盖 request 构造、
  OrderManager、direct 或 SHM handoff、OrderSession encode 和 counting write；
- 正式 benchmark 不产生业务日志：logger level 为 `critical`，成功路径的 `NOVA_INFO`
  在 frontend 过滤；
- 五组 no-log SHM A/B 中 Gate full `p50` 从 `2151.5` 降至 `1874 ns`，改善
  `12.90%`；Bitget 从 `2013.5` 降至 `1725 ns`，改善 `14.33%`，均为 5/5 同向；
- 五组 direct A/B 中 Gate/Bitget full `p50` 分别改善 `11.15%/11.73%`，均为 5/5
  同向；
- Strategy、SHM handoff、OrderSession 三段的 `p50` 都下降，证明收益不是把 text 成本
  从 Strategy 转移到 session；SHM command 从 168 bytes 缩小到 104 bytes；
- Gate direct `p99.9` 与历史 tail 方向不稳定，因此不声明稳定 tail 收益；
- typed replay CSV byte-identical，最终代码作为 `bcdc358` 接受。

详细 A/B、MAD、tail 边界、profile、测试和 replay 证据见
`docs/gate_bitget_trading_latency_optimization_report.md`。

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
