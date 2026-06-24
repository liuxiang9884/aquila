# LeadLag 实盘运行 Runbook

## 目的

本文是 LeadLag 实盘运行和长时间测试的当前入口。它只保留当前边界、运行顺序、验证入口和仍未完成项；旧的逐任务实现计划、历史命令流水账和单次运行原始报告已清理。

## 当前边界

- `tools/lead_lag/live_strategy.cpp` 是 LeadLag live runner 入口。
- 默认 validate-only / signal-only，不提交真实订单。
- 只有同时满足显式 `--execute`、`strategy.mode=live`、API 凭据、feedback SHM 和 realtime data reader 时，才进入真实 live-orders runtime。
- 缺少凭据时 `RunLiveOrders()` 返回 exit code `2`；真实订单模式收到 `OrderFeedbackKind::kContinuityLost` 后停止 trading loop，并返回 handoff exit code `10`。
- V1 对齐 Sirius 边界：策略持仓由订单回报推导；停机后用 REST final check / emergency flatten 校验真实账户；不新增独立 `AccountPositionFeedbackSession`。
- `scripts/lead_lag/run_live_with_guard.py` 是真实订单测试推荐外层入口，负责 REST preflight、runner 退出监控、final REST check 和异常 stop-and-flat。
- Gate submit / cancel `5xx` response 只表示请求结果未知，不是确定拒单。当前 Gate runtime adapter 会把这类 response 映射为 `OrderResponseKind::kUnknownResult`；`OrderManager` 保留 pending order 等待 private feedback，LeadLag 标记对应 symbol `needs_reconcile` 并暂停新开仓。若对应订单随后收到 terminal private feedback，且该 symbol 没有其他 pending unknown order、没有 global continuity lost / manual intervention 等更高等级 degraded 状态，策略会自动清除该 symbol 的 unknown-result pause 并恢复新开仓；否则仍需要 REST reconcile 或 stop-and-flat。
- 真实订单模式不写 per-signal CSV；信号与下单意图通过 `lead_lag_signal_triggered`、`lead_lag_order_intent`、`lead_lag_order_intent_rejected` 和 `lead_lag_order_submitted` 中的 signal timing 字段对齐，主要包括 `signal_decision_ns`、`symbol_id`、`action`、`side`、`reduce_only`、`raw_price`、`lead_exchange_ns`、`lead_local_ns`、`signal_lead_id`、`lead_freshness_ns`、`lag_exchange_ns`、`lag_local_ns`、`signal_lag_id` 和 `lag_freshness_ns`；freshness 定义为 `signal_decision_ns - *_exchange_ns`。当前开仓 freshness guard 只作用于 `kOpenLong` / `kOpenShort`；每个 `[[lead_lag.pairs]]` 必须直接配置整数毫秒字段 `max_lead_freshness_ms` / `max_lag_freshness_ms`，当前 checked-in 配置通常为 lead `5ms`、lag `20ms`；close / stoploss 不受该 guard 影响。普通 open / close 的 `raw_price` 是 lag 侧对手价，买单为 lag ask，卖单为 lag bid，stoploss action 保留当前策略的保护价模型。成功提交后的订单主事实源是 `lead_lag_order_submitted`，其中包含 `local_order_id`、最终 `position_id`、`position_event`、`position_direction`、`entry_local_order_id`、`signal_role`、`order_role`、`quantity_text` 和 `price_text`；`lead_lag_order_response`、`lead_lag_order_feedback` 和 `lead_lag_order_finished` 输出处理 Ack、回报或终态时策略已看到的 `lead_exchange_ns` / `lag_exchange_ns`，response / feedback 还按状态输出 `<stage>_lead_id` / `<stage>_lag_id`，例如 `ack_lead_id`、`cancelled_lag_id`、`filled_lead_id` 和 `rejected_lag_id`；feedback 额外输出 `exchange_update_ns`、`local_receive_ns` 和 `fill_price` 供成交定位；终态日志 `lead_lag_order_finished` 同步输出 `position_id`、`position_direction`、`order_role`、`entry_local_order_id` 和 `order_finished_local_ns`。`scripts/lead_lag/analyze_order_detail.py --positions-output <path> --latency-output <path>` 可在生成 `order_detail.csv` 的同时生成 `position.csv` 和 `latency.csv`：`position.csv` 按 `run_id + symbol_id + position_id` 配对 entry / exit，每个有成交 exit 输出一行 closed / partial_closed slice，未平 entry 输出 open 行，并保留 entry / exit 的 `lead_exchange_ns`、`lead_local_ns`、`lead_freshness_ns`、`lag_exchange_ns`、`lag_local_ns`、`lag_freshness_ns` 和 `raw_price`；`gross_pnl` 用 average fill price、matched volume 和 `contract_multiplier` 计算，`net_pnl` 再扣除 config 估算 fee。`latency.csv` 一行对应一个本地订单，输出 send / ack / finish 本地时间、ack RTT、send-to-finish、ack-to-finish、exchange timestamp、signal / stage BBO id、lead / lag freshness 和 exchange-to-local 诊断字段；延迟判断优先看本地 RTT，不把本地和交易所时钟直接相减当作单程网络延迟。
- 启动前 freshness 配置生成流程使用 fusion canonical data reader 跑 `lead_lag_freshness_preflight` 采样 `n` 分钟；若随后要用相同 pair 和相同 fusion data session 启动实盘交易，采样结束后 fusion data process 可以继续运行，不需要为生成配置而停掉。preflight 输出 `freshness_summary.json` 后，用 `scripts/lead_lag/apply_freshness_preflight_summary.py --summary-json <summary> --config-in <pairs.toml> --config-out <generated.toml> --csv-out <audit.csv>` 更新 live pair TOML 中的 `max_lag_freshness_ms`。当前规则只使用 lag `p50_ms`：`p50 <= 20ms -> 20ms`、`20ms < p50 <= 50ms -> 50ms`、`50ms < p50 <= 100ms -> 100ms`、`p50 > 100ms -> 200ms`；CSV 同时保留 lag `mean_ms` / `p95_ms` 供审计。不要把 preflight C++ 直接输出的 `mean + 3std` candidate TOML 当作当前 live lag freshness 配置来源。
- `scripts/lead_lag/generate_live_report.py` 是真实订单运行结束后的报告生成入口，给定 `--run-id`、策略日志、策略配置和可选 guard stdout 后，生成 `reports/<run_id>/report.md`、`signal.csv`、`order_detail.csv`、`position.csv`、`latency.csv` 和同目录字段说明副本；生成后再由 agent 检查、commit，并在用户要求时 push。
- 已成交率 / 未成交 IOC 复查时，使用 `docs/lead_lag_cancelled_order_fillability_analysis.md` 的方案：从 `order_detail.csv` 筛选完全未成交开仓 cancel，用 `signal_lag_id` / `ack_lag_id` / `accepted_lag_id` / `cancelled_lag_id` 对齐 lag fusion BookTicker bin，按 `order_price` 判断 BBO 视角下 signal 到 cancel、Ack 到 cancel 和 accepted 到 cancel 区间是否触达可成交价格。
- Agent 触发词、实盘启动巡检和 report 打包提交流程见 `docs/lead_lag_live_operations_pipeline.md`。
- 运行 CPU 分区必须遵守 `docs/runtime_cpu_allocation.md`：当前 32 物理 core 机器上 `0-15` 为实盘保留区，`16-31` 为测试 / diagnostics / benchmark 区；测试任务不得占用实盘 hot path core，除非用户明确授权本轮例外。
- 2026-05-25 live run 中出现一笔 `219.023ms` Ack RTT outlier；分析见 `docs/lead_lag_ack_latency_outlier_analysis.md`。后续已落地 Gate `OrderSession` Ack latency diagnostic、affinity profile overlay 和 report diagnostic 字段；2026-05-26 拆核 30 分钟 run 没有复现 Ack outlier，最大 Ack RTT `6.738ms`，但仍未证明 2026-05-25 outlier 根因。
- 2026-05-27 当前接手决策：IOC partial-fill / decimal filled close 不再作为当前阶段 active blocker；后续如果 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再按具体问题复查。
- 2026-06-05 复盘 30-symbol run `20260604_0646_30symbols_30d_private` 时，使用 `signal_decision_ns - lag_exchange_ns` 统计 signal 下单时使用的 latest lag 对手价 freshness；该 run 在 freshness guard 代码生效前启动，因此这些数值是事后分析，不是当时的拦截结果。全量 `7017` 个 signal 的 lag freshness median `30.896ms`、p95 `789.799ms`、p99 `2478.972ms`、max `15371.972ms`；开仓 `6837` 单 median `32.205ms`、p95 `791.429ms`，其中 `3921/6837` 大于 `20ms`，事后估算会被当前 `max_lag_freshness_ms=20` 拦截。触发来源分组显示 `kBinance` 触发 `6954` 单，lag freshness p95 `793.368ms`；`kGate` 触发 `63` 单，p95 `0.857ms`。这说明旧 run 中 Binance 触发、用 latest lag 对手价下单时，经常使用明显 stale 的 lag quote；后续评估 30-symbol live 必须用新 binary 重启并让 per-pair freshness guard 生效。
- 2026-06-06 启动的 30-symbol 30 天实盘 run `20260606_052542_30symbols_30d_private_lagref_forceclaim` 已在 2026-06-09 09:04:09 UTC 停止，运行目录 `/home/liuxiang/tmp/20260606_052542_30symbols_30d_private_lagref_forceclaim/`。该 run 使用临时策略配置 `/home/liuxiang/tmp/20260606_052542_30symbols_30d_private_lagref_forceclaim/configs/strategy__lead_lag_30symbols_live_strategy_lagref_20260606.toml`，lead freshness 统一 `5ms`，lag freshness 按 symbol 配置；Gate data / order / feedback 使用 private plain，Binance data session 使用 public TLS。停止原因是 `lead_lag_feedback_continuity_lost scope=kGlobal reason=kSessionDisconnected`，策略 summary 为 `exit_code=10`、`recovery_state=degraded_needs_reconcile`、`needs_reconcile=true`；guard 随后执行 emergency flatten，结果 `verified_flat`，final open orders 为空，30 个目标合约 final position size 均为 `0`。
- 2026-06-10 已生成该 run 正式 report：`reports/20260606_052542_30symbols_30d_private_lagref_forceclaim/`。report 显示 signal `36457`，submitted / Gate send ok / ack / finished 均为 `16207`，有成交 order `1144`，position 明细 `618`；finished 状态为 `kCancelled=14228`、`kFilled=781`、`kPartiallyCancelled=363`、`kRejected=835`。actual gross PnL `-8.1375999 USDT`、actual net PnL `-19.41671335034 USDT`，raw gross PnL `-20.5388865 USDT`、raw net PnL `-31.81799995034 USDT`。生成 report 后的 Gate REST 只读复核文件在 `/home/liuxiang/tmp/20260606_052542_30symbols_30d_private_lagref_forceclaim/rest_check_20260610_084550_current/`，目标合约无非零仓位、无 open orders。
- 2026-06-17 启动的 fusion 行情实盘 run `20260617_073936_28symbols_no_h_30d_fusion_off_l0_live` 已停止，运行目录 `/home/liuxiang/tmp/20260617_073936_28symbols_no_h_30d_fusion_off_l0_live/`。该 run 使用 28 symbols（已删除 `TON_USDT` 和 `H_USDT`）、Gate / Binance fusion `N=4`、data session L0、fusion metadata off、release build、Gate data / order / feedback private plain、Binance public TLS。停机 final REST check 发现 `WLD_USDT +150`、`ENA_USDT +106`、`ZEC_USDT +20`、`NEAR_USDT +43`，guard emergency flatten 后 verified flat；停后独立 REST dry-run `/home/liuxiang/tmp/20260617_073936_28symbols_no_h_30d_fusion_off_l0_live/rest_status_after_stop_20260618_0210.json` 显示 `ok=true` 且目标合约 flat。根因是 Gate submit `500 INTERNAL / Request Timeout` 被旧逻辑当 terminal `kRejected`，但后续 private feedback / REST 显示实际 filled / partial-filled，导致策略 execution group 和 Gate single-mode 净仓分叉；`XLM_USDT` 的 reduce-only `empty position` reject 风暴来自真实净仓已被 opposite open net flat，而策略仍持 synthetic long。修复提交 `9a13268` 已把 Gate `5xx` 转为 `kUnknownResult`，保留 pending order 并暂停新开仓；后续实现已支持对应 terminal feedback 精确解决 unknown order 后自动恢复该 symbol 新开仓。
- 2026-06-06 12:27 UTC 对该 run 做快照分析，快照目录 `/home/liuxiang/tmp/20260606_052542_30symbols_30d_private_lagref_forceclaim/analysis_snapshot_20260606_122754_current/`。当时 signal `4387`，order submitted / finished `1706`，freshness rejected `2681`，其中 `stale_lag_quote=2594`、`stale_lead_quote=87`；finished 状态为 `kCancelled=1560`、`kFilled=89`、`kPartiallyCancelled=48`、`kRejected=9`。有成交订单 `137`，matched position `75`，gross PnL `+0.1246 USDT`，estimated fee `1.2433 USDT`，net PnL `-1.1187 USDT`；raw gross PnL `-0.2326 USDT`，raw net `-1.4759 USDT`。滑点整体为 `-0.3572 USDT`，entry `-0.3531 USDT`、exit `-0.0041 USDT`，说明该快照下滑点整体略有利但不足以覆盖 fee。Ack RTT p50 `0.630ms`、p95 `4.502ms`、p99 `31.342ms`、max `473.738ms`；Ack exchange-to-local p50 `0.269ms`、p95 `0.732ms`；send-to-finish local p50 `7.970ms`、p95 `1421ms`。
- 2026-06-06 12:36 UTC 对同一 run 拆解 lag freshness，快照目录 `/home/liuxiang/tmp/20260606_052542_30symbols_30d_private_lagref_forceclaim/analysis_snapshot_20260606_123601_lag_freshness_cause/`。开仓 signal 全部由 `kBinance` 触发；开仓 lag freshness p50 `152.073ms`、p95 `838.611ms`，但 `lag_exchange_ns -> lag_local_ns` p50 `0.264ms`、p95 `0.665ms`、p99 `2.023ms`，`lag_local_ns -> signal_decision_ns` p50 `151.809ms`、p95 `838.348ms`。对 `2603` 个 `stale_lag_quote` rejected open signal，`lag_exchange_ns -> lag_local_ns` p50 `0.258ms`、p95 `0.530ms`，`lag_local_ns -> signal_decision_ns` p50 `353.605ms`、p95 `1027.652ms`，network-dominant stale case 为 `0`。当前证据支持：主要原因不是网络接收慢，而是策略触发时本地最新 Gate lag quote 本身已经很久没有更新。若要严格区分“Gate 未发新 BBO”与“data session / reader 未消费到”，下一步需要补 per-symbol data session update counter、SHM publish counter 和 strategy reader consume counter。
- `signal.csv`、`order_detail.csv`、`position.csv` 和 `latency.csv` 字段说明见 `docs/lead_lag_live_report_csv_schema.md`。
- replay / signal-only live 只有显式 `--signals-output` 才写 signal CSV。

