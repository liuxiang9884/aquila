# LeadLag parallel fixed-slot 测试与合并证据计划

## 状态与基线

- 状态：待用户 review；review 完成前不实施本计划中的测试补充、replay 或 benchmark。
- candidate：`feature/lead-lag-parallel-fixed-slot-v4`，PR #13，计划编写前 HEAD 为 `23e872b`。
- production baseline：`main@83c5e12`。
- 当前实现 contract 见 `docs/lead_lag_fixed_ordered_slot_pool_parallel.md`。
- 当前只有自动测试证据；没有 `parallel > 1` replay、fresh benchmark 或真实订单证据。
- 最新 Bitget 12 小时 evidence bundle
  `/home/liuxiang/tmp/20260720_162559_bitget_live_evidence_bundle` 只包含 Bitget canonical
  BookTicker segments，没有 Binance binary，不能单独作为双侧 LeadLag replay 输入。

## 目标

为 PR #13 建立可以审计的 merge gate，证明以下事项：

1. 多个 active execution group 在正常、乱序和故障事件下保持精确归因，不发生跨 group 状态污染。
2. `group_id`、runtime-local `group_index`、slot reuse、`parallel` capacity 和 reconcile contract
   与设计一致。
3. Gate、Bitget order gateway SHM v4 及 LeadLag strategy 的 `group_id` 传播形成无网络、无真实订单的
   端到端证据。
4. report 使用 `(run_id, symbol_id, group_id)` 表达跨运行、跨 symbol 的 group identity，且
   `submitted_v2` 不回退到旧 `parent_id` 语义。
5. replay / signal-only 结果具有确定性，并能区分基础设施正确性与实际策略是否产生重叠 group。
6. `parallel=1` 相对 `main@83c5e12` 没有超过已锁定门槛的热路径回退；`parallel=2/4/8/16`
   的容量和扩展成本有 fresh benchmark 证据。

本计划完成只表示 PR #13 的基础设施达到 merge 条件，不表示 `parallel > 1` 已达到真实订单
production-readiness，也不声明 fillability、PnL 或风险收益改善。

## 非目标

- 不启动真实订单；任何真实订单继续要求当次单独授权和完整 guarded runbook。
- 不把 live fillability、成交率、PnL 或最大安全资金规模作为 PR #13 merge gate。
- 不重新加入 account limiter，不修改现有 risk contract。
- 不把 fanout=4 作为本计划的 live 证据；fanout 只在本地测试中用于验证 children 共享 group。
- 不把 `group_index` 写入 SHM、日志、CSV 或其他稳定格式。
- 不修改当前 checked-in live config 的 `parallel=1`。
- 不在本分支继续修改 Gate/Bitget order formatter，不评估线程或进程 topology。
- 不修改 `kernel.perf_event_paranoid` 或其他系统设置。
- 不在本计划内适配 PR #11 的 BBO fillability、fast-fill 或 slippage 高级分析；这里只锁定新
  identity/schema 的正确归因。

## 已锁定决策

### Merge gate 与 production-readiness 分离

- PR #13 merge gate 覆盖状态机、SHM 集成、report identity、replay/parity 和性能安全。
- 合并后的真实订单测试属于独立 production-readiness 阶段，不能反向成为基础设施 PR 的必要条件。
- real-order 结果会混合策略、市场和基础设施效果，不用于证明基础设施实现本身正确。

### Parallel 阶梯

- `parallel=2`：完整语义基线；两个 group 已足以暴露错归因、乱序和 slot reuse 问题。
- `parallel=4`：第一层扩展行为。
- `parallel=8`：第二层扩展行为。
- `parallel=16`：编译期容量和压力边界，不解释为 live 推荐值。
- 所有容量都必须验证 fail-fast、满容量拒绝、释放后复用和 group identity 不复用。

### 测试方法

- 手写确定性场景锁定每条重要 contract。
- 固定 seed 的 model-based 测试扩大事件排列覆盖；禁止使用随机 seed 产生不可复现的 CI 失败。
- 状态机门通过后才能进入 SHM 集成；所有离线门通过后才能讨论真实订单。

### 性能门槛

