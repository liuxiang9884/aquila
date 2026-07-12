# Aquila Documentation Consolidation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将仓库长期文档统一为每个主题一个当前事实源，删除完成态 plan/spec 和重复文档，同时保留独立契约、操作 runbook 与可复现证据。

**Architecture:** 先按领域把仍有效的 contract、命令、安全边界和证据迁移到目标事实源，再删除旧文件；最后统一 README、onboarding、AGENTS 和全仓引用。正式整理只形成一个原子提交，并排除用户未提交的 `core/market_data/types.h`。

**Tech Stack:** Markdown、Git、ripgrep、POSIX shell

---

### Task 1: 建立 Bitget trading 单一事实源并清理完成态 plan/spec

**Files:**
- Create: `docs/bitget_trading.md`
- Delete: `docs/bitget_trading_follow_up.md`
- Delete: `docs/superpowers/specs/2026-07-10-bitget-order-session-design.md`
- Delete: `docs/superpowers/specs/2026-07-10-bitget-order-feedback-session-design.md`
- Delete: `docs/superpowers/specs/2026-07-10-bitget-order-session-rtt-probe-design.md`
- Delete: `docs/superpowers/specs/2026-07-11-bitget-order-gateway-design.md`
- Delete: `docs/superpowers/plans/2026-07-08-bitget-book-ticker-market-data.md`
- Delete: `docs/superpowers/plans/2026-07-10-bitget-order-feedback-session.md`
- Delete: `docs/superpowers/plans/2026-07-10-bitget-order-session-implementation.md`
- Delete: `docs/superpowers/plans/2026-07-10-bitget-order-session-rtt-probe.md`
- Delete: `docs/superpowers/plans/2026-07-11-bitget-order-gateway.md`

- [ ] **Step 1: 从 4 份 Bitget trading spec 提取长期内容**

只迁移下列内容：OrderSession/FeedbackSession/Gateway/RTT probe 的当前组件职责、direct response 与 terminal feedback 语义、`clientOid`/request correlation、continuity/unknown result、安全边界、配置与代码入口、验证命令、真实 live 证据边界和 P0/P1 后续阻断。删除逐任务实现步骤、已完成 checklist、历史方案比较和重复背景。

- [ ] **Step 2: 写入 `docs/bitget_trading.md`**

目标章节固定为：

```text
# Bitget UTA Trading
## 当前范围与证据边界
## 组件与进程结构
## OrderSession contract
## OrderFeedbackSession contract
## OrderGateway 与 LeadLag 接入
## RTT probe 与 live 证据
## 配置、代码与验证入口
## 实盘前置阻断
## 后续方向与优先级
```

`direct operation response` 不得描述为 accepted/fill/cancel terminal；gateway/LeadLag 未发送真实订单；dedicated-account flat、IP 白名单和余额只描述为当次证据。

- [ ] **Step 3: 删除完成态 Bitget plan/spec**

使用 patch 删除上述 9 份旧文件；保留本轮 documentation consolidation 的 design/plan，直到 Task 8 最终迁移规则后再删除。

- [ ] **Step 4: 检查 Bitget 内容迁移完整性**

Run:

```bash
rg -n 'clientOid|UnknownResult|ContinuityLost|terminal feedback|run-end flat|fanout=1|REST baseline|reconcile' docs/bitget_trading.md
```

Expected: 每项关键边界均至少有一处当前说明，且不存在完成态 checklist。

### Task 2: 统一 Gate trading 文档

**Files:**
- Create: `docs/gate_trading.md`
- Delete: `docs/agent-handoff-gate-trade-architecture.md`
- Delete: `docs/gate_order_gateway_shm_design.md`
- Modify: `docs/gate_order_session_rtt_probe_design.md`
- Modify: `docs/strategy_order_component_model.md`

- [ ] **Step 1: 合并 Gate handoff 与 gateway SHM design**

`docs/gate_trading.md` 使用以下结构：

```text
# Gate Trading
## 当前实现状态与证据边界
## 协议事实
## 进程、线程与所有权
## OrderSession / FeedbackSession contract
## OrderGateway SHM contract
## Strategy fanout 与故障语义
## 配置、代码与验证入口
## 未完成边界
```