## 关键配置

| 文件 | 用途 |
| --- | --- |
| `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` | requested 12-symbol signal-only / dry-run runtime；文件名保留历史 `11symbols`，内容已追加 `ETH_USDT`。 |
| `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml` | requested 12-symbol live-orders runtime；真实订单 `--execute` 默认使用该配置，不要误用 signal-only config。 |
| `config/strategies/lead_lag_requested_11symbols_20260522.toml` | requested 12-symbol LeadLag pair / execute / risk 配置。 |
| `config/data_sessions/gate_data_session_requested_20260521.toml` | Gate requested symbols data session。 |
| `config/data_sessions/binance_data_session_requested_20260521.toml` | Binance requested symbols data session。 |
| `config/data_readers/strategy_data_reader_requested_20260521.toml` | LeadLag requested symbols realtime reader。 |
| `config/order_feedback/gate_order_feedback_session.toml` | Gate private order feedback session。 |
| `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml` | live-orders 核心链路 affinity profile；目标为 Gate MD CPU2、Binance MD CPU3、strategy / order owner CPU4、feedback CPU6、log CPU5。 |

该 affinity profile 位于 `docs/runtime_cpu_allocation.md` 定义的 `0-15` 实盘保留区内。后续调整 profile 时，必须同步确认没有把测试 / benchmark core 混入实盘 hot path，也不要把测试 data session / recorder 放到 `0-15`。

