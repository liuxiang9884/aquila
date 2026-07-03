# LeadLag FixedOrderedSlotPool 与 parallel=n 迁移说明

本文记录截至 2026-07-03 的当前事实、相关文档入口和后续建议。它只整理 `FixedOrderedSlotPool<T, kCapacity>` 作为 LeadLag multi-group 容器的迁移边界；不是已批准的实现计划，也不表示生产 `ExecutionState` 已经切换。

## 当前事实

- `core/base/fixed_ordered_slot_pool.h` 已提供固定容量 FIFO ordered slot pool：`kCapacity <= 64`，`Initialize(n)` 会把运行时请求容量 clamp 到编译期容量，初始化后不做动态分配。
- `test/core/base/base_structures_test.cpp` 已覆盖 capacity clamp、full reject、低 slot 复用但 FIFO active order 不变、head / middle / tail erase、`FindIndexIf()` 只按 active FIFO 顺序扫描，以及 `Clear()` 后复用。
- `benchmark/strategy/lead_lag_group_container_benchmark.cpp` 已有 `FixedOrderedSlotPoolGroupContainer`，对比 `ActiveIndex`、`FixedOrderedSlotPool` 和 `LinkedList`，覆盖 full-active `n=2/4/8/16/32/64` 等场景。
- 当前 `strategy/lead_lag/execution_state.h` 仍使用 `std::vector<ExecutionGroup>`，`ExecutionState::Init(parallel)` 通过 `groups_.assign(parallel, ExecutionGroup{})` 建立运行时容量；不要假定已经切到 `FixedOrderedSlotPool`。
- 当前 `ExecutionGroup` 已支持多 child order：`pending_local_order_ids`、`pending_order_roles`、`unknown_result_local_order_ids`、`pending_open_order_count` 和 `pending_close_order_count` 等字段承载同一 execution group 下的 fanout / retry / unknown-result 子订单。
- `order_session_fanout=m` 与 `parallel=n` 是两层概念：fanout 表示一个 signal 向多条 order route 发送多个 child order；`parallel` 表示同一个 pair 同时允许多少个 active execution group。一个 fanout signal 仍只占一个 `parallel` slot。
- 2026-07-03 Grill Me 讨论后，当前 LeadLag 直接迁移的候选上限锁定为 `kMaxLeadLagExecutionGroups = 16`；`parallel > 16` 不应由 `FixedOrderedSlotPool::Initialize()` 静默 clamp，生产配置应 fail fast。

## 相关入口

- `core/base/fixed_ordered_slot_pool.h`：固定容量 FIFO ordered slot pool API。
- `test/core/base/base_structures_test.cpp`：容器行为单测。
- `benchmark/strategy/lead_lag_group_container_benchmark.cpp`：group-container 成本对比。
- `strategy/lead_lag/execution_state.h`：当前生产状态机和 `std::vector<ExecutionGroup>` 容器。
- `test/strategy/lead_lag_feedback_state_test.cpp`：feedback / unknown-result / multi child 状态测试，其中已有 `parallel=2` 场景。
- `test/strategy/lead_lag_strategy_interface_test.cpp`：strategy 层 `parallel_limit`、fanout child、rejected intent 等行为测试。
- `docs/gate_order_gateway_shm_design.md` 的 “Strategy Fanout 语义”：说明 fanout child 共享 `parent_id`，但一个 signal 只占一个 `parallel` slot。
- `docs/strategy_order_component_model.md` 的 “多路 OrderSession 扩展边界”：说明 gateway 不解释 parent signal / child group / winner / overfill。
- `docs/superpowers/plans/2026-06-30-gate-order-gateway-shm.md`：历史实现 plan 中的 fanout strategy tests 和 `execute.parallel * order_session_fanout` bounded scan 要求。

## 迁移边界

如果把 `ExecutionState` 从 `std::vector<ExecutionGroup>` 迁移到 `FixedOrderedSlotPool<ExecutionGroup, 16>`，应保持交易语义不变，但不需要保持旧 `std::vector` 容器 API 兼容：

- `parallel` 仍是每个 pair 的最大 active execution group 数，不是 child order 数。
- `order_session_fanout` 生成的多个 child order 必须落在同一个 execution group 下，共享 execution group / position lifecycle 级 `parent_id`。
- 旧的 `groups()` / `mutable_groups()` 不应继续暴露为 vector-like API。调用方应改用 active FIFO 遍历 helper 或明确的 `FindGroupById()` / slot lookup helper，避免把 inactive slot 当成 active group。
- slot index 只能作为容器内部地址，不应替代 `group_id`、`parent_id` 或 `local_order_id`。slot 被 erase 后会复用；如果未来把 slot index 写入订单元数据，回查时必须同时校验 `group_id`，防止旧订单命中新复用 slot。
- 当前直接迁移阶段可以保留既有 `parent_id` 日志 / report 语义，不把 `parent_id` 去留和容器替换绑在同一个改动里；`group_id` 和 `parent_id` 继续是策略级 identity。
- pending order lookup 在直接迁移阶段可保持 bounded scan：`active_group_count <= 16`，每组 pending child 数由当前 fixed arrays 限制。不要为了当前 LeadLag 单点迁移新增一套 LeadLag-only pending hash index；更好的低延迟目标见下方 “Future 改进”。
- 行情扫描或 active group 遍历期间不要直接 erase 当前容器。`FixedOrderedSlotPool::Erase()` 会更新 `active_indices()` 顺序；若遍历中需要清理 group，先收集 `group_id` / slot index，再在遍历结束后统一 erase。
- `parallel > 16` 不能静默改变实盘语义。生产配置解析应直接拒绝；测试 / benchmark 如果需要探索更大 `n`，使用独立 benchmark fixture，不复用 live config 语义。