保留 command/event payload、route state、queue-full、Ack/final response、unknown result 和 continuity 的当前 ABI/语义；删除历史讨论过程和已完成实现步骤。

- [ ] **Step 2: 精简通用组件模型与 RTT probe**

`strategy_order_component_model.md` 只保留跨交易所通用职责和 contract；Gate 专属字段与 gateway 装配改为链接 `gate_trading.md`。RTT probe 文档只保留运行方式、指标口径、安全边界、最新有效证据和下一步，不重复完整 Gate 组件架构。

- [ ] **Step 3: 检查 Gate 关键 contract**

Run:

```bash
rg -n 'route_states|queue|UnknownResult|ContinuityLost|fanout|Ack|terminal' docs/gate_trading.md
```

Expected: SHM、路由、恢复和订单事实边界均可定位。

### Task 3: 统一 market-data fusion 文档并压缩证据

**Files:**
- Create: `docs/market_data_fusion.md`
- Delete: `docs/gate_fastest_route_fusion_design.md`
- Delete: `docs/gate_fastest_route_fusion_threaded_bundle_guide.md`
- Delete: `docs/trade_fastest_route_fusion_design.md`
- Modify: `docs/gate_fastest_route_fusion_shadow_results.md`
- Modify: `docs/fusion_tardis_bbo_comparison.md`

- [ ] **Step 1: 建立 fusion 当前事实源**

`market_data_fusion.md` 保留 BookTicker/Trade 算法、source/fusion thread 边界、typed SHM、sidecar metadata、config、生命周期、代码入口、验证命令和证据索引。删除 V1/V2 历史推导、已排除方案的长篇比较和重复 shadow 输出。

- [ ] **Step 2: 压缩两份证据文档**

每份证据只保留日期、输入/运行条件、数据口径、关键 p50/p99/p99.9 或对账结果、结论和不可外推边界。已清理的临时产物不得描述为当前可读入口。

- [ ] **Step 3: 检查行情证据与订单收益边界**

Run:

```bash
rg -n '不代表|不能外推|fillability|PnL|p99' docs/market_data_fusion.md docs/gate_fastest_route_fusion_shadow_results.md docs/fusion_tardis_bbo_comparison.md
```

Expected: 行情 pipeline 证据与订单 fillability/PnL 明确分离。

### Task 4: 统一 LeadLag live、latency、fillability 与策略说明

**Files:**
- Create: `docs/lead_lag_live_operations.md`
- Delete: `docs/lead_lag_live_operations_pipeline.md`
- Delete: `docs/lead_lag_live_runtime_plan.md`
- Create: `docs/lead_lag_latency_analysis.md`
- Delete: `docs/lead_lag_ack_latency_outlier_analysis.md`
- Delete: `docs/lead_lag_benchmark_environment_tail_analysis.md`
- Delete: `docs/lead_lag_runtime_latency_improvement_plan.md`
- Modify: `docs/exchange_matching_fillability_notes.md`
- Delete: `docs/lead_lag_cancelled_order_fillability_analysis.md`
- Modify: `strategy/lead_lag/README.md`
- Delete: `docs/lead_lag_go_cpp_strategy_alignment.md`
- Modify: `docs/lead_lag_live_report_csv_schema.md`
- Modify: `docs/lead_lag_reconcile_design.md`
- Modify: `docs/lead_lag_live_replay_testing.md`
- Modify: `docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md`
- Modify: `docs/lead_lag_fixed_ordered_slot_pool_parallel.md`

- [ ] **Step 1: 合并 live pipeline 与 runtime runbook**

`lead_lag_live_operations.md` 按“启动 pipeline、运行期 guard、停止与 flat、report pipeline、通过判定、当前阻断”组织。保留真实订单必须显式授权、REST flat、freshness/slippage preflight、run dir 隔离、reconcile 和 report 命令；删除旧轮次状态与重复背景。

- [ ] **Step 2: 合并 latency 文档**

`lead_lag_latency_analysis.md` 按“指标口径、已确认根因、最新证据、Ack RTT 分层方法、复现命令、当前未决项”组织。只保留有命令或运行证据支撑的结论。

- [ ] **Step 3: 合并 fillability 方法**