- `parallel=1` 与 `main@83c5e12` 使用同机、同 Release flags、固定测试 CPU、交错 A/B。
- 相关热路径动态分配必须为 `0`。
- median 回退不得超过 `5%`；能够稳定测量的 tail 回退不得超过 `10%`。
- `parallel=2/4/8/16` 不要求与 `parallel=1` 等时延，但不得出现分配、异常跃升或不符合实现
  复杂度的超线性增长。

## 核心不变量

每个确定性场景和 model-based step 后按可观察范围检查：

1. `active_group_count <= configured parallel <= 16`。
2. active `group_id` 非零、互不重复；清理后不复用旧 `group_id`，发生 `uint64_t` wrap 时创建失败。
3. `group_index` 可以复用，但 `GroupAt(group_index, group_id)` 只有 index 和 id 同时匹配才返回 group。
4. terminal response / feedback 只允许修改订单记录的精确 group；禁止扫描其他 slot fallback。
5. stale 或伪造的 `(group_index, group_id)` 不得修改当前 slot owner、pending order、持仓、trailing
   price 或 retry counter。
6. mismatch、`UnknownResult` 和 `ContinuityLost` 按各自 contract 进入 reconcile/degraded；新开仓暂停，
   已知持仓的 close / stoploss 能力不被错误移除。
7. 一个 fanout signal 只占一个 parallel slot；children 的 `local_order_id/route_id` 唯一且共享同一个
   `group_id`。
8. group 的 pending orders、累计成交量、signed position、stage 和 terminal 清理结果与已接受的
   authoritative feedback 一致；重复事件保持幂等，至少不得跨 group 修改状态。
9. `parallel_limit` 只在 active group 达到配置容量时拒绝；释放 slot 后可以创建新 group，但新 group
   必须获得更大的 `group_id`。
10. report identity 跨运行使用 `run_id`，运行内跨 symbol 使用 `symbol_id`，不能只按裸 `group_id`
    聚合。

## 风险—证据映射

| 风险 | 主要证据 | 通过条件 |
| --- | --- | --- |
| terminal 错投到其他 group | 确定性场景 + model-based | 只有精确 `(group_index, group_id)` 被修改 |
| slot reuse 后 late event 污染新 owner | reuse/stale terminal 场景 | 新 group 完全不变，pair 进入既定 reconcile 状态 |
| partial/duplicate/out-of-order 导致数量漂移 | feedback permutation 场景 | pending、filled、position 和 stage 与 reference model 一致 |
| 一个 group 故障破坏其他持仓退出 | multi-group UnknownResult/ContinuityLost/stoploss 场景 | 新开仓暂停，其他已知持仓仍可生成正确 reduce-only exit |
| SHM v4 丢失或改写 `group_id` | core + Gate + Bitget worker/integration tests | place/cancel/response 全链路字段 byte/value 一致 |
| report 跨 symbol/run 串组 | analyzer multi-group fixture | `(run_id, symbol_id, group_id)` 归因唯一，无 `parent_id` fallback |
| replay 非确定或 parallel 行为不可观察 | synthetic replay + 双侧 signal-only parity | 相同输入结果一致；容量行为和 group sequence 可解释 |
| 固定 slot 引入热路径回退 | main/candidate A/B benchmark | 满足 allocation、median 和 tail 门槛 |
| 测试只验证自身实现、reference model 同源 | 独立简化模型 + 明确不变量 | model 不调用生产状态转换 helper；逐 step 比较公开结果 |

## 实施阶段

### 阶段 0：冻结输入、环境和 test-first 证据

1. 记录 candidate commit、`main@83c5e12`、compiler、CMake flags、CPU topology、governor、当前负载
   和测试 CPU；临时结果统一写入
   `/home/liuxiang/tmp/lead_lag_parallel_fixed_slot_testing_<timestamp>/`。
2. 为 A/B 建立独立 detached baseline worktree/build；不修改 main worktree，不复用 candidate object。
3. 保存当前 focused tests、Python tests 和 benchmark inventory。
4. 本次是在实现完成后补强证据。新增测试先在 candidate 上表达 contract，再把适用的 test-only patch
   放到 `main@83c5e12` scratch baseline：因 fixed-slot/group metadata 缺失而编译失败或断言失败，作为
   recovered test-first evidence。无法跨 API 编译的用例必须记录具体缺失 contract，不能伪造运行失败。
