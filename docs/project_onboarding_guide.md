# aquila 新对话导读

这份文档是接手入口，只保留当前事实、入口索引、验证命令和下一步建议。历史推导、完整 run 输出、字段细节和 benchmark 细节放在专题文档或 `reports/` 中；当前 branch / ahead / dirty 状态只信 `git status --short --branch`。

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

进入设计、架构、实现计划或关键交易链路取舍时，先按 `AGENTS.md` 询问是否启用 Grill Me；普通设计建议 `grill-me-basic`，订单 / 行情 / 风控 / 恢复 / 并发 / 低延迟主路径建议 `grill-me-enhanced`。

## 方向索引

| 方向 | 优先文档 |
| --- | --- |
| LeadLag live / report | `docs/lead_lag_live_operations_pipeline.md`、`docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_report_csv_schema.md`、`docs/lead_lag_reconcile_design.md` |
| LeadLag 策略语义 / Go 对照 | `strategy/lead_lag/README.md`、`docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` |
| Cancelled order / fillability | `docs/lead_lag_cancelled_order_fillability_analysis.md`、`docs/exchange_matching_fillability_notes.md` |
| Ack latency / RTT probe | `docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_runtime_latency_improvement_plan.md`、`docs/gate_order_session_rtt_probe_design.md`、`docs/diagnostic_fields.md` |
| DataReader / data session | `docs/data_reader_config.md`、`docs/data_session_config.md`、`docs/data_session_shm_communication_design.md` |
| Gate / Binance fusion | `docs/gate_fastest_route_fusion_design.md`、`docs/trade_fastest_route_fusion_design.md`、`docs/gate_fastest_route_fusion_shadow_results.md`、`docs/gate_fastest_route_fusion_threaded_bundle_guide.md`、`docs/fusion_tardis_bbo_comparison.md` |
| Gate OBU / OrderBook | `docs/gate_obu_order_book_notes.md`、`exchange/gate/sbe/schema/gate_fex_ws_latest.xml`、`exchange/gate/sbe/message_dispatcher.h` |
| Bitget market data | `docs/agent-handoff-bitget-market-data.md`、`docs/data_session_config.md`、`docs/data_session_shm_communication_design.md` |
| Bitget trading | `docs/superpowers/specs/2026-07-10-bitget-order-session-design.md`、`docs/superpowers/specs/2026-07-10-bitget-order-feedback-session-design.md` |
| Gate trading / order gateway | `docs/agent-handoff-gate-trade-architecture.md`、`docs/gate_order_gateway_shm_design.md`、`docs/strategy_order_component_model.md` |
| Runtime CPU / ENA IRQ | `docs/runtime_cpu_allocation.md` |
| FixedOrderedSlotPool / parallel=n | `docs/lead_lag_fixed_ordered_slot_pool_parallel.md`、`core/base/fixed_ordered_slot_pool.h` |
| TUI / account monitor | `docs/tui_onboarding_guide.md`、`docs/tui_gate_account_monitor_design.md` |
| Instrument catalog | `docs/futures_contract_metadata_fields.md`、`docs/agent-handoff-binance-market-data.md` |

## 当前事实

