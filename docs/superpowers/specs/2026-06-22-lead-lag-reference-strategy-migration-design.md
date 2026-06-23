# LeadLag Reference Strategy 迁移设计

## 背景

`reference/leadlag-current-strategy-package.zip` 打包了一份 Go 版 LeadLag 当前策略实现，来源说明如下：

- package commit：`e69bd9e`
- 策略代码提交：`86b1540`
- 原版对比 commit：`541bce2`
- 解压阅读路径：`/home/liuxiang/tmp/reference_strategy/leadlag-current-strategy-package`

本设计说明如何把该 reference 中的策略更新迁移到当前 C++ LeadLag。设计目标不是一次性照搬全部行为，而是在不破坏现有 live 安全边界的前提下，先建立可观测性和 shadow 评估，再逐步启用执行行为变更。

## 当前 C++ 基线

当前 C++ LeadLag 主要入口和行为：

- 配置：`strategy/lead_lag/config.h`、`strategy/lead_lag/config.cpp`
- 信号：`strategy/lead_lag/signal.h`
- 策略运行与订单：`strategy/lead_lag/strategy.h`
- 成本模型：`strategy/lead_lag/cost_model.h`
- live runner：`tools/lead_lag/live_strategy.cpp`
- report 生成：`scripts/lead_lag/analyze_order_detail.py`、`scripts/lead_lag/generate_live_report.py`

关键差异：

- C++ 仍使用 `trigger.drift_limit`，且在 `SignalEngine::OnLeadTick()` 中位于 open signal 前。
- C++ 订单价格使用 `execute.open_slippage` / `execute.close_slippage` 的 tick-based slippage。
- C++ freshness guard 是 pair 级固定阈值 `max_lead_freshness_ms` / `max_lag_freshness_ms`，只拦截 open。
- C++ report 已有 `raw_price`、`order_price`、signal timing、stage BBO id、Ack `x_in_time` / `x_out_time`，但没有 Go reference 的 `signal_decision` attribution schema。
- C++ 成本模型 `EntryCostBreakdown::RequiredEdge()` 只包含 fee、lead noise、lag noise；spread 和 lag spread buffer 仍作为 embedded friction。

近期 `SKYAI_USDT` 分析显示，未成交 IOC 的主要问题更像是机会窗口短、BBO 一档量薄、public BBO 与撮合视角存在时序差，而不是简单的 signal 到 Gate `x_in_time` 太慢。因此迁移设计优先增强诊断和 shadow guard，而不是直接加大激进价格或默认启用新 guard。

## Reference 更新摘要

Go reference 的主要新增能力：

- 先形成 open signal，再执行 post-signal guard。
- `lag_vol_guard`：基于 lag mid jump count、短窗振幅和 cooldown 拦截开仓。
- `drift_guard`：基于 instant ratio、ratio std、ratio mean 拦截开仓。
- `cost.taker_buffer`：支持 fixed/auto，开仓和平仓 buffer 分离，可选择是否计入成本模型。
- freshness auto warmup：按 exchange 的 BBO `LocalTimeMS - ServerTimeMS` 估计 `mean + 3 * std` threshold。
- normal close retry aggressive：普通 close 连续失败后提高 close buffer floor。
- `signal_decision` attribution：记录 signal sent/blocked、guard snapshot、raw BBO price、effective order price、depth L1/L2 和 stable linkage。
- order lifecycle 价格语义拆分：`avg_fill_price`、`raw_price`、`limit_price`、`order_price`。

迁移到 C++ 时不照搬 reference 的 runtime auto warmup。`entry_buffer` / `normal_close_buffer` 和 freshness threshold 的自动估计应在 live 启动前由配置生成流程完成；实时策略只加载固定参数并做判断。

## 方案取舍

### 方案 A：Diagnostic-first 分阶段迁移

先迁移配置结构、shadow guard 计算和日志/report，不改变 live 下单行为。完成离线和短 live 评估后，再逐项启用 guard 和 taker buffer。

优点：

- 对当前 live 风险最低。
- 能直接回答“哪些信号会被新 guard 拦、是否减少 SKYAI 类薄盘口订单”。
- 可以用同一批 replay/live report 对比旧行为和 shadow 新行为。

缺点：

- 到真正改变成交率需要多一步。
- 初期代码会多一些 shadow 字段和 report 逻辑。

### 方案 B：行为对齐快速迁移

直接把 reference 默认行为移植到 C++，包括默认启用 `lag_vol_guard` / `drift_guard`、taker buffer auto 和 attribution。

