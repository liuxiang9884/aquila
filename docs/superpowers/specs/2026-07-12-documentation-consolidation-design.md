# Aquila 文档统一与精简设计

## 状态

已批准，等待用户复核书面设计后进入实施计划。

## 目的

当前仓库 `docs/` 有 48 份 Markdown、约 984 KiB。部分完成态 implementation plan / spec、历史设计推导和当前专题文档重复，多个领域也同时存在 handoff、design、guide、plan 等入口，增加检索和维护成本。

本次整理的目标是建立“一个主题一个当前事实源”的文档结构，同时保留必须独立维护的配置契约、操作 runbook 和可复现证据。整理不设置硬性文件数或字数目标；预计 `docs/` 从 48 份降至约 28 份。

## 范围

纳入整理：

- `docs/`
- 根 `README.md`
- `strategy/lead_lag/README.md`
- `AGENTS.md` 中受文档路径和维护规则影响的内容

不重写：

- `reports/` 历史快照
- `reference/` 外部参考材料

如果未纳入重写的文档引用了被删除或重命名的路径，只修复该引用或补充历史路径说明，不改写其历史结论。

本次只修改 Markdown，不修改生产代码、配置或测试代码。用户未提交的 `core/market_data/types.h` `Orderbook` 草案必须保持原样，不得纳入提交。

## 目标文档层级

### 入口层

`README.md` 和 `docs/project_onboarding_guide.md` 只保留项目入口、当前事实摘要、代码入口、验证命令和可执行的下一步。它们不承载完整设计推导、字段表、benchmark 原始输出或 live smoke 流水账。

### 领域事实源

Bitget trading、Gate trading、market-data fusion、LeadLag live、TUI、WebSocket 等主题各保留一个当前架构和状态入口。完成态 plan / spec 中仍有效的 contract、边界和使用方式迁移到对应领域事实源。

### 专项契约与操作文档

配置参考、SHM ABI、diagnostic fields、CSV schema、reconcile 和 live pipeline 等需要独立查阅、直接执行或持续同步的内容保持独立。专项文档不重复领域状态和历史实现过程。

### 证据文档

Benchmark、live smoke、replay 对账和 A/B 结果可以保持独立，但只保留可复现场景、命令或输入、关键结果、结论和适用边界。删除逐轮优化流水、重复推导和已经失效的临时产物索引。

## 合并与删除映射

### Bitget trading

- 将 `docs/superpowers/specs/2026-07-10-bitget-order-session-design.md`、`2026-07-10-bitget-order-feedback-session-design.md`、`2026-07-10-bitget-order-session-rtt-probe-design.md` 和 `2026-07-11-bitget-order-gateway-design.md` 中仍有效的协议语义、组件边界、安全约束和验证入口并入当前 Bitget trading 事实源。
- 将 `docs/bitget_trading_follow_up.md` 扩展并重命名为 `docs/bitget_trading.md`，同时覆盖已实现基线、当前 contract、操作边界和后续阻断。
- 删除上述已完成 spec 和 `docs/superpowers/plans/` 下 5 份已完成 implementation plan。
- Bitget market data 继续以 `docs/agent-handoff-bitget-market-data.md` 为事实源。
- 如果 `docs/superpowers/plans/` 和 `docs/superpowers/specs/` 清空，则删除空目录。

### Gate trading

- 合并 `docs/agent-handoff-gate-trade-architecture.md` 与 `docs/gate_order_gateway_shm_design.md`，形成 `docs/gate_trading.md`。
- 保留跨交易所通用的 `docs/strategy_order_component_model.md`。
- 保留承担独立操作和证据职责的 `docs/gate_order_session_rtt_probe_design.md`，精简其中重复架构描述。

### Market-data fusion

- 合并 `docs/gate_fastest_route_fusion_design.md`、`docs/gate_fastest_route_fusion_threaded_bundle_guide.md` 和 `docs/trade_fastest_route_fusion_design.md`，形成 `docs/market_data_fusion.md`。
- 保留 `docs/gate_fastest_route_fusion_shadow_results.md` 和 `docs/fusion_tardis_bbo_comparison.md` 作为证据文档，并压缩为复现条件、结果、结论和边界。

### LeadLag

