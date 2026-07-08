# LeadLag FixedOrderedSlotPool 与 parallel=n 迁移说明

本文记录截至 2026-07-03 的当前事实、相关文档入口和已锁定迁移边界。它整理 `FixedOrderedSlotPool<T, kCapacity>` 作为 LeadLag multi-group 容器的迁移方案；不表示生产 `ExecutionState` 已经切换。

## 当前事实

- `core/base/fixed_ordered_slot_pool.h` 已提供固定容量 FIFO ordered slot pool：`kCapacity <= 64`，`Initialize(n)` 会把运行时请求容量 clamp 到编译期容量，初始化后不做动态分配。
- `test/core/base/base_structures_test.cpp` 已覆盖 capacity clamp、full reject、低 slot 复用但 FIFO active order 不变、head / middle / tail erase、`FindIndexIf()` 只按 active FIFO 顺序扫描，以及 `Clear()` 后复用。
- `benchmark/strategy/lead_lag_group_container_benchmark.cpp` 已有 `FixedOrderedSlotPoolGroupContainer`，对比 `ActiveIndex`、`FixedOrderedSlotPool` 和 `LinkedList`，覆盖 full-active `n=2/4/8/16/32/64` 等场景。
- 当前 `strategy/lead_lag/execution_state.h` 仍使用 `std::vector<ExecutionGroup>`，`ExecutionState::Init(parallel)` 通过 `groups_.assign(parallel, ExecutionGroup{})` 建立运行时容量；不要假定已经切到 `FixedOrderedSlotPool`。
- 当前 `ExecutionGroup` 已支持多 child order：`pending_local_order_ids`、`pending_order_roles`、`unknown_result_local_order_ids`、`pending_open_order_count` 和 `pending_close_order_count` 等字段承载同一 execution group 下的 fanout / retry / unknown-result 子订单。
- `order_session_fanout=m` 与 `parallel=n` 是两层概念：fanout 表示一个 signal 向多条 order route 发送多个 child order；`parallel` 表示同一个 pair 同时允许多少个 active execution group。一个 fanout signal 仍只占一个 `parallel` slot。
- 2026-07-03 Grill Me 讨论后，LeadLag 直接迁移上限锁定为 `kMaxLeadLagExecutionGroups = 16`；`parallel > 16` 不应由 `FixedOrderedSlotPool::Initialize()` 静默 clamp，生产配置应 fail fast。
- 同次讨论锁定：本次迁移同时删除 `parent_id`，改用 core order API / SHM / logs / report CSV 中的 `group_id` 和 `group_index`；不做历史日志兼容，旧 live log 需要使用旧版本脚本分析。

## 相关入口

- `core/base/fixed_ordered_slot_pool.h`：固定容量 FIFO ordered slot pool API。
- `test/core/base/base_structures_test.cpp`：容器行为单测。
- `benchmark/strategy/lead_lag_group_container_benchmark.cpp`：group-container 成本对比。
- `strategy/lead_lag/execution_state.h`：当前生产状态机和 `std::vector<ExecutionGroup>` 容器。
- `test/strategy/lead_lag_feedback_state_test.cpp`：feedback / unknown-result / multi child 状态测试，其中已有 `parallel=2` 场景。
- `test/strategy/lead_lag_strategy_interface_test.cpp`：strategy 层 `parallel_limit`、fanout child、rejected intent 等行为测试。
- `docs/gate_order_gateway_shm_design.md` 的 “Strategy Fanout 语义”：当前文档仍以 `parent_id` 描述 fanout child 归组；本次迁移需要同步改成 `group_id` / `group_index`。
- `docs/strategy_order_component_model.md` 的 “多路 OrderSession 扩展边界”：说明 gateway 不解释 parent signal / child group / winner / overfill。

## 迁移边界

如果把 `ExecutionState` 从 `std::vector<ExecutionGroup>` 迁移到 `FixedOrderedSlotPool<ExecutionGroup, 16>`，应保持交易语义不变，但不需要保持旧 `std::vector` 容器 API 兼容：

