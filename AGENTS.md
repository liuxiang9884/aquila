# AGENTS

本文件定义 `aquila` 项目的项目级代理协作约定。进入本仓库工作时，默认先读取并遵守这里的规则。

## 默认约定

- 默认使用中文回答，除非用户明确要求其他语言。
- 文档（`docs/`、`README.md`、设计/审查/计划等 markdown）使用中文撰写；代码注释（`.h`、`.cpp` 中的注释和 docstring）使用英文撰写。提交信息（commit message）使用英文。
- 文档中的描述性语言默认使用中文；专业名词、协议字段、枚举值、配置项、命令行参数、文件路径、代码标识符、日志原文和外部 API 名称保留英文或原文。不要把非专业叙述整段写成英文。
- 提交 PR 时，PR 标题与正文以中文为主；命令行、报错原文、外部链接和代码片段保留英文 / 原文。
- 优先遵循仓库现有的 `CMake + C++20` 结构、构建脚本和依赖管理方式，不做与当前任务无关的重构。
- 这是一个面向 crypto 的高频交易系统仓库，默认同时关注正确性、确定性、低延迟、可恢复性和可观测性。
- 对性能、时延、吞吐、并发安全和交易行为相关结论，必须基于实际测试、benchmark、profile 或运行证据，不凭主观判断宣称完成。
- 代理自行创建的 log、scratch config、临时输出、benchmark 临时产物和编译 / 测试临时目录默认写入 `~/tmp`（当前机器为 `/home/liuxiang/tmp`）。需要影响编译器或工具临时文件时，显式设置 `TMPDIR=/home/liuxiang/tmp`；除非用户明确要求或外部工具强制，不要把新的项目临时文件写入 `/tmp`。

## 对话生命周期和 onboarding

### 新对话启动

每个新对话进入本仓库后，先执行：

```bash
git status --short --branch
git log --oneline -8
```

然后按顺序读取：

```text
AGENTS.md
README.md
docs/project_onboarding_guide.md
docs/evaluation_support.md
```

如果继续 Gate 交易架构，再读 `docs/gate_trading.md`；如果继续 Binance 行情，再读
`docs/agent-handoff-binance-market-data.md`；如果继续 data session / config，再读
`docs/data_session_config.md`。读取后以 `docs/project_onboarding_guide.md` 的“最近已完成”“代码入口”
和“下一步建议”为当前事实源，再结合 `git status` / `git log` 判断是否存在未提交或未 push 的工作。

### Onboarding 文档书写规则

`docs/project_onboarding_guide.md` 是新对话接手入口，不是完整历史记录或设计说明。更新 onboarding 时默认遵守：

- 只写摘要和索引：记录当前事实、关键入口、重要边界、验证命令和下一步建议；不要把设计推导、实现细节、完整 benchmark 输出、live smoke 原始日志或逐提交历史写进 onboarding。
- 细节放到对应专题文档：Gate 交易放 Gate handoff / specs，DataReader 放 data reader / trading component 文档，LeadLag 放 strategy README / reconstruction / audit / replay 对账文档，benchmark 和性能分析放对应 benchmark / 对比文档。
- 已完成内容按模块合并：只说明“已经完成什么、现在入口在哪、仍缺什么”，不要保留每轮优化过程、历史数值流水账或已废弃方案。
- 性能和 live 证据只保留最新摘要：格式优先为日期、场景或命令、关键结果和边界说明；需要追溯原始输出时，在 onboarding 中给出专题文档路径。
- 下一步建议必须可执行：按方向列出优先动作、先读哪些文档、从哪些代码入口接手；避免宽泛 backlog。
- 不设置硬性大小上限；但如果 onboarding 明显膨胀，应优先压缩旧完成项和优化细节，而不是继续追加段落。
- “给下一个对话的 onboarding 提示”也只保留下一轮真正需要的事实和索引，避免复制整段历史。
- 已完成 implementation plan / spec 不是长期事实源；实现完成后先把仍有效的 contract、安全边界、验证入口和后续阻断迁移到
  对应领域专题文档，再删除完成态 plan / spec。每个主题只保留一个当前事实源，避免 handoff、design、guide 和 plan 重复维护。

### 结束对话触发词

当用户输入“结束对话”，或明确要求结束当前对话并交接时，默认自动执行下面的收尾流程：

