# LeadLag FixedOrderedSlotPool 与 parallel=n

本文记录 LeadLag multi-group execution 的当前容器、标识和故障语义。策略信号和持仓语义见
`strategy/lead_lag/README.md`，order gateway 的通用 contract 见 `docs/strategy_order_component_model.md`。

## 当前实现

- 每个 pair 的 `ExecutionState` 使用
  `FixedOrderedSlotPool<ExecutionGroup, kMaxLeadLagExecutionGroups>`，编译期上限为 `16`，初始化后不分配容器内存。
- `execute.parallel` 表示同一个 pair 同时允许的 active execution group 数，合法范围是 `1..16`。配置解析和 runtime 初始化都会拒绝越界值，不依赖容器的 capacity clamp。
- `execute.order_session_fanout` 表示一个 signal 最多使用的 ready order route 数。fanout child 共享同一个 execution group，但分别拥有唯一 `local_order_id` 和各自的 `route_id`；一个 fanout signal 只占一个 parallel slot。
- active group 按创建顺序遍历；erase 后 slot 可以复用，但 active FIFO 顺序保持不变。生产调用方通过 active traversal helper 或精确 slot lookup 访问，不暴露 vector-like `groups()` / `mutable_groups()` API。

## Execution group 标识

- `group_id` 是 pair runtime 内从 `1` 开始单调递增的 execution group identity。稳定归因键是 `(symbol_id, group_id)`；跨运行还必须加 run/session identity。
- `group_index` 是 `FixedOrderedSlotPool` 的 runtime-local slot index，只存在于 `StrategyOrder` 本地元数据中，用于 terminal response / feedback 的 O(1) 定位。它不进入 SHM，也不是日志或 report 的稳定 join key。
- 下单时 LeadLag 把 `group_id` 写入 `OrderPlaceRequest`，把 `group_index` 通过 `OrderLocalMetadata` 写入 `StrategyOrder`。order gateway SHM v4 原样传播 `group_id`，Gate、Bitget 和诊断工具均保留该字段。
- core / gateway 的 generic `parent_id` 继续用于通用 fanout correlation，不由 gateway 解释。LeadLag 自身的 order log、`order_detail.csv` 和 `latency.csv` 使用 `group_id`，新 analyzer 不再读取旧 `parent_id` 日志。

## Terminal feedback 与 slot reuse

response / feedback 先按 `local_order_id` 找到 `StrategyOrder`，再执行
`GroupAt(order.group_index, order.place_request.group_id)`：

1. slot occupied 且 `group_id` 相同，才允许修改 execution group；
2. slot 已空或已被其他 group 复用时，禁止扫描 fallback，也禁止把 terminal event 应用到其他 group；
3. mismatch 会输出 `lead_lag_order_group_mismatch`，将该 pair 标记为 `needs_reconcile` 并暂停新开仓；既有 active group 和 position 保留，close / stoploss 仍可继续执行；
4. terminal `StrategyOrder` 仍按自身生命周期 retire，避免损坏本地订单容量，但被保护的 execution group 不因该回报被清理或改写。

`group_id` 计数器发生 `uint64_t` wrap 时返回创建失败，不复用旧 id。slot index 可以复用，因此任何只校验
`group_index` 的实现都不符合当前 contract。

## 代码与验证入口

- `core/base/fixed_ordered_slot_pool.h`：固定容量 FIFO ordered slot pool。
- `strategy/lead_lag/execution_state.h`：group 创建、active traversal、精确 terminal apply 和 mismatch 保护。
- `strategy/lead_lag/strategy.h`：group metadata 写入订单、pair degraded / reconcile 行为及结构化日志。
- `core/trading/order_types.h`、`core/trading/order_manager.h`：`group_id` 和本地 `group_index` contract。
- `core/trading/order_gateway_shm_types.h`：SHM v4 `group_id` contract。
- `test/strategy/lead_lag_feedback_state_test.cpp`：slot reuse、mismatch 和 multi-group terminal 状态测试。
- `test/strategy/lead_lag_strategy_interface_test.cpp`：parallel、fanout、runtime fail-fast 和策略降级行为测试。
- `scripts/lead_lag/analyze_order_detail.py`：只解析新 `group_id` schema 的 order / latency report。

性能结论必须重新运行 `benchmark/strategy/lead_lag_group_container_benchmark.cpp` 及相关 submit / feedback
benchmark 后给出。当前基础设施不构成 `parallel > 1` 的 live fillability、PnL 或风险收益证据；所有现有 live
config 仍应保持 `parallel=1`，任何真实订单需要单独授权和完整 guarded runbook。
