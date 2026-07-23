# LeadLag FixedOrderedSlotPool 与 parallel=n

本文记录 LeadLag multi-group execution 的当前容器、标识和故障语义。策略信号和持仓语义见
`strategy/lead_lag/README.md`，order gateway 的通用 contract 见 `docs/strategy_order_component_model.md`。

## 当前实现

- 每个 pair 的 `ExecutionState` 使用
  `FixedOrderedSlotPool<ExecutionGroup, kMaxLeadLagExecutionGroups>`，编译期上限为 `16`；slot payload
  在 Strategy 初始化时按 effective `execute.parallel` 一次性分配，初始化后不扩容、不分配容器内存。
- `ExecutionGroup` 按 64-byte cache line 对齐。生产 feedback 路径保持独立的 code alignment，
  `ApplyFinishedOrder` 保持独立 cold-to-hot boundary；这些布局是 `parallel=1` A/B 性能门的一部分，
  不能只凭源码结构移除，修改后必须重跑 production-like benchmark。
- `execute.parallel` 表示同一个 pair 同时允许的 active execution group 数。`0` 无效；配置或
  programmatic 值大于 `16` 时，Strategy 输出 `lead_lag_parallel_clamped` warning，并把内部
  effective value clamp 为 `16`。generic pool 的编译期硬上限仍防止分配超过 16 个 slots。
- `execute.order_session_fanout` 表示一个 signal 最多使用的 ready order route 数。fanout child 共享同一个 execution group，但分别拥有唯一 `local_order_id` 和各自的 `route_id`；一个 fanout signal 只占一个 parallel slot。
- active group 按创建顺序遍历；erase 后 slot 可以复用，但 active FIFO 顺序保持不变。生产调用方通过 active traversal helper 或精确 slot lookup 访问，不暴露 vector-like `groups()` / `mutable_groups()` API。

## Execution group 标识

- `group_id` 是 pair runtime 内从 `1` 开始单调递增的 execution group identity。稳定归因键是 `(symbol_id, group_id)`；跨运行还必须加 run/session identity。
- `group_id=0` 是未归组 / 未分配的哨兵值。LeadLag 创建的 execution group 和真实订单必须使用非零
  `group_id`；没有 group 语义的通用 producer 可以显式保留 `0`，gateway 不用 `local_order_id`
  或其他字段自动补写。
- `group_index` 是 `FixedOrderedSlotPool` 的 runtime-local slot index，只存在于 `StrategyOrder` 本地元数据中，用于 terminal response / feedback 的 O(1) 定位。它不进入 SHM、日志、CSV 或其他持久化格式。
- 下单时 LeadLag 把 `group_id` 写入 `OrderPlaceRequest`，把 `group_index` 通过 `OrderLocalMetadata` 写入 `StrategyOrder`。order gateway SHM v4 原样传播 `group_id`，Gate、Bitget 和诊断工具均保留该字段。
- SHM v4 只保留一个跨 strategy / gateway / exchange response 的归组字段 `group_id`，不再携带独立
  `parent_id`。fanout child 由唯一 `local_order_id` 和各自的 `route_id` 区分；同一 execution group
  的 open、close、stoploss、retry 和 fanout child 共享 `group_id`。新日志和 CSV 不输出
  `parent_id` 兼容别名，旧日志中的该字段只作为 legacy schema 事实保留。

## Terminal feedback 与 slot reuse

response / feedback 先按 `local_order_id` 找到 `StrategyOrder`，再执行
`GroupAt(order.group_index, order.place_request.group_id)`：

1. slot occupied 且 `group_id` 相同，才允许修改 execution group；
2. slot 已空或已被其他 group 复用时，禁止扫描 fallback，也禁止把 terminal event 应用到其他 group；
3. mismatch 会输出 `lead_lag_order_group_mismatch`，将该 pair 标记为 `needs_reconcile` 并暂停新开仓；既有 active group 和 position 保留，close / stoploss 仍可继续执行；
4. terminal `StrategyOrder` 仍按自身生命周期 retire，避免损坏本地订单容量，但被保护的 execution group 不因该回报被清理或改写。

