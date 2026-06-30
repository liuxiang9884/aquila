# aquila 新对话导读

这份文档是接手入口，只保留当前事实、入口索引、验证命令和下一步。历史推导、完整 run 输出、字段细节和 benchmark 细节放在专题文档或 `reports/` 中；当前 branch / ahead / dirty 状态只信 `git status`。

## 先做什么

在 `/home/liuxiang/dev/aquila` 先运行：

```bash
git status --short --branch
git log --oneline -8
```

然后按顺序读：

```text
AGENTS.md
README.md
docs/project_onboarding_guide.md
docs/evaluation_support.md
```

按方向继续读：

| 方向 | 优先文档 |
| --- | --- |
| LeadLag live / report | `docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_operations_pipeline.md`、`docs/lead_lag_live_report_csv_schema.md`、`docs/lead_lag_reconcile_design.md` |
| LeadLag cancelled order fillability | `docs/lead_lag_cancelled_order_fillability_analysis.md`、`docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_report_csv_schema.md` |
| LeadLag benchmark / tail | `docs/lead_lag_benchmark_environment_tail_analysis.md`、`strategy/lead_lag/README.md` |
| Gate Ack latency / RTT probe | `docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_runtime_latency_improvement_plan.md`、`docs/gate_order_session_rtt_probe_design.md`、`docs/diagnostic_fields.md` |
| DataReader / data session | `docs/data_reader_config.md`、`docs/data_session_config.md`、`docs/data_session_shm_communication_design.md` |
| Gate / Binance fastest-route fusion | `docs/gate_fastest_route_fusion_design.md`、`docs/gate_fastest_route_fusion_shadow_results.md`、`docs/gate_fastest_route_fusion_threaded_bundle_guide.md` |
| Fusion / Tardis BBO 对账 | `docs/fusion_tardis_bbo_comparison.md`、`scripts/market_data/compare_fusion_tardis_book_ticker.py` |
| Runtime CPU 分配 | `docs/runtime_cpu_allocation.md` |
| Gate 交易架构 | `docs/agent-handoff-gate-trade-architecture.md`、`docs/strategy_order_component_model.md` |
| Gate 多路 OrderSession / MGW | `docs/agent-handoff-gate-trade-architecture.md`、`docs/strategy_order_component_model.md`、`docs/gate_order_session_rtt_probe_design.md` |
| TUI / account monitor | `docs/tui_onboarding_guide.md`、`docs/tui_gate_account_monitor_design.md` |
| Instrument catalog / market data scripts | `docs/futures_contract_metadata_fields.md`、`docs/agent-handoff-binance-market-data.md` |

## 当前事实