5. 如果新增测试直接发现 candidate 缺陷，先保存最小失败复现；生产修复必须使用独立原子 commit，
   不得通过放宽断言掩盖问题。

阶段 0 产物至少包括 `environment.txt`、commit/build metadata、现有测试结果和 recovered failure 记录。

### 阶段 1：`parallel=2` 确定性状态机场景

优先扩展 `test/strategy/lead_lag_feedback_state_test.cpp` 和
`test/strategy/lead_lag_strategy_interface_test.cpp`。只有多个测试会共享且具有独立价值时才新建 test
helper；单文件 helper 留在匿名 namespace。

场景矩阵至少包括：

1. 两个 open group 按提交顺序和逆序收到 terminal；filled/cancelled/rejected 分别只影响目标 group。
2. Ack 在 terminal 前后到达、重复 Ack、重复 partial/terminal；不得重复累计或修改其他 group。
3. partial fill → filled、partial fill → partially-cancelled、partial close → retry；两个 group 的数量和
   retry counter 独立。
4. group A close/retry 时 group B 保持 hold；group A stoploss 时 group B 仍能正常 close，反向组合也覆盖。
5. 清理 group A、slot 被 group C 复用后，A 的 late terminal 到达；C 的所有业务状态保持不变。
6. 订单的 `group_index` 指向一个 active slot、但 `group_id` 指向另一个仍 active group；验证禁止扫描
   fallback，并进入 `needs_reconcile`。
7. 一个 group `UnknownResult` 时全 pair 暂停新开仓；已知 position 的 reduce-only close/stoploss 路径保留。
8. `ContinuityLost` 发生在多个 open/hold/close group 并存时，验证 strategy-wide handoff 与 group 状态不被
   伪造为 terminal。
9. active count 达到 `2` 后下一次 open 返回 `parallel_limit`；清理一个 group 后创建的新 group 复用 slot
   但不复用 id。
10. long/short、normal close/stoploss 和 fanout children 的 identity 对称性。

本阶段 pass 条件是场景矩阵全部通过，且每个场景显式检查目标 group、非目标 group、pair recovery state
和 OrderManager retirement 状态。

### 阶段 2：固定 seed model-based 状态机

新建独立 test target（建议
`test/strategy/lead_lag_execution_state_model_test.cpp`），reference model 放在该 test 的匿名 namespace，
不进入 `evaluation/`，除非后续 benchmark 也确实复用它。

1. reference model 只表达 slot ownership、monotonic group id、pending orders、signed position、stage、retry
   和 degraded/open-pause 不变量，不调用生产 `ExecutionState` 的状态转换 helper。
2. event generator 覆盖 create open、submit child、partial、filled、cancelled、rejected、start close、
   close terminal、clear/reuse、duplicate、stale terminal、UnknownResult 和 ContinuityLost。
   Ack 顺序不改变 `ExecutionState`，由阶段 1 的 Strategy/OrderManager 场景单独验证。
3. 使用 checked-in 固定 seed；失败输出 capacity、seed、step 和最小必要事件前缀，保证本地可复现。
4. `parallel=2` 使用最完整 event set；`4/8/16` 使用相同 contract 做容量扩展。初始预算建议每个 capacity
   至少 16 seeds、每个 seed 10,000 steps；若 Debug 时间不可接受，只能减少非关键 valid-event 重复，
   不能删除 stale/mismatch/recovery 类事件。
5. 每一步比较生产状态与 reference model；每个非 degraded seed 结束时用 model-generated terminal/close
   事件 drain，检查没有意外 pending order 或无法解释的 active group。degraded seed 保留既定 handoff 状态，
   不伪造 flat 或恢复结果。
6. focused Debug 通过后，用 `release_asan` 运行该 target；ASAN 结果不替代普通 Debug/Release 结果。

### 阶段 3：Core、Gate、Bitget SHM v4 集成

本阶段不连接交易所、不读取 credentials、不创建真实订单。使用唯一的本地测试 SHM 名称并保证测试退出后
正常释放。

1. 扩展 core SHM/client tests，使用至少两个并发 place request，验证 command sequence、`local_order_id`、
   `parent_id`、`group_id`、route 和 cancel correlation 不串联。