`group_id` 计数器发生 `uint64_t` wrap 时返回创建失败，不复用旧 id。slot index 可以复用，因此任何只校验
`group_index` 的实现都不符合当前 contract。

## 2026-07-23 离线验证证据

本节证据基于 candidate `feature/lead-lag-parallel-fixed-slot-v4` 和 production baseline
`main@87bdc08`。所有临时产物均位于 `/home/liuxiang/tmp`，没有连接 order gateway、没有使用
credentials，也没有发送真实订单。

### 状态机、SHM、report 与 sanitizer

- affected C++ Release suite 等价 `19/19` 通过；核心跨边界 9 个 target 在 Debug/Release 均
  `9/9` 通过，覆盖 LeadLag feedback/model、Strategy/OrderManager、core SHM client 以及 Gate/Bitget
  gateway worker。
- `lead_lag_execution_state_model_test` 在 `release_asan` 下使用
  `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1` 通过。
- LeadLag Python tests `134/134`、Bitget trading Python tests `63/63` 通过。为避免污染系统 Python，
  测试依赖只安装到 `/home/liuxiang/tmp/aquila-lead-lag-python-test-venv`。
- synthetic dual-feed replay 对 `parallel=1/2/4/8/16` 各运行两次，CSV 逐字节确定；每个容量都填满
  slots、拒绝额外 open、释放后复用 slot 且 `group_id` 继续递增。
- evaluation include/link 边界无命中；checked-in live config 无 `parallel=2..16`，生产目录没有
  account limiter。

### 同时间窗真实行情离线 replay

`20260720_162559_bitget_combined46_n6_fanout1_12h` 的外发 evidence bundle 只包含 Bitget 行情，但原始
run directory 仍保留同时间窗 Binance segments。因此已经完成真实双侧行情的纯离线 replay，不再需要把
不同时间窗数据拼接：

- 输入为首个 30 分钟稳定合并流，共 `7,883,631` 条 BookTicker：Bitget `3,343,514`、Binance
  `4,540,117`；按 `local_ns` 单调合并，时间相同则 Bitget 在前。
- `parallel=1` 两次结果相同：`174` signals（`142` open、`32` close），最大 active group 为 `1`。
- `parallel=2` 两次结果相同：`194` signals（`142` open、`52` close），最大 active group 为 `2`；
  10 个 symbol 自然出现两个并行 active group。
- candidate `parallel=1` 与 `main@87bdc08` 的 `174` 个核心 intent 在 action、side、price、
  reduce-only、timing、BBO 和 position direction 上逐项一致。新旧结果的差异只来自 baseline
  不具备的新 `group_id/trailing_price` schema。

原始 replay、配置、hash 和逐字段 parity 摘要位于
`/home/liuxiang/tmp/lead_lag_parallel_market_replay_20260723T084500Z`。

### Fresh production-like A/B benchmark

benchmark 使用同机、同 Release flags、固定 CPU 和交错 balanced blocks。production-like target 只开启
初始化 seed hook，不开启逐阶段 test observer。结果位于
`/home/liuxiang/tmp/lead_lag_parallel_final_ab_20260723T070100Z`。

| 路径 | `parallel=1` candidate 相对 baseline | 结论 |
| --- | --- | --- |
| Gate terminal feedback | 五组 P50 median delta 为 `-0.292%/-2.276%/+2.362%/+0.227%/+0.980%`；P99 全部改善 `2.375%..6.787%` | 通过 P50 `<=5%`、P99 `<=10%` |
| Bitget terminal feedback | P50 `+3.398%`，P99 `-1.482%` | 通过 |
| open submit | P50 `+3.547%`，P99 `-0.983%` | 通过 |
| active lead/lag tick CPU | `-0.870%/-0.682%` | 通过 |