1. 运行 `git status --short --branch` 和 `git log --oneline -8`，确认当前分支、未提交改动和最近提交。
2. 对照当前实现、配置和最近提交，整理相关文档；重点更新 `docs/project_onboarding_guide.md` 的当前状态、代码入口、验证命令和下一步建议。
3. 如果本轮改动影响 evaluation 边界、data session config、WebSocket 行为或 Gate / Binance handoff，同步更新对应文档。
4. 在 onboarding 中保留或更新一段“给下一个对话的 onboarding 提示”，让下一轮对话可以直接按该段话接手。
5. 跑本次文档整理所需的最小验证，至少包括 `git diff --check`；如果触碰 evaluation 边界，再运行 evaluation 边界检查。
6. 按项目规则自动提交本次文档整理，commit message 使用英文。
7. 提交成功后自动 push 当前分支到其 configured upstream / 默认远端；如果 push 失败，最终回复必须说明失败原因和当前 ahead/behind 状态。
8. 最终回复中给出提交哈希、push 结果、验证结果，并直接贴出给下一个对话使用的 onboarding 提示段落。

“结束对话”流程只做收尾、同步和交接，不主动开启新的功能实现。

### 实盘交易操作触发词

LeadLag 实盘启动和 report 生成的详细 agent pipeline 见 `docs/lead_lag_live_operations.md`；`AGENTS.md` 只保留触发词索引。

- 当用户输入“启动实盘测试”、“启动 12 pair 跑一小时”、“开始 live smoke”、“启动交易端”、“跑一段实盘交易”或等价表达时，执行 `docs/lead_lag_live_operations.md` 的“启动 Pipeline”。
- 当用户输入“总结上一次实盘交易”、“生成上一次实盘 report”、“生成本次实盘 report”、“打包 report”或等价表达时，执行 `docs/lead_lag_live_operations.md` 的“Report Pipeline”。
- 真实订单启动前仍必须遵守 `docs/project_onboarding_guide.md`、`docs/lead_lag_live_operations.md` 和 `docs/lead_lag_reconcile_design.md` 中的当前阻断条件、测试顺序和安全边界。

## 项目背景

`aquila` 用于实现 crypto 高频交易系统。当前仓库以 `CMake` 为主构建入口，核心工作通常会围绕以下方向展开：

- 交易所接入与协议适配
- 行情接入、解码与低延迟处理
- 下单、撤单、回报处理与订单状态管理
- 风控、限流、熔断与异常恢复
- 策略执行链路与内部事件流转
- benchmark、回归测试与性能验证

因此，这个项目更适合使用能够提升方案拆解、调试、测试、验证和性能分析质量的 skills，而不是偏前端或产品设计类流程。

## 多轮 Review 触发词

当用户输入“多轮review”“多轮 review”“multi-round review”或等价表达时，默认先询问用户本次 review 的目标，不自行假设目标。
用户给出目标后，按下面流程执行：

1. 明确 review 范围、目标和停止条件；如果目标是“代码逻辑没有问题”，重点检查控制流、状态机、错误处理、并发边界、资源所有权、数据一致性和恢复语义，不把单纯风格偏好作为阻断问题。
2. 每一轮至少派一个 subagent 做只读 review；如果当前工具不支持 subagent，先说明限制，再用主会话执行等价的只读 review。
3. 每轮 review 结束后，先向用户输出本轮发现的问题，按 Critical / Important / Minor 分组，并尽量给出文件路径、行号和影响说明。
4. 根据 review 结果执行修复：Critical 必须修，Important 默认修；Minor 只有在影响既定目标、风险低或用户要求时修。修复时保持改动最小，不引入无关重构。
5. 修复后运行与本轮修改对应的最小验证；验证通过后提交本轮修复，commit message 使用英文，且不要裹带无关改动。
6. 提交后进入下一轮 review。直到某一轮在既定目标范围内找不出需要处理的问题为止；最后输出最终验证、最后提交和仍未覆盖的风险边界。

## 高频系统设计和实现原则

- 默认把最低延迟作为最高优先级，其次才是吞吐量、资源利用率和其他性能指标。
- 评估方案时，优先关注主路径延迟和尾延迟，不只看平均吞吐或平均耗时。
- 如果某个方案能提升吞吐量，但会显著增加排队、批处理、额外拷贝、额外同步或尾延迟，应默认谨慎采用。
- 为了吞吐量引入的抽象、缓冲和并发设计，前提是不破坏低延迟主路径的确定性和可预测性。
- 尽量少用虚函数；如果可以，优先使用组合或 `CRTP` 替代，避免在关键路径中引入不必要的动态分发和间接跳转成本。
- 当最低延迟与系统正确性、确定性、可恢复性发生冲突时，不允许为了追求更低延迟牺牲系统正确行为。
- 任何关于低延迟或吞吐量收益的结论，都必须由 benchmark、profile、链路压测或实际运行证据支撑。

## Adaptive Development 工作流

本仓库统一使用 `adaptive-development` 代替 Superpowers 开发流程。Skill 的可提交事实源位于
`docs/skills/adaptive-development/SKILL.md`；如果当前 Codex catalog 已安装该 skill，使用 `$adaptive-development`，否则直接读取仓库副本并执行相同流程。