requested 配置当前覆盖：

```text
PROVE_USDT, RAVE_USDT, ZEC_USDT, SIREN_USDT, ETC_USDT, DASH_USDT,
RIVER_USDT, SUI_USDT, INJ_USDT, ENA_USDT, BRETT_USDT, ETH_USDT
```

`RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 是 Gate decimal-size 合约；instrument catalog 由 `order_size_min=0.1` 推导 `quantity_step=0.1` 和 `quantity_decimal_places=1`。C++ order / feedback / Gate encoder / LeadLag sizing 已使用 `double quantity` + `quantity_text`，Gate WS 下单带 `X-Gate-Size-Decimal: 1` 时把 JSON `size` 编码为 string。

12-symbol 策略当前启用：

- `lead_lag.risk.max_gross_notional = 2000.0`：限制 strategy 全局持仓和 pending open reservation 的总 notional；只拒绝新开仓，不阻止 reduce-only close。
- `execute.open_slippage = 2`、`execute.close_slippage = 2`：12 个 symbol 均按 `price_tick` 调整 IOC limit 价格；slippage 只影响实际下单价，不改变 signal 触发条件。

## Stop Loss 设计建议

当前 `strategy/lead_lag/signal.h` 中的 stop loss 是 trailing stop：

- 只在 lag tick 路径检查；同一个 holding group 先检查 stoploss，再检查普通 close。
- 开仓订单 terminal 且有成交均价后，`trailing_price` 初始化为 `AverageFillPrice()`。
- long 持仓用 lag bid 单向上移 `trailing_price`，当 `lag_bid / trailing_price - 1.0 <= -execute.trailing_stop` 时触发 `kStoplossLong`。
- short 持仓用 lag ask 单向下移 `trailing_price`，当 `-(lag_ask / trailing_price - 1.0) <= -execute.trailing_stop` 时触发 `kStoplossShort`。
- stoploss order 是 reduce-only IOC limit；long raw price 为 `lag_bid * 0.995`，short raw price 为 `lag_ask * 1.005`，实际下单价还会继续叠加 `execute.close_slippage`。

2026-06-02 对 `LAB_USDT` 实盘亏损样本复盘后，当前判断是：现有 stop loss 更适合作为灾难兜底，不适合作为主要亏损控制层。`execute.trailing_stop = 0.01` 等价于 100bps trailing 触发，对 LAB 这类高波动短周期 signal 偏宽；触发后的 `0.995` / `1.005` 保护价再叠加 close slippage，也会让成交优先级和最差可接受价格耦合在一起。

后续推荐改造方向：

1. 保留当前 trailing stop 作为灾难兜底，默认仍可放在约 100bps 级别，避免行情快速反向时持仓无限暴露。
2. 增加更近的策略退出层，命名上与 stoploss 区分，例如 `protective_close` / `risk_close`；初始实验可按 20-50bps 或基于实时 volatility 的动态阈值触发，用于处理普通反向波动，而不是等到灾难 stop。
3. 把 stoploss 触发阈值和执行保护价拆开配置，例如 `stoploss_trigger_bps`、`stoploss_execution_slippage_ticks` 或 `stoploss_execution_slippage_bps`；不要继续把 `0.995` / `1.005` 硬编码和 `close_slippage` 叠加作为唯一执行模型。
4. report 分析中继续按 `action` 区分 `kClose*` 与 `kStoploss*`，并单独统计 stoploss 触发前的 trailing fallback、执行滑点、实际 / raw PnL，避免把策略 close 亏损和灾难 stop 亏损混在一起。

## 已完成证据摘要

- BTC_USDT flat-account emergency smoke、tiny-position emergency smoke 和隔离 `ContinuityLost` stop-and-flat smoke 已完成。
- ZEC_USDT `--smoke-open-close` 小额 filled open / close 和 `--smoke-unfilled-cancel` 小额挂单撤单 smoke 已完成；最终 REST 复核 open orders 为空、position `size=0`。
- 本地端到端 benchmark 已覆盖 signal-to-submit 路径和 feedback 回报路径。
- 2026-05-22 release 11-symbol live-orders guarded run 不是通过项：只完成 1 组 RIVER_USDT strategy open / close；RAVE_USDT IOC partial fill 在 REST 上可见，但当时 private feedback / strategy terminal feedback 缺失，guard 停机后平仓。
- 2026-05-23/24 已修复 decimal quantity、Gate decimal-size WS 编码、Gate `futures.orders` 高精度 fill price parser、REST final check / emergency flatten decimal residual 判断；按 2026-05-27 当前接手决策，这些项不再作为当前阶段 active blocker，后续遇到 terminal feedback、filled close 或 REST residual 异常再复查。
- `--smoke-submit-reject` 和 `gate_order_session_failure_probe` 已有诊断入口和测试，但 ZEC_USDT 安全 IOC、BTC zero-size submit、nonexistent cancel live 探测均未收到最终 failure response，不能计入已完成 smoke。
- 2026-05-26 已落地 Gate `OrderSession` Ack latency diagnostic、runtime affinity profile overlay、report diagnostic 字段和 `exchange_lifecycle_ns = finish_exchange_ns - ack_exchange_ns`；30 分钟拆核 run 正常退出 flat，最大 Ack RTT `6.738ms`，最大 exchange Ack-to-finish `37.336ms`，无成交。

## 推荐测试顺序

1. 启动 Gate / Binance data session 和 Gate order feedback session，确认 SHM 和 feedback 都在预期路径。
2. 如需 live / replay 信号对比，执行 `docs/lead_lag_live_replay_testing.md` 中的 `lead_lag_live_replay_signal_parity <duration>`，不使用 `--execute`。
3. 真实订单复核前，先跑 targeted tests：

```bash
ctest --test-dir build/debug -R '(lead_lag|signal_csv_writer|gate_order_feedback|gate_submit_response_parser|order_latency)' --output-on-failure
```

4. 如果本轮目标是重新复查 decimal-size / IOC partial-fill，使用小额 live smoke：
   - 选择 allowlist symbol，优先覆盖 `RAVE_USDT` 或同类 decimal-size 合约。
   - 使用 `scripts/lead_lag/run_live_with_guard.py` 包住 `lead_lag_strategy --execute`。
   - 所有临时 log、stdout、REST summary 和运行产物写入 `/home/liuxiang/tmp/<run_id>`。
   - 结束后检查 strategy terminal feedback、feedback session summary、REST open orders、position `size`、`value` 和 `margin` residual。
5. 常规 12-symbol guarded live smoke / latency run 继续按 `docs/lead_lag_live_operations_pipeline.md` 执行，并保留 guard、REST final check 和 affinity profile。
6. 12-symbol smoke 稳定后，再继续 30 分钟、2-4 小时或更长时间真实订单 guarded run。

## 常用命令形态

signal-only：

```bash
./build/release/tools/lead_lag_strategy \
  --config config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml \
  --connect-data \
  --duration-sec <duration_sec> \
  --signals-output /home/liuxiang/tmp/<run_id>/live_signals.csv