共享 AWS/KVM 环境中个别 whole block 存在显著频率漂移，因此门槛使用计划中预先锁定的 balanced-block
median；P999 只作诊断，不作为硬门。`ExecutionGroup` 64-byte alignment 消除了此前可复现的
`parallel=2` cache-layout 回退：open-submit P2 相对 P1 由约 `+65%` 恢复为 `+0.248%`。

容量扩展结果符合实现复杂度：

- active lead/lag tick 在 P16 相对 P1 分别为 `+9.121%/+7.765%`；
- Gate/Bitget terminal feedback P50 在 P16 相对 P1 分别为 `+5.963%/+3.453%`；
- exact group apply 的 P1/P2/P4/P8/P16 P50 均为 `47ns`；
- fixed-slot container 的 225 个 iteration sample 动态分配全部为 `0`，相邻容量最大 ratio
  `2.177`，低于 O(n) 路径预设调查线 `2.2`。

本机 `kernel.perf_event_paranoid=4`，因此没有 cycles/callgraph 证据，也未修改系统设置。

### 全量构建边界

candidate 与 `main@87bdc08` 使用同一 Release 全量构建命令，分别在 42% 和 28% 被同一个既有错误阻断：

```text
benchmark/websocket/third_party_frame_codec_comparison_benchmark.cpp:4:
fatal error: third_party/websocket/websocket.h: No such file or directory
```

因此不能声明全仓 build/ctest 通过；该错误不是本分支引入，affected targets 和上述 focused verification
均已单独通过。

## 代码与验证入口

- `core/base/fixed_ordered_slot_pool.h`：初始化时按 effective capacity 分配、运行期固定容量的 FIFO
  ordered slot pool；move 后源对象重置为 inactive，可安全重新初始化。
- `strategy/lead_lag/execution_state.h`：group 创建、active traversal、精确 terminal apply 和 mismatch 保护。
- `strategy/lead_lag/strategy.h`：group metadata 写入订单、pair degraded / reconcile 行为及结构化日志。
- `core/trading/order_types.h`、`core/trading/order_manager.h`：`group_id` 和本地 `group_index` contract。
- `core/trading/order_gateway_shm_types.h`：SHM v4 `group_id` contract。
- `test/strategy/lead_lag_feedback_state_test.cpp`：slot reuse、mismatch 和 multi-group terminal 状态测试。
- `test/strategy/lead_lag_strategy_interface_test.cpp`：parallel、fanout、runtime fail-fast 和策略降级行为测试。
- `scripts/lead_lag/analyze_order_detail.py`：只解析新 `group_id` schema 的 order / latency report。

## 合并与 production-readiness 边界

- 上述证据只支持 PR #13 的 multi-group 基础设施 merge gate，不证明 `parallel>1` 的 live fillability、
  PnL、风险收益或最大安全资金规模。用户已明确当前不把该分支合并进 main。
- 所有 checked-in live config 继续保持 `parallel=1`。第一次 `parallel=2` 真实订单测试必须另写当次
  guarded runbook，重新选择自然出现重叠 group 的 symbol，并取得用户单独授权。
- 首次候选继续限定 Bitget、`fanout=1`、每个 group `open_notional<=10 USDT`；任何
  `UnknownResult`、`ContinuityLost`、group mismatch、unresolved order、非 flat 或 guard 异常都必须
  停止本轮并进入 stop-and-flat/handoff。
- 当前 main 不含 account limiter，本分支也没有重新加入。
- PR #11 的 report/BBO fillability/fast-fill/slippage 分析必须按新 `group_id/submitted_v2` schema
  单独升级；本分支不提供旧 schema 兼容层。
- 后续变更固定 slot、cache layout、terminal apply 或 logging boundary 时，必须重跑本节 A/B benchmark；
  不能沿用 2026-07-23 的性能结论。