2. Gate 与 Bitget 分别验证：place command → worker/session adapter → Ack/terminal response event 的
   `group_id` 保真；不同 group 的 response 逆序返回时仍匹配原始 `local_order_id/group_id`。
3. 在 `strategy_order_feedback_shm_integration_test` 或边界更清楚的新 integration target 中，连接
   OrderManager/Strategy 与隔离 feedback SHM，注入两个 group 的乱序 authoritative feedback。
4. 验证 `group_index` 只存在于本地 `StrategyOrder`，SHM payload/event 和结构化日志不新增该字段。
5. Gate/Bitget smoke/probe 的 group id 测试继续保留；smoke 只运行本地 scripted transport 模式，不使用
   production endpoint。

本阶段至少覆盖以下现有 target：

- `core_order_gateway_shm_types_test`
- `core_order_gateway_shm_test`
- `core_order_gateway_client_test`
- `gate_order_gateway_worker_test`
- `bitget_order_gateway_worker_test`
- `strategy_order_feedback_shm_integration_test`
- `lead_lag_feedback_state_test`
- `lead_lag_strategy_interface_test`

### 阶段 4：Report identity 与 schema fixture

主要修改 `scripts/test/lead_lag/analyze_order_detail_test.py`；只有 report 汇总也消费新 fixture 时才同步扩展
`generate_live_report_test.py`。

构造一个完全本地的 synthetic log，至少包含：

1. 同一 run 中两个 symbol 都从 `group_id=1` 开始。
2. 同一 symbol 的 `group_id=1` 和 `group_id=2` 同时 active，并以反序完成。
3. 一个 group 下两个以上 fanout child，具有不同 `local_order_id/route_id`。
4. open、partial、terminal、exit/retry 和 finished 行都带新 `group_id`。
5. 另一个 run 重复相同 `symbol_id/group_id`，证明 run identity 隔离。
6. 混入只有 `parent_id`、没有 `group_id` 的 legacy submitted 行，确认 analyzer 忽略该行。

通过条件：

- 正常 submitted 行全部标记 `source_schema=submitted_v2`。
- order/latency/execution/position 关联不跨 `(run_id, symbol_id, group_id)`。
- fanout children 保持独立 order rows，但共享同一 group identity。
- legacy submitted 不生成伪造的新 schema order，也不把 `parent_id` 填入 `group_id`。
- 本阶段不改变 PR #11 高级分析的统计定义。

### 阶段 5：Replay、parity 与 signal-only

本阶段分成确定性 replay 和真实行情 signal-only 两层；两者都禁止 `--execute`。

#### 5.1 Synthetic dual-feed replay

1. 分别生成最小 Gate lag + Binance lead、Bitget lag + Binance lead typed BookTicker trace，稳定触发两个以上
   重叠 group、normal close、stoploss 和 `parallel_limit`。
2. 对 `parallel=1/2/4/8/16` 各运行两次 replay；相同 config/input 的核心交易意图和 group sequence 必须一致。
3. `parallel=1` 与 `main@83c5e12` 比较 action、side、price、quantity、reduce-only 和事件顺序；新 schema
   identity 字段单独比较，不能用它制造旧 baseline mismatch。
4. `parallel=2` 必须实际观察到两个 active group；`4/8/16` 的 synthetic trace 必须填满对应容量并验证下一次
   open 被 `parallel_limit` 拒绝。

#### 5.2 双侧行情 signal-only parity

1. 先查找 latest run 原始 Binance recorder segments；当前 evidence bundle 内没有该输入，不能把 Bitget-only
   segments 与不同时段 Binance 数据拼接。
2. 如果无法找到同时间窗双侧数据，按 `docs/lead_lag_live_replay_testing.md` 新做隔离的双侧 recorder +
   signal-only capture；不连接 order gateway/feedback，不使用 `--execute`。
3. recorder 必须满足 typed binary、`skipped=0`、`overruns=0`；live/replay 核心 intent parity 按现有文档判定。
4. 记录每个 symbol 的最大 `active_group_count`、group 生命周期和 `parallel_limit`。未自然出现重叠 group 不让
   merge gate 失败，因为 synthetic replay 已验证基础设施；但该 symbol 不进入后续 live 候选。
5. signal-only 产物全部写入 `/home/liuxiang/tmp`，不写入 canonical report/bin 目录。