优点：

- 和 reference 行为最接近。
- 一次性完成策略语义切换。

缺点：

- 开仓样本会大幅变化，难以区分 guard、buffer、成本模型各自影响。
- live 风险高，尤其是默认启用 guard 和 auto buffer 会改变生产行为。

### 方案 C：完整 feature flag 迁移

一次性实现全部能力，但每个行为都用 feature flag 控制，默认全部关闭，仅在回测或指定配置中开启。

优点：

- 代码最终形态完整。
- 行为风险可控。

缺点：

- 初次实现面太大，测试矩阵复杂。
- 需要同时处理配置、策略、订单、attribution、report 多条链路，容易引入中间状态错误。

推荐采用方案 A。它最适合当前阶段：先把信号机会、guard 拦截、价格语义和 report 口径做清楚，再决定哪些行为值得上线。

## 目标行为

迁移后的 C++ LeadLag 应支持三种运行模式：

| 模式 | 目的 | 行为 |
|---|---|---|
| `legacy` | 保持当前生产行为 | 使用现有 `drift_limit`、tick slippage、fixed freshness。 |
| `shadow` | 评估 reference 更新 | 当前生产行为不变，但用启动前生成的固定参数计算 post-signal guard、taker buffer 和 signal decision 字段。 |
| `reference_enabled` | 启用 reference 行为 | post-signal guard 和 configured taker buffer 参与真实下单与成本模型。 |

初始实现只要求 `legacy` 和 `shadow`。`reference_enabled` 在离线评估完成后再实现。

## 配置设计

在 TOML 中保持现有字段兼容：

- `trigger.drift_limit`
- `execute.open_slippage`
- `execute.close_slippage`
- `max_lead_freshness_ms`
- `max_lag_freshness_ms`

新增字段建议放在每个 pair 下，避免跨 symbol 共享状态造成误解。下面是单个 `[[lead_lag.pairs]]` 内的示意片段：

```toml
[[lead_lag.pairs]]
symbol = "EXAMPLE_USDT"

[lead_lag.pairs.trigger.lag_vol_guard]
mode = "off" # off; shadow 待 attribution 实现后开放
jump_threshold = 0.005
jump_count = 3
jump_window = "5m"
amplitude_threshold = 0.025
amplitude_window = "1s"
cooldown = "15m"

[lead_lag.pairs.trigger.drift_guard]
mode = "off" # off; shadow 待 attribution 实现后开放
drift_instant = 0.015
ratio_std = 0.008
ratio_std_window = "1m"
drift_mean = 0.02
drift_mean_window = "1m"

[lead_lag.pairs.execute.taker_buffer]
mode = "off" # off | shadow
entry_fixed_pct = 0.0
normal_close_fixed_pct = 0.0
exclude_from_cost_model = false
source = "manual" # manual | generated

[lead_lag.pairs.execute.freshness_shadow]
mode = "off" # off | shadow
lead_threshold_ms = 0
lag_threshold_ms = 0
source = "manual" # manual | generated

[lead_lag.pairs.execute]
normal_close_retry_aggressive = false
```

设计约束：

- 新字段缺省必须保持现有 live 行为。
- taker buffer 的 `mode=shadow` 和 freshness shadow 只记录，不拦截，不改下单价，不改成本模型。
- `lag_vol_guard` / `drift_guard` 的 shadow attribution 尚未实现，Phase 1 配置层只接受 `off`。
- Phase 1 配置只接受 `off` / `shadow`；`FeatureMode::kEnforce` 作为后续实现预留，执行路径和 report 统计落地后再开放配置。
- `entry_fixed_pct` / `normal_close_fixed_pct` 是 ratio，配置层只要求字段可解析为数值。
- `normal_close_retry_aggressive` 在 Phase 5 执行路径落地前必须保持 `false`。
- `trigger.drift_limit` 暂时保留；后续实现 `drift_guard.mode=enforce` 前，必须明确两者互斥或执行顺序。
- 实时策略不接受 `auto_warmup`、`auto_fallback_pct` 这类 runtime learning 配置。自动估计只存在于启动前配置生成流程，输出进入固定字段。
- C++ 配置仍使用 TOML，不引入 JSON 配置读取。

## 信号和 Guard 设计

新增一个 open signal 中间结构，概念上对应 reference 的 `openSignal`：

- `action`
- `side`
- `raw_price`
- `quantity`
- `signal_value`
- `threshold`
- `exit_threshold`
- `target_space`
- `required_edge`
- `lead_move`
- `lag_move`
- `price_diff`
- `lag_part_ratio`
- `lag_spread_pct`
- `lag_spread_buffer`