除非用户明确要求，本仓库不调用 `superpowers:*` 工作流。其他用于特定工具、外部文档或格式处理的窄域 skill 不受此限制。

每次讨论涉及编码、实现、测试、构建或运行配置、性能优化、长期技术目标或反复技术迭代时，先向用户输出：

```text
Level: L0|L1|L2|L3 — <一句判断依据>. Workflow: <本轮启用的质量门>.
```

输出分类后直接执行，不询问用户分类是否正确；用户有不同意见时按用户指定调整。执行中发现影响范围、不确定性、不可逆性或后果上升时，只能向上升级 Level，并立即补齐更高级别的质量门。

### Level 摘要

- `L0`：只读解释、调查、诊断或 review；不写计划，不修改文件，不创建 branch、commit 或 PR。
- `L1`：无预期行为变化的机械修改；不写计划，做最小 diff review、最小验证和原子提交。
- `L2`：边界明确、可验证且易回滚的局部行为修改；只写会话内简短计划，优先先建立失败测试或最小复现，再实现、review、focused verification 和原子提交。
- `L3`：高影响、高后果、难回滚、跨模块或长期有效的设计与实现；首次修改前必须使用专用 branch 和 worktree，写 markdown 计划，完成独立阶段 review、完整风险验证、原子提交、push 和 PR。

### aquila 的 L3 升级条件

除 skill 的通用条件外，下列任务默认归类为 `L3`：

- 修改交易所协议、行情解码或对外发布的 typed ABI / persistent format。
- 修改订单状态机、风控、资金安全、成交回报、恢复、reconcile、幂等或断线重连语义。
- 修改线程 / 进程模型、并发所有权、同步方式、内存可见性或低延迟主路径。
- 启动或改变真实订单、实盘安全门、stop-and-flat、账户状态和 emergency 操作。
- 声明或追求性能、尾延迟、吞吐、fillability 或 PnL 收益。
- 新建长期架构、跨模块 contract、长期技术目标，或反复优化已经暴露结构性问题。

仓库已有的 build、test、benchmark、live 安全和文档事实源规则继续生效；`adaptive-development` 负责选择流程强度，不替代领域 contract。

### 共同质量门

- `L2` / `L3` 的行为修改优先先证明测试或最小复现因目标行为缺失而失败；不要求每个函数单独测试，也不因测试后补而机械删除已有实现。
- 性能结论必须由 fresh benchmark、profile、压测或实际运行证据支持。
- 每次修改都必须形成原子 commit；只暂存本任务拥有的文件或 hunk，不裹带用户已有改动。
- `L3` 必须创建 PR；`L1` / `L2` 是否使用 branch/worktree 由主代理根据对当前主分支功能的影响和隔离收益决定。用户明确要求 branch、worktree 或 subagent 时必须执行。
- 是否主动使用 subagent 由主代理决定；主代理必须检查 diff 并重新验证。
- 没有 fresh 验证证据时不得宣称完成；无法运行必要验证时，只报告已验证范围和剩余风险。

### Grill Me 设计追问

每次进入设计讨论、架构方案、实现计划或关键交易链路取舍时，默认先主动询问用户是否启用 Grill Me
追问流程；用户确认后再启用对应 skill，不要擅自开启。

- `grill-me-basic`：基础版，适合轻量设计讨论、单模块方案澄清或用户想快速被追问时使用。
- `grill-me-enhanced`：加强版，适合跨模块架构、线程 / 进程模型、交易状态机、实盘安全边界、性能取舍等高风险设计审查。

询问时应给出明确建议：普通设计默认建议基础版；涉及订单链路、行情链路、风控、恢复、并发或低延迟主路径时默认建议加强版。如果用户拒绝或只想继续普通讨论，按 `adaptive-development` 分类继续，不再切换到其他通用开发流程。

## C++ 编码和依赖使用约定

- 新写或修改的项目自有 C++ 代码，命名遵循官方 Google C++ Style Guide 的 `Naming` 章节；缩进、换行、include 排序等机械格式遵循仓库根目录 `.clang-format`。该约定只采用 Google 的命名规则，不扩大为整篇 Google C++ Style Guide；第三方代码和生成代码优先保持上游或生成器输出风格。
- 项目已通过 vcpkg 使用 `magic_enum`，需要把枚举值转换为字符串时，优先直接使用 `magic_enum::enum_name(value)`，不要再手写 enum-to-string 的 `switch` 或专门的 `ToString` 包装函数。
- 项目代码中需要使用 map / unordered map 语义时，统一使用 Abseil 容器；哈希映射使用 `absl::flat_hash_map`，有序映射优先使用 `absl::btree_map`，不要新增 `std::map` 或 `std::unordered_map`，除非有明确兼容性理由并保持局部化。
- 项目已通过 vcpkg 使用 header-only 模式的 `fmtlib`，所有打印输出优先使用 `fmt::print`，所有字符串格式化优先使用 `fmt::format`、`fmt::format_to` 或 `fmt::format_to_n`。
- 需要写入已有缓冲区、避免动态分配或控制截断行为时，优先使用 `fmt::format_to_n`；低延迟热路径中不要为了格式化引入不必要的临时 `std::string`。
- 不新增 `printf`、`fprintf`、`snprintf`、`std::cout`、`std::format` 或 `std::to_string` 等新的格式化/打印路径，除非有明确兼容性或系统接口理由，并在代码中保持局部化。