### 阶段 6：Fresh performance A/B

baseline 与 candidate 使用相同 compiler、Release flags、benchmark source、CPU 和参数。若为补充
parallel 参数而修改 benchmark harness，必须把相同 harness 以 test-only 方式应用到 baseline，禁止比较两个不同
测量边界。

覆盖：

- `lead_lag_group_container_benchmark`
- `lead_lag_strategy_benchmark` 的 active tick/no-signal 路径
- `lead_lag_runtime_benchmark` 的 open submit 路径
- `lead_lag_feedback_runtime_benchmark` 的 terminal feedback / exact group apply 路径

执行要求：

1. 使用 `docs/runtime_cpu_allocation.md` 的测试 CPU，设置 `TMPDIR=/home/liuxiang/tmp`；不占用实盘 CPU。
2. 至少 5 组交错 A/B，每组至少 10 repetitions；保存 JSON/raw stdout、环境快照和汇总脚本输出。
3. `parallel=1`：allocation `0`；median 回退 `<=5%`；只有 benchmark 能提供可靠 percentile/distribution
   时才应用 tail `<=10%`，不能把 Google Benchmark aggregate 最大值冒充 p99。
4. exact `GroupAt`/terminal apply 预期 O(1)：`parallel=16` 的 median 若比 `parallel=2` 高出 `>20%`，先按
   cache、benchmark boundary 和噪声复核；五组中至少四组仍超出则本阶段失败。
5. active traversal/clear/insert 允许 O(n)：相邻 `4→8`、`8→16` 的 median ratio 以 `2.2` 为调查线；五组
   中至少四组持续超过且无法由额外固定工作解释，则视为非预期超线性并失败。
6. 任一热路径出现动态分配直接失败；性能门失败时不进入 merge 完成结论。
7. `perf` 因 `kernel.perf_event_paranoid=4` 不可用；记录该限制，不修改系统设置，也不声明 cycles/callgraph
   证据。

### 阶段 7：Final review、回归与 merge handoff

1. 对照本计划逐项建立 requirement → test/result 映射，检查完整 diff、错误路径和 reconcile/exit 能力。
2. 运行 affected Debug/Release build、focused C++ tests、LeadLag Python tests、ASAN model test、replay/parity、
   benchmark 和文档边界检查。
3. 尝试全量仓库 build/test。若仍被既有 `third_party/websocket/websocket.h` 缺失阻断，必须同时记录
   baseline/candidate 的同类失败和已通过的 affected target，不能宣称全量 build 通过；该外部 blocker 是否阻止
   merge 在最终 review 时单独列出。
4. 检查所有 checked-in live config 仍为 `parallel=1`，没有 account limiter、fanout 或真实订单范围变化。
5. 每个阶段保持独立原子 commit；测试发现的生产修复单独提交，不与 report/benchmark/documentation 混合。
6. 把仍有效的 contract、验证命令和证据摘要迁移到
   `docs/lead_lag_fixed_ordered_slot_pool_parallel.md`，onboarding 只保留摘要和索引，然后删除本完成态 plan。
7. 更新 PR #13 中文正文，push branch；只有全部 merge gate 达成后才建议合并。

## 建议验证命令

实际 target 可在实施时按 CMake inventory 补充，但不能缩小上述证据范围。

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --parallel 8 --target \
  lead_lag_feedback_state_test \
  lead_lag_strategy_interface_test \
  lead_lag_execution_state_model_test \
  strategy_order_feedback_shm_integration_test \
  core_order_gateway_shm_types_test \
  core_order_gateway_shm_test \
  core_order_gateway_client_test \
  gate_order_gateway_worker_test \
  bitget_order_gateway_worker_test

ctest --test-dir build/debug --output-on-failure -R \
  '^(lead_lag_feedback_state_test|lead_lag_strategy_interface_test|lead_lag_execution_state_model_test|strategy_order_feedback_shm_integration_test|core_order_gateway_shm_types_test|core_order_gateway_shm_test|core_order_gateway_client_test|gate_order_gateway_worker_test|bitget_order_gateway_worker_test)$'

python3 -m unittest discover -s scripts/test/lead_lag -p '*_test.py'