- 项目是 C++20 / CMake 的 crypto 低延迟交易系统；默认同时关注正确性、确定性、低延迟、可恢复性和可观测性。
- 公共 order / runtime contract 在 `core/trading/*` + `aquila::core`；Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h`。
- Gate / Binance `BookTicker` data session、SHM、strategy `DataReader`、Gate submit/cancel、Gate private `futures.orders` feedback、`OrderManager`、`TradingRuntime`、LeadLag replay / live-orders、TUI market data monitor demo 已落地。
- 2026-06-30 Gate 多路 `OrderSession` / `MultiOrderSessionGateway` baseline 已落地，`gate_order_session_rtt_probe` 的 fanout batch scheduler 也已在 dry-run / unit-test 层落地，但 threaded fanout 和 live execute fanout timing 尚未接入。多路层目标是把策略 / OMS 已生成的 child order 低 skew 地 fanout 到 N 条 Gate trading connection；duplicate / split、winner、overfill 和非赢家 cancel 触发点都属于策略 / OMS，不属于 gateway。`MultiOrderSessionGateway` 仍在 `OrderManager` 后，内部管理显式 `RouteTable(local_order_id -> session_index)`、session health 和账户级 submit / cancel / pending budget；每个 child order 必须有唯一 `local_order_id`，cancel 默认回到原始 session；`OrderFeedbackSession` 仍保持单账户级事实源。当前最小实现是 `1 thread : n OrderSession` baseline，运行时传入 session 列表，测试 / 首个实验使用 `n=4`；目标线程模型仍是 per-session worker capability，baseline 不宣称 fillability 改善。
- 当前 32 物理 core 机器必须遵守 `docs/runtime_cpu_allocation.md`：`0-15` 为实盘保留区，`16-31` 为测试 / diagnostics / benchmark 区；测试任务默认不要占用实盘 hot path core。
- 临时 log、scratch config、live snapshot、benchmark 临时产物默认写入 `/home/liuxiang/tmp`。
- 2026-06-06 已完成 LeadLag 策略多轮结构 review / refactor，并逐轮跑 LeadLag tests 和 strategy / runtime / feedback benchmark 后提交。最终重点改动包括 market-calc 诊断拆分、open signal 公共计算、active signal finalization、external order submission / preparation 拆分和 benchmark fixture 修正。
- LeadLag `OpenSignalSubmitPath` benchmark tail 已归档到 `docs/lead_lag_benchmark_environment_tail_analysis.md`：当前本机是 KVM / AWS 环境，未做 CPU isolation / `nohz_full`，CPU16 有 timer / RCU / softirq / IRQ 干扰证据；`p50` / median 更接近代码路径，`p99` / max 不应在当前环境下直接归因到策略代码。
- 30-symbol 30 天实盘 run `20260606_052542_30symbols_30d_private_lagref_forceclaim` 已在 2026-06-09 09:04:09 UTC 因 Gate order feedback continuity lost 停止；guard 已 emergency flatten 并 `verified_flat`。运行目录是 `/home/liuxiang/tmp/20260606_052542_30symbols_30d_private_lagref_forceclaim/`，正式 report 在 `reports/20260606_052542_30symbols_30d_private_lagref_forceclaim/`。
- 2026-06-14 Gate / Binance 多路最快行情融合 V1 已落地：`gate_book_ticker_fusion` 和 `binance_book_ticker_fusion` 独立进程从 N 路 source `BookTicker` SHM 读取，按 `(symbol_id, BookTicker.id)` first arrival 输出 canonical SHM。sidecar metadata 由编译期开关 `AQUILA_BOOK_TICKER_FUSION_METADATA_MODE=file|off` 控制，默认 `file`；`off` build 不打开 metadata writer，也不构造 metadata record。V1 不等待、不回看、不补洞、不比较 payload、不在 hot path 统计 duplicate / conflict。BTC / ETH、`N=4`、30 分钟 Gate shadow 和后续 Gate / Binance 各 30-symbol、`N=4`、30 分钟 release L4 shadow 已完成；最新 30-symbol 结果显示 Gate fusion p99 / p99.9 相对最佳单路改善 `31.71%` / `91.97%`，Binance 改善 `7.79%` / `39.75%`，fusion hop p99 分别为 `0.794us` / `1.395us`。Gate L4 attribution 显示 `>5ms` source outlier 主导阶段是 `exchange_ns -> kernel_rx_ns`；Binance 因 TLS 不能拆出 `kernel_rx_ns`，但 parser / SHM publish 不是主因。详见 `docs/gate_fastest_route_fusion_shadow_results.md`。
- 2026-06-28 已新增 live fusion canonical BBO 与 Tardis `book_ticker` CSV 的离线对账脚本和 20260627 30-symbol 结果记录：`scripts/market_data/compare_fusion_tardis_book_ticker.py` 按 `exchange_ns // 1_000_000` 与 Tardis `timestamp // 1_000`、price / quantity tick units 做 multiset 对账，并支持 `--near-ms` 对同价量邻近时间差做分类。结果显示 Binance Tardis 侧 30-symbol 全部可 strict 匹配到 fusion，Tardis 是 fusion 总量的 `99.9352%`，差异为 fusion-only `59,731` 条；Gate strict 差异主要来自 timestamp 语义偏移，`near_ms=5` 后 Tardis-only candidate 为 `10,085` 条。Gate BTC_USDT fusion `id` 无重复、相邻 BBO 无完全重复，`id` 严格递增；`66` 个 `exchange_ns` 回退点应按 `id` / 发布顺序复现，只在时间轴统计时按 `exchange_ns` 排序。Tardis `timestamp` 是交易所提供的 us 时间，`local_timestamp` 是 Tardis 接收时间；Gate JSON BBO 时间字段是 `result.t`。详情见 `docs/fusion_tardis_bbo_comparison.md`。
- 2026-06-16 已新增 LeadLag fusion live smoke 配置族：有效 universe 为原 30-symbol 删除 `TON_USDT` 后加入 `BTC_USDT` / `ETH_USDT`，共 31 个 symbol。删除 `TON_USDT` 的原因是 Gate private plain `futures.book_ticker` 返回 `unknown currency pair TON_USDT`，会导致整批订阅失败。Gate / Binance launch config 分别是 `config/market_data_fusion/gate_data_fusion_31symbols_no_ton_book_ticker_4sources.toml` 和 `config/market_data_fusion/binance_data_fusion_31symbols_no_ton_book_ticker_4sources.toml`；strategy config 是 `config/strategies/lead_lag_31symbols_no_ton_fusion_live_strategy_20260616.toml`，data reader 读取 canonical fusion SHM。
- 2026-06-17 启动的 fusion 行情实盘 run `20260617_073936_28symbols_no_h_30d_fusion_off_l0_live` 已停止，运行目录为 `/home/liuxiang/tmp/20260617_073936_28symbols_no_h_30d_fusion_off_l0_live/`。停机 final REST check 发现 `WLD_USDT +150`、`ENA_USDT +106`、`ZEC_USDT +20`、`NEAR_USDT +43`，guard 已 emergency flatten；停后独立 REST dry-run `/home/liuxiang/tmp/20260617_073936_28symbols_no_h_30d_fusion_off_l0_live/rest_status_after_stop_20260618_0210.json` 显示 `ok=true`、目标合约 flat。根因是 Gate submit `500 INTERNAL / Request Timeout` 被旧逻辑当确定 `kRejected`，但后续 private feedback / REST 显示实际 filled / partial-filled，导致策略状态和 Gate single-mode 净仓分叉；`XLM_USDT` 后续因真实净仓已 flat 而出现大量 reduce-only `empty position` reject。修复提交 `9a13268` 已把 Gate `5xx` response 映射为 core `OrderResponseKind::kUnknownResult`，`OrderManager` 保留订单等待 feedback，LeadLag 标记对应 symbol `needs_reconcile` 并暂停新开仓；后续实现已支持对应 terminal feedback 精确解决 unknown order 后自动恢复该 symbol 新开仓。
- 2026-06-19 已启动新的 28-symbol fusion live run `20260619_095317_28symbols_no_h_30d_fusion_off_l0_live`，运行目录 `/home/liuxiang/tmp/20260619_095317_28symbols_no_h_30d_fusion_off_l0_live/`。本轮使用 release build、Gate / Binance fusion `N=4`、data session L0、fusion metadata off，策略 / guard universe 删除 `H_USDT` 和 `TON_USDT`；Gate lag 侧 4 路 source recorder 和 canonical fusion recorder 均在 `bin/` 落盘。2026-06-19 12:43 UTC 快照显示进程仍在、5 个 recorder 文件继续增长、REST dry-run 无挂单 / 无持仓、异常关键字为 0；快照 report 在 `/home/liuxiang/tmp/20260619_095317_28symbols_no_h_30d_fusion_off_l0_live/status_reports_20260619_124323/20260619_095317_28symbols_no_h_30d_fusion_off_l0_live/`。下一轮接手时必须重新检查进程、guard、REST dry-run 和 recorder 增长，不要假定该 run 仍在同一状态。
- 2026-06-19 已落地 LeadLag cancelled order fillability 分析方案文档 `docs/lead_lag_cancelled_order_fillability_analysis.md`。当前订单日志 / report 已包含 `signal_lead_id` / `signal_lag_id`、`ack_*_id`、`accepted_*_id`、`cancelled_*_id`、`filled_*_id` 等 stage BBO id，可用于把 `order_detail.csv` 的未成交开仓 cancel 和 lag fusion BookTicker bin 对齐。
- 2026-06-24 已完成 LeadLag Go reference 迁移边界整理：`freshness_auto` 和 `taker_buffer` 均不放入实时策略热路径。freshness 启动前由 `lead_lag_freshness_preflight` + `scripts/lead_lag/apply_freshness_preflight_summary.py` 生成固定 `max_lag_freshness_ms`；`freshness_shadow` 已从策略内删除。taker buffer 启动前由 `scripts/lead_lag/generate_preflight_config_params.py --lag-price-tick` 把 pct 转成 `open_slippage_ticks` / `close_slippage_ticks`，再由 `scripts/lead_lag/apply_taker_buffer_slippage.py` 写回策略 TOML 并输出 audit CSV；`execute.taker_buffer` 仅保留参考价诊断，不改变真实下单路径。当前 Go / C++ 对照和剩余差异见 `strategy/lead_lag/README.md` 的“当前 Go / C++ 对照”和“Go reference 迁移边界”。
- 2026-06-25 已完成 replay-only `lag_vol_guard` audit 工具和 ORDI_USDT release 验证：`lead_lag_replay --lag-vol-guard-audit-output` 维护独立 Go-like guard 状态，只对 open signal 输出 `lag_vol_guard_audit.csv` 的 would-block snapshot，不改变 live / replay 策略行为；30 分钟 release 稳定性循环 196 轮输出一致。`scripts/lead_lag/summarize_guard_audit.py` 可按 `symbol_id + signal_lag_id + action` 对齐 `order_detail.csv` entry order，并按 `position_id` 汇总 blocked / allowed 的 cancel、fill 和 PnL。字段见 `docs/diagnostic_fields.md` 的 “LeadLag Lag Vol Guard Audit CSV”。
- 2026-06-25 ORDI_USDT 三天 Tardis replay 参数 sweep 结论：`lag_vol_guard` 的 `cooldown=3m/5m/10m/15m` 都降低 signal-only PnL；默认 `15m` 过滤 `62/1175` 个 open signal，主要由 cooldown 扩大，且被过滤 trade 在 0 tick 和 5 ticks 滑点口径下整体仍为正贡献。因此当前只合并离线 audit 能力，不把 `lag_vol_guard` 推进到实盘策略 hot path；除非后续更宽 symbol / 更长区间给出反证，否则不要设计 live shadow 或 enforce。
- 2026-06-25 已按 Go reference 口径将 LeadLag `drift_guard` 实盘化为 open-only emergency sanity guard，并替代旧 pre-signal `trigger.drift_limit`。新配置为 `[lead_lag.pairs.trigger.drift_guard] enabled / drift_instant / ratio_std / ratio_std_window / drift_mean / drift_mean_window`；parser 会拒绝旧 `drift_limit` 和 `drift_guard.mode`，checked-in 策略 TOML 已迁移。guard 在 signal 触发后、订单 intent / synthetic accounting 前拦截 `kOpenLong` / `kOpenShort`，close / stoploss 不受影响；命中时输出 `lead_lag_order_intent_rejected reason=drift_guard`，report 以 `source_schema=intent_rejected_v1` 关联到 signal。`capacity.drift_guard_window_capacity` 默认和 checked-in 配置为 `131072`，用于降低 1 分钟窗口 live hot path 扩容风险；若要声明某次 live 不扩容，需要基于 tick-rate 实测或继续上调容量。`lead_lag_replay --lag-vol-guard-audit-output` 也会复用生产 `DriftGuardState` 输出 raw ratio / std / mean 和 `drift_guard_outcome`。旧 `drift_limit` 与当前 `drift_guard` 的异同、以及需要回退时的代码 / 配置 / 测试清单，见 `strategy/lead_lag/README.md` 的“`drift_limit` 历史实现与回退说明”。
- 2026-06-25 已拆分 LeadLag normal close 与 stoploss slippage：执行配置使用 `open_slippage_ticks`、`close_slippage_ticks`、`stoploss_slippage_ticks`、`close_retry_times` 和 `close_retry_slippage_step_ticks`；旧 `open_slippage`、`close_slippage`、`normal_close_retry_aggressive` 不再解析。normal close 首次用 `close_slippage_ticks`，第 `n` 次 retry 用 `close_slippage_ticks + n * close_retry_slippage_step_ticks`；`close_retry_times=0` 表示不做 normal close retry。stoploss 与 stoploss retry 始终用 `stoploss_slippage_ticks`。
- 2026-06-25 已整理过期文档：删除旧 agent 计划 / 设计稿和已废弃的 Gate fastest-route fusion implementation plan；threaded bundle 文档保留为 `docs/gate_fastest_route_fusion_threaded_bundle_guide.md`。
- 2026-06-26 已完成 LeadLag Go / C++ 对齐补充：`parallel-limit` 从 pre-signal reject 改为 post-signal open guard，命中时先记录 `lead_lag_signal_triggered` 再输出 `lead_lag_order_intent_rejected reason=parallel_limit`；report/parser 已覆盖 `intent_rejected_v1` join。C++ 不迁入 Go `taker_buffer` auto warmup，但 open signal 的 `required_edge` 会把 fixed `open_slippage_ticks` / `close_slippage_ticks` 按 `ticks * lag_price_tick / trigger_price` 折算为基础 entry / normal close 执行成本；`execute.taker_buffer` 仍只做参考价诊断和 preflight trace。多轮 review 已补 open short slippage cost 回归、删除旧 plan 文档、补 ORDI_USDT 三天 zero ticks vs 5 ticks replay 证据，并把 `docs/lead_lag_go_cpp_strategy_alignment.md` 第 1 项更新为当前 post-signal `parallel_limit` 事实。最新 focused 验证包括 `ctest --test-dir build/debug -R 'lead_lag' --output-on-failure`、`lead_lag_signal_test`、`lead_lag_strategy_interface_test`、`analyze_order_detail_test.py`、`generate_live_report_test.py` 和 `git diff --check`。详情见 `docs/lead_lag_go_cpp_strategy_alignment.md`。