当前 `SignalEngine::TryOpenLong()` / `TryOpenShort()` 已经计算大部分字段，但只返回 `SignalDecision`。迁移时应避免重复计算：

- 把 open metrics 保留为可传递结构。
- `SignalDecision` 继续作为外部接口。
- strategy runtime 层持有 `OpenSignalEvaluation`，用于 post-signal guard 和 attribution。

Phase 1 不应直接重排真实下单路径。实现上应保留当前 legacy decision 和 submit 链路，同时新增一个 reference shadow evaluator：

- legacy evaluator：继续使用现有 `SignalEngine::OnLeadTick()` 结果，真实下单行为不变。
- reference shadow evaluator：在诊断路径中先计算 candidate open signal，再按 reference 顺序计算 would-block。

未来 `reference_enabled` 的 Guard 执行顺序建议：

1. close / stoploss 仍优先于 open。
2. open signal 计算。
3. `parallel` / degraded pause。
4. `lag_vol_guard`。
5. `drift_guard`。
6. fixed 或 generated freshness threshold。
7. risk / sizing / order text / order submit。

第一阶段 shadow 模式下，reference evaluator 的步骤 3-6 只输出 would-block，不改变现有下单；当前已有的 parallel、degraded pause、`drift_limit` 和 fixed freshness 仍按 legacy 路径真实生效。

## Taker Buffer 和价格设计

当前 C++ 价格逻辑：

- `raw_price` 来自 lag 对手价。
- `order_price` = `raw_price +/- slippage_ticks * price_tick` 后按 side rounding。

Reference 价格逻辑：

- open long：`lag ask * (1 + entry_buffer)`
- open short：`lag bid * (1 - entry_buffer)`
- normal close long：`lag bid * (1 - normal_close_buffer)`
- normal close short：`lag ask * (1 + normal_close_buffer)`

迁移建议：

- 启动前配置生成流程负责给出 `entry_fixed_pct` 和 `normal_close_fixed_pct`。
- 实时策略只读取固定百分比，不维护 spread warmup、max spread 或 fallback buffer 状态。
- 第一阶段只新增 shadow 字段：`reference_entry_buffer_pct`、`reference_close_buffer_pct`、`reference_order_price`。
- 不改变当前 `order_price`。
- 第二阶段如果启用 taker buffer，需要明确 tick slippage 与 percent buffer 的组合方式。

组合方式推荐：

```text
effective_price = raw_price adjusted by percent buffer
rounded_order_price = side-aware round(effective_price)
```

不建议同时叠加 percent buffer 和 tick slippage，除非配置显式声明。两者同时启用时应在配置校验中拒绝，或要求 `price_aggression.mode = "ticks" | "pct"` 二选一。

成本模型：

- Phase 1 不改 `EntryCostBreakdown` 和 `RequiredEdge()`。
- 后续实现 taker buffer enforce 时，再新增 `entry_taker_buffer` / `normal_close_taker_buffer` 或等价字段。
- 后续只有 taker buffer enforce 且 `exclude_from_cost_model=false` 时纳入 `RequiredEdge()`；Phase 1 不接受 `taker_buffer.mode=enforce`。

## Freshness 设计

当前 C++ 已有 fixed freshness guard：

- pair 级 `max_lead_freshness_ms`
- pair 级 `max_lag_freshness_ms`
- 只对 open 生效
- freshness = `signal_decision_ns - *_exchange_ns`

Reference auto freshness 依赖 BBO `LocalTimeMS - ServerTimeMS`，并在 warmup 结束后 freeze `mean + 3 * std`。C++ 迁移不在实时策略中做这个 warmup，而是在启动前生成配置时完成：

1. 配置生成工具读取指定历史窗口或启动前采样窗口的 lead / lag BBO。
2. 对每个 exchange 计算 `local_ms - exchange_ms` 的样本分布。
3. 生成固定阈值：`threshold_ms = max(1, ceil(mean + 3 * std))`，保证 shadow 配置通过正阈值校验。
4. 输出到候选配置或 patch 文件。
5. 实时策略加载固定阈值并用当前 signal freshness 做比较。

迁移建议：

- 保留 fixed freshness 作为生产事实源。
- 第一阶段新增 generated freshness shadow stats：当前 freshness、generated threshold、would-block。
- 不允许实时策略在运行中更新 freshness threshold。
- 若未来 enforce generated freshness，应由启动前生成的配置写入 `max_lead_freshness_ms` / `max_lag_freshness_ms`，并在 report 中输出 `source=generated`。