```

真实订单 guarded run：

```bash
scripts/lead_lag/run_live_with_guard.py \
  --settle usdt \
  --contract <SYMBOL> \
  --poll-timeout-sec 30 \
  --affinity-profile config/runtime_affinity/lead_lag_requested_12symbols_node0.toml \
  --affinity-output-dir /home/liuxiang/tmp/<run_id>/configs \
  --no-pretty \
  -- \
  ./build/release/tools/lead_lag_strategy \
    --config config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml \
    --connect-data \
    --execute \
    --duration-sec <duration_sec>
```

账户复核：

```bash
scripts/gate/account/query_gate_account.py orders --contract <SYMBOL> --status open --no-pretty
scripts/gate/account/query_gate_account.py positions --contract <SYMBOL> --no-pretty
```

## 通过判定

- live runner 按预期 exit code 退出；非零退出必须由 guard summary 解释。
- final REST check 证明 in-scope open orders 为空。
- position `size=0`，且 `value` / `margin` residual 符合当前 decimal residual 判断。
- strategy terminal feedback 与 REST finished order 能对齐；IOC partial fill 必须有 terminal feedback 或被明确标记为未通过。
- feedback session 没有未解释断线、decode unrecoverable、SHM queue full 或 continuity lost。
- 对性能或延迟的结论必须附本轮 benchmark / live log 证据。

## 下一步

- 继续 failure response 探测前，先确认 Gate 可返回最终 error 的安全请求形态；Gate `5xx` 不应当作确定 error 回归。
- 后续 12-symbol latency / guarded run 必须按 `docs/lead_lag_live_operations_pipeline.md` 使用 affinity profile，并在 report 中同时区分 Ack RTT、send-to-finish 和 exchange Ack-to-finish。
- IOC partial-fill / decimal filled close 当前不再作为 active blocker；如果后续 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再恢复 targeted small smoke 复查。
- 后续补 REST reconcile / low-frequency drift guard 时，优先覆盖 `kUnknownResult` 订单、feedback gap 和 Gate single-mode net position 与策略 synthetic position 分叉。
- account / position realtime feedback 是 V2 可选能力，不是当前 V1 长跑前置项。
