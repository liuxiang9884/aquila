# aquila 新对话导读

## 目的

这份文档给新的模型对话或新接手开发者使用。目标是在不重读历史对话的前提下，快速确认 `aquila` 当前状态、事实源、代码入口、验证命令和下一步。

## 30 秒速览

- 项目：面向 crypto 高频交易的 C++20 低延迟交易系统。
- 构建：CMake + `build.sh`，依赖通过本机 `$HOME/vcpkg`。
- 当前重点：WebSocket 内核、Gate / Binance 行情、data session / SHM、strategy `DataReader`、Gate submit/cancel、order feedback SHM、Gate private `futures.orders` feedback、`OrderManager`、`TradingRuntime`、Gate runtime adapter、`demo` 策略 live smoke、LeadLag replay / live-orders 链路，以及 TUI Symbol Workbench / market data monitor demo 均已落地。
- 当前边界：LeadLag strategy 层生产订单闭环已完成；`lead_lag_strategy --execute` 已接到真实 live-orders runtime，并在 `ContinuityLost` 后停止、返回 handoff exit code。V1 flat-account、tiny-position、continuity-lost stop-and-flat、ZEC 小额 filled open / close、unfilled-cancel smoke 和本地端到端 benchmark 已完成；外围 `run_live_with_guard.py` 已负责 preflight、final REST check 和异常 stop-and-flat。2026-05-22 release 11-symbol live-orders guarded run 只完成 1 组完整 strategy open / close，并暴露 Gate IOC partial-fill terminal feedback 缺失和 decimal-size REST flat 判断不足；当前 C++ order / feedback / Gate encoder / LeadLag sizing 已支持 decimal quantity，Gate `futures.orders` parser 已补高精度 fill price 的 IOC partial-fill terminal 单元测试，REST final check / emergency flatten 已支持 decimal size 与 `value` / `margin` residual 判断。按 2026-05-27 当前接手决策，IOC partial-fill / decimal filled close 不再作为当前阶段 active blocker，后续如果 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再按具体问题复查。当前版本不新增独立 `AccountPositionFeedbackSession`；account / position realtime feedback 作为 V2 可选能力。TUI 当前仍是只读 monitor demo：market data 可从现有 Gate / Binance `BookTicker` SHM 读取并降级显示 `NA`，订单、仓位、PnL 和 health 还未接真实账户数据。
- 当前 LeadLag latency 状态：2026-05-25 `219.023ms` Ack RTT outlier 已补 diagnostic / affinity / report 字段，2026-05-26 拆核 30 分钟 run 未复现；Ack RTT outlier 当前处于 inactive investigation，等待复现后再结合 order write path diagnostic、调度证据和多连接对照分析。2026-05-29 已新增下单写路径分段时间戳、socket send queue 观测和 LeadLag BBO-to-signal / signal-to-order 字段；2026-05-30 已修复 report / CSV 中跨时钟域的 `bbo_to_strategy_ns` / `trigger_to_request_send_ns` 输出，跨时钟域时置空并写 warning。2026-05-30 Gate Ack latency diagnostic 阈值已配置化，默认 `ack_rtt_threshold_ns=20000000`，测试时可设 `0` 做每 Ack 采样；热路径和限流边界见 `docs/diagnostic_fields.md`。
- 当前 LeadLag live 证据：仓库默认 12-symbol live 配置仍是 `open_slippage=2` / `close_slippage=2` ticks；2026-05-29 的 3-tick / 0-tick run 只用 `/home/liuxiang/tmp/<run_id>/configs` 临时覆盖。2026-05-29 两轮隔离 run 和 2026-05-30 4 小时 run 中 Gate data / order / feedback 均使用 private non-TLS `fxws-private.gateapi.io:80`，Binance data 仍是 public TLS。2026-05-30 4 小时 2-tick run `20260530_071328_12pair_live_2ticks_private_4h` 正常 flat，`signals=62`、entry submitted `58`、closed positions `4`，net PnL `-0.1383270504`，no-slip net `-0.0475420504`；Ack RTT p50 `0.584ms`、p95 `4.641ms`、max `13.283ms`，未触发 `20ms` Ack diagnostic；最大 send-to-finish `120.623ms` / exchange lifecycle `114.832ms`，属于 terminal lifecycle 尾部，不是 Ack path outlier。
- Gate `OrderSession` RTT probe 第一版设计已落地，仍是 measurement-only，不自动 score / 切换；当前已有 V1a single-session dry-run scaffold、pinned session config builder、sample flow / executor / id allocator / CSV writer 前置逻辑和 `--live-preflight`，支持 `ioc` / `gtc` / `ioc+gtc` order mode 及同 cycle order session 非阻塞 pacing。下一步先接入 feedback reader、REST guard 和 single-session live order sample，再扩展到多 `connect_ip` 采集 GTC place、GTC cancel、IOC place Ack RTT。若需要每笔 Ack 的 write path / socket queue / runtime loop 细节，临时把 order session config 的 `ack_rtt_threshold_ns=0` 且把 `max_logs_per_second` 设高于该 session 下单速率；不要作为长期生产默认。
- 当前建议分支入口：`main`。
- 核心原则：正确性、确定性、最低延迟、尾延迟可控、固定容量、少动态分配；性能结论必须有 benchmark / profile / live probe 证据。

## 新对话第一步

先运行：

```bash
git -C /home/liuxiang/dev/aquila status --short --branch
git -C /home/liuxiang/dev/aquila log --oneline -8
```

再读：

```text
AGENTS.md
README.md
docs/project_onboarding_guide.md
docs/evaluation_support.md
```

按方向继续读：

| 方向 | 优先文档 |
| --- | --- |
| Gate 交易架构 | `docs/agent-handoff-gate-trade-architecture.md` |
| TUI / account monitor | `docs/tui_onboarding_guide.md`、`docs/tui_gate_account_monitor_design.md` |
| Binance 行情 | `docs/agent-handoff-binance-market-data.md` |
| data session / config | `docs/data_session_config.md`、`docs/data_reader_config.md`、`docs/data_session_shm_communication_design.md` |
| 交易组件边界 | `docs/strategy_order_component_model.md` |
| LeadLag fixed 策略 | `strategy/lead_lag/README.md`、`docs/leadlag-fixed-strategy-reconstruction-guide.md` |
| LeadLag 实盘长跑 / 测试 | `docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_operations_pipeline.md`、`docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_runtime_latency_improvement_plan.md`、`docs/gate_order_session_rtt_probe_design.md`、`docs/lead_lag_reconcile_design.md`、`docs/diagnostic_fields.md` |
| LeadLag live / replay 测试 runbook | `docs/lead_lag_live_replay_testing.md` |
| ORDI replay / 对账 | `docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` |
| WebSocket 性能 | `docs/websocket_client_future_optimizations.md`、`docs/websocket_read_write_benchmark_comparison.md` |

## 当前事实源

以本节、`git status`、`git log` 和当前代码为准；旧执行计划、旧 spec 和重复讨论文档已清理，不再作为事实源。

截至 2026-05-30：

