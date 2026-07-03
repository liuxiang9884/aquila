# LeadLag FixedOrderedSlotPool 与 parallel=n 迁移说明

本文记录截至 2026-07-03 的当前事实、相关文档入口和后续建议。它只整理 `FixedOrderedSlotPool<T, kCapacity>` 作为 LeadLag multi-group 容器的迁移边界；不是已批准的实现计划，也不表示生产 `ExecutionState` 已经切换。

## 当前事实

- `core/base/fixed_ordered_slot_pool.h` 已提供固定容量 FIFO ordered slot pool：`kCapacity <= 64`，`Initialize(n)` 会把运行时请求容量 clamp 到编译期容量，初始化后不做动态分配。
- `test/core/base/base_structures_test.cpp` 已覆盖 capacity clamp、full reject、低 slot 复用但 FIFO active order 不变、head / middle / tail erase、`FindIndexIf()` 只按 active FIFO 顺序扫描，以及 `Clear()` 后复用。
- `benchmark/strategy/lead_lag_group_container_benchmark.cpp` 已有 `FixedOrderedSlotPoolGroupContainer`，对比 `ActiveIndex`、`FixedOrderedSlotPool` 和 `LinkedList`，覆盖 full-active `n=2/4/8/16/32/64` 等场景。
- 当前 `strategy/lead_lag/execution_state.h` 仍使用 `std::vector<ExecutionGroup>`，`ExecutionState::Init(parallel)` 通过 `groups_.assign(parallel, ExecutionGroup{})` 建立运行时容量；不要假定已经切到 `FixedOrderedSlotPool`。
- 当前 `ExecutionGroup` 已支持多 child order：`pending_local_order_ids`、`pending_order_roles`、`unknown_result_local_order_ids`、`pending_open_order_count` 和 `pending_close_order_count` 等字段承载同一 execution group 下的 fanout / retry / unknown-result 子订单。
- `order_session_fanout=m` 与 `parallel=n` 是两层概念：fanout 表示一个 signal 向多条 order route 发送多个 child order；`parallel` 表示同一个 pair 同时允许多少个 active execution group。一个 fanout signal 仍只占一个 `parallel` slot。

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

如果把 `ExecutionState` 从 `std::vector<ExecutionGroup>` 迁移到 `FixedOrderedSlotPool<ExecutionGroup, kCapacity>`，应保持外部语义不变：

- `parallel` 仍是每个 pair 的最大 active execution group 数，不是 child order 数。
- `order_session_fanout` 生成的多个 child order 必须落在同一个 execution group 下，共享 execution group / position lifecycle 级 `parent_id`。
- slot index 只能作为容器内部地址，不应替代 `group_id`、`parent_id` 或 `local_order_id`。slot 被 erase 后会复用，不能出现在持久日志、CSV join key 或跨模块身份语义里。
- `group_id` 和 `parent_id` 继续是策略级 identity；terminal feedback、close、stoploss、retry、unknown-result reconcile 都必须能通过这些 identity 回到正确 group。
- pending order lookup 仍可先保持 bounded scan：`active_group_count <= parallel`，每组 pending child 数由当前 fixed arrays 限制。只有 benchmark 或 profile 证明 scan 是瓶颈时，再考虑新增 `local_order_id -> slot index` cache。
- 如果新增 lookup cache，必须在 terminal feedback、cancel terminal、unknown-result resolve、`ClearGroup` 和 slot erase 时同步清理，避免 slot 复用后误命中新 group。
- 行情扫描或 active group 遍历期间不要直接 erase 当前容器。`FixedOrderedSlotPool::Erase()` 会更新 `active_indices()` 顺序；若遍历中需要清理 group，先收集 `group_id` / slot index，再在遍历结束后统一 erase。
- `parallel > kCapacity` 不能静默改变实盘语义。至少要在启动时比较 requested parallel 与实际 initialized capacity；推荐 live 路径 fail fast，测试 / benchmark 路径可允许 warning + cap。

## 建议实现顺序

1. 先加 focused tests，不改实现：覆盖 `parallel=2/4` 下的 open group FIFO 顺序、head / middle / tail group close、slot 复用后 `group_id` 单调递增、pending child feedback 命中正确 group、unknown-result resolve 后不影响新复用 slot。
2. 在 `ExecutionState` 内部引入小的 group container adapter，先保持 `std::vector` backend，通过同一组 tests 锁住行为。
3. 增加 `FixedOrderedSlotPool` backend，但不改策略外部接口；让 `ExecutionState` 的查找、遍历、clear 都走 adapter。
4. 只在所有 LeadLag focused tests 通过后，替换默认 backend；不要同时改 fanout、parallel-limit、close retry 或 recover 语义。
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

短期不要直接把生产 `ExecutionState` 容器替换为 `FixedOrderedSlotPool`。更稳妥的下一步是先做 adapter + tests，把 `parallel=n` 多 group 行为、fanout child 聚合和 erase/reuse 边界固定下来。等行为回归稳定后，再切 backend 并用 benchmark 验证是否值得进入 hot path。