## 启动前配置生成设计

新增一个离线或 preflight 配置生成步骤，职责是把 reference 的 runtime auto 部分转成固定配置：

```text
historical or pre-start BookTicker/Depth samples
  -> parameter generator
  -> generated config patch / generated TOML
  -> live strategy loads fixed parameters
```

生成内容：

- `entry_fixed_pct`
- `normal_close_fixed_pct`
- `freshness_shadow.lead_threshold_ms`
- `freshness_shadow.lag_threshold_ms`
- 可选审计元数据：样本窗口、样本数、mean、std、max、percentile、生成时间、输入 run_id。

实时策略边界：

- 不保存 warmup sample。
- 不计算 rolling mean/std 用于更新 threshold。
- 不计算 max spread 用于更新 buffer。
- 不因为 warmup 未完成改变交易行为。
- 配置中缺少 generated 参数时，shadow 对应功能 fail fast 或 disabled，不能退回 runtime auto；enforce 对应功能在执行路径落地前由配置层拒绝。

## Attribution 和 Report 设计

当前 live report 的事实源主要来自日志：

- `lead_lag_signal_triggered`
- `lead_lag_order_intent`
- `lead_lag_order_intent_rejected`
- `lead_lag_order_submitted`
- `lead_lag_order_response`
- `lead_lag_order_feedback`
- `lead_lag_order_finished`

新增 attribution 不应替代这些日志，而是先作为补充事件：

`lead_lag_signal_decision`

建议字段：

- `schema_version`
- `symbol`
- `symbol_id`
- `action`
- `side`
- `decision`：`sent`、`blocked`、`shadow_blocked`、`no_trigger`
- `block_reason`
- signal timing：复用现有 `SignalTiming`
- open metrics：`lead_move`、`lag_move`、`target_space`、`required_edge`
- current price：`raw_price`、`current_order_price`
- reference shadow price：`reference_order_price`
- guard snapshot：parallel、lag vol、drift、freshness fixed/generated
- BBO depth snapshot：初期只记录 BBO L1；Depth L1/L2 等 C++ 有稳定 depth 数据路径后再加

Report 侧新增统计：

- signal 总数、triggered 数、sent 数、blocked 数。
- 各 guard would-block / enforce-block 数。
- shadow reference order price 与当前 order price 的 tick 差。
- shadow required edge 与当前 required edge 差。
- 对已取消 IOC，按 guard would-block 分组统计 x_in/x_out `any` 和 `full` 可成交率。

字段说明要同步更新：

- `docs/diagnostic_fields.md`
- `docs/lead_lag_live_report_csv_schema.md`
- `strategy/lead_lag/README.md`

## 数据流

```text
BookTicker
  -> RawMarketState / recorders / alignment / threshold
  -> open signal metrics
  -> SignalDecision
  -> post-signal guard evaluation
  -> signal_decision log
  -> order intent / order submit
  -> order response / feedback / finished
  -> analyze_order_detail.py
  -> live report CSV / report.md
```

第一阶段不改变订单发送链路，只在 `SignalDecision` 与 order intent 之间增加 shadow evaluation 和日志。

## 错误处理和安全边界

- 新配置字段非法时，config parser 必须 fail fast，不使用隐式修正。
- shadow 计算出现 NaN / Inf / 无效 BBO 时，只记录 unavailable，不影响下单。
- enforce 模式下，如果 guard 需要的状态不可用，必须有明确行为：
  - lag vol guard 状态不可用：默认 allow。
  - drift guard ratio 不可用：默认 allow。
  - freshness threshold 启用但 latency 不可用：block。
- normal close retry aggressive 不影响 stoploss close。
- live mode 默认必须仍是 legacy 行为，除非配置显式开启 shadow；enforce 执行路径落地前配置层拒绝 enforce。
- 新增日志不能在热路径中引入无界分配或高频字符串拼接；默认 bounded 输出，full 输出只用于 replay / diagnostic run。

## 验证策略

第一阶段测试：

- Config parser：
  - 缺省字段保持 legacy。
  - `mode=shadow` 解析。
  - 执行路径未实现的 `mode=enforce` fail fast。
  - 非法 mode、负阈值、无效 duration 拒绝。
- Cost model：
  - buffer disabled 时 required edge 与现有测试一致。
  - shadow buffer 不改变 required edge。
  - enforce buffer 且不 exclude 时纳入 required edge 属于后续阶段。