## Evaluation 辅助代码约定

- `evaluation/` 只存放服务对比、benchmark 和测试验证的共享辅助代码；它不是生产路径的一部分。
- `evaluation/` 通过 header-only target `aquila_evaluation` 暴露，只允许 `test/` 和 `benchmark/` target 链接。
- `core/`、`exchange/`、`tools/` 不允许 include `evaluation/`，也不允许链接 `aquila_evaluation`。
- 只被单个 test 或 benchmark 使用的 helper，优先放在该 `.cpp` 的匿名 namespace；只被 test 使用且不会被 benchmark 复用的 helper 放在对应 `test/` 目录；只被 benchmark 使用且不会被 test 复用的 helper 放在对应 `benchmark/` 目录。
- 同时被 test 和 benchmark 使用，或作为生产实现稳定对照的 helper，放在 `evaluation/`，并使用对应的 `aquila::<domain>::evaluation` namespace。
- 如果生产文件需要说明某个对照实现、fixture 或 benchmark-only helper，只写英文注释指向 `evaluation/`、`test/` 或 `benchmark/` 的具体文件，不把实现留在生产 header 中。
- 修改 `evaluation/` 相关边界时，提交前至少运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

上述命令期望无命中。

## 项目级执行规则

- 修改交易所协议、行情处理、订单状态机、风控、恢复逻辑或线程模型时，优先补充或更新测试。
- 涉及顺序、幂等、重试、断线重连、超时、账户状态或成交回报处理的变更，必须显式验证结果。
- 当接口层已经明确参数类型和格式约束时，优先在类型、注释或文档中说明约束，不额外堆砌重复的运行时类型检查。
- 做性能优化时，先用 benchmark、profile 或明确热路径分析锁定瓶颈，避免凭直觉增加复杂度。
- 对初始化后不变、且会在热路径中反复使用的信息，优先在构建阶段缓存，不要把重复判断留到每次消息处理或每次下单路径里。
- 优先减少热路径中的动态分配、拷贝、格式化、无意义抽象层和低价值日志；如果结果区域会被完全覆盖，优先考虑直接分配而不是先填默认值。
- 优先为高频且可明确判定的主路径设计快路径；对低概率场景不要过早泛化，避免为罕见分支引入长期复杂度。
- 并发相关修改必须明确线程边界、对象所有权、同步方式和内存可见性假设；必要时在代码中留下简洁注释说明设计约束。
- 涉及性能结论的修改，完成后必须重新跑对应 benchmark 或最小性能验证，确认收益真实存在且没有引入行为回归。
- 需要做 A/B test、shadow 对照或候选策略评估时，默认不要在既有实盘策略进程和热路径中新增 shadow mode / 双路逻辑；应使用独立 replay、signal-only 或 live 进程，配合独立配置、日志和 report 运行候选实现，再做离线对账。只有用户明确要求并重新评估热路径成本后，才考虑在原进程内加入对照逻辑。
- 新增、重命名或删除诊断字段、log key、stats 字段、report CSV 字段时，必须同步更新 `docs/diagnostic_fields.md`；临时诊断字段要写清用途和删除条件，详细字段说明只放在该文档中。
- 默认沿用仓库现有构建入口；当前优先使用 `build.sh`、`cmake` 和 `ctest` 组织构建与验证。
- 修改应尽量最小化，避免顺手引入无关重构。
- 如果工作区已有未提交更改，先读取并理解现状，再在现有基础上做最小修改。
- 提交应保持原子性：代码实现、文档同步、benchmark 或额外整理如果不是同一项最小闭环，优先分开提交；当一项实现已验证完成时，先单独提交代码，再继续后续文档或 benchmark 修改，避免把中间状态混进同一提交。
- 任何一次修改完成后，默认自动提交到当前 branch；提交前仍应确保本次修改已有对应验证证据，且不要裹带无关改动。

## 一句话原则

先确认链路和约束，再实现；先验证根因，再修复；先跑证据，再宣称完成。
