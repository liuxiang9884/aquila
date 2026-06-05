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
| Gate Ack latency / RTT probe | `docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_runtime_latency_improvement_plan.md`、`docs/gate_order_session_rtt_probe_design.md`、`docs/diagnostic_fields.md` |
| DataReader / data session | `docs/data_reader_config.md`、`docs/data_session_config.md`、`docs/data_session_shm_communication_design.md` |
| Runtime CPU 分配 | `docs/runtime_cpu_allocation.md` |
| Gate 交易架构 | `docs/agent-handoff-gate-trade-architecture.md`、`docs/strategy_order_component_model.md` |
| TUI / account monitor | `docs/tui_onboarding_guide.md`、`docs/tui_gate_account_monitor_design.md` |
| Instrument catalog / market data scripts | `docs/futures_contract_metadata_fields.md`、`docs/agent-handoff-binance-market-data.md` |

## 当前事实

- 项目是 C++20 / CMake 的 crypto 低延迟交易系统；默认同时关注正确性、确定性、低延迟、可恢复性和可观测性。
- 公共 order / runtime contract 在 `core/trading/*` + `aquila::core`；Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h`。
- Gate / Binance `BookTicker` data session、SHM、strategy `DataReader`、Gate submit/cancel、Gate private `futures.orders` feedback、`OrderManager`、`TradingRuntime`、LeadLag replay / live-orders、TUI market data monitor demo 已落地。
- 当前 32 物理 core 机器必须遵守 `docs/runtime_cpu_allocation.md`：`0-15` 为实盘保留区，`16-31` 为测试 / diagnostics / benchmark 区；测试任务默认不要占用实盘 hot path core。
- 临时 log、scratch config、live snapshot、benchmark 临时产物默认写入 `/home/liuxiang/tmp`。

## LeadLag Live

- 真实订单启动和 report 生成必须按 `docs/lead_lag_live_operations_pipeline.md` 执行；不要手写半套流程。
- 真实订单默认配置仍是 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`。30-symbol 配置入口是 `config/strategies/lead_lag_30symbols_live_strategy_20260604.toml`，其 nested strategy config 为 `config/strategies/lead_lag_30symbols_2bps_notional_20260604.toml`。
- `scripts/lead_lag/run_live_with_guard.py` 的 REST guard 凭据默认从 strategy config 的 `[strategy.order_session].config` 继续读取 order session TOML；显式传 `--api-key` / `--api-secret` 时必须和 order session credentials env 名称一致。
- Gate data / order / feedback 在 private plain 场景使用 `fxws-private.gateapi.io:80`；Binance data session 仍按 public TLS bookTicker。
- 开仓 freshness guard 只拦截 `kOpenLong` / `kOpenShort`，close / stoploss 不受影响。每个 `[[lead_lag.pairs]]` 直接配置整数毫秒字段 `max_lead_freshness_ms` / `max_lag_freshness_ms`；freshness 定义为 `signal_decision_ns - lead_exchange_ns` 和 `signal_decision_ns - lag_exchange_ns`。
- `scripts/lead_lag/generate_live_report.py` 生成 `signal.csv`、`order_detail.csv`、`position.csv`、`latency.csv` 和 schema 副本。报告支持 Ack RTT 三段拆解、滑点、actual / raw PnL 和胜率。
- 30-symbol report 必须显式传 `--instrument-catalog config/instruments/usdt_futures_common_gate_binance_20260602.csv`，否则默认 12-symbol catalog 可能导致 multiplier / PnL 不完整。
- `config/instruments/usdt_futures_common_gate_binance_20260602.csv` 是 Gate / Binance USDT 永续交集合约 catalog；`contract_multiplier` 是 report / PnL 使用的显式字段，当前与 `notional_multiplier` 保持一致。
- 2026-06-05 已停止 30-symbol run `20260604_0646_30symbols_30d_private` 并生成正式 report：`reports/20260604_0646_30symbols_30d_private/`。该进程在 freshness guard 相关 commits 前启动，未运行新 guard；事后用 `signal_decision_ns - lag_exchange_ns` 统计开仓 lag freshness，`6837` 个 open signal 的 p50 `32.205ms`、p95 `791.429ms`，其中 `3921/6837` 大于 `20ms`，说明当前 `max_lag_freshness_ms=20` 会强过滤旧 run 中大量 stale lag quote 下单。详细结论见 `docs/lead_lag_live_runtime_plan.md`。
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
- 尚未完成：REST reconcile、feedback WS 断线未知订单恢复、batch/amend/cancel-all。account / position realtime feedback 是 V2 可选能力，不是当前 LeadLag V1 前置项。
- `gate_account_tui` 当前仍是只读 monitor demo；`--live-market-data` 只读既有 Gate / Binance `BookTicker` SHM。订单、仓位、PnL 和 health 还未接真实账户数据。

