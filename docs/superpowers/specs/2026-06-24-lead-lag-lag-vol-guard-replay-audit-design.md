# LeadLag Lag Vol Guard Replay Audit 设计

> Superseded note（2026-06-25）：本文件保留为历史设计记录。当前事实源见
> `strategy/lead_lag/README.md`、`docs/project_onboarding_guide.md` 和
> `docs/diagnostic_fields.md`；`drift_guard` 已替代旧 `drift_limit` 并作为
> open-only emergency sanity guard 实盘化，audit CSV 的 drift 字段不再是
> `not_evaluated` 占位。

## 背景

当前 LeadLag Go reference 迁移边界已经明确：`freshness_auto` 和 `taker_buffer`
不进入实时策略热路径，`lag_vol_guard` / `drift_guard` 也不应直接放进 live
策略。下一步要先回答一个更窄的问题：如果 Go-like `lag_vol_guard` 作为
post-signal guard 存在，它会挡掉哪些 C++ legacy open signal，这些被挡信号是否
更集中在未成交、坏 PnL 或短机会窗口样本中。

现有 C++ `lead_lag_replay` 能按历史 `BookTicker` 顺序复现当前策略信号，并可通过
`--signals-output` 输出 triggered signal CSV。单独使用 `signal.csv` 不足以精确
复原 `lag_vol_guard`，因为 guard 需要 signal 前连续 lag BBO mid 序列、jump window、
amplitude window 和 cooldown 状态。因此第一版采用 replay 内嵌 audit：在 replay
过程中维护独立 guard 状态，只在 open signal 出现时写 audit CSV。

## 目标

- 在不改变 `Strategy` live path、不改变 replay synthetic position accounting 的前提下，
  评估 Go-like `lag_vol_guard` 对 open signal 的过滤效果。
- 输出稳定的 `lag_vol_guard_audit.csv`，包含每个 open signal 的 guard snapshot、
  would-block 结果和关键参数。
- 提供离线汇总脚本，把 guard audit 与已有 `order_detail.csv` / `position.csv`
  对齐，统计被挡组和未挡组的成交、cancel 和 PnL 差异。
- 第一版只评估 `lag_vol_guard`。`drift_guard` 字段保留为 `not_evaluated` 或 snapshot
  保留输出，不参与 block 结论。

## 非目标

- 不开放 `lead_lag.pairs.trigger.lag_vol_guard.mode=shadow/enforce`。
- 不修改 `SignalEngine` 的真实决策顺序，不替代现有 `drift_limit`。
- 不把 guard 逻辑放入 `tools/lead_lag/live_strategy` 或真实订单运行路径。
- 不在第一版实现 `drift_guard` enforce、taker buffer price model 或 close retry。
- 不用 audit 结果直接生成实盘配置。

## 推荐方案

新增 replay-only audit 能力：

```bash
./build/debug/tools/lead_lag_replay \
  --config <strategy-runtime.toml> \
  --data-reader-config <replay-data-reader.toml> \
  --signals-output /home/liuxiang/tmp/<run>/signals.csv \
  --lag-vol-guard-audit-output /home/liuxiang/tmp/<run>/lag_vol_guard_audit.csv
```

`lead_lag_replay` 的 wrapper 持有独立 `LagVolGuardAuditState`，不放入
`strategy/lead_lag/Strategy` 对象。replay 每处理一条 `BookTicker` 后：

1. 如果是 lag BBO，更新对应 symbol 的 guard state。
2. 如果 replay 策略产生 open signal，用当前 guard state 评估 would-block，并写 CSV。
3. 如果 would-block 原因是 hot trigger，则 audit state 推进 cooldown，用于后续 open signal。
4. 不把 audit 结果反馈给 strategy，不改变信号、position accounting 或 replay summary。

这样 replay signal 仍代表当前 C++ legacy 行为，audit CSV 只回答“如果 Go-like
`lag_vol_guard` 在 open signal 后执行，会产生什么过滤结果”。

## 方案取舍

### 方案 A：纯 Python post-process

输入 `signal.csv` 和 lag fusion `BookTicker` binary，离线重建 lag window 和 cooldown。

优点是实现快，不改 C++。缺点是 signal 与 replay 内部状态、price-changed 过滤和
synthetic position accounting 不在同一进程内，后续容易出现对齐偏差。