TMPDIR=/home/liuxiang/tmp cmake --build build/release --parallel 8 --target \
  lead_lag_group_container_benchmark \
  lead_lag_strategy_benchmark \
  lead_lag_runtime_benchmark \
  lead_lag_feedback_runtime_benchmark

rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
rg -n 'parallel\s*=\s*([2-9]|1[0-6])' config
git diff --check
```

两个 evaluation 边界命令和 live config 命令都期望无命中。benchmark 具体 filter、CPU、repetitions 和 JSON
输出路径在阶段 0 冻结后写入 run metadata，不在仓库内生成临时结果。

## 原子提交边界

建议按以下最小闭环提交：

1. 本 plan。
2. 确定性状态机测试。
3. model-based test target。
4. core/Gate/Bitget/Strategy SHM integration 测试。
5. report multi-group fixture。
6. replay fixture/runner 或仅测试工具改动。
7. benchmark harness 改动；原始结果不提交到仓库。
8. 每个实际生产缺陷修复各自独立提交。
9. 完成态事实源更新与 plan 删除。

每个提交都只暂存本任务拥有的文件或 hunk。某阶段没有代码改动时只保存外部证据，不制造空提交。

## 停止条件

- 任一状态机不变量失败：停止，不进入 SHM/replay/benchmark。
- reference model 与生产结果不一致：先确认模型 contract；不得通过让模型调用生产 helper 消除差异。
- SHM `group_id` 丢失、错配或 `group_index` 泄漏：停止并按协议/ABI 缺陷处理。
- report 发生跨 run/symbol/group 串联或 legacy fallback：停止，不生成合并建议。
- 缺少双侧 replay 输入且 fresh signal-only capture 也无法完成：标记 replay gate blocked，不以 Bitget-only
  bundle 替代。
- `parallel=1` 超过性能门或任一热路径出现动态分配：停止合并建议，先定位回退。
- 任何步骤意外需要 credentials、production endpoint 或 `--execute`：立即停止；本计划不包含该授权。

## 回滚

- 测试、fixture、runner 和 benchmark harness 分阶段提交，可逐提交回退，不改变外部账户状态。
- 测试发现的生产修复必须保持独立 commit；若修复无法在本分支证明正确，回退修复并保留失败证据，
  PR #13 不进入 merge-ready。
- SHM v4 producer/consumer 仍要求一起升级并重启；本计划不引入 v3/v4 混跑兼容。
- 完成本计划后按 onboarding 规则把长期事实迁移到领域文档并删除完成态 plan，避免双重事实源。

## 合并后 production-readiness 边界

本节只记录下一阶段约束，不构成真实订单计划或授权。

- 先用 replay/signal-only 选择能自然产生重叠 group 的 symbol，再决定 symbol count 和 duration。
- 首个 real-order 候选限定 Bitget、`fanout=1`、`parallel=2`、每个 group
  `open_notional <= 10 USDT`。
- 总暴露使用现有 strategy risk 配置约束，不重新加入 account limiter。
- 任何 `UnknownResult`、`ContinuityLost`、group mismatch、unresolved order、非 flat 或 guard 异常都停止
  本轮并进入 stop-and-flat/handoff。
- 最终必须由 REST open orders/positions/open orders 复核证明 flat。
- 具体 symbols、数量、duration、总风险上限和命令必须另写当次 guarded live plan，并获得用户单独授权。

## 剩余风险

- 当前 12 小时 bundle 缺少 Binance binary；找到原始同时间窗输入或 fresh 双侧 capture 前，market replay
  证据不完整。
- model-based reference model 可能复制实现假设；必须保持模型简化且不复用生产状态转换代码。
- Google Benchmark 不天然提供单次调用 p99；只有现有 latency distribution/counter 能可靠给出 tail 时才应用
  tail 硬门。
- 本机共享负载、频率和 cache 状态可能影响 5% 门；交错 A/B、固定 CPU 和五组一致性用于控制噪声，不能用
  单次结果下结论。
- signal-only 未观察到重叠 group 不表示基础设施错误，但表示还不能选择该 symbol 做后续 live。
- PR #11 必须后续适配新 schema；本计划通过不等于 PR #11 已兼容。
- 既有完整 build 可能继续被缺失 websocket header 阻断；必须如实区分环境 blocker 与本分支回归。