- `parallel` 仍是每个 pair 的最大 active execution group 数，不是 child order 数。
- `order_session_fanout` 生成的多个 child order 必须落在同一个 execution group 下，共享同一个 per-symbol `group_id`，但各自拥有唯一 `local_order_id` 和 route-level `route_id`。
- 旧的 `groups()` / `mutable_groups()` 不应继续暴露为 vector-like API。调用方应改用 active FIFO 遍历 helper 或明确的 `FindGroupById()` / slot lookup helper，避免把 inactive slot 当成 active group。
- `group_id` 是 `symbol_id` 内单调递增的 execution group identity；跨 symbol 唯一归因必须使用 `(symbol_id, group_id)`，不能只用 `group_id`。
- `group_index` 是 `FixedOrderedSlotPool` slot 地址，只用于本进程 O(1) 定位和诊断。它可以进入 order / response / feedback log，但不是稳定 join key。slot 被 erase 后会复用，回报路径必须同时校验订单里的 `group_id` 与当前 slot 的 `ExecutionGroup::group_id` 一致。
- `OrderCreateRequest`、`StrategyOrder`、`OrderResponseEvent`、order gateway SHM command / event、Gate gateway log、LeadLag order log 和 report CSV 都使用 `group_id` / `group_index`。本次迁移彻底删除 `parent_id` 字段，不保留新脚本对旧 `parent_id` 日志的兼容解析。
- pending order lookup 的目标路径是：response / feedback 先通过 `local_order_id` 找到 `StrategyOrder`，再用 `order.group_index` O(1) 定位 `ExecutionGroup`，并校验 `order.group_id`。只有 `group_index` 无效或校验失败时才进入 reconcile / unknown 分支，不回退到全量扫描作为正常路径。
- 行情扫描或 active group 遍历期间不要直接 erase 当前容器。`FixedOrderedSlotPool::Erase()` 会更新 `active_indices()` 顺序；若遍历中需要清理 group，先收集 `group_id` / slot index，再在遍历结束后统一 erase。
- `parallel > 16` 不能静默改变实盘语义。生产配置解析应直接拒绝；测试 / benchmark 如果需要探索更大 `n`，使用独立 benchmark fixture，不复用 live config 语义。

## Core multi-group metadata

本次迁移把 multi-group 归属元数据下沉到 `core/trading`，避免 LeadLag 私有 side table：

- `group_id` 和 `group_index` 放在 `core::OrderCreateRequest` / `core::StrategyOrder`，默认值为 `0` / invalid。非 multi-group 策略可以继续不设置。
- `group_id` 和 `group_index` 需要透传到 `core::OrderResponseEvent`、order gateway SHM command / event、Gate `OrderResponse` / log fields。这样跨进程 gateway send / response 诊断和 LeadLag strategy log 不再依赖 `parent_id`。
- `group_index` 只在 runtime 内做快速定位；report 主归因键使用 `symbol_id + group_id + local_order_id + route_id`。`group_index` 可作为 diagnostic column 输出，但不能作为 position / latency / order detail 的唯一关联键。
- 新脚本只解析新字段。旧 live log 不再保证可由新脚本分析。

## 建议实现顺序

1. 先更新 core order contract 和 order gateway SHM：删除 `parent_id`，新增 `group_id` / `group_index`，并更新 Gate order gateway send / response / diagnostic logs。
2. 增加 `kMaxLeadLagExecutionGroups = 16`，并在 LeadLag config parse / runtime init 处拒绝 `parallel > 16`；不要依赖 `FixedOrderedSlotPool::Initialize()` clamp。
3. 直接把 `ExecutionState` 的 storage 替换为 `FixedOrderedSlotPool<ExecutionGroup, kMaxLeadLagExecutionGroups>`，删除 vector-like `groups()` / `mutable_groups()` 对外暴露，改成 active FIFO 遍历 helper 和明确的 `GroupAt(group_index, group_id)` lookup helper。
4. 下单时把当前 group 的 `group_id` / `group_index` 写入 `OrderCreateRequest`，再进入 `StrategyOrder` 和 SHM。response / feedback 处理时通过 `StrategyOrder.group_index` O(1) 定位 group，并校验 `group_id`；校验失败进入 reconcile / ignored-stale 分支。
5. 同步迁移 LeadLag logs、test hooks、`scripts/lead_lag/analyze_order_detail.py`、`scripts/lead_lag/generate_live_report.py`、脚本测试、`docs/diagnostic_fields.md` 和 `docs/lead_lag_live_report_csv_schema.md`。新脚本只支持 `group_id` / `group_index`，不兼容旧 `parent_id`。
6. 重新跑 group-container benchmark、LeadLag strategy / feedback focused tests、order gateway focused tests、report script tests 和 submit-path benchmark；性能结论只基于这些新结果表述。

## 建议验证

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/release --target core_base_structures_test lead_lag_group_container_benchmark lead_lag_submit_breakdown_benchmark -j8
./build/release/test/core/base/core_base_structures_test
ctest --test-dir build/release --output-on-failure -R 'lead_lag_feedback_state|lead_lag_strategy_interface|lead_lag_signal|lead_lag_config'
taskset -c 16 ./build/release/benchmark/strategy/lead_lag_group_container_benchmark --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
taskset -c 16 ./build/release/benchmark/strategy/lead_lag_submit_breakdown_benchmark --benchmark_filter='BM_OrderGatewayFanout(CurrentPlaceOrder|BatchModel)4Routes(/|$)' --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
```

## 当前建议

当前建议按一个不可中断 implementation plan 完成端到端迁移，但允许拆多个原子提交：先 core / SHM contract，再 LeadLag `FixedOrderedSlotPool<ExecutionGroup, 16>` 与 order group metadata，再 report scripts / docs / tests。最终分支不能停在半迁移状态，因为 `parent_id` 删除会同时影响生产下单、gateway 诊断和 report 生成。