- 项目是 C++20 / CMake 的 crypto 低延迟交易系统；默认同时关注正确性、确定性、低延迟、可恢复性和可观测性。临时 log、scratch config、live snapshot、benchmark 临时产物默认写入 `/home/liuxiang/tmp`。
- 2026-07-08 已按用户要求清理 2026-07-01 LeadLag A/B live 的相关输出和数据文件。不要再把旧 A/B tmp run dir 或 partial report 目录作为本地可读事实源；文档中只保留已摘录的历史口径。
- 历史 implementation plan / spec 归档已删除。当前事实源以本文件、专题文档、代码和测试为准。
- Gate / Binance / Bitget `BookTicker` 与 `Trade` data session 可发布到同一个 SHM object 内的 typed channel。`BookTicker` 现在同时带 `exchange_ns`、`event_ns`、`local_ns`，typed binary payload 为 72 bytes；`Trade` 使用统一字段名 `event_ns`，payload 仍为 64 bytes。`RealtimeDataReader` 按 source `feed` 读取 `book_ticker` / `trade`；`HistoricalDataReader`、`data_reader_probe` 和 Python numpy 脚本只接受 typed binary format v1，`binary_file` source 必须显式写 `feed`，header feed/type 必须与 TOML 一致。旧 raw struct artifact 或旧 ABI typed artifact 需要重录。
- 2026-07-10 Gate `futures.obu` / Order Book V2 讨论和 quick probe 摘要已记录到 `docs/gate_obu_order_book_notes.md`。当前结论：`obu.50` 固定 20ms、`obu.400` 固定 100ms；`400` 是最大深度，不保证 full snapshot 满 400 档；SBE `obu` template id 为 3，levels 是 array-of-struct，bid / ask entry count 来自 repeating group header 的 `numInGroup`；仓库已有 SBE 生成代码和 dispatcher 识别，但尚未实现 OBU decoder、本地 order book 维护器、深度 typed channel 或 recorder。
- 当前工作区可能保留用户未提交的 `core/market_data/types.h` `Orderbook` 草案。继续实现前先用 `git status --short --branch` 和 `git diff -- core/market_data/types.h` 确认，不要把它当作已完成 ABI；讨论中的方向是把该结构作为维护后的 published snapshot，而不是 raw OBU update。
- Bitget UTA public SBE `books1` BBO 已接入并合并到 `main`：`seq -> BookTicker.id`，`sts * 1000 -> exchange_ns`，`ts * 1000 -> event_ns`，data session ingress `CLOCK_REALTIME -> local_ns`。Bitget `publicTrade` 已接入：`execId -> Trade.id`，`side=0/1 -> Buy/Sell`，`sts * 1000 -> exchange_ns`，`ts * 1000 -> event_ns`，group index/count 写入 `batch_index` / `batch_count`。原 `feature/bitget-bbo-market-data` worktree 和本地 branch 已删除。行情入口和配置见 `docs/agent-handoff-bitget-market-data.md`。
- 2026-07-10 Bitget UTA v3 单路 `OrderSession` 已实现：使用 high availability private endpoint `wss://vip-ws-uta.bitget.com/v3/ws/private`，覆盖 login、limit GTC/IOC place、single cancel、request correlation、operation response、runtime adapter、TOML config、login-only probe 和 release benchmark target。operation response 只表示请求的直接响应，不确认 accepted、fill 或 cancel terminal；当前没有接入 LeadLag，也没有发送真实订单。40 秒 login-only smoke 已观察到 login accepted 和 ping/pong，未发送 place/cancel。
- 2026-07-10 Bitget UTA v3 单路 `OrderFeedbackSession` 已实现：独立 private connection login 后只订阅 account-wide `order` topic，按 `clientOid=a-<local_order_id>` 路由现有 feedback SHM；覆盖累计 `new` / partial / filled / cancelled facts、V1 scope validation、同 generation decode continuity latch、disconnect continuity、TOML config 和 login/subscribe-only probe。parser/session/config/SHM integration tests 已通过；text hot path 使用 order-data-first classification，避免每条数据重复跑 control parser。release CPU 16、3 repetitions、每项 20,000 percentile samples 的本机最新基线中，accepted median p50/p99/p99.9 为 `420/475/939 ns`，session classification→accepted 为 `421/467/950 ns`，parser→SHM→drain 为 `441/484/935 ns`，原始 JSON 在 `/home/liuxiang/tmp/bitget_order_feedback_parser_benchmark_review_round3.json`。这不是 live feedback latency；REST baseline/reconcile、rate limiter、LeadLag、`fill` / `fast-fill` 和真实订单仍未实现。
- 2026-07-10 已新增 Bitget UTA REST read-only query 脚本 `scripts/bitget/account/query_bitget_account.py`，默认读取 `BITGET_TEST_KEY` / `BITGET_TEST_SECRET` / `BITGET_TEST_PASSPHRASE`，支持 `account`、`orders`、`positions`。实测普通与 VIP UTA REST endpoint 都因当前出站 IP `52.69.2.245` 不在 Bitget key 白名单而返回 `40018 Invalid IP`；用户会后续处理白名单后再测。
- 2026-07-08 Bitget BBO normal vs high speed public SBE 30 分钟同步 A/B 已记录在 `docs/agent-handoff-bitget-market-data.md`：3 symbols、N=4、BBO-only、同构 source/fusion/recorder 下，normal fusion latency p99 `2.650 ms`、p99.9 `3.032 ms`，high speed p99 `14.193 ms`、p99.9 `40.289 ms`；high speed 没有带来 BBO 延迟收益或更多消息量。该证据只覆盖行情接入和 fusion pipeline。
- Instrument catalog 现在有两个主要入口：`config/instruments/usdt_futures.csv` 是 16-symbol 小型默认 catalog，包含 Gate / Binance / Bitget；`config/instruments/usdt_future_universe.csv` 是大 universe，包含 Gate 494 行、Binance 494 行和 Bitget 438 行，保留 `contract_multiplier`。旧 `config/instruments/usdt_futures_common_gate_binance_20260701.csv` 已重命名，不应继续引用。
- Gate 交易 / private feedback / REST read-only 相关凭据环境变量已统一加 `GATE_` 前缀：`GATE_TEST_KEY` / `GATE_TEST_SECRET`、`GATE_PROBE_KEY` / `GATE_PROBE_SECRET`。非 `reports/` 范围不再使用旧 `TEST_*` / `PROBE_*` 名称；历史 report 快照中的旧名不作为当前配置事实。
- Gate 多路 `OrderSession` 已有单进程 baseline 和独立 `order-gateway-process` / SHM V2。生产形态是 strategy 进程 1 个 owner thread、gateway 进程 N 个 `OrderSessionWorker` thread，跨进程用 N 路 command / event queue 和 `route_states[16]`。gateway 只负责发送和回传 Ack / response，不解释 duplicate / split、winner、overfill 或非赢家 cancel。当前不宣称 fillability 或 latency 收益。
- Gate `5xx` submit / cancel 经 runtime adapter 转为 `OrderResponseKind::kUnknownResult`，`OrderManager` 保留订单等待 feedback；LeadLag 对相关 symbol 标记 `needs_reconcile` 并暂停新开仓。后续恢复 / reconcile 仍按 `docs/lead_lag_reconcile_design.md`。
- LeadLag live report 的多 entry FIFO 匹配已在 `f077842` 修复；`over_closed` 现在只表示 exit 成交量超过 report 可匹配 entry 剩余量，不再代表多路 duplicate entry 只取第一笔 entry 的误分类。字段见 `docs/lead_lag_live_report_csv_schema.md`。
- `core/base/fixed_ordered_slot_pool.h` 已提供 `FixedOrderedSlotPool<T, kCapacity>`，单测和 group-container benchmark 已覆盖；生产 `strategy/lead_lag/execution_state.h` 仍使用 `std::vector<ExecutionGroup>`，不要假定已切换。若继续迁移，当前候选是 `FixedOrderedSlotPool<ExecutionGroup, 16>`，同时删除 `parent_id` 并改用 `group_id` / `group_index`。
- 2026-07-08 fusion 结构收敛和多轮 review 修复已完成：核心在 `core/market_data/fusion/`，tools 层入口是 `tools/market_data/data_fusion_tool_support.h`、`tools/market_data/data_fusion_launch_config_parser.h`、`tools/gate/gate_data_fusion.cpp` 和 `tools/binance/binance_data_fusion.cpp`。一个 `data_fusion` process 可按 launch config 启用 `book_ticker`、`trade` 或两者；每个 enabled feed 一个 fusion thread，只为 enabled feed 创建对应 source / canonical SHM channel。
- Fusion 冷路径校验已 fail-fast：错误 TOML 类型、旧 schema、重复 / 重叠 SHM 名、重复 channel、source / fusion / log backend CPU 冲突、不可用 required CPU、fusion config 与 source override 不一致都会启动前拒绝。Binance feed override 后会刷新 stream target；退出顺序是 source stop+join 后让 fusion drain 到 idle。
- Fusion release microbench 证据在 `/home/liuxiang/tmp/fusion_refactor_perf/multi_round_review_round7_fix.json`；Trade fastest-route fusion 的 2026-07-07 30-symbol、4 路、30 分钟真实行情 smoke 摘要在 `/home/liuxiang/tmp/trade_fusion_30symbols_20260707_020400/latency_analysis/trade_fusion_latency_summary.json`。这些证据只覆盖行情 fusion pipeline / microbench，不代表订单 fillability 或 PnL 收益。
- Gate BTC fill probe 的独立 runbook 已合并到 `docs/exchange_matching_fillability_notes.md`。旧 2026-07-03 / 2026-07-04 probe 配置仍指向已清理的 2026-07-01 fusion SHM；新 probe 必须先生成当次 scratch config 并完成 validate / preflight / REST read-only flat check。
- 当前 32 物理 core 机器按 `docs/runtime_cpu_allocation.md` 管理：`0-15` 为实盘保留区，`16-31` 为测试 / diagnostics / benchmark 区。CPU isolation、ENA IRQ pool、interrupt moderation 等仍是候选系统优化，不代表已上线或已证明收益。