### 方案 B：直接把 guard 放进 Strategy shadow

在 `Strategy` 内新增 shadow guard 状态和日志。

优点是最贴近未来 live 集成。缺点是会触碰实时策略热路径和日志路径，和当前“不直接进入
live hot path”的边界冲突。

### 方案 C：replay 内嵌 audit，加 Python 汇总

C++ replay 负责时序敏感 guard snapshot，Python 负责和订单 / position report 对齐。

这是推荐方案。它把 guard 状态放在 replay 工具边界内，既能复用当前策略 replay
时序，也避免改动 live path。缺点是需要新增一个 replay-only C++ helper 和一个汇总脚本，
但范围清晰，测试成本可控。

## Guard 语义

第一版按 Go reference 的 `lag_vol_guard` 语义实现：

- 默认参数：
  - `jump_threshold = 0.005`
  - `jump_count = 3`
  - `jump_window = 5m`
  - `amplitude_threshold = 0.025`
  - `amplitude_window = 1s`
  - `cooldown = 15m`
- lag tick 更新：
  - 使用 lag BBO mid：`(bid + ask) / 2`。
  - 如果 previous/current mid 无效、非正数或相等，不新增样本。
  - 否则写入 amplitude window 的 current mid。
  - 计算 `abs(cur_mid / prev_mid - 1)`，写入 jump window。
  - window 时间戳优先使用 `BookTicker.exchange_ns`，为 0 时沿用 `BookTickerEventTimeNs()`。
- open signal 评估：
  - `lag_vol_jump_count` 是 `jump_window` 内 `absRTick >= jump_threshold` 的数量。
  - `lag_vol_amplitude` 是 `amplitude_window` 内 `max_mid / min_mid - 1`。
  - `lag_vol_hot = lag_vol_jump_count >= jump_count || lag_vol_amplitude > amplitude_threshold`。
  - 如果当前 signal time 小于 `cooldown_until_ns`，`would_block_reason=lag-vol-guard-cooldown`。
  - 否则如果 `lag_vol_hot=true`，`would_block_reason=lag-vol-guard-trigger`，并设置
    `cooldown_until_ns = signal_time_ns + cooldown_ns`。
  - 其他情况 `would_block=false`。

## CLI

新增 `lead_lag_replay` 参数：

```text
--lag-vol-guard-audit-output <path>
--lag-vol-guard-jump-threshold <double>       default 0.005
--lag-vol-guard-jump-count <uint32>           default 3
--lag-vol-guard-jump-window <duration>        default 5m
--lag-vol-guard-amplitude-threshold <double>  default 0.025
--lag-vol-guard-amplitude-window <duration>   default 1s
--lag-vol-guard-cooldown <duration>           default 15m
```

这些参数只作用于 replay audit。它们不读取、也不写入 live strategy TOML 中的
`trigger.lag_vol_guard` 字段，避免把实验参数误认为实盘配置。

## Audit CSV

`lag_vol_guard_audit.csv` 一行对应一个 replay open signal。建议字段：

```text
open_signal_index,symbol,symbol_id,action,side,
trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,
signal_lead_id,signal_lag_id,raw_price,
would_block,would_block_reason,
lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,
lag_vol_cooldown_active,lag_vol_cooldown_until_ns,
jump_threshold,jump_count_threshold,jump_window_ns,
amplitude_threshold,amplitude_window_ns,cooldown_ns,
drift_instant,ratio_std,drift_mean,drift_guard_outcome
```

字段说明：

- `open_signal_index`：audit CSV 内 open signal 顺序号，从 0 开始。
- `symbol` / `symbol_id`：来自 pair config 和 signal diagnostics。
- `action` 只允许 `kOpenLong` / `kOpenShort`。
- `signal_lead_id` / `signal_lag_id`：来自 signal 时刻的 latest BBO id，用于和
  live report 或 recorder bin 对齐。
- `raw_price`：open signal 的 lag 对手价，即当前 C++ legacy raw order price。
- `would_block_reason`：`none`、`lag-vol-guard-cooldown` 或
  `lag-vol-guard-trigger`。
- `drift_*` 第一版输出 `nan` 或空值，`drift_guard_outcome=not_evaluated`。

## 离线汇总