- 合并 `docs/lead_lag_live_operations_pipeline.md` 与 `docs/lead_lag_live_runtime_plan.md`，形成 `docs/lead_lag_live_operations.md`；`AGENTS.md` 中的实盘触发词改指向新入口。
- 合并 `docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_benchmark_environment_tail_analysis.md` 和 `docs/lead_lag_runtime_latency_improvement_plan.md`，形成 `docs/lead_lag_latency_analysis.md`。
- 将 `docs/lead_lag_cancelled_order_fillability_analysis.md` 的仍有效方法和边界并入 `docs/exchange_matching_fillability_notes.md`。
- 将 `docs/lead_lag_go_cpp_strategy_alignment.md` 的当前差异、已锁定决策和仍有效证据摘要并入 `strategy/lead_lag/README.md`。
- 保留 `docs/lead_lag_live_report_csv_schema.md`、`docs/lead_lag_reconcile_design.md`、`docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` 和仍在推进的 `docs/lead_lag_fixed_ordered_slot_pool_parallel.md`，删除其中与入口文档重复的状态说明。

### TUI

合并 `docs/tui_onboarding_guide.md` 与 `docs/tui_gate_account_monitor_design.md`，形成 `docs/tui.md`，统一当前实现、运行方式、并发边界和后续数据源设计。

### WebSocket

合并 `docs/websocket_client_future_optimizations.md`、`docs/websocket_frame_codec_receive_strategies.md` 和 `docs/websocket_read_write_benchmark_comparison.md`，形成 `docs/websocket_client.md`。只保留当前实现结构、已采用选择、仍有效的备选方向、benchmark 证据摘要和验证入口。

### 配置、契约与入口

- `docs/data_reader_config.md`、`docs/data_session_config.md` 和 `docs/data_session_shm_communication_design.md` 职责不同，保持独立并删除重复状态。
- `docs/diagnostic_fields.md` 继续作为字段事实源；性能结果迁移到对应证据文档后，只保留字段定义、生命周期和删除条件。
- `docs/lead_lag_live_report_csv_schema.md` 继续作为 CSV contract，不承担运行状态或历史分析。
- `README.md` 压缩为 onboarding、构建、测试、核心工具和文档入口；交易所细节、长命令说明和历史状态转移到领域文档。
- `docs/project_onboarding_guide.md` 只保留最新摘要和索引，并同步所有新文件名。

## 内容迁移规则

删除旧文件前，逐项判断内容是否属于以下任一类别：

1. 当前代码或配置的 contract。
2. 安全、恢复、实盘或并发边界。
3. 可直接执行的命令、配置入口或验证方法。
4. 后续工作仍依赖的未完成阻断。
5. 有复现条件和适用边界的性能、live 或对账证据。

属于上述类别的内容迁移到对应事实源；纯实现步骤、已完成 checklist、逐轮 review 记录、重复背景、过期下一步和已失效临时路径直接删除。无法由当前代码、配置、Git 状态或仍存在证据确认的描述不得作为当前事实保留。

## 验证

整理完成后执行以下检查：

1. 使用 `git status --short --branch` 确认只包含文档整理和用户原有 `core/market_data/types.h` 草案。
2. 使用 `rg` 检查仓库中不存在已删除文件名或旧事实源引用。
3. 检查 Markdown 相对链接目标存在。
4. 检查文档列出的代码路径、配置路径和验证命令对应入口仍存在。
5. 对照 `AGENTS.md` 检查 onboarding 顺序、实盘触发词、结束对话流程和文档维护规则仍一致。
6. 运行 `git diff --check`。

纯文档整理不运行无关 benchmark、live 操作或完整 C++ 测试。若整理过程中意外触碰非 Markdown 文件，停止并先恢复到只修改文档的范围。

## 提交边界

- 本设计说明先单独提交，供用户书面复核。
- 正式整理形成一个原子文档提交，commit message 使用英文。
- 不裹带 `core/market_data/types.h` 或其他既有改动。
- 不自动 push。
- 正式整理完成后，将长期维护规则并入 `AGENTS.md`，并把本设计说明作为完成态 spec 删除。

## 验收条件

- 每个主题有且只有一个明确的当前事实源。
- 配置、ABI、字段、CSV 和操作 runbook 可以直接定位，不依赖历史 plan/spec。
- Benchmark、live 和对账结论保留复现条件与适用边界。
- README 和 onboarding 不重复专题细节。
- 被删除或重命名文档不存在悬空引用。
- `reports/` 和 `reference/` 的历史内容未被改写。
- 用户未提交的 `core/market_data/types.h` 草案保持原样。