## 代码入口

| 模块 | 入口 |
| --- | --- |
| Trading core | `core/trading/order_types.h`、`core/trading/order_manager.h`、`core/trading/trading_runtime.h`、`core/trading/order_feedback_shm.h` |
| Gate trading | `exchange/gate/trading/order_session.h`、`exchange/gate/trading/order_feedback_session.h`、`exchange/gate/trading/order_feedback_parser.h`、`exchange/gate/trading/order_session_runtime_adapter.h` |
| Bitget trading | `exchange/bitget/trading/order_session.h`、`exchange/bitget/trading/operation_response_parser.h`、`exchange/bitget/trading/order_feedback_parser.h`、`exchange/bitget/trading/order_feedback_session.h`、`config/order_sessions/bitget_order_session.toml`、`config/order_feedback/bitget_order_feedback_session.toml`、`tools/bitget/bitget_order_session_probe.cpp`、`tools/bitget/bitget_order_feedback_session.cpp` |
| Order gateway | `core/trading/order_gateway_client.h`、`exchange/gate/trading/order_gateway_worker.h`、`tools/gate/gate_order_gateway.cpp` |
| Market data | `core/market_data/types.h`、`core/market_data/data_shm.h`、`core/market_data/realtime_data_reader.h`、`exchange/gate/market_data/*`、`exchange/binance/market_data/*`、`exchange/bitget/market_data/*`、`exchange/bitget/sbe/*` |
| Gate OBU / OrderBook | `docs/gate_obu_order_book_notes.md`、`exchange/gate/sbe/generated/gate/messages/obu.hpp`、`exchange/gate/sbe/generated/gate/messages/orderBookUpdate.hpp`、`exchange/gate/sbe/message_dispatcher.h`、`core/market_data/types.h` |
| DataReader recorder | `tools/market_data/data_reader_recorder.cpp`、`core/market_data/historical_data_reader.h`、`docs/data_reader_config.md` |
| Fusion | `core/market_data/fusion/book_ticker.h`、`core/market_data/fusion/trade.h`、`core/market_data/fusion/fastest_route.h`、`core/market_data/fusion/thread.h`、`core/market_data/fusion/metadata.h`、`tools/gate/gate_data_fusion.cpp`、`tools/binance/binance_data_fusion.cpp`、`tools/bitget/bitget_data_fusion.cpp` |
| LeadLag | `strategy/lead_lag/strategy.h`、`strategy/lead_lag/config.*`、`strategy/lead_lag/execution_state.h`、`tools/lead_lag/replay.cpp`、`tools/lead_lag/live_strategy.h` |
| LeadLag reports | `scripts/lead_lag/analyze_order_detail.py`、`scripts/lead_lag/generate_live_report.py` |
| Fill probe | `tools/gate/fill_probe/main.cpp`、`tools/gate/fill_probe/state_machine.*`、`docs/exchange_matching_fillability_notes.md` |
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
ctest --test-dir build/debug -R '^bitget_(order|operation)' --output-on-failure
ctest --test-dir build/debug -R '(gate_.*market_data|binance_.*market_data|data_session_config|data_reader_config|core_market_data|data_reader_recorder)' --output-on-failure
ctest --test-dir build/debug -R '(gate_.*market_data|binance_.*market_data|bitget_.*market_data|bitget_sbe|data_session_config|data_reader_config|core_market_data|data_reader_recorder)' --output-on-failure
ctest --test-dir build/debug -R '(core_market_data_shm|core_market_data_fusion|fusion_config|gate_data_fusion_config|binance_data_fusion_config|bitget_data_fusion_config|data_fusion_tool_support|data_session_config|websocket_config|fusion_cli_traits|book_ticker_fusion_cli|trade_fusion_cli)' --output-on-failure
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
```

Evaluation 边界修改后运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望无命中。

## 下一步建议

1. Bitget trading 下一步先处理 session 外的 REST bootstrap/reconcile 和 reconnect unknown window，再设计 account-level rate limiter 与 LeadLag wiring；这些边界完成前不做真实订单 smoke，也不把 login/subscription success 或 component benchmark 当作订单连续性、fillability 或 live latency 证据。实现入口和已锁定边界见 `docs/superpowers/specs/2026-07-10-bitget-order-feedback-session-design.md` 与 `docs/superpowers/plans/2026-07-10-bitget-order-feedback-session.md`。
2. 新实盘启动仍按 `docs/lead_lag_live_operations_pipeline.md`，先 rebuild release binary，并串联 freshness preflight 与 taker-buffer slippage preflight。真实订单启动前做 REST read-only flat check、guard 配置复核和 run dir 隔离。
3. Gate 多路 `OrderSession` 若继续推进，先读 `docs/gate_order_gateway_shm_design.md`、`docs/agent-handoff-gate-trade-architecture.md` 和 `docs/strategy_order_component_model.md`。下一步是 guarded 小额 smoke，用 live report / probe 量化 skew、Ack RTT 和 fillability；涉及订单链路取舍时先询问 Grill Me，默认建议 `grill-me-enhanced`。
4. Fillability 复查先读 `docs/exchange_matching_fillability_notes.md` 和 `docs/lead_lag_cancelled_order_fillability_analysis.md`。旧 BTC probe 的 99% fill rate 不能外推到 LeadLag signal-conditioned fillability；新 probe 必须使用当次 live fusion SHM 和 scratch config。
5. DataReader / recorder 相关变更先读 `docs/data_reader_config.md` 和 `docs/data_session_shm_communication_design.md`。typed binary header feed/type 是强约束；旧 raw artifact 不兼容。需要大 symbol universe 时使用 `config/instruments/usdt_future_universe.csv`，不要再引用旧 `usdt_futures_common_gate_binance_20260701.csv` 文件名。
6. Gate OBU / OrderBook 继续讨论先读 `docs/gate_obu_order_book_notes.md`。下一步优先确认 published `OrderBook<10>` ABI：是否包含 `symbol_id` / `exchange`、count 类型、array-of-struct 还是 struct-of-arrays、是否继续用 `double` 发布；实现前先补 Gate OBU decoder / local book builder 单测，覆盖 `numInGroup` 不满 level、empty update、`size == 0` 删除和 gap resubscribe。
7. Fusion / data session 继续优化前先区分行情 pipeline 证据和订单收益。Binance / Bitget 每路 source 对比只覆盖尾部 SHM 可见窗口；不要把行情 latency 或 microbench 直接宣称为 fillability / PnL。Bitget endpoint A/B 复测应沿用同构启动、同 symbols、BBO-only、同 recorder 结构和独立 CPU 绑定。
8. Ack latency / RTT outlier 复现时用 private plain all-stage config，分开看 Ack RTT、Gate `x_in -> x_out`、上行 / 下行、socket timestamping 和 pcap residual。
9. FixedOrderedSlotPool / `parallel=n` 迁移必须端到端同步 core order contract、order gateway SHM、LeadLag execution state、日志、report CSV、脚本测试和文档。不能停在 `parent_id` 与 `group_id` 混用的半迁移状态。
10. CPU isolation、ENA IRQ 或 interrupt moderation 调整先读 `docs/runtime_cpu_allocation.md`，并用 A/B 证据记录 CPU、affinity、IRQ、kernel 和命令；没有 benchmark / profile / live smoke 证据时不宣称性能收益。

## 给下一个对话的提示

先运行 `git status --short --branch` 和 `git log --oneline -8`，再读 `AGENTS.md`、`README.md`、本文件和 `docs/evaluation_support.md`；当前 branch / ahead / dirty 只信 `git status`。派发 subagent 必须按 `AGENTS.md` 选择项目级 `aquila_xhigh_worker`。进入设计、架构、实现计划或关键交易链路取舍时先询问 Grill Me，关键交易链路建议 `grill-me-enhanced`。

Bitget `OrderFeedbackSession` 已按批准设计实现并完成 parser/session/config/SHM integration tests 与 release benchmark，入口是 `exchange/bitget/trading/order_feedback_parser.h`、`exchange/bitget/trading/order_feedback_session.h`、`config/order_feedback/bitget_order_feedback_session.toml` 和 `tools/bitget/bitget_order_feedback_session.cpp`。V1 只订阅 account-wide `order`，`Ready()` 不表示 REST baseline/reconcile 或订单连续性；下一步是 session 外 REST bootstrap/reconcile、reconnect unknown window、account-level rate limiter 和 LeadLag wiring，这些完成前不做真实订单。Bitget key 仍需用户处理 IP 白名单后再测 REST 账户可用资金。

Bitget `OrderSession` direct operation response 不能当作 accepted / fill / cancel terminal。若 `git status` 显示 `core/market_data/types.h` dirty，先读 diff；该文件是用户未提交的 `Orderbook` 草案，不是已完成 ABI。Gate OBU / OrderBook 仍只完成讨论和 quick probe；其他方向继续按本文件“方向索引”和“下一步建议”选择专题文档。