## LeadLag Live

- 真实订单启动和 report 生成必须按 `docs/lead_lag_live_operations_pipeline.md` 执行；不要手写半套流程。
- Gate submit / cancel `5xx` response 只表示请求结果未知，不再视为确定 reject。LeadLag 收到 `OrderResponseKind::kUnknownResult` 后会保留 pending order、标记对应 symbol `needs_reconcile` 并暂停新开仓；后续 private terminal feedback 可把订单转成 filled / cancelled / rejected 并推进 execution group。若该 terminal feedback 精确解决该 symbol 的所有 pending unknown order，且没有 global continuity lost / manual intervention 等更高等级 degraded 状态，策略会自动恢复该 symbol 新开仓。真实订单前必须确认使用包含 `9a13268` 及后续 unknown-result auto-resume 修复的 release binary。
- 真实订单默认配置仍是 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`。30-symbol 配置入口是 `config/strategies/lead_lag_30symbols_live_strategy_20260604.toml`，其 nested strategy config 为 `config/strategies/lead_lag_30symbols_2bps_notional_20260604.toml`。
- 如使用 fusion 行情做 LeadLag live smoke，配置入口改为 `config/strategies/lead_lag_31symbols_no_ton_fusion_live_strategy_20260616.toml`；其 nested pair config 为 `config/strategies/lead_lag_31symbols_no_ton_2bps_notional_20260616.toml`，data reader 为 `config/data_readers/strategy_data_reader_31symbols_no_ton_fusion_20260616.toml`。真实订单前必须先按 `docs/lead_lag_live_operations_pipeline.md` 启动并检查 Gate / Binance `data_fusion`、canonical SHM 和 metadata 文件。
- `scripts/lead_lag/run_live_with_guard.py` 的 REST guard 凭据默认从 strategy config 的 `[strategy.order_session].config` 继续读取 order session TOML；显式传 `--api-key` / `--api-secret` 时必须和 order session credentials env 名称一致。
- Gate data / order / feedback 在 private plain 场景使用 `fxws-private.gateapi.io:80`；Binance data session 仍按 public TLS bookTicker。
- 开仓 freshness guard 只拦截 `kOpenLong` / `kOpenShort`，close / stoploss 不受影响。每个 `[[lead_lag.pairs]]` 直接配置整数毫秒字段 `max_lead_freshness_ms` / `max_lag_freshness_ms`；freshness 定义为 `signal_decision_ns - lead_exchange_ns` 和 `signal_decision_ns - lag_exchange_ns`。
- freshness 和 taker buffer 的 auto / warmup 逻辑均不在实时策略内运行。真实订单配置应先完成启动前 preflight：freshness 用 `lead_lag_freshness_preflight` 采样 fusion / data reader BBO 并生成固定 freshness；taker buffer 用 lag BBO spread pct 加 lag `price_tick` 生成固定 `open_slippage_ticks` / `close_slippage_ticks`。taker buffer preflight 不覆盖 `stoploss_slippage_ticks`、`close_retry_times` 或 `close_retry_slippage_step_ticks`。
- `scripts/lead_lag/generate_live_report.py` 生成 `signal.csv`、`order_detail.csv`、`position.csv`、`latency.csv` 和 schema 副本。报告支持 Ack RTT 三段拆解、滑点、actual / raw PnL 和胜率。
- 未成交 IOC 开仓复查优先按 `docs/lead_lag_cancelled_order_fillability_analysis.md` 执行：从 `order_detail.csv` 筛选完全未成交 `entry` cancel，用 `signal_lag_id` / `ack_lag_id` / `accepted_lag_id` / `cancelled_lag_id` 对齐 Gate lag fusion BookTicker bin，并按 `order_price` 判断 BBO 视角下 signal / Ack / accepted 到 cancel 区间是否触达可成交价格。
- 30-symbol report 必须显式传 `--instrument-catalog config/instruments/usdt_futures_common_gate_binance_20260602.csv`，否则默认 12-symbol catalog 可能导致 multiplier / PnL 不完整。
- `config/instruments/usdt_futures_common_gate_binance_20260602.csv` 是 Gate / Binance USDT 永续交集合约 catalog；`contract_multiplier` 是 report / PnL 使用的显式字段，当前与 `notional_multiplier` 保持一致。
- 2026-06-05 已停止 30-symbol run `20260604_0646_30symbols_30d_private` 并生成正式 report：`reports/20260604_0646_30symbols_30d_private/`。该进程在 freshness guard 相关 commits 前启动，未运行新 guard；事后用 `signal_decision_ns - lag_exchange_ns` 统计开仓 lag freshness，`6837` 个 open signal 的 p50 `32.205ms`、p95 `791.429ms`，其中 `3921/6837` 大于 `20ms`，说明当前 `max_lag_freshness_ms=20` 会强过滤旧 run 中大量 stale lag quote 下单。详细结论见 `docs/lead_lag_live_runtime_plan.md`。
- 2026-06-09 已结束的 30-symbol run `20260606_052542_30symbols_30d_private_lagref_forceclaim` 使用最新 per-pair freshness guard：lead 统一 `5ms`，lag 按 symbol 单独配置；Gate data / order / feedback 均为 private plain，Binance data session 为 public TLS。正式 report 显示 signal `36457`、submitted / finished `16207`、有成交 order `1144`、actual net PnL `-19.4167 USDT`、raw net PnL `-31.8180 USDT`；退出原因为 feedback continuity lost，guard flatten 后目标合约 flat。2026-06-06 12:36 UTC freshness 拆解显示 lag freshness 主要来自 lag quote 本身未更新，而不是 `exchange_ns -> local_ns` 网络接收慢。详细数据见 `docs/lead_lag_live_runtime_plan.md` 和 `reports/20260606_052542_30symbols_30d_private_lagref_forceclaim/report.md`。
- 历史 live report 保留在 `reports/`；onboarding 不再复制完整 PnL、slippage 或 latency 数值。

## Ack Latency / RTT Probe

- Order Ack outlier attribution 由 `AQUILA_ORDER_ACK_DIAG_LEVEL=0..5` 控制，默认 `L4`；`L4` 包含 socket timestamping attribution。
- 只有 private plain transport 成功 apply `SO_TIMESTAMPING` 后才启动 socket timestamp probe；TLS、apply 失败、取消写、未完整 write complete 都不会写入有效 attribution。
- `ts_available=false` 表示缺少 socket timestamping 归因，不要把 0 当真实时间。
- 当前主要是 software timestamping，不能严格证明 packet leaves / returns NIC；`enp55s0` 当前无 hardware timestamp。若要继续拆 Gate 内部阶段，需要 Gate 侧 trace / support，或引入链路侧证据。
- `gate_order_session_rtt_probe` 是 measurement-only 工具，不自动 score / 切换连接。默认连接列表为 `config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`；8 条 private plain 全阶段配置为 `config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml`。

## DataReader / Data Session

- `RealtimeDataReader::Poll(handler)` 是单事件接口，`Drain(handler, max_events)` 是批量接口；多 source round-robin 不做全局时间排序。
- `data_reader_recorder` 可从 Gate / Binance `BookTicker` SHM 写 merged replay binary，支持 rotation + manifest replay config。
- Gate / Binance live data session 默认用 `CLOCK_REALTIME` 记录 `BookTicker.local_ns`，语义是 data session 接入 WebSocket frame 后、进入 parser / decoder 前的本机 Unix epoch ns。
- Gate SBE `BookTicker.exchange_ns` 使用 `bbo.time` 的 WebSocket server send timestamp，不是同消息里的 engine update `t`。
- 比较不同 Gate private IP 行情延迟时，必须记录 data session endpoint / owner CPU，并按 `exchange_ns -> local_ns`、SHM publish / reader 时间、`skipped` / `overruns` 分组统计。
- recorder 是只读 SHM consumer，可与 LeadLag / demo 实盘并行；完整 dump 使用临时 `drain` 配置，观察 `overruns` / `skipped` 和 CPU 抢占。

## Gate Trading / TUI

- Gate `OrderSession` submit/cancel、login HMAC-SHA512、固定缓冲区 request encoder、response correlation、同步 `OrderResponse` 回调和 benchmark 已落地。
- C++ trading quantity contract 是 `double quantity` + `quantity_text`；Gate WebSocket 下单带 `X-Gate-Size-Decimal: 1` 时 JSON `size` 编码为 string。
- `OrderFeedbackSession` 使用 private `futures.orders`；`OrderManager::OnOrderFeedback()` 已处理 accepted、partial filled、filled、cancelled / terminal、rejected 和 continuity lost。
- Gate `5xx` submit / cancel error 经 runtime adapter 转为 core `OrderResponseKind::kUnknownResult`，不是 terminal reject；普通业务拒单仍为 `kRejected` / `kCancelRejected`。
- 多路 `OrderSession` 当前已完成参数化 baseline gateway：`exchange/gate/trading/multi_order_session_gateway.h` 支持 `1 thread : n OrderSession`、`gateway_route_id=0..n-1` 显式路由、`core::kAutoGatewayRoute` ready-session round-robin、cancel / cache / forget 回原 session；测试使用 `n=4`。`gate_order_session_rtt_probe` 的 `FanoutBatchDispatchScheduler` 已覆盖同一 batch 授予全部 session、等待全部 `samples_started` 后再 cooldown 的调度模型，但 live `--execute` 仍未切到 fanout batch。Ack / final response 仍由各 `OrderSession` 按 `local_order_id` fan-in 到 `TradingRuntime -> OrderManager -> Strategy`，gateway 不解释 Ack。fanout 目标形态仍是在单 strategy process 内由 `OrderSessionWorker[i]` 独占 `OrderSession[i]` 和对应 trading WebSocket connection，以降低同一 signal 多 child order 写入多条 connection 的 dispatch skew；baseline 不宣称 fillability 改善。回报仍由单独 `gate-feedback-process` 的单个 `OrderFeedbackSession` 写 SHM，再由策略进程读取。
- 尚未完成：REST reconcile、feedback WS 断线未知订单恢复、batch/amend/cancel-all。account / position realtime feedback 是 V2 可选能力，不是当前 LeadLag V1 前置项。
- `gate_account_tui` 当前仍是只读 monitor demo；`--live-market-data` 只读既有 Gate / Binance `BookTicker` SHM。订单、仓位、PnL 和 health 还未接真实账户数据。

## 代码入口

| 模块 | 入口 |
| --- | --- |
| Trading core | `core/trading/order_types.h`、`core/trading/order_manager.h`、`core/trading/trading_runtime.h`、`core/trading/order_feedback_shm.h` |
| Gate trading | `exchange/gate/trading/order_session.h`、`exchange/gate/trading/order_feedback_session.h`、`exchange/gate/trading/order_feedback_parser.h`、`exchange/gate/trading/order_session_runtime_adapter.h` |
| WebSocket diagnostics | `core/websocket/critical_session.h`、`core/websocket/plain_socket.h`、`core/websocket/socket_timestamping.h`、`exchange/gate/trading/order_latency_diagnostics.h` |
| Market data | `core/market_data/types.h`、`core/market_data/realtime_data_reader.h`、`core/market_data/data_shm.h`、`exchange/gate/market_data/*`、`exchange/binance/market_data/*` |
| Gate / Binance fastest-route fusion | `core/common/book_ticker_fusion_metadata_mode.h`、`core/market_data/book_ticker_fusion.h`、`core/market_data/book_ticker_fusion_config.h`、`core/config/book_ticker_fusion_config.*`、`core/market_data/book_ticker_fusion_metadata.h`、`core/market_data/book_ticker_fusion_metadata_policy.h`、`core/market_data/book_ticker_fusion_runner.h`、`core/market_data/book_ticker_fusion_thread.h`、`tools/market_data/book_ticker_fusion_cli.*`、`tools/market_data/book_ticker_fusion.cpp`、`tools/market_data/binance_book_ticker_fusion.cpp`、`tools/market_data/data_fusion_tool_support.h`、`tools/gate/gate_data_fusion.cpp`、`tools/binance/binance_data_fusion.cpp`、`tools/gate/gate_data_fusion_config.*`、`tools/binance/binance_data_fusion_config.*`、`config/market_data_fusion/gate_book_ticker_fusion_4sources.toml`、`config/market_data_fusion/binance_book_ticker_fusion_4sources.toml`、`config/market_data_fusion/gate_data_fusion_book_ticker_4sources.toml`、`config/market_data_fusion/binance_data_fusion_book_ticker_4sources.toml`、`scripts/market_data/analyze_book_ticker_fusion_latency.py`、`scripts/market_data/compare_fusion_tardis_book_ticker.py`、`docs/gate_fastest_route_fusion_shadow_results.md`、`docs/fusion_tardis_bbo_comparison.md` |
| LeadLag | `strategy/lead_lag/strategy.h`、`strategy/lead_lag/config.*`、`tools/lead_lag/replay.cpp`、`tools/lead_lag/live_strategy.h`、`tools/lead_lag/lag_vol_guard_audit.*`、`scripts/lead_lag/summarize_guard_audit.py` |
| LeadLag benchmark docs | `docs/lead_lag_benchmark_environment_tail_analysis.md`、`benchmark/strategy/lead_lag_runtime_benchmark.cpp`、`benchmark/strategy/lead_lag_strategy_benchmark.cpp` |
| Reports | `scripts/lead_lag/analyze_order_detail.py`、`scripts/lead_lag/generate_live_report.py` |
| Cancelled order fillability | `docs/lead_lag_cancelled_order_fillability_analysis.md`、`scripts/market_data/split_book_ticker_by_symbol.py`、`scripts/lead_lag/analyze_order_detail.py` |
| Instrument catalog | `scripts/instruments/generate_common_usdt_perp_catalog.py`、`scripts/gate/market_data/query_futures_contracts.py`、`scripts/binance/market_data/query_um_futures_contracts.py` |
| TUI | `monitor/tui/gate_account_tui.cpp`、`monitor/market_data/market_data_thread.*` |

## 常用验证命令

```bash
./build.sh debug
ctest --test-dir build/debug --output-on-failure
git diff --check
```

Focused checks:

```bash
ctest --test-dir build/debug -R '(core_order_pool|strategy|gate_order|gate_submit|order_session_config|order_feedback|lead_lag|signal_csv_writer)' --output-on-failure
ctest --test-dir build/debug -R '(gate_.*market_data|binance_.*market_data|data_session_config|data_reader_config|core_market_data|data_reader_recorder)' --output-on-failure
ctest --test-dir build/debug -R 'book_ticker_fusion|core_market_data_book_ticker_fusion' --output-on-failure
ctest --test-dir build/debug -R 'websocket_(critical_session|socket_timestamping|plain_socket)_test|order_session_config_test|gate_order_session_rtt_probe_test|gate_order_session_test' --output-on-failure
ctest --test-dir build/debug -R monitor_ --output-on-failure
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/summarize_guard_audit_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/instruments/generate_common_usdt_perp_catalog_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/analyze_book_ticker_fusion_latency_test.py
```

Evaluation 边界修改后运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望无命中。

## 下一步建议

1. LeadLag Go reference 迁移：`lag_vol_guard` 已完成 replay-only audit 和 ORDI_USDT sweep，当前证据不支持进入 live hot path；`drift_guard` 已替代旧 `drift_limit` 并实盘化为 open-only emergency sanity guard。后续优先只做更宽 symbol / 更长区间 replay 或小额 live smoke 的阈值复核，不恢复旧 pre-signal `drift_limit`。当前 Go / C++ 对照、迁移边界、回放命令，以及旧 `drift_limit` 的可回退实现清单见 `strategy/lead_lag/README.md` 的“当前 Go / C++ 对照”“Go reference 迁移边界”“`drift_limit` 历史实现与回退说明”和“回放与输出”。
2. Cancelled order fillability：如果继续订单成交复查，先读 `docs/lead_lag_cancelled_order_fillability_analysis.md`，再确认 `20260619_095317_28symbols_no_h_30d_fusion_off_l0_live` 是否仍在运行；如果仍运行，先做只读 REST dry-run、生成最新 status report，并把 `gate_live_fusion_canonical.bin` 按目标 symbol 拆分。
3. LeadLag live：新实盘启动仍按 `docs/lead_lag_live_operations_pipeline.md`，并先 rebuild release binary。启动前应串联 freshness preflight 和 taker-buffer pct->slippage-ticks preflight，生成最终策略 TOML 和 audit CSV。
4. Ack latency：复现 outlier 时用 private plain all-stage config，分开看 Ack RTT、Gate `x_in -> x_out`、上行 / 下行、socket timestamping 和 pcap residual。
5. Gate / Binance 多路行情 / data session latency：继续讨论 N 路最快行情融合时，先读 `docs/websocket_client_future_optimizations.md` 的 `Live Feed Selection`、`docs/gate_fastest_route_fusion_design.md`、`docs/gate_fastest_route_fusion_shadow_results.md`、`docs/gate_fastest_route_fusion_threaded_bundle_guide.md` 和 `docs/fusion_tardis_bbo_comparison.md`。真实订单切换 fusion 行情前必须按 live pipeline 做 preflight。
6. Gate trading：后续优先补 REST reconcile、feedback 断线恢复和更完整的 stop-and-flat 语义；继续围绕 `kUnknownResult` 增加低频 REST drift guard，避免策略状态与 Gate single-mode 净仓长期分叉。
7. Gate 多路 OrderSession：若继续多路下单设计或实现，先读 `docs/agent-handoff-gate-trade-architecture.md` 的“多路 OrderSession 讨论结论”和 `docs/strategy_order_component_model.md` 的“多路 OrderSession 扩展边界（未实现）”。当前 baseline 已验证 route / cancel / cache / forget，RTT probe 已有 fanout batch scheduler；下一步优先把 live execute 接到 fanout batch，记录 enqueue / dequeue / write complete / Ack timing 和 feedback 路由稳定性，再做 per-session worker 对照。涉及订单链路、反馈、并发或低延迟取舍时，按 `AGENTS.md` 先询问是否启用 Grill Me，默认建议 `grill-me-enhanced`；正式宣称 fill-rate / latency 收益前必须拿 live smoke 或 probe 证据。

## 给下一个对话的提示

先运行 `git status --short --branch` 和 `git log --oneline -8`，再读 `AGENTS.md`、`README.md`、本文件和 `docs/evaluation_support.md`；当前 branch / ahead / dirty 只信 `git status`。进入设计、架构、实现计划或关键交易链路取舍时，先按 `AGENTS.md` 询问是否启用 Grill Me；普通设计建议 `grill-me-basic`，订单 / 行情 / 风控 / 恢复 / 并发 / 低延迟主路径建议 `grill-me-enhanced`。Fusion / Tardis BBO 对账见 `docs/fusion_tardis_bbo_comparison.md`：Binance 20260627 30-symbol Tardis 侧全部 strict 匹配到 fusion，Tardis 是 fusion 总量的 `99.9352%`；Gate strict 差异主要来自 timestamp 语义，`near_ms=5` 后 Tardis-only candidate 为 `10,085` 条，Gate BTC_USDT fusion 应按 `id` / 发布顺序复现，不按 `exchange_ns` 重排。LeadLag 当前 Go / C++ 对照见 `strategy/lead_lag/README.md` 和 `docs/lead_lag_go_cpp_strategy_alignment.md`：`freshness_auto` / `taker_buffer` 不进实时策略热路径；live 前用 freshness preflight 生成固定 `max_lag_freshness_ms`，用 taker-buffer preflight 把 pct 转成 `open_slippage_ticks` / `close_slippage_ticks`，且不覆盖 `stoploss_slippage_ticks` 或 normal close retry；执行配置已拆分 close / stoploss slippage，normal close retry 用 `close_retry_times` + `close_retry_slippage_step_ticks`，旧 slippage key 不再解析；`parallel-limit` 是 post-signal open guard；`lag_vol_guard` 只保留 replay-only audit，ORDI_USDT 三天 sweep 不支持进入 live hot path；`drift_guard` 已替代旧 `trigger.drift_limit`，定位为 open-only emergency sanity guard，parser 拒绝旧 `drift_limit` / `drift_guard.mode`，如需回退旧 drift_limit 先读 README 的回退说明。Gate 多路 `OrderSession` / MGW fanout 结论见 `docs/agent-handoff-gate-trade-architecture.md` 和 `docs/strategy_order_component_model.md`；当前已落地 `1 thread : n OrderSession` baseline gateway，测试 / 首个实验使用 `n=4`，支持显式 route table、子订单唯一 `local_order_id`、cancel / cache / forget 回原 session、单账户级 `OrderFeedbackSession`，Ack / final response 按 `local_order_id` fan-in 到 `TradingRuntime -> OrderManager -> Strategy`，gateway 不解释 Ack；`gate_order_session_rtt_probe` 已有 fanout batch scheduler 的 dry-run / unit-test 模型，但 live execute fanout timing 和 per-session worker capability 尚未实现。duplicate / split、winner、overfill 和非赢家 cancel 触发点属于策略 / OMS。订单 fillability 继续读 `docs/lead_lag_cancelled_order_fillability_analysis.md`，用 `signal_lag_id` / `ack_lag_id` / `accepted_lag_id` / `cancelled_lag_id` 对齐 Gate lag fusion canonical BookTicker bin，字段名是 `cancelled_*_id`。新实盘启动仍必须按 `docs/lead_lag_live_operations_pipeline.md`，先 rebuild release binary，并串联 freshness 与 taker-buffer slippage preflight。