## Future 改进：Core multi-group infra

更长期、更低延迟的方向，是把 multi-group 执行容器和订单归属元数据下沉到 `core/trading` infra，而不是让 LeadLag 独自维护一套策略私有索引：

- 在 core 层提供通用 execution group container / handle 模型，底层可用 `FixedOrderedSlotPool`，策略只持有 group handle 或 stable group id，不直接依赖具体容器布局。
- `OrderCreateRequest` / `StrategyOrder` 增加 group 元数据，例如 `execution_group_id` 和 `execution_group_index`。下单创建订单时把当前 group id 与 slot index 写入订单；response / feedback 先通过 `local_order_id` 找到 `StrategyOrder`，再用 `execution_group_index` O(1) 定位 group，并校验 `execution_group_id` 是否仍匹配当前 slot。
- `group_index` 是运行时 slot 地址，只能用于本进程快速定位；`group_id` 是 symbol 内的 execution group identity。slot 复用后，`group_index` 单独不能作为安全 key。
- 由于 signal、pair 和下单当前都按 `symbol_id` 分区，未来可以重新评估 `parent_id` 是否仍有必要。若改为用 `(symbol_id, execution_group_id, local_order_id, route_id)` 作为 report / diagnostics 归因键，必须同步迁移 `docs/diagnostic_fields.md`、`docs/lead_lag_live_report_csv_schema.md`、report 脚本和 Gate order gateway 日志字段，不能只改策略内语义。
- 在 core infra 化完成前，不建议新增 LeadLag 专用的 `local_order_id -> slot` hash cache；否则未来还要把同一套生命周期清理、slot 复用校验和测试迁移到 core。

## 建议实现顺序

1. 先加 focused tests，不改实现：覆盖 `parallel=2/4/16` 下的 open group FIFO 顺序、head / middle / tail group close、slot 复用后 `group_id` 单调递增、pending child feedback 命中正确 group、unknown-result resolve 后不影响新复用 slot。
2. 增加 `kMaxLeadLagExecutionGroups = 16`，并在 LeadLag config parse / runtime init 处拒绝 `parallel > 16`；不要依赖 `FixedOrderedSlotPool::Initialize()` clamp。
3. 直接把 `ExecutionState` 的 storage 替换为 `FixedOrderedSlotPool<ExecutionGroup, kMaxLeadLagExecutionGroups>`，删除 vector-like `groups()` / `mutable_groups()` 对外暴露，改成 active FIFO 遍历 helper 和明确的 group lookup helper。
4. 保持当前 `parent_id`、fanout、parallel-limit、close retry、recover 和 report 字段语义不变；不要在同一个改动中引入 core group metadata 或 `parent_id` 退场。
5. 重新跑 group-container benchmark、LeadLag strategy / feedback focused tests 和 submit-path benchmark；性能结论只基于这些新结果表述。

## 建议验证

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target core_base_structures_test lead_lag_group_container_benchmark lead_lag_submit_breakdown_benchmark -j8
./build/release/test/core/base/core_base_structures_test
ctest --test-dir build/release --output-on-failure -R 'lead_lag_feedback_state|lead_lag_strategy_interface|lead_lag_signal|lead_lag_config'
taskset -c 16 ./build/release/benchmark/strategy/lead_lag_group_container_benchmark --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
taskset -c 16 ./build/release/benchmark/strategy/lead_lag_submit_breakdown_benchmark --benchmark_filter='BM_OrderGatewayFanout(CurrentPlaceOrder|BatchModel)4Routes(/|$)' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## 当前建议

短期如果继续这条线，建议按 `kMaxLeadLagExecutionGroups = 16` 直接替换 LeadLag `ExecutionState` storage，并先用 focused tests 锁住 `parallel=n`、fanout child 聚合、terminal erase 和 slot reuse 行为。更大的结构性优化是把 group container / order group metadata 下沉到 `core/trading`，让 `StrategyOrder` 自带 `execution_group_id` 和 `execution_group_index`；这应作为后续独立 infra 改动推进，不和当前 LeadLag 容器替换混在一起。