## 代码入口

| 模块 | 入口 |
| --- | --- |
| Trading core | `core/trading/order_types.h`、`core/trading/order_manager.h`、`core/trading/trading_runtime.h`、`core/trading/order_feedback_shm.h` |
| Gate trading | `exchange/gate/trading/order_session.h`、`exchange/gate/trading/order_feedback_session.h`、`exchange/gate/trading/order_feedback_parser.h`、`exchange/gate/trading/order_session_runtime_adapter.h` |
| WebSocket diagnostics | `core/websocket/critical_session.h`、`core/websocket/plain_socket.h`、`core/websocket/socket_timestamping.h`、`exchange/gate/trading/order_latency_diagnostics.h` |
| Market data | `core/market_data/types.h`、`core/market_data/realtime_data_reader.h`、`core/market_data/data_shm.h`、`exchange/gate/market_data/*`、`exchange/binance/market_data/*` |
| LeadLag | `strategy/lead_lag/strategy.h`、`strategy/lead_lag/config.*`、`tools/lead_lag/replay.cpp`、`tools/lead_lag/live_strategy.h` |
| Reports | `scripts/lead_lag/analyze_order_detail.py`、`scripts/lead_lag/generate_live_report.py` |
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
ctest --test-dir build/debug -R 'websocket_(critical_session|socket_timestamping|plain_socket)_test|order_session_config_test|gate_order_session_rtt_probe_test|gate_order_session_test' --output-on-failure
ctest --test-dir build/debug -R monitor_ --output-on-failure
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/instruments/generate_common_usdt_perp_catalog_test.py
```

Evaluation 边界修改后运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望无命中。

## 下一步建议

1. LeadLag live：长跑或复盘先按 `docs/lead_lag_live_operations_pipeline.md`；report 使用正确 instrument catalog，重点看 actual / raw PnL、slippage、Ack RTT、send-to-finish 和 exchange lifecycle。
2. Freshness guard：后续 30-symbol live 必须用最新 release binary 重启；优先检查 `signal.csv` / `order_detail.csv` 中 `lead_freshness_ns`、`lag_freshness_ns`、`freshness_guard_pass` 和 `freshness_reject_reason`，确认 stale lag quote 不再进入开仓下单。
3. Ack latency：复现 outlier 时用 private plain all-stage config，分开看 Ack RTT、Gate `x_in -> x_out`、上行 / 下行、socket timestamping 和 pcap residual。
4. Data session latency：先补 data session 分层诊断，再考虑 RX software timestamping、`TCP_INFO`、pcap 或 hardware timestamp；不要只凭 recorder binary 判断 SHM / reader 问题。
5. Gate trading：后续优先补 REST reconcile、feedback 断线恢复和更完整的 stop-and-flat 语义。
6. TUI：下一步做 monitor 专用 Gate orders raw parser、REST snapshot、account model 和 health sampler。

## 给下一个对话的提示

先运行 `git status --short --branch` 和 `git log --oneline -8`，再读 `AGENTS.md`、`README.md`、本文件和 `docs/evaluation_support.md`。当前 branch / ahead / dirty 只信 `git status`。LeadLag 真实订单按 `docs/lead_lag_live_operations_pipeline.md`；30-symbol run `20260604_0646_30symbols_30d_private` 已停止，report 在 `reports/20260604_0646_30symbols_30d_private/`。该 run 是 freshness guard 前启动的旧进程，事后分析显示 open signal lag freshness p95 `791.429ms`，按 `max_lag_freshness_ms=20` 会过滤 `3921/6837` 个 open signal；下一轮 30-symbol live 必须用最新 release binary 重启并确认 freshness guard 字段。30-symbol report 记得传 `--instrument-catalog config/instruments/usdt_futures_common_gate_binance_20260602.csv`。Data session / recorder / RTT probe / benchmark 默认放 `16-31` 测试 core，实盘 hot path 保留 `0-15`。