将 cancelled-order 的 BBO 对齐、可成交性判定、分类和解释边界并入 `exchange_matching_fillability_notes.md`。保留 signal-conditioned 与非条件样本不可互相外推的限制。

- [ ] **Step 4: 合并 Go/C++ alignment**

在 `strategy/lead_lag/README.md` 中保留 `parallel-limit`、`lag_vol_guard`、`drift_guard`、freshness、report semantics 和 slippage cost model 的当前差异与已锁定决策；删除修改前实现流水和重复代码摘录。

- [ ] **Step 5: 精简保留的 LeadLag 专项文档**

CSV schema 只保留字段 contract；reconcile 只保留当前 V1/V2 状态、恢复语义和验证；replay/ORDI/fixed-slot 文档删除与 onboarding 重复的当前状态，但不删除其独立测试、证据或未完成迁移边界。

- [ ] **Step 6: 检查 live 安全边界**

Run:

```bash
rg -n '显式授权|REST|flat|freshness|slippage|reconcile|run dir|report' docs/lead_lag_live_operations.md
```

Expected: 启动、停止、恢复和报告流程均有明确入口。

### Task 5: 统一 TUI 与 WebSocket 文档

**Files:**
- Create: `docs/tui.md`
- Delete: `docs/tui_onboarding_guide.md`
- Delete: `docs/tui_gate_account_monitor_design.md`
- Create: `docs/websocket_client.md`
- Delete: `docs/websocket_client_future_optimizations.md`
- Delete: `docs/websocket_frame_codec_receive_strategies.md`
- Delete: `docs/websocket_read_write_benchmark_comparison.md`

- [ ] **Step 1: 合并 TUI**

`tui.md` 保留当前 demo/market-data 状态、运行方式、代码入口、线程与队列所有权、account monitor 数据源边界、测试命令和下一步。删除 onboarding 与 design 之间的重复说明。

- [ ] **Step 2: 合并 WebSocket**

`websocket_client.md` 保留当前 read/write/frame codec 架构、已采用的 mirrored-ring/bounded pump 选择、prepared write/active spin 配置、仍有效备选方向、benchmark 摘要、适用边界和验证入口。删除第三方实现逐行转述与旧 benchmark 流水。

- [ ] **Step 3: 检查当前实现与证据分界**

Run:

```bash
rg -n '当前实现|适用边界|benchmark|验证入口|未来' docs/tui.md docs/websocket_client.md
```

Expected: 当前能力、证据和未来方向分节明确。

### Task 6: 精简配置、契约和其他独立专题文档

**Files:**
- Modify: `docs/data_reader_config.md`
- Modify: `docs/data_session_config.md`
- Modify: `docs/data_session_shm_communication_design.md`
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/agent-handoff-binance-market-data.md`
- Modify: `docs/agent-handoff-bitget-market-data.md`
- Modify: `docs/futures_contract_metadata_fields.md`
- Modify: `docs/gate_obu_order_book_notes.md`
- Modify: `docs/runtime_cpu_allocation.md`
- Modify: `docs/evaluation_support.md`

- [ ] **Step 1: 删除跨文档重复状态**

配置文档保留 schema、默认值、合并规则、示例和验证；SHM 文档保留 ABI/protocol；diagnostic fields 只保留字段定义、生命周期和删除条件；exchange handoff 只保留该交易所行情事实与入口。

- [ ] **Step 2: 精简独立专题**

Instrument metadata、Gate OBU、runtime CPU 和 evaluation 文档保留当前 contract、证据边界、入口和下一步；删除 onboarding 已覆盖的项目背景与重复通用命令。

- [ ] **Step 3: 检查字段与路径仍存在**

Run:

```bash
rg -n '^## ' docs/data_reader_config.md docs/data_session_config.md docs/data_session_shm_communication_design.md docs/diagnostic_fields.md
```

Expected: 各文件职责单一，字段/配置/协议章节仍完整。

### Task 7: 压缩入口文档并统一所有引用

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`
- Modify: `docs/project_onboarding_guide.md`
- Modify: tracked Markdown files containing deleted paths

- [ ] **Step 1: 压缩 README**

只保留项目定位、onboarding、依赖、build/test、主要可执行工具类别和领域文档索引。删除已经由领域文档覆盖的长篇 Gate/Bitget 操作说明和 benchmark 逐项解释。