- `main` 已完成 Task1 order feedback SHM transport、Task2 Gate private `futures.orders` parser、`OrderFeedbackSession`、`OrderManager::OnOrderFeedback()`、trading runtime production loop、Gate adapter 和 `demo` 策略 3 轮 live smoke。
- 公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`；旧 `core/strategy/*` 和 `strategy/order_types.h` / `strategy/order_manager.h` 兼容头已删除。
- Gate runtime adapter 已迁到 `exchange/gate/trading/order_session_runtime_adapter.h` + `aquila::gate::OrderSessionRuntimeAdapter`，不再放在 `tools/`。
- C++ trading quantity contract 已改为 `double quantity` + `quantity_text`：`core::OrderCreateRequest` / `core::StrategyOrder`、`OrderFeedbackEvent` / feedback SHM、`OrderManager`、Gate private feedback parser、Gate order session encoder 和 `OrderSessionRuntimeAdapter` 已支持 decimal quantity；Gate order session 在带 `X-Gate-Size-Decimal: 1` 的连接上把 JSON `size` 编码为 string，并由 `quantity_text` 按 side 生成正负文本。`kOrderFeedbackShmVersion` 已 bump 到 2，旧 feedback SHM 需要重建。
- 2026-05-20 `gate_demo_strategy` 用临时 3 轮配置完成 BTC_USDT live smoke；feedback 发布 6 个 `kFilled` event，REST 复核 open orders 为空、`position size=0`、`pending_orders=0`。
- 2026-05-22 TUI / monitor 已完成 `monitor/` skeleton、FTXUI Symbol Workbench demo、health / alert / balance 静态布局、monitor 专用 market data SHM reader、optional source fallback、one-shot live dump snapshot 和 monitor smoke tests。当前 `gate_account_tui --live-market-data` 只读现有 Gate / Binance data session SHM，不自动启动 data session；缺失 SHM 时显示 `NA` 并产生 alert。
- 2026-05-23 requested 12-symbol / ETH_USDT 配置整理已验证：`data_session_config_test`、`strategy_config_test`、`lead_lag_config_test` 和 `ctest --test-dir build/debug -R lead_lag --output-on-failure` 均通过；未跑全量 ctest。
- 2026-05-24 文档整理已完成：旧 `docs/superpowers/` plan/spec、重复交易组件讨论、LeadLag 静态审计、单次运行报告和旧 signal report artifact 已删除；`docs/agent-handoff-gate-trade-architecture.md` 已压缩为当前 Gate 交易 handoff，下一轮不再读取被删除的历史文档。
- 2026-05-24 `AGENTS.md` 已明确：普通开发修改仍按用户要求决定是否 push；用户输入“结束对话”时，收尾流程会自动提交交接文档并 push 当前分支到 upstream / 默认远端。
- 2026-05-25 LeadLag requested 12-symbol live order 配置已把 `execute.open_slippage` / `execute.close_slippage` 统一设为 `2` ticks；当时重建并验证 `lead_lag_config_test` / `ctest --test-dir build/debug -R lead_lag_config --output-on-failure` 通过。
- 2026-05-25 完成一轮 12-symbol guarded live-orders 1 小时运行，使用 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml` 和 2-tick slippage。guard 最终 `normal_exit_flat` / `ok=true` / `exit_code=0`，最终 REST flat、open orders 为空；strategy log 统计 `signals=58`、`orders=58`、`finished=58`、`filled=0`，全部 terminal `kCancelled`。该 run 证明 guarded 运行和反馈闭环在无成交场景正常结束。
- 2026-05-25 上述 live run 的报告已生成到 `reports/20260525_133511_12pair_live_2ticks/`，包含 `report.md`、`signal.csv`、`order_detail.csv`、`position.csv`、`latency.csv` 和字段说明副本；latency 统计基于 `ack_rtt_ns`，58 单 p50 `5.628ms`、p95 `10.845ms`、max `219.023ms`。
- 2026-05-26 已把 `219.023ms` Ack RTT outlier 的问题、同线程口径和后续证据整理到 `docs/lead_lag_ack_latency_outlier_analysis.md`。关键结论：Ack 来自 Gate order session，不是 feedback session；`request_send_local_ns` 和 `ack_local_receive_ns` 是同一 runtime 线程上的本地时间点。CPU4 在 2026-05-25 outlier 所在窗口满载且多个 active-spin 组件绑 CPU4，本地调度 / 同核竞争是强候选，但原始日志不能证明具体根因。2026-05-28 当前状态：该 Ack RTT outlier 先等待后续复现，不再基于单次样本继续推断或修改 order session 架构。
- 2026-05-26 已落地 Gate `OrderSession` Ack latency diagnostic、runtime affinity profile overlay、report diagnostic 字段和 `exchange_lifecycle_ns = finish_exchange_ns - ack_exchange_ns`。同日 12-symbol guarded live-orders 30 分钟拆核 run 正常退出 flat，`signals=10`、`orders=10`、`finished=10`、`filled=0`，报告在 `reports/20260526_043440_12pair_live_30m/`；最大 Ack RTT `6.738ms`，没有复现 219ms outlier。最大 send-to-finish 为 DASH_USDT `45.977ms`，其中 exchange Ack-to-finish `37.336ms`，属于 Gate IOC terminal lifecycle 延迟，不是 Ack path outlier。2026-05-28 当前候选假设：terminal lifecycle latency 可能来自 Gate 交易所内部订单队列 / IOC terminal lifecycle 延迟；该判断仍未证明，后续复现时按 exchange lifecycle 与 session / endpoint 维度记录证据。
- 2026-05-28 已新增 `docs/gate_order_session_rtt_probe_design.md`：第一版计划实现独立 `gate_order_session_rtt_probe` measurement tool，对多个 `connect_ip` 采集 `gtc_place_ack_rtt_ns`、`gtc_cancel_ack_rtt_ns` 和 `ioc_place_ack_rtt_ns`，暂不自动 score / 自动切换。RTT probe 前置 IP discovery 脚本已支持 system resolver、explicit UDP resolver、历史连接日志、candidate file、pinned WebSocket handshake 和可选 `futures.login` 验证；2026-05-28 多 resolver 1800s discovery 得到 48 个候选 IP，随后 48/48 通过 `futures.login`。probe 改为 Gate `BookTicker` 行情事件驱动，不固定只测 ZEC；默认 `order_mode="ioc+gtc"` 时先串行跑一轮 GTC place / Ack 后立即 cancel，再串行跑一轮 IOC place，也可选 `ioc` 或 `gtc` 单独子路径；GTC / IOC 各自下单前都用该 symbol 最新 BBO 和 catalog `price_limit_down * 0.5` 计算不会成交的 passive buy price。当前连接维度已迁到 `probe.inputs.connections_file` 指向的 CSV，字段为 `name,group,host,connect_ip,port,enable_tls,worker_cpu_id`；行顺序就是 session 顺序，`name` 唯一，`connect_ip` 可重复，重复 IP 表示多条独立连接。仓库默认 CSV 为 `config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`：前 8 行沿用 2026-05-29 `gate_rtt_8ip_selected_private_old4_ioc_500ms_500ms_30m_20260529_091715` 的 IP 列表，第 9-12 行为同一个 private no-TLS `fxws-private.gateapi.io:80` / `10.0.1.154` 重复连接。TOML 不再配置 `candidate_ip_file`、`active_session_count`、`max_candidates`、`worker_cpu_ids` 或 `endpoint_overrides`；`samples_per_session` 控制每条连接采样轮数。当前代码入口：`tools/gate/order_session_rtt_probe/connection_plan.*`、`config.*`、`run_plan.h`、`live_run_plan.h`、`main.cpp`、`sample_csv_writer.*`；sample CSV 已新增 `session` / `group` 字段。feedback / REST / continuity / dedicated-account / reduce-only safety guardrail 在 V1a 配置中仍不能关闭；connection log 仍是 planned。
- 2026-05-31 RTT probe 已支持 `cycle_cooldown_us` / `order_session_interval_us` 微秒粒度 pacing；旧 `*_ms` 字段仍按毫秒解析。sample CSV 现在可写入 Ack diagnostic window 快照，包括 runtime hook / DriveRead、write path、socket send queue、`TCP_INFO` 和 socket timestamping 分段字段；当 `ack_rtt_threshold_ns=0` 时每 Ack 都会写入，且不依赖 diagnostic log 是否被 `max_logs_per_second` 限流。8 条 private plain 全阶段测试配置已新增：`config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml`、`config/order_session_rtt_probe/gate_order_session_rtt_private8_plain_connections.csv` 和 `config/order_sessions/gate_order_session_rtt_probe_allstage.toml`。
- 多 Gate `OrderSession` / WebSocket 连接即使使用同一 URL，Ack RTT 分布也可能不同；相同 hostname 不保证相同 remote IP、gateway shard、per-connection queue、网络路径或本地 owner thread 状态。`OrderSession` 多连接诊断已落地 `order_session_id`、connected endpoint / owner CPU、send CPU、ack CPU、diagnostic CPU 和可选 `TCP_INFO`；`TCP_INFO` 默认关闭，需要 `order_session.diagnostics.enable_tcp_info=true`。Ack latency diagnostic 默认只有单笔 Ack RTT 严格大于 `20ms` 才输出；2026-05-30 已把 `ack_rtt_threshold_ns`、send-to-drive-read / drive-read / timeout 阈值和 `max_logs_per_second` 暴露到 `[order_session.diagnostics]`，默认值保持不变。测试时可将 `ack_rtt_threshold_ns=0` 做每 Ack 采样，但 `AllowLog()` 和 `NOVA_WARNING` 会进入 Ack 热路径，`TCP_INFO` 打开后还会采 `getsockopt(TCP_INFO)`，只适合短期诊断。2026-05-27 3 条独立 order session 并发 smoke 证明同一 hostname 会落到不同 remote IP，endpoint / CPU / TCP_INFO 字段在真实订单路径可用；每条连接只有 1 笔订单，不构成稳定 RTT 分布结论。多连接测试必须按 session / connection 维度记录 remote endpoint、local port、owner CPU、Ack RTT 分布和 TCP diagnostics，细节见 `docs/lead_lag_runtime_latency_improvement_plan.md` 和 `docs/diagnostic_fields.md`。
- WebSocket endpoint 配置使用 `host` / 可选 `connect_ip` / `port`：`connect_ip` 为空时 TCP 解析 `host`，非空时 TCP 直连该 IP；TLS SNI / 证书校验和 WebSocket Host 始终使用 `host`。2026-05-27 Python 和 C++ live probe 已验证 Gate 三个 remote IP 均可用 logical host 模式完成 WebSocket handshake，private login-only 也返回 `200`；把 IP 作为 TLS hostname 会因证书 hostname mismatch 失败。该能力不包含自动选优，细节见 `docs/lead_lag_runtime_latency_improvement_plan.md`。
- 2026-05-29 已提交 `763b73b Add live order latency diagnostic fields`：Gate `OrderSession` 增加 order encode、WebSocket frame encode、enqueue、write pump、`send()` / `SSL_write()`、write complete 和 socket send queue 诊断字段；LeadLag 增加 `trigger_exchange_ns`、`trigger_local_ns`、`on_book_ticker_entry_ns`、`signal_decision_ns` 等 BBO 到信号 / 下单口径。字段集中登记在 `docs/diagnostic_fields.md`，report analyzer 已合并相关字段。
- 2026-05-29 完成两轮隔离 12-symbol live-orders run，Gate data session、Gate order session 和 Gate order feedback session 均使用 private non-TLS endpoint `fxws-private.gateapi.io:80`；Binance data session 仍使用 public TLS `fstream.binance.com:443`。3-tick run `20260529_122135_12pair_live_3ticks_private` 正常 flat 退出，49 个 signal / order，4 个成功 position、6 个 close slice，估算 net PnL `-0.44015368688`，entry 平均滑点约 `2.75` ticks、exit 加权平均约 `1.53` ticks。0-tick run `20260529_135710_12pair_live_0ticks_private` 正常 flat 退出，85 个 signal，84 个 entry order、1 个 close order，仅 `DASH_USDT` short 部分成交 1 张后平仓；open / close 执行滑点均为 0 ticks，gross PnL `+0.0001`，估算 fee `0.000126064`，net PnL `-0.000026064`。
- 2026-05-29 0-tick run 中 6 个 entry order 被 Gate final response 拒绝：Ack 都很快到达，但 final response 为 `http_status=504`、`error_label=INTERNAL`、`error_message=Request Timeout`，其中 5 笔集中在 14:51:26-14:51:31 UTC 的 burst，另 1 笔在 14:57:16 UTC 单独出现。结合 Gate 官方文档，`5xx` / `INTERNAL` 属于服务端内部处理错误；本地日志只能证明请求已成功发出且 Gate 已 Ack，不能定位到 Gate 内部具体是 order gateway、account / risk、matching 或内部 RPC timeout。
- 2026-05-30 4 小时 12-symbol private non-TLS 2-tick run `20260530_071328_12pair_live_2ticks_private_4h` 正常 `normal_exit_flat`，final REST check flat，strategy / guard / feedback 已退出。最终 `signals=62`、orders submitted `62`、entry submitted `58`、exit submitted `4`、实际开仓成交 `4`、closed positions `4`、最终 open position `0`、intent / order reject `0`。PnL：gross `-0.069985`、fee `0.0683420504`、net `-0.1383270504`；no-slip gross `+0.0208`、no-slip net `-0.0475420504`，滑点 gross cost `-0.090785`。最大 Ack RTT `13.283ms`，低于默认 `20ms` diagnostic 阈值；最大 send-to-finish `120.623ms`，对应 exchange lifecycle `114.832ms`，归类为 terminal lifecycle tail。`final_analysis/latency.csv` 中 `bbo_to_strategy_ns` 和 `trigger_to_request_send_ns` 因跨时钟域置空，warning 为 `cross_clock_bbo_to_strategy_ns;cross_clock_trigger_to_request_send_ns`。
- 2026-05-30 已提交 `fcf452c Guard LeadLag report timing against cross clocks`：report / CSV 不再把 data session monotonic `trigger_local_ns` 与 strategy epoch-style 本地时间直接相减；跨时钟域字段置空并写 warning。已提交 `131ee84 Make Gate order ack diagnostics configurable` 和 `9de09b6 Document Gate ack diagnostic hot path`：`OrderSessionConfig -> OrderSessionRuntimeAdapter -> OrderSession` 传递 Ack latency diagnostic config，`config/order_sessions/gate_order_session.toml` 显式写默认阈值，文档记录每 Ack 采样和 `AllowLog()` 热路径成本。
- 2026-05-25 LeadLag 实盘启动、10 分钟巡检、report 生成和 zip 打包的 agent pipeline 已从 `AGENTS.md` 迁到 `docs/lead_lag_live_operations_pipeline.md`；`AGENTS.md` 只保留触发词索引，`docs/lead_lag_live_runtime_plan.md` 和本 onboarding 已加入索引。
- 工作区状态以 `git status` 为准；如出现本地未提交或未跟踪文件，先确认用途和归属再处理。

## 已完成摘要

### WebSocket / 行情

- WebSocket P0/P1/P2/P3 主体已完成：DNS / TCP / TLS / WebSocket cold path、active spin hot path、read/write pump、prepared write、heartbeat、reconnect/backoff、probe 和 benchmark。
- Gate futures SBE BBO 行情已落地：SBE schema / generated headers、message dispatch、BBO decoder、market data client、data session、live probe 和 benchmark。
- Binance USD-M futures JSON bookTicker 行情已落地：raw stream target、`simdjson::ondemand` parser、client/session、live probe 和 benchmark。
- Gate / Binance data session TOML parser、instrument catalog、SHM sink、startup tools 和 log config 已落地。
- `BookTicker` 作为统一行情结构进入 strategy `DataReader`；生产热路径不保存字符串 symbol，只保存内部 `symbol_id`。
- Gate / Binance 合约元数据脚本已输出统一一类下单前字段，字段语义见 `docs/futures_contract_metadata_fields.md`。
- Gate decimal-size 合约的 catalog metadata 已从 `order_size_min` 推导 `quantity_step` / `quantity_decimal_places`；`RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 的 Gate 行当前为 `quantity_step=0.1`、`quantity_decimal_places=1`。C++ 下单 / 回报 / LeadLag sizing 已消费该 metadata。2026-05-24 RAVE 小额 order session probe 确认未 quote 的 `"size":0.1` 会被 Gate 以 `INVALID_REQUEST` / `Mismatch type int64 with value number` 拒绝；带 decimal header 时 quote 为 `"size":"0.1"` 后不再 400，REST 复核 open orders 为空、position `size=0` / `pending_orders=0`。REST final check / emergency flatten 已带 decimal-size header、解析 decimal position size 并检查 `value` / `margin` residual；按 2026-05-27 当前接手决策，decimal filled close 不再作为当前阶段 active blocker。

### DataReader / SHM

- `RealtimeDataReader` 和 `HistoricalDataReader` 已按 concept 形式落地，不要求虚基类。
- `Poll(handler)` 是单事件接口，`Drain(handler, max_events)` 是批量接口；`HistoricalDataReader` 额外提供 `finished()`。
- `DataReader` 不做 merge：实时多路 merge 归上游 data session / producer，历史多路 merge 归离线预处理。
- `RealtimeDataReader` 构造期要求至少一个 realtime source；多 source round-robin 使用构造期双表扫描。
- `HistoricalDataReader` 构造期 mmap 非空 binary 文件，热路径不打开文件、不抛异常。
- `DataReaderConfig::max_events_per_drain` 是 finite / replay reader 的外层 `Drain()` budget；旧字段 `max_events_per_source` 已删除。
- reader stats 已聚焦数据流本身；`poll_calls` / `empty_polls` 归 runtime / scheduler diagnostics，当前可通过 `TradingRuntimeDiagnostics` 记录。
- 2026-05-06 live drain 验证中 Gate / Binance source 均未检测到 SHM ring overrun。
- `data_reader_recorder` 已落地：输入 data reader TOML，使用 `RealtimeDataReader::Drain()` 从 Gate / Binance `BookTicker` SHM 写出合并后的 replay binary；默认单文件输出，也可通过 `[recorder].rotation_enabled = true` 按时间切分 segment。输出是连续 `aquila::BookTicker` 结构体记录，不加 header，记录顺序沿用 reader 实际 handler 输出顺序。rotation 默认 `rotation_interval_sec = 3600`，当前只支持 `--mode truncate`；关闭后的 `.bin` segment 会写入 manifest JSONL，可用 `scripts/market_data/manifest_to_data_reader_config.py` 生成多文件 replay TOML。
- 2026-05-24 live record smoke 已完成：Gate / Binance data session 写 SHM，临时 `drain` recorder 写出 `15,685` 条裸 `BookTicker` binary，Gate `1,825` 条、Binance `13,860` 条，两个 source 的 `skipped=0`、`overruns=0`；`data_reader_probe` 的 historical mode 通过 `HistoricalDataReader` 读完同一文件。
- `data_reader_recorder` 是只读 SHM consumer，可以和 LeadLag / demo 策略实盘交易并行运行；完整 replay dump 必须使用临时 `drain` 配置，默认 `latest` 配置只适合状态采样。

### TUI / Account Monitor

- `monitor/` 已作为独立顶层目录落地，依赖方向为 `monitor/* -> core/config/exchange`；生产交易链路不反向依赖 `monitor/`。
- `gate_account_tui` 当前支持 interactive TUI、`--dump`、`--view health`、`--live-market-data` 和 `--market-data-config`。默认无参数显示静态 Symbol Workbench demo；live market data 需要外部 Gate / Binance data session 已经发布 SHM。
- Symbol Workbench 当前覆盖 requested 11 symbols：`PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`、`DASH_USDT`、`RIVER_USDT`、`SUI_USDT`、`INJ_USDT`、`ENA_USDT`、`BRETT_USDT`；默认选中 `ZEC_USDT`。
- `MarketDataThread` 使用 monitor 专用 SHM reader，支持 optional source attach 失败后继续运行；按 Gate / Binance 严格单调 `BookTicker.id` coalesce 最新 BBO，每 100ms 通过 SPSC 只向 UI 推 changed rows。
- `--dump --live-market-data` 使用 one-shot snapshot，从 visible window 按 `earliest_visible + drain` 读取并渲染一帧，适合 SSH / 隧道环境检查。interactive live path 仍沿用 config 中的 `latest + drain`。
- market data diagnostics 已可见：SHM unavailable、reader overrun 和 UI dropped batch 会进入 alert；当前 `BookTicker` 不含 `last_price`、最新成交量、24h volume、turnover / value，这些字段在 TUI 中显示 `NA`。
- 订单、仓位、PnL 和 health 仍是 demo / 静态数据；后续需要实现 monitor 专用 order source、REST snapshot、account model 和真实 health sampler。

### Gate 交易

- Gate `OrderSession` 第一版 submit/cancel C++ 主路径已落地：login HMAC-SHA512、固定缓冲区 request encoder、place/cancel、response correlation、同步 `OrderResponse` 回调和 benchmark。
- `OrderManager` 负责订单对象、订单池、状态机和直接 session 发送；不缓存 Gate wire fields，不维护 exchange order id 索引，不暴露两阶段 `PrepareOrder()` / `SubmitOrder()`。
- 下单 RTT 观测字段已进入 `StrategyOrder`：`request_send_local_ns`、`ack_local_receive_ns`、`response_local_receive_ns` 使用本机 Unix epoch ns，其中 `request_send_local_ns` 是请求提交给 WebSocket 发送路径前的本地时间；`ack_exchange_ns` / `response_exchange_ns` 来自 Gate submit response header 的 `x_out_time` 或 `response_time`；`accepted_exchange_ns` / `finish_exchange_ns` 来自 private `futures.orders` feedback。旧 `exchange_update_ns` 仍保留为最后一次已应用 feedback 的兼容字段。
- Task1 order feedback SHM transport 已落地：固定 8 lane、Nova SPSC、宽结构 `OrderFeedbackEvent`、continuity lost control event、publisher / reader / config / tests / benchmark。
- Task2 Gate order feedback 已落地：private `futures.orders` parser、`OrderFeedbackSession`、SHM publish、disconnect continuity lost 和 `OrderManager::OnOrderFeedback()`。
- `OrderManager::OnOrderFeedback()` 已处理 accepted、partial filled、filled、cancelled / terminal、rejected 和 continuity lost；accepted 后保存 exchange order id 并更新 cancel cache，terminal 后清理 cache。
- `gate_strategy_order` 已作为 `OrderManager` + Gate WebSocket 单笔下单工具落地；`gate_order_session_failure_probe` 已作为独立 `OrderSession` failure response 诊断工具落地；`gate_demo_strategy` 已作为 runtime live smoke 工具落地。
- 尚未完成：REST reconcile、feedback WS 断线未知订单恢复、batch/amend/cancel-all；account / position realtime feedback 可作为 V2 风控状态能力评估，不是当前 LeadLag V1 实盘前置项。

### Trading Runtime

- `TradingRuntime<StrategyT, OrderSessionT, DataReaderT>` 已落地，生产 `Create()` 从已解析 config 构造 data reader、order session、order manager、context、strategy 和可选 feedback reader。
- Gate production 路径使用 `OrderSessionT::SetRuntimeHook()` 在 WebSocket active spin loop 同线程轮询 feedback SHM / data reader。
- `OnOrderResponse()` 和 `OnOrderFeedback()` 都先更新 `OrderManager`，再调用 strategy hook。
- Gate adapter 不创建后台线程、不维护 command queue；place / cancel 在 StrategyThread / Gate session 同线程直接调用。
- `Ready() == false` 是上行交易能力硬边界；feedback continuity lost 是下行订单事实流连续性信号，不由 runtime 统一禁止开仓。

### LeadLag

- LeadLag C++ 策略层、`OnBookTicker()` replay 信号主链路、config / metadata、raw market state、alignment、threshold、signal / execution state、feedback state 和 order retire 已落地；接手入口为 `strategy/lead_lag/README.md` 和 `docs/leadlag-fixed-strategy-reconstruction-guide.md`。
- `docs/lead_lag_live_runtime_plan.md` 是实盘 runbook；`docs/lead_lag_live_operations_pipeline.md` 是 agent 启动 / 巡检 / report pipeline；`docs/lead_lag_live_replay_testing.md` 定义 `lead_lag_live_replay_signal_parity <duration>`，所有产物写入 `/home/liuxiang/tmp/<run_id>`。
- `lead_lag_strategy --execute` 已接 Gate live-orders runtime；缺凭据 exit code `2`，收到 `ContinuityLost` 返回 handoff exit code `10`。外围 `run_live_with_guard.py` 已负责 REST preflight、final check 和异常 emergency flatten。
- 已完成 flat-account、tiny-position、隔离 `ContinuityLost` stop-and-flat、ZEC 小额 filled open / close、unfilled-cancel smoke 和本地端到端 benchmark。2026-05-22 release run 暴露的 IOC partial-fill terminal feedback 与 decimal residual 问题已代码修复；按 2026-05-27 当前接手决策，不再作为当前阶段 active blocker，后续遇到 terminal feedback、filled close 或 REST residual 异常再复查。
- requested 12-symbol 配置覆盖 `PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`、`DASH_USDT`、`RIVER_USDT`、`SUI_USDT`、`INJ_USDT`、`ENA_USDT`、`BRETT_USDT`、`ETH_USDT`；真实订单 `--execute` 默认用 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`，signal-only 用 `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml`。
- 12-symbol live order 仓库默认配置当前 `open_slippage=2` / `close_slippage=2` ticks，`max_gross_notional=2000.0`，`max_holding_position` 未启用；2026-05-29 的 3-tick / 0-tick run 只通过 `/home/liuxiang/tmp/<run_id>/configs` 临时覆盖，不代表仓库默认值已修改。`RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 走 0.1 张 decimal quantity，Gate WS 下单带 decimal header 时 quote JSON `size`。
- `scripts/lead_lag/analyze_order_detail.py` 可生成 `order_detail.csv`、`position.csv` 和 `latency.csv`；真实订单模式不写 per-signal CSV，日志通过 `trigger_ticker_id` 关联 signal / intent / submitted order。

### Evaluation

- `evaluation/` 已作为 test / benchmark 共享辅助代码目录落地，不是生产路径。
- `aquila_evaluation` 是 header-only target，只允许 `test/` 和 `benchmark/` target 链接。
- `core/`、`exchange/`、`tools/` 不允许 include `evaluation/`，也不允许链接 `aquila_evaluation`。

## 文档索引

| 文档 | 什么时候读 | 关键内容 |
| --- | --- | --- |
| `AGENTS.md` | 每次新会话最先读 | 中文/英文约定、低延迟原则、测试 / benchmark / 提交规则 |
| `README.md` | 了解构建和工具入口 | build、ctest、benchmark、probe、latency compare |
| `docs/evaluation_support.md` | 增加 test / benchmark 共享辅助代码 | `evaluation/` 边界和提交前检查 |
| `docs/diagnostic_fields.md` | 新增或删除诊断字段、log key、stats、report CSV 字段 | 按组件登记字段用途、表面、生命周期、删除条件和代码入口 |
| `docs/futures_contract_metadata_fields.md` | 处理合约基础信息 | 统一 metadata 字段、Gate / Binance 映射、数量单位差异 |
| `docs/tui_onboarding_guide.md` | 接手 TUI / account monitor | 当前范围、运行命令、实现入口、未完成项 |
| `docs/tui_gate_account_monitor_design.md` | 继续 TUI 设计或实现 | Symbol Workbench、market data SHM、order / health 线程模型和测试建议 |
| `docs/strategy_order_component_model.md` | 细化交易组件边界 | DataReader、OrderSession、OrderFeedbackSession、OrderManager、Strategy |
| `docs/data_session_config.md` | 修改 data session 配置 | instrument catalog、subscribe symbols、WS / log / SHM 配置 |
| `docs/data_reader_config.md` | 修改 strategy reader 配置 | SHM source、read mode、Poll / Drain、diagnostics policy |
| `docs/data_session_shm_communication_design.md` | 维护行情 SHM | DataShmPublisher、BookTickerShmReader、overrun 边界 |
| `docs/agent-handoff-gate-trade-architecture.md` | 继续 Gate 交易架构 | 当前 Gate 协议事实、线程 / 进程边界、组件职责、代码入口和验证入口 |
| `docs/agent-handoff-binance-market-data.md` | 继续 Binance 行情 | raw stream、JSON parser、client/session、benchmark |
| `strategy/lead_lag/README.md` | 快速理解 LeadLag 目录 | 模块职责、OnBookTicker 主流程、replay 输出、边界 |
| `docs/lead_lag_live_runtime_plan.md` | 准备 LeadLag 长时间实盘运行和测试 | signal-only runner、订单闭环、`ContinuityLost` 应急链路、live smoke、benchmark 顺序 |
| `docs/lead_lag_live_operations_pipeline.md` | 执行 LeadLag 实盘 agent 操作 | 启动触发词、guarded live run pipeline、10 分钟巡检、report 生成和 zip 打包 |
| `docs/lead_lag_ack_latency_outlier_analysis.md` | 继续分析 LeadLag live Ack 延迟 outlier | 219ms Ack RTT 事实、同线程口径、拆核 30 分钟 run 证据、Ack RTT 与 exchange Ack-to-finish 口径区分 |
| `docs/lead_lag_runtime_latency_improvement_plan.md` | 继续 LeadLag latency 验证 | 已落地 diagnostic / affinity / report 字段、仍未完成的复现和调度归因要求 |
| `docs/gate_order_session_rtt_probe_design.md` | 设计 Gate `OrderSession` 连接 RTT 测量 / 寻优前置工具 | measurement-only 第一版、IP discovery、GTC place / cancel / IOC place RTT、probe order 安全边界、线程模型和未来 scout 方案 |
| `docs/lead_lag_live_replay_testing.md` | 准备 LeadLag live / replay 对比测试 | 标准测试名、输出目录、临时 config、使用程序、signal parity 分析方法 |
| `docs/lead_lag_reconcile_design.md` | 准备 LeadLag `ContinuityLost` 应急处理 | stop-and-flat V1、Python REST 撤单 / reduce-only 市价平仓、V2 read-only reconcile 边界 |
| `docs/leadlag-fixed-strategy-reconstruction-guide.md` | 继续 LeadLag 迁移 | fixed Go 语义、Aquila 映射、信号链路重建 |
| `docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` | ORDI replay / 对账 | Tardis / HDF 输入差异、signal key、slip PnL |

## 代码入口

### Core / Config

| 文件 | 职责 |
| --- | --- |
| `core/common/result.h` | 通用 `Result<T>` |
| `core/common/types.h` | 项目通用枚举 |
| `core/common/constants.h` | 通用常量 |
| `core/trading/order_id.h` | `strategy_id` + `strategy_order_id` 编解码 `local_order_id` |
| `core/utils/numeric.h` | `fast_float::from_chars` 数字转换 helper |
| `core/utils/mapped_file.h` | read-only mmap RAII helper |
| `core/config/websocket_config.h` | WebSocket TOML 配置和 `ConnectionConfig` 转换 |
| `core/config/instrument_catalog.h` | 启动期 instrument CSV catalog |
| `core/config/data_reader_config.h` | Strategy data reader TOML parser / loader |
| `core/config/strategy_config.h` | Trading runtime `[strategy]` TOML parser / loader |

### Market Data

| 文件 | 职责 |
| --- | --- |
| `core/market_data/types.h` | 统一 `BookTicker` |
| `core/market_data/data_reader_concepts.h` | DataReader concept 约束 |
| `core/market_data/realtime_data_reader.h` | Strategy 侧 realtime SHM reader |
| `core/market_data/historical_data_reader.h` | Binary replay reader |
| `core/market_data/data_shm.h` | `DataShmPublisher` 和 `BookTickerShmReader` |
| `tools/market_data/data_reader_recorder.*` | Gate / Binance `BookTicker` SHM 到 replay binary recorder，支持单文件和 rotation segment |
| `exchange/gate/market_data/*` | Gate SBE BBO client / session / config |
| `exchange/binance/market_data/*` | Binance bookTicker stream / parser / client / session / config |
| `tools/market_data/data_reader_probe.cpp` | 多 SHM source reader probe |

### Monitor / TUI

| 文件 | 职责 |
| --- | --- |
| `monitor/CMakeLists.txt` | `aquila_monitor` library 和 `gate_account_tui` executable |
| `monitor/tui/gate_account_tui.cpp` | TUI 入口，支持 static demo、live market data、dump 和 health view |
| `monitor/tui/symbol_workbench_view.h` | Symbol Workbench FTXUI 布局 |
| `monitor/tui/runtime_health_view.h` | health / alert view 布局 |
| `monitor/tui/quit_events.h` | `q` / `Esc` / Ctrl-C quit event 处理 |
| `monitor/model/account_monitor_snapshot.h` | TUI 可见 snapshot model |
| `monitor/model/market_data_view_model.h` | market data batch 到 UI 行的转换 |
| `monitor/model/monitor_spsc_queue.h` | monitor 内部固定容量 SPSC queue |
| `monitor/demo/symbol_workbench_demo_data.*` | 当前静态 demo 数据 |
| `monitor/market_data/market_data_thread.*` | monitor 专用 market data reader thread 和 one-shot snapshot |
| `monitor/market_data/market_data_store.h` | 按 `(exchange, symbol_id)` coalesce latest BBO |
| `monitor/market_data/market_data_update.h` | MarketData batch / row / diagnostics payload |
| `config/monitors/gate_account_tui_market_data.toml` | TUI market data SHM reader 配置 |

### Trading / Gate

| 文件 | 职责 |
| --- | --- |
| `core/trading/order_types.h` | 公共订单请求、订单对象、response / feedback event 类型 |
| `core/trading/order_pool.h` | 固定容量订单池 |
| `core/trading/order_manager.h` | 模板化 `OrderManager<OrderSessionT>` |
| `core/trading/strategy_context.h` | strategy 窄下单接口 |
| `core/trading/trading_runtime.h` | production runtime |
| `core/trading/order_feedback_event.h` | order feedback 宽结构 event ABI |
| `core/trading/order_feedback_shm.h` | 8 lane feedback SHM transport |
| `exchange/gate/trading/order_session.h` | Gate submit/cancel WebSocket session |
| `exchange/gate/trading/order_latency_diagnostics.h` | Gate Ack latency diagnostic window、阈值和 per-session log rate limit |
| `exchange/gate/trading/order_session_config.*` | Gate order session TOML parser；`[order_session.diagnostics]` 解析 `enable_tcp_info`、Ack diagnostic 阈值和 `max_logs_per_second` |
| `exchange/gate/trading/order_session_runtime_adapter.h` | Gate runtime adapter |
| `exchange/gate/trading/order_feedback_parser.h` | Gate private `futures.orders` parser |
| `exchange/gate/trading/order_feedback_session.h` | Gate feedback session |
| `tools/gate/strategy_order.cpp` | 单笔 Gate WS 下单工具 |
| `tools/gate/order_session_failure_probe.cpp` | 独立 Gate `OrderSession` failure response 诊断工具 |
| `tools/gate/demo_strategy.*` | `demo` strategy 和 live smoke 工具 |
| `tools/gate/order_feedback_session.cpp` | Gate feedback session 启动工具 |

### LeadLag

| 文件 | 职责 |
| --- | --- |
| `strategy/lead_lag/config.*` | LeadLag config parser / loader |
| `strategy/lead_lag/raw_market_state.h` | raw quote state 和 same-price 语义 |
| `strategy/lead_lag/window_stats.h` / `recorders.h` | rolling stats、noise、spread、move quantile |
| `strategy/lead_lag/alignment.h` | drift / alignment phase |
| `strategy/lead_lag/threshold.h` | threshold state |
| `strategy/lead_lag/cost_model.h` / `signal.h` / `execution_state.h` | signal / execution / feedback state |
| `strategy/lead_lag/strategy.h` | replay 信号主链路 |
| `tools/lead_lag/replay.cpp` | binary replay 输出 signal CSV |

### Tools / Scripts

| 文件 | 用途 |
| --- | --- |
| `tools/websocket/probe.cpp` | 单连接 live probe |
| `tools/websocket/latency_compare.cpp` | public/private latency compare |
| `tools/gate/futures_book_ticker_probe.cpp` | Gate futures BBO live probe |
| `tools/binance/futures_book_ticker_probe.cpp` | Binance bookTicker live probe |
| `tools/gate/data_session.cpp` | Gate data session 启动工具 |
| `tools/binance/data_session.cpp` | Binance data session 启动工具 |
| `tools/gate/order_session_rtt_probe/` | Gate `OrderSession` RTT probe V1a；当前为配置解析、candidate IP 读取、dry-run plan、`--live-preflight`、pinned session config builder、sample flow / executor / id allocator / CSV writer 前置逻辑；支持 `ioc` / `gtc` / `ioc+gtc` order mode 和同 cycle order session 非阻塞 pacing；sample flow 已记录 Ack 接收时间 / stage status，校验 Ack / final response `local_order_id`，并覆盖 GTC cancel reject / feedback fill / timeout -> reduce-only close、close Ack 等待 terminal feedback 和 close terminal confirmation 纯状态流转 |
| `scripts/gate/discover_gate_ws_ips.py` | Gate WebSocket `connect_ip` 候选发现；支持 system / explicit UDP resolver、历史日志和 pinned handshake 验证 |
| `scripts/gate/query_gate_account.py` | Gate read-only account / order / position 查询 |
| `scripts/gate/place_futures_order.py` | Gate REST futures 下单 / 撤单测试 |
| `scripts/gate/run_futures_order_smoke.py` | Gate REST 小额多轮 smoke |
| `scripts/gate/query_futures_contracts.py` | Gate futures 合约元数据 |
| `scripts/binance/query_um_futures_contracts.py` | Binance USD-M futures 合约元数据 |

## 当前重要结论

### WebSocket

- 生产 decode 默认使用 mirrored receive ring；单帧和 repeated `Poll()` 多帧 drain 走 direct delivery。
- ready metadata ring 已移到 `evaluation/websocket/queued_frame_codec.h` 作为对照路径。
- data frame fast path 覆盖 `FIN=1`、`RSV=0`、text/binary、server unmasked、payload length `<= 65535`。
- write path 已完成 mask key pool、8-byte chunk XOR、dedicated control write slot、prepared write 和 business write budget。
- `MessageCallback` 和 typed handler ref 同时保留；typed handler 价值主要是减少 Gate session 适配层和保留编译期组合空间，不要写成已验证性能收益。
- `BasicWebSocketClient::stop_requested_` 只作为停止位，使用 `memory_order_relaxed`；`Stop()` 中的 `Wakeup()` 用于打断阻塞等待，不承担内存同步语义。

### Gate 交易架构

当前推荐线程模型：

```text
StrategyThread + Gate OrderSession
GateOrderFeedbackThread + Gate OrderFeedbackSession
feedback SHM lane -> StrategyThread
```

关键边界：

- `OrderSession` 是上行交易指令和轻量 API response 通道，只覆盖 login、place、cancel、ack/result/error、request correlation 和同步 response 回调。
- `OrderFeedbackSession` 是下行私有订单事实通道，第一版只使用 private `futures.orders`，不接 `futures.usertrades`。
- `OrderManager` 是订单状态 owner，统一创建本地订单、分配 `local_order_id`、调用 session 发单 / 撤单，并消费 response / feedback 推进状态。
- strategy 只保存策略级 execution group 和自己关心的 `local_order_id`，通过 `StrategyContext` 下单 / 撤单 / 查询订单。
- `Ready() == false` 表示不应发起新的上行交易指令；断线时 `OrderSession` 清空 correlation，不构造假的 rejected/cancelled response，不直接改变订单状态。
- continuity lost 通过普通 feedback event 进入 `OrderManager`，不再通过 shared epoch atomic。

### Trading Runtime

- `TradingRuntime::Run()` 在支持 `SetRuntimeHook()` 的 order session 上使用同线程 hook mode。
- Gate WebSocket active spin loop 每轮先驱动 runtime hook，runtime 轮询 feedback SHM，并在 `OrderSessionT::Ready()` 为 true 后 poll data reader。
- `Ready() == false` 只 gate 行情驱动交易意图，不阻塞 order response 或 feedback drain。
- live reader 每轮调用 `Poll(runtime)`；finite replay reader 优先使用 `Drain(runtime, data_reader.max_events_per_drain)`。
- `gate_demo_strategy` 默认 dry-run；真实提交必须显式 `--execute`，并需要先启动 `gate_order_feedback_session --connect`。

### DataReader

- `DataReader` 是 Strategy-facing capability / concept，不要求运行时多态继承体系。
- `RealtimeDataReader` 无 EOF，不提供 `finished()`；`HistoricalDataReader` 通过 `finished()` 表达 EOF。
- 多 source round-robin 是实时 reader 的公平调度，不是全局时间排序。
- `Poll()` / `Drain()` 是 `noexcept` 热路径；配置校验、SHM attach、binary 文件检查和 mmap 失败保留在冷路径。
- `Diagnostics` 是记录器 / policy，`Stats` 是计数快照；不要在组件内部泛化命名为 `Metrics`。

### TUI / Account Monitor

- TUI 是独立只读 monitor，不是交易系统事实源；不能接入 `TradingRuntime`，也不应把 UI ledger 写回策略或订单状态机。
- 第一版跨线程边界是 worker thread 本地状态 + SPSC queue + UI thread owned visible model，不用 mutex 共享 UI model。
- market data 从既有 Gate / Binance `BookTicker` SHM 读取；SHM 缺失是可见降级状态，不由 TUI 自动启动 data session。
- 当前 `BookTicker` ABI 只有 BBO 和 id / timestamp 字段；`last_price`、成交量、turnover / value 显示 `NA` 是设计边界。
- 后续 order / position / PnL 需要 monitor 专用 raw event 和 REST snapshot；不要直接把交易系统 `OrderFeedbackEvent` 当作 account monitor 主事件。

### LeadLag

- `leadlag::Strategy::OnBookTicker()` 已串起 raw market state、alignment、recorder、threshold、signal engine 和 synthetic position accounting。
- 当前 replay 使用 `PositionAccountingMode::kSyntheticSignals`，不依赖真实订单 session，也不等价于真实 order/fill 回测。
- 时间口径、exact empirical quantile mode 和 synthetic replay 边界需要先和策略研发确认，再继续生产订单回报闭环。
- fixed Go 源码参考在 `third_party/strategy/wt-invariant-strategy-leadlag-must-fix/`，该目录被 git ignore。

## 常用验证命令

### 基础

```bash
./build.sh debug
./build.sh release
ctest --test-dir build/debug --output-on-failure
git diff --check
```

### WebSocket

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
ctest --test-dir build/release -R websocket_ --output-on-failure
taskset -c 2 ./build/release/benchmark/websocket/frame_codec_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/runtime_loopback_benchmark
```

### Gate / Binance 行情

```bash
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/gate_futures_book_ticker_probe --contract BTC_USDT --symbol-id 1 --duration-ms 10000
./build/debug/tools/binance_futures_book_ticker_probe --contract BTCUSDT --symbol-id 1 --duration-ms 10000
ctest --test-dir build/debug -R '(gate_.*market_data|binance_.*market_data|data_session_config)' --output-on-failure
```

### DataReader

```bash
./build/debug/test/config/data_reader_config_test
./build/debug/test/core/market_data/core_market_data_realtime_data_reader_test
./build/debug/test/core/market_data/core_market_data_historical_data_reader_test
./build/debug/test/core/market_data/core_market_data_shm_test
TMPDIR=/home/liuxiang/tmp ./build/debug/test/tools/market_data/data_reader_recorder_test
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/manifest_to_data_reader_config_test.py
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
./build/debug/tools/data_reader_recorder --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml --output /home/liuxiang/tmp/live_merged_book_ticker.bin --mode truncate
```

Live drain / recorder 验证需要先启动 Gate / Binance data session 写 SHM，再用临时 drain 配置运行 probe 或 recorder；不要把仓库默认 `strategy_data_reader.toml` 改成 drain。实盘交易并行录制时观察 recorder 统计里的 per-source `overruns` / `skipped`，并避免抢占交易关键线程 CPU。

### TUI / Monitor

```bash
cmake --build build/debug --target gate_account_tui monitor_symbol_workbench_demo_data_test monitor_symbol_workbench_view_test monitor_market_data_view_model_test monitor_market_data_store_test monitor_spsc_queue_test monitor_market_data_thread_test -j 8
ctest --test-dir build/debug -R monitor_ --output-on-failure
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
```

未启动 Gate / Binance data session 时，dump smoke 期望显示 `market data unavailable` / SHM unavailable alert，并保留 Gate / Binance 行情 `NA` 行。如果已启动 data session，dump snapshot 应能从 visible SHM 读取 bid / ask；`last_price`、volume、turnover 仍应显示 `NA`。

### Gate Trading

```bash
ctest --test-dir build/debug -R '(core_order_pool|strategy|gate_order|gate_submit|order_session_config|order_feedback)' --output-on-failure
ctest --test-dir build/debug -R '^(order_session_config_test|gate_order_latency_diagnostics_test|gate_order_session_runtime_adapter_test|gate_order_session_rtt_probe_test)$' --output-on-failure
./build/debug/tools/gate_strategy_order --contract BTC_USDT --side buy --order-type limit --size 1 --price 81000 --tif gtc
./build/debug/tools/gate_order_feedback_session --config config/order_feedback/gate_order_feedback_session.toml --duration-sec 0.1
./build/debug/tools/gate_demo_strategy --config config/strategies/demo_strategy.toml
```

真实 Gate live smoke 需要 `PROBE_KEY` / `PROBE_SECRET`、外网和显式 `--execute`；执行后必须用 REST 查询确认无残留 open orders / position。

### Gate OrderSession RTT probe

```bash
cmake --build build/debug --target gate_order_session_rtt_probe gate_order_session_rtt_probe_test
ctest --test-dir build/debug -R gate_order_session_rtt_probe --output-on-failure
./build/debug/tools/gate_order_session_rtt_probe --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml
./build/debug/tools/gate_order_session_rtt_probe --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml --live-preflight
```

connections CSV 当前默认读 `config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`；临时实验优先复制 CSV 到
`/home/liuxiang/tmp` 后用 `--connections-file` 覆盖。真实 Gate live smoke 仍需要 `PROBE_KEY` / `PROBE_SECRET`、外网、
明确 `--execute` 和运行前后 REST flat / open orders 复核。

### LeadLag

```bash
ctest --test-dir build/debug -R '(lead_lag|signal_csv_writer)' --output-on-failure
./build/debug/tools/lead_lag_replay --config config/strategies/lead_lag_ordi_replay.toml --signals-output /tmp/lead_lag_compare/tardis_signal.csv
scripts/lead_lag_replay_pnl.py /tmp/lead_lag_compare/tardis_signal.csv --slippage-ticks 0 --trades-output /tmp/lead_lag_compare/tardis_slip0.csv
```

### Evaluation 边界

修改 `evaluation/` 或相关 CMake 边界后运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望结果为空。

## 下一步建议

### Gate 交易

1. 读取 `docs/agent-handoff-gate-trade-architecture.md` 和 `docs/strategy_order_component_model.md`。
2. 从 `core/trading/trading_runtime.h`、`exchange/gate/trading/order_session_runtime_adapter.h`、`tools/gate/demo_strategy.h` 和 `tools/gate/demo_strategy.cpp` 接手。
3. failure protocol probe 已有独立工具，但安全 live 请求未拿到最终 failure response；继续前先基于 Gate 官方协议或 dedicated account 明确可返回最终 error 的请求形态。
4. V2 再评估 read-only reconcile / resume：未知订单状态、本地状态恢复、人工介入、新开仓暂停 / 恢复条件。
5. 接入 symbol metadata / risk check：启动期缓存合约元数据，submit 前校验 tick、quantity、notional、reduce-only 等。
6. 增加端到端 benchmark：覆盖 `TradingRuntime -> OrderSessionRuntimeAdapter -> OrderSession` 下单请求和 `OrderFeedbackSession -> SHM -> TradingRuntime` 回报消费。

### DataReader / 交易组件

1. 读取 `docs/strategy_order_component_model.md`、`docs/data_reader_config.md` 和 `docs/data_session_shm_communication_design.md`。
2. DataReader recorder live record smoke / replay 可读性验证已完成；recorder 已支持 `[recorder]` rotation、默认 1 小时切分和 manifest 生成 replay config。若继续 recorder，下一步可做更长时间 guarded recording、录制脚本化或与 LeadLag / demo 实盘并行录制验证。继续使用临时 `drain` 配置，输出到 `/home/liuxiang/tmp`，观察 `overruns` / `skipped` 和 CPU 抢占，不要把仓库默认 `strategy_data_reader.toml` 改成 drain。
3. 若继续比较 Gate data session 不同 private IP 的行情延迟，不能只看 recorder/replay `local_ns` 后再反推网络路径；`local_ns` 是 data session 接入时刻。需要像 order session probe 一样按连接记录 `host` / `connect_ip` / remote endpoint / owner CPU，并按 `BookTicker.exchange_ns -> data_session local_ns`、SHM publish / reader 侧时间和 `skipped` / `overruns` 分组统计。
4. 若继续 DataReader feed 扩展，优先讨论 trade / order book 的 typed storage + unified scan table。
5. 如果生产工具需要导出 runtime loop diagnostics，再在具体 tool / strategy runner 中选择启用 `TradingRuntimeDiagnostics` 并低频打印 stats。
6. 若继续下一个组件，建议按 `OrderSession`、`OrderFeedbackSession`、`OrderManager`、`Strategy` 的顺序继续讨论接口边界。

### LeadLag

1. 读取 `docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_reconcile_design.md` 和 `strategy/lead_lag/README.md`，确认当前 runner gating、live / replay 测试名、strategy 层订单闭环和 `ContinuityLost` handoff 边界。
2. 如继续 signal-only 长跑，可使用 `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` 观察 12 个 requested symbol；真实订单 `--execute` 使用 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`。若用户说 `lead_lag_live_replay_signal_parity <duration>`，直接按 `docs/lead_lag_live_replay_testing.md` 执行 live signal、DataReader binary record、replay 和 signal CSV 对比，所有产物写入 `/home/liuxiang/tmp/<run_id>`。decimal-size 合约的 C++ 下单 / 回报 / LeadLag sizing 已支持 1 位小数，Gate WS 下单带 decimal header 时已 quote JSON `size`，REST final check / emergency flatten 已完成 decimal residual 代码级修复；当前阶段不再把 decimal filled close 作为 active blocker。
3. V1 emergency smoke、外围 guard wrapper、ZEC 小额 filled open / close 和 unfilled-cancel smoke 已完成；submit rejected / cancel-rejected 安全 live 探测未通过，不计入完成项。
4. IOC partial-fill / decimal filled close 当前不再作为 active blocker；后续如果 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再恢复 targeted small smoke 复查。
5. 若继续 Ack latency 复核，按 `docs/lead_lag_live_operations_pipeline.md` 使用 affinity profile，并在 report 中同时区分 Ack RTT、send-to-finish 本地闭环和 exchange Ack-to-finish；`219ms` Ack RTT outlier 当前等待复现，根因只有在复现并结合 write path diagnostic / pidstat / perf sched 后才能下结论。`config/order_sessions/gate_order_session.toml` 已显式写出 Ack diagnostic 默认阈值；短期诊断可临时设 `ack_rtt_threshold_ns=0` 且调高 `max_logs_per_second` 来每 Ack 采样 write path / socket queue / runtime loop，但这会把 `AllowLog()`、日志格式化 / 入队和可选 `TCP_INFO` 采样带入 Ack 热路径，不作为长期生产默认。terminal lifecycle latency 先按 Gate 内部订单队列 / IOC lifecycle 候选假设标记，后续复现时单独分析，不并入 Ack path。若继续连接寻优，先读 `docs/gate_order_session_rtt_probe_design.md`，当前已有 V1a dry-run scaffold、sample flow / executor / id allocator / CSV writer 前置逻辑、`ioc` / `gtc` / `ioc+gtc` order mode、同 cycle order session 非阻塞 pacing、GTC cancel reject / feedback fill / timeout -> reduce-only close、close Ack 等待 terminal feedback 的纯状态流转和 `--live-preflight`，可先运行 `build/debug/tools/gate_order_session_rtt_probe --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml` 验证 candidate IP / cycle plan；下一步先接入 feedback reader、REST guard 和 single-session live order sample，不自动 score / 切换。2026-05-29 0-tick run 的 `504 INTERNAL Request Timeout` 是 Gate final response 阶段服务端内部处理 timeout，Ack 很快，不能当作本地发送路径慢。
6. 之后再做更长时间真实订单运行 guardrails 和继续补齐风控审查；当前已先加入 LeadLag strategy 全局 `max_gross_notional`，`max_holding_position` 可选但本版本暂不启用。account / position realtime feedback 作为 V2 可选能力，不作为当前 V1 前置项。failure response 继续前先确认 Gate 可返回最终 error 的请求形态。

### TUI / Account Monitor

1. 读取 `docs/tui_onboarding_guide.md` 和 `docs/tui_gate_account_monitor_design.md`，以当前 `monitor/` 实现为边界。
2. 下一步优先实现 monitor 专用 Gate orders raw parser 和 fixture tests；不要直接复用交易系统 `OrderFeedbackEvent` 作为 TUI 主事件。
3. 实现启动期 REST snapshot：open orders、positions、account summary；运行期先做 drift 标记，不自动修正或交易。
4. 实现 `MonitorOrderBook`、`PositionLedger`、`PnlLedger` 和真实 `AccountMonitorThread`，通过 SPSC 向 UI thread 发布 order / health batch。
5. 后续如果接 order pool SHM，应保留 `OrderSource` 可替换边界，使 UI model 不依赖具体来源。
6. market data 若要展示 `last_price`、成交量或 turnover，需要新增 trade / ticker SHM 或低频 REST ticker；补齐前继续显示 `NA`，不要用 bid / ask 伪造。

## 结束对话固定流程

用户输入“结束对话”时，只做收尾、同步和交接，不主动开启新功能：

1. 运行 `git status --short --branch` 和 `git log --oneline -8`。
2. 对照当前实现、配置和最近提交，更新相关文档，重点维护本 onboarding 的当前状态、入口、验证命令和下一步建议。
3. 如果触碰 evaluation、data session config、WebSocket、Gate / Binance handoff 或 README，同步对应文档。
4. 更新“给下一个对话的 onboarding 提示”。
5. 至少运行 `git diff --check`；如触碰 evaluation 边界，再运行 evaluation 边界检查。
6. 自动提交文档整理，commit message 使用英文。
7. 提交成功后自动 push 当前分支到其 configured upstream / 默认远端；如果 push 失败，最终回复必须说明失败原因和当前 ahead/behind 状态。
8. 最终回复给出提交哈希、push 结果、验证结果，并贴出下一轮 onboarding 提示段落。

## 给下一个对话的 onboarding 提示

请先在 `/home/liuxiang/dev/aquila` 运行 `git status --short --branch` 和 `git log --oneline -8`，再读 `AGENTS.md`、`README.md`、`docs/project_onboarding_guide.md`、`docs/evaluation_support.md`。以 onboarding 当前事实源、代码入口、重要结论和下一步建议为准；当前 branch / ahead / dirty 状态只信 `git status`。公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`，Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h`。

DataReader / data session：先读 `docs/data_reader_config.md`、`docs/data_session_config.md` 和 `docs/data_session_shm_communication_design.md`。`data_reader_recorder` 已能用 `RealtimeDataReader::Drain()` 从 Gate / Binance `BookTicker` SHM 写 merged replay binary，支持 rotation + manifest replay config；recorder 是只读 SHM consumer，可和 LeadLag / demo 实盘并行，完整 dump 使用临时 `drain` 配置并观察 `overruns` / `skipped` / CPU 抢占。若继续比较 Gate data session 不同 private IP 的行情延迟，注意 recorder/replay 中的 `local_ns` 是 data session 接入时刻；需要按 data session 连接记录 `host` / `connect_ip` / remote endpoint / owner CPU，并按 `BookTicker.exchange_ns -> local_ns`、SHM publish / reader 侧时间和 `skipped` / `overruns` 分组统计。

TUI：先读 `docs/tui_onboarding_guide.md` 和 `docs/tui_gate_account_monitor_design.md`。当前 `gate_account_tui --live-market-data` 只读现有 Gate / Binance `BookTicker` SHM；订单、仓位、PnL 和 health 仍未接真实账户数据，下一步是 monitor 专用 Gate orders raw parser、REST snapshot 和 account model。

LeadLag：先读 `docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_operations_pipeline.md`、`docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_runtime_latency_improvement_plan.md`、`docs/gate_order_session_rtt_probe_design.md`、`docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_reconcile_design.md` 和 `docs/diagnostic_fields.md`。真实订单 `--execute` 默认用 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`；仓库默认 12-symbol live 配置仍是 `open_slippage=2` / `close_slippage=2` ticks。2026-05-29 已提交 `763b73b Add live order latency diagnostic fields`，新增 Gate 下单写路径分段、socket send queue 和 LeadLag BBO-to-signal / signal-to-order 字段；2026-05-30 已提交 `fcf452c Guard LeadLag report timing against cross clocks`，report / CSV 中跨时钟域的 `bbo_to_strategy_ns` / `trigger_to_request_send_ns` 会置空并写 warning。2026-05-29 / 2026-05-30 live run 中 Gate data / order / feedback 都用 private non-TLS `fxws-private.gateapi.io:80`，Binance data 仍是 public TLS；2026-05-30 4 小时 2-tick run `20260530_071328_12pair_live_2ticks_private_4h` 正常 flat，`signals=62`、entry submitted `58`、closed positions `4`，net PnL `-0.1383270504`，no-slip net `-0.0475420504`，Ack RTT max `13.283ms`，低于默认 `20ms` diagnostic 阈值，最大 send-to-finish `120.623ms` / exchange lifecycle `114.832ms` 属于 terminal lifecycle tail。Gate Ack diagnostic 已配置化：`ack_rtt_threshold_ns` 默认 `20000000`，短期诊断可设 `0` 做每 Ack 采样并调高 `max_logs_per_second`，但会把限流判断、日志和可选 `TCP_INFO` 采样带入 Ack 热路径，不作为长期生产默认。Ack RTT outlier 当前等待复现后再结合 write diagnostic / 调度证据分析；`gate_order_session_rtt_probe` 连接列表已迁到 CSV，默认 12 行配置在 `config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`，可用 `--connections-file` 覆盖并允许重复 `connect_ip`，不自动 score / 切换。