新增 Python 汇总脚本：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/summarize_guard_audit.py \
  --guard-audit /home/liuxiang/tmp/<run>/lag_vol_guard_audit.csv \
  --order-detail reports/<run_id>/order_detail.csv \
  --position reports/<run_id>/position.csv \
  --summary-json /home/liuxiang/tmp/<run>/guard_audit_summary.json \
  --summary-md /home/liuxiang/tmp/<run>/guard_audit_summary.md
```

对齐规则：

1. 优先用 `symbol_id + signal_lag_id + action + order_role=entry` 对齐
   `order_detail.csv`。
2. 如果同一 key 有多行，使用日志顺序或 `signal_index` 作为 tie-breaker。
3. `position.csv` 通过 `symbol_id + position_id` 关联到 entry order。
4. 无法对齐的 audit 行必须计数并输出到 summary，不能静默丢弃。

第一版 summary 输出：

- open signal 总数、would-block 总数、block rate。
- 按 symbol / action 的 open 和 block rate。
- 被挡组 vs 未挡组的 submitted / filled / partially filled / cancelled 数量。
- entry cancel 中 `cumulative_filled_quantity == 0` 的数量和比例。
- 若 `position.csv` 可用，按 would-block 分组统计 gross / net PnL、closed / open 数量。
- unmatched audit rows、unmatched order rows 和字段缺失 warnings。

## 错误处理

- CLI 参数非法时 fail fast，不退回默认值继续运行。
- audit output 打不开时 fail fast。
- lag BBO bid / ask 非正数、mid 非正数、时间戳非正数时跳过该 lag update，并在 summary
  计数中记录 skipped updates。
- event time 倒退时仍保留 Go-like window trim 语义：只按当前 now 裁剪，不主动清空 state；
  同时在 audit summary 中记录 `non_monotonic_event_time_count`。
- 如果 replay 没有任何 open signal，仍生成只有 header 的 CSV，并在 summary 中输出
  `open_signal_count=0`。

## 测试

C++ 测试：

- guard state 单元测试覆盖 jump count、amplitude、cooldown trigger、cooldown block。
- 参数默认值与 CLI override 解析测试。
- replay audit writer 测试 header 和基本 row 格式。
- `lead_lag_replay` 未传 audit output 时行为和现有 summary 不变。

Python 测试：

- `summarize_guard_audit.py` 能加载 audit / order / position CSV。
- 能按 `symbol_id + signal_lag_id + action` 对齐订单。
- 能统计 unmatched rows 和 zero-fill cancelled entry。
- 缺少 `position.csv` 时仍输出 order-level summary。

建议验证命令：

```bash
./build.sh debug
ctest --test-dir build/debug -R 'lead_lag|signal_csv_writer' --output-on-failure
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/summarize_guard_audit_test.py
git diff --check
```

如果本次只改 replay/tool 和 scripts，不修改 `evaluation/` 边界，仍可跑常规 diff check；
若后续移动 helper 到 `evaluation/`，必须补跑：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

## 成功标准

- 默认运行 `lead_lag_replay` 时现有输出和行为不变。
- 传入 `--lag-vol-guard-audit-output` 后，replay 额外输出 guard audit CSV，
  但 signal count、open / close / stoploss summary 不因 audit 改变。
- audit CSV 能回答每个 open signal 是否会被 Go-like `lag_vol_guard` 拦截，以及原因是
  trigger 还是 cooldown。
- summary 能把 guard 结果和 `order_detail.csv` / `position.csv` 对齐，并给出被挡组与未挡组
  的成交、cancel 和 PnL 差异。
- 设计和实现不触碰 live hot path，也不开放 live enforce 配置。

## 后续扩展

- 在 lag vol audit 稳定后，再用同一 replay audit 框架加入 `drift_guard` snapshot。
- 如果 `lag_vol_guard` 显示明确收益，再单独设计 live shadow 或 enforce；届时必须重新讨论它与
  当前 `drift_limit`、freshness guard 和 risk / sizing 的执行顺序。
- 如果需要评估 fillability，可把 audit result 合并进 cancelled order fillability 分析，
  继续使用 `signal_lag_id`、`ack_lag_id`、`accepted_lag_id`、`cancelled_lag_id`
  对齐 Gate lag fusion canonical BookTicker bin。