- [ ] **Step 2: 压缩 onboarding**

只保留最新完成摘要、领域索引、关键代码入口、验证命令、下一步建议和下一对话提示。Bitget、Gate、fusion、LeadLag 等历史过程改为链接新事实源。

- [ ] **Step 3: 更新 AGENTS 触发词与维护规则**

把 LeadLag live/report 触发词统一指向 `docs/lead_lag_live_operations.md`；补充完成态 plan/spec 应在有效约束迁移后删除的长期规则；不改变代码、提交、live 安全和 subagent 约定。

- [ ] **Step 4: 全仓修复旧路径**

Run:

```bash
rg -n 'bitget_trading_follow_up|agent-handoff-gate-trade-architecture|gate_order_gateway_shm_design|gate_fastest_route_fusion_design|gate_fastest_route_fusion_threaded_bundle_guide|trade_fastest_route_fusion_design|lead_lag_live_operations_pipeline|lead_lag_live_runtime_plan|lead_lag_ack_latency_outlier_analysis|lead_lag_benchmark_environment_tail_analysis|lead_lag_runtime_latency_improvement_plan|lead_lag_cancelled_order_fillability_analysis|lead_lag_go_cpp_strategy_alignment|tui_onboarding_guide|tui_gate_account_monitor_design|websocket_client_future_optimizations|websocket_frame_codec_receive_strategies|websocket_read_write_benchmark_comparison' --glob '*.md'
```

Expected: 无命中；历史 report 如必须保留旧文件名，应改为明确的历史路径说明且不形成链接。

### Task 8: 删除本轮完成态 design/plan、验证并原子提交

**Files:**
- Delete: `docs/superpowers/specs/2026-07-12-documentation-consolidation-design.md`
- Delete: `docs/superpowers/plans/2026-07-12-documentation-consolidation.md`
- Remove empty directories: `docs/superpowers/specs/`, `docs/superpowers/plans/`, `docs/superpowers/`

- [ ] **Step 1: 将长期规则确认已进入 `AGENTS.md`**

Run:

```bash
rg -n '完成态|事实源|专题文档|onboarding' AGENTS.md
```

Expected: 已规定完成态 plan/spec 在有效内容迁移后删除，onboarding 只承担摘要和索引。

- [ ] **Step 2: 删除本轮 design/plan 与空目录**

使用 patch 删除本轮两个文件。Git 不跟踪空目录，无需额外提交目录删除。

- [ ] **Step 3: 检查 Markdown 相对链接**

对所有 tracked Markdown 中形如 `` `docs/...` ``、Markdown link 和明确文件路径的引用逐项验证目标存在；对命令行通配符、历史路径说明和输出示例进行人工区分。

Run:

```bash
git ls-files '*.md' | while read -r file; do rg -o 'docs/[A-Za-z0-9_./-]+\.md' "$file"; done | sort -u
```

Expected: 输出中的当前路径均存在，已删除路径无命中。

- [ ] **Step 4: 检查提交范围与 whitespace**

Run:

```bash
git status --short --branch
git diff --check
git diff --stat
```

Expected: 除用户原有 `M core/market_data/types.h` 外，只包含预期 Markdown 新增、修改和删除；`git diff --check` 无输出。

- [ ] **Step 5: 检查设计验收条件**

Run:

```bash
find docs -type f -name '*.md' | wc -l
du -sh docs
rg -n 'docs/superpowers/(plans|specs)' --glob '*.md'
```

Expected: `docs/` 约 28 份 Markdown；总体积显著低于整理前约 984 KiB；完成态 plan/spec 引用无命中。

- [ ] **Step 6: 原子提交正式整理**

Stage only Markdown changes:

```bash
git add -A -- '*.md'
git diff --cached --check
git diff --cached --name-status
git commit -m "docs: consolidate project documentation"
```

Expected: commit 成功，`core/market_data/types.h` 不在 staged diff 中。

- [ ] **Step 7: 最终状态验证**

Run:

```bash
git status --short --branch
git log --oneline -3
git show --stat --oneline --summary HEAD
```

Expected: 只剩用户原有 `M core/market_data/types.h`；HEAD 为正式文档整理提交；不执行 push。