- Signal / guard：
  - pre-signal drift limit 保持 legacy。
  - generated freshness shadow 输出 would-block，但不改变 `SignalDecision`。
  - lag vol guard jump/amplitude/cooldown 属于后续阶段。
  - drift guard instant/std/mean 属于后续阶段。
  - fixed freshness 与 generated freshness shadow 并存。
- Order price：
  - current tick slippage 行为保持。
  - reference shadow price side-aware rounding。
- Report：
  - `analyze_order_detail.py` 能解析 `lead_lag_signal_decision`。
  - `generate_live_report.py` 输出 guard summary。

验证命令按阶段选择：

```bash
./build.sh --preset debug
ctest --test-dir build/debug --output-on-failure -R 'lead_lag|strategy'
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
git diff --check
```

涉及 live 行为前，还需要 replay / signal-only 对比：

```bash
./build/debug/tools/lead_lag_replay ...
./build/debug/tools/lead_lag_strategy --validate-only ...
```

## 分阶段实施建议

### Phase 1：Shadow attribution

目标：不改变交易行为，只增加 `lead_lag_signal_decision` 和 shadow guard/price 字段。

交付：

- C++ config 新增 optional shadow config 结构，保留 enforce 枚举但配置层暂不开放，默认 legacy。
- 启动前配置生成流程输出 fixed taker buffer 和 generated freshness 候选值。
- Guard state 和 evaluation helper。
- signal decision log。
- report parser / schema 文档更新。
- replay 或 live report 能统计 would-block。

### Phase 2：离线评估

目标：用已有 live run 数据评估 reference guard 是否能过滤薄盘口/短窗口信号。

交付：

- 对 `20260619_SKYAI_fillability` 类样本追加 guard 分组分析。
- 用历史 BBO/Depth 生成 fixed taker buffer 和 freshness threshold 候选配置。
- 统计 shadow blocked 与 x_in/x_out any/full、成交/未成交的关系。
- 给出推荐 guard 参数范围。

### Phase 3：Taker buffer enforce

目标：在小 symbol 集合或 smoke run 中测试 percent buffer 是否改善成交率。

交付：

- `price_aggression.mode = ticks | pct` 二选一。
- enforce percent buffer 下单价。
- buffer 成本模型开关。
- open/close price attribution 和 PnL report 对齐。

### Phase 4：Guard enforce

目标：将已经验证有效的 lag vol / drift / generated freshness guard 开启到 live。

交付：

- per-pair guard enforce config。
- live preflight 输出 guard 配置摘要。
- report 输出 enforce-block 数和 missed opportunity 分析。

### Phase 5：Normal close retry aggressive

目标：改善普通 close IOC 反复未完全成交的残余仓位处理。

交付：

- close failure counter。
- normal close aggressive floor。
- stoploss close 排除。
- close attribution 和 position report 对齐。

## 开放问题

1. 后续开放 `drift_guard.mode=enforce` 时，`drift_limit` 与 `drift_guard` enforce 同时配置应 fail fast 还是定义顺序。建议 fail fast。
2. percent taker buffer 与 tick slippage 是否允许叠加。建议不允许叠加，配置二选一。
3. 配置生成流程使用多长历史窗口。建议先用与目标 live session 同一 symbol 的近 30 天样本生成候选，再用最近 live run 做 sanity check。
4. Depth L1/L2 是否能在当前 C++ live 数据路径稳定获得。若不能，第一阶段 attribution 只记录 BBO L1，taker buffer 先从 BBO spread 或已有 depth 产物生成。
5. `signal_decision` 是否需要写入独立 CSV，还是只进入 strategy log 后由 report parser 提取。建议先走 strategy log，避免新增生产写文件路径。
6. Reference 默认 guard enabled，但 C++ 迁移默认 legacy。建议后续若需要完全对齐 reference，再提供单独 profile，而不是改变默认 live 行为。

## 成功标准

Phase 1 完成后，应满足：

- 默认配置下，现有 signal、order price、freshness、risk 和 order lifecycle 行为不变。
- 打开 shadow 后，report 能回答每个 open signal 是否会被 lag vol / drift / generated freshness 拦截。
- 对未成交 IOC，report 能按 shadow guard 和 reference shadow price 分组比较 x_in/x_out 可成交性。
- 所有新增字段有 schema 文档，且 `analyze_order_detail.py` / `generate_live_report.py` 能稳定解析。
- 核心 lead_lag / strategy 测试通过，且 evaluation 边界检查无命中。
