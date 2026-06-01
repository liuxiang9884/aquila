# aquila 新对话导读

这份文档是新对话接手入口，只保留当前事实、入口、验证命令和下一步。历史推导、完整 run 输出和字段细节放在专题文档中；当前 branch、ahead 和 dirty 状态只信 `git status`。

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
| WebSocket / socket timestamping | `docs/diagnostic_fields.md`、`docs/gate_order_session_rtt_probe_design.md` |
| Gate 交易架构 | `docs/agent-handoff-gate-trade-architecture.md`、`docs/strategy_order_component_model.md` |
| Gate Ack latency / RTT probe | `docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_runtime_latency_improvement_plan.md`、`docs/gate_order_session_rtt_probe_design.md`、`docs/diagnostic_fields.md` |
| LeadLag live / replay | `docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_operations_pipeline.md`、`docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_reconcile_design.md`、`strategy/lead_lag/README.md` |
| DataReader / data session | `docs/data_reader_config.md`、`docs/data_session_config.md`、`docs/data_session_shm_communication_design.md` |
| TUI / account monitor | `docs/tui_onboarding_guide.md`、`docs/tui_gate_account_monitor_design.md` |
| Binance 行情 | `docs/agent-handoff-binance-market-data.md` |
| Evaluation 边界 | `docs/evaluation_support.md` |

## 当前状态

- 项目是 C++20 / CMake 的 crypto 低延迟交易系统，优先级是正确性、确定性、低延迟和可观测性；性能结论必须有 benchmark、profile 或 live probe 证据。
- 公共 order / runtime contract 已迁到 `core/trading/*` + `aquila::core`；Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h`。
- Gate / Binance 行情、data session / SHM、strategy `DataReader`、Gate submit/cancel、order feedback SHM、Gate private `futures.orders` feedback、`OrderManager`、`TradingRuntime`、Gate runtime adapter、`demo` 策略 live smoke、LeadLag replay / live-orders、TUI Symbol Workbench / market data monitor demo 都已落地。
- TUI 当前仍是只读 monitor demo：market data 可从现有 Gate / Binance `BookTicker` SHM 读取并降级显示 `NA`；订单、仓位、PnL 和 health 还未接真实账户数据。
- LeadLag strategy 层生产订单闭环已完成；`lead_lag_strategy --execute` 已接真实 live-orders runtime，并在 `ContinuityLost` 后停止、返回 handoff exit code。外围 `run_live_with_guard.py` 负责 preflight、final REST check 和异常 stop-and-flat。
- 当前不新增独立 `AccountPositionFeedbackSession`；account / position realtime feedback 作为 V2 可选能力。
- IOC partial-fill / decimal filled close 按 2026-05-27 决策不再作为当前阶段 active blocker；若 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再按具体问题复查。
- 2026-05-31 已完成 onboarding 和主要专题文档精简；`docs/project_onboarding_guide.md` 只保留事实源、入口、验证命令和下一步，细节继续放在各专题文档。

## 近期重要事实

### LeadLag Live

- 真实订单默认配置：`config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`。
- 仓库默认 12-symbol live 配置仍是 `open_slippage=2` / `close_slippage=2` ticks；2026-05-29 的 3-tick / 0-tick run 只通过 `/home/liuxiang/tmp/<run_id>/configs` 临时覆盖。
- 2026-05-29 / 2026-05-30 live run 中 Gate data / order / feedback 使用 private non-TLS `fxws-private.gateapi.io:80`，Binance data 仍使用 public TLS。
- 2026-05-30 4 小时 2-tick run `20260530_071328_12pair_live_2ticks_private_4h` 正常 flat：`signals=62`、entry submitted `58`、closed positions `4`，net PnL `-0.1383270504`，no-slip net `-0.0475420504`。
- 同一 run 最大 Ack RTT `13.283ms`，低于默认 `20ms` diagnostic 阈值；最大 send-to-finish `120.623ms` / exchange lifecycle `114.832ms` 属于 terminal lifecycle tail，不是 Ack path outlier。
- 2026-05-30 report / CSV 已防止跨时钟域误算：`bbo_to_strategy_ns` / `trigger_to_request_send_ns` 跨时钟域时置空并写 warning。

### Ack Latency / Socket Timestamping

- 2026-05-25 `219.023ms` Ack RTT outlier 仍等待复现；单次样本不能证明根因。复现后需要结合 write path diagnostic、runtime loop / 调度证据、endpoint / CPU / TCP_INFO 和多连接对照分析。
- Ack latency diagnostic 已配置化：默认 `ack_rtt_threshold_ns=20000000`；短期诊断可设 `0` 做每 Ack 采样并调高 `max_logs_per_second`，但会把限流判断、日志和可选 `TCP_INFO` 采样带入 Ack 热路径，不作为长期生产默认。
- 2026-05-31 WebSocket / Gate order socket timestamping attribution 已落地。默认编译 ON；`AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION=OFF` 构建时 apply、errqueue drain 和 probe 匹配为 no-op。
- `CriticalSession` 按 `request_sequence` 维护 active probe，用 Linux `SOF_TIMESTAMPING_OPT_ID_TCP` byte-stream id 把 TX event 匹配到 request 写入范围；control frame 只推进全局 kernel id，不扩展订单范围。
- 只有 runtime config 开启且 private plain transport 成功 apply `SO_TIMESTAMPING` 后才启动 probe；TLS、apply 失败、取消写、encode failure、commit failure、partial / 未完整 write complete 都会让 `ts_available=false` 或释放 probe，避免错配。
- Socket timestamping 可把 Ack RTT 拆到 `write_complete -> tx_software -> tx_ack -> rx_software -> ack_receive` software-level 大段。`ts_available=false` 表示缺归因，不要把 0 当真实时间。
- 当前主要是 software timestamping，不能严格证明 packet leaves / returns NIC；RX software timestamp 是 `recvmsg()` 粒度，多 Ack 合并时可能共享同一个 RX 时间戳。若要确认 NIC / 网络 / Gate 侧边界，需要硬件 timestamp 字段或 pcap。
- 2026-06-01 8 条 private plain / no TLS RTT probe 半小时测试未复现 `219ms` outlier；`1798` 个 Ack 中 p50 `0.613ms`、p95 `0.842ms`、p99 `2.632ms`、max `18.709ms`，`invalid=0`、`fill=0`、结束 REST flat / no open orders。Top tail 集中在 `write_complete_ns -> ts_rx_software_ns`，本机 write、runtime loop、DriveRead、RX 后处理和 TCP retrans / notsent backlog 未显示异常。

### Gate OrderSession RTT Probe

- `gate_order_session_rtt_probe` 是 measurement-only 工具，不自动 score / 切换连接。
- 连接维度已迁到 CSV：`name,group,host,connect_ip,port,enable_tls,worker_cpu_id`。行顺序就是 session 顺序；`name` 唯一；`connect_ip` 可重复，表示多条独立连接。
- 默认 12 行连接列表：`config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`。可用 `--connections-file` 覆盖。
- 8 条 private plain 全阶段配置：`config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml`，连接列表为 `config/order_session_rtt_probe/gate_order_session_rtt_private8_plain_connections.csv`，base order session config 为 `config/order_sessions/gate_order_session_rtt_probe_allstage.toml`。
- `cycle_cooldown_us` / `order_session_interval_us` 已支持微秒粒度 pacing；旧 `*_ms` 字段仍按毫秒解析。
- sample CSV 可写 Ack diagnostic window 快照：runtime hook / DriveRead、write path、socket send queue、`TCP_INFO`、socket timestamping 分段字段。`ack_rtt_threshold_ns=0` 时每 Ack 写入 sample CSV，不依赖 diagnostic log 是否被 `max_logs_per_second` 限流。
- `--execute` 当前支持 `order_mode="ioc"` 的 single-session / multi-session live sample，并已接 feedback SHM reader；REST preflight / run-end guard 仍未在工具内真正执行，真实测试前后需要外部 REST / 人工确认 flat。
- 下一步：补工具内 REST guard，复核 IOC execute 的 failure / invalid 语义，然后再启用 `gtc` / `ioc+gtc` live execute 和离线分布 / top tail 阶段拆解脚本。

### DataReader / Recorder

- `RealtimeDataReader::Poll(handler)` 是单事件接口，`Drain(handler, max_events)` 是批量接口；多 source round-robin 不做全局时间排序。
- `HistoricalDataReader` mmap 非空 binary 文件，额外提供 `finished()`。
- `data_reader_recorder` 可从 Gate / Binance `BookTicker` SHM 写 merged replay binary，支持 rotation + manifest replay config。
- recorder 是只读 SHM consumer，可和 LeadLag / demo 实盘并行；完整 dump 使用临时 `drain` 配置，观察 `overruns` / `skipped` 和 CPU 抢占，不要把仓库默认 reader config 改成 drain。
- 若比较 Gate data session 不同 private IP 的行情延迟，不能只看 recorder/replay `local_ns`。`local_ns` 是 data session 接入时刻；需要按 data session 连接记录 `host` / `connect_ip` / remote endpoint / owner CPU，并按 `BookTicker.exchange_ns -> local_ns`、SHM publish / reader 侧时间、`skipped` / `overruns` 分组统计。

### Gate 交易

- Gate `OrderSession` submit/cancel 主路径、login HMAC-SHA512、固定缓冲区 request encoder、response correlation、同步 `OrderResponse` 回调和 benchmark 已落地。
- C++ trading quantity contract 已改为 `double quantity` + `quantity_text`。Gate WebSocket 下单带 `X-Gate-Size-Decimal: 1` 时 JSON `size` 编码为 string；decimal-size REST final check / emergency flatten 已支持 decimal size 与 `value` / `margin` residual。
- `OrderFeedbackSession` 使用 private `futures.orders`；`OrderManager::OnOrderFeedback()` 已处理 accepted、partial filled、filled、cancelled / terminal、rejected 和 continuity lost。
- 尚未完成：REST reconcile、feedback WS 断线未知订单恢复、batch/amend/cancel-all。account / position realtime feedback 可作为 V2 风控能力评估，不是当前 LeadLag V1 前置项。

### TUI / Account Monitor

- `monitor/` 是独立顶层目录，依赖方向为 `monitor/* -> core/config/exchange`；生产交易链路不反向依赖 `monitor/`。
- `gate_account_tui` 支持 interactive TUI、`--dump`、`--view health`、`--live-market-data` 和 `--market-data-config`。
- market data 只读既有 Gate / Binance data session SHM；SHM 缺失是可见降级状态，不自动启动 data session。
- 当前 `BookTicker` 不含 `last_price`、成交量、turnover / value，这些字段在 TUI 中显示 `NA`。
- 后续 order / position / PnL 需要 monitor 专用 raw event、REST snapshot、account model 和 health sampler。

## 代码入口

### WebSocket / Diagnostics

| 文件 | 职责 |
| --- | --- |
| `core/websocket/critical_session.h` | hot-path read/write pump、prepared write、socket timestamping probe 生命周期和 TX/RX 归因 |
| `core/websocket/socket_timestamping.h` | Linux `SO_TIMESTAMPING` apply、errqueue drain、stage duration 计算和 compile-time attribution 开关 |
| `core/websocket/plain_socket.h` | private non-TLS socket、handshake 后 apply socket timestamping、`recvmsg()` RX timestamp |
| `core/websocket/cold_path_loop.h` | TCP/TLS/WebSocket cold path；plain transport 在 WebSocket handshake 完成后启用 timestamping |
| `core/websocket/prepared_write.h` | fixed-capacity prepared write slot；attribution ON 时保存 request sequence / probe slot |
| `docs/diagnostic_fields.md` | 诊断字段、log key、CSV/report 字段语义和删除条件 |

### Trading / Gate

| 文件 | 职责 |
| --- | --- |
| `core/trading/order_types.h` | 公共订单请求、订单对象、response / feedback event 类型 |
| `core/trading/order_manager.h` | 模板化 `OrderManager<OrderSessionT>` |
| `core/trading/trading_runtime.h` | production runtime |
| `core/trading/order_feedback_shm.h` | 8 lane feedback SHM transport |
| `exchange/gate/trading/order_session.h` | Gate submit/cancel WebSocket session |
| `exchange/gate/trading/order_latency_diagnostics.h` | Gate Ack latency diagnostic window、阈值和 per-session log rate limit |
| `exchange/gate/trading/order_session_config.*` | Gate order session TOML parser；解析 TCP_INFO、Ack diagnostic 和 timestamping config |
| `exchange/gate/trading/order_session_runtime_adapter.h` | Gate runtime adapter |
| `exchange/gate/trading/order_feedback_parser.h` | Gate private `futures.orders` parser |
| `exchange/gate/trading/order_feedback_session.h` | Gate feedback session |
| `tools/gate/order_session_rtt_probe/` | Gate `OrderSession` RTT probe V1a |

### Market Data / DataReader

| 文件 | 职责 |
| --- | --- |
| `core/market_data/types.h` | 统一 `BookTicker` |
| `core/market_data/realtime_data_reader.h` | Strategy 侧 realtime SHM reader |
| `core/market_data/historical_data_reader.h` | Binary replay reader |
| `core/market_data/data_shm.h` | `DataShmPublisher` 和 `BookTickerShmReader` |
| `tools/market_data/data_reader_recorder.*` | SHM 到 replay binary recorder |
| `exchange/gate/market_data/*` | Gate SBE BBO client / session / config |
| `exchange/binance/market_data/*` | Binance bookTicker stream / parser / client / session / config |

### LeadLag / Monitor

| 文件 | 职责 |
| --- | --- |
| `strategy/lead_lag/strategy.h` | LeadLag replay 信号主链路 |
| `strategy/lead_lag/config.*` | LeadLag config parser / loader |
| `tools/lead_lag/replay.cpp` | binary replay 输出 signal CSV |
| `scripts/lead_lag/analyze_order_detail.py` | live report / order detail / latency CSV 分析 |
| `monitor/tui/gate_account_tui.cpp` | TUI 入口 |
| `monitor/market_data/market_data_thread.*` | monitor 专用 market data reader thread 和 one-shot snapshot |

## 常用验证命令

### 基础

```bash
./build.sh debug
ctest --test-dir build/debug --output-on-failure
git diff --check
```

### WebSocket / Socket Timestamping / Gate RTT Probe

```bash
cmake --build build/debug --target websocket_critical_session_test websocket_socket_timestamping_test websocket_plain_socket_test gate_order_session_test gate_order_session_rtt_probe_test order_session_config_test
ctest --test-dir build/debug -R 'websocket_(critical_session|socket_timestamping|plain_socket)_test|order_session_config_test|gate_order_session_rtt_probe_test|gate_order_session_test' --output-on-failure
cmake --build /home/liuxiang/tmp/aquila-build-no-ts --target websocket_critical_session_test websocket_socket_timestamping_test websocket_plain_socket_test gate_order_session_test gate_order_session_rtt_probe_test order_session_config_test
ctest --test-dir /home/liuxiang/tmp/aquila-build-no-ts -R 'websocket_(critical_session|socket_timestamping|plain_socket)_test|order_session_config_test|gate_order_session_rtt_probe_test|gate_order_session_test' --output-on-failure
./build/debug/tools/gate_order_session_rtt_probe --config config/order_session_rtt_probe/gate_order_session_rtt_probe.toml --live-preflight
./build/debug/tools/gate_order_session_rtt_probe --config config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml --live-preflight
```

### Gate / Binance 行情和 DataReader

```bash
ctest --test-dir build/debug -R '(gate_.*market_data|binance_.*market_data|data_session_config|data_reader_config|core_market_data|data_reader_recorder)' --output-on-failure
./build/debug/tools/gate_data_session
./build/debug/tools/binance_data_session
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1 --log-every 1
./build/debug/tools/data_reader_recorder --config /home/liuxiang/tmp/aquila_strategy_data_reader_drain.toml --output /home/liuxiang/tmp/live_merged_book_ticker.bin --mode truncate
```

### Gate Trading / LeadLag / TUI

```bash
ctest --test-dir build/debug -R '(core_order_pool|strategy|gate_order|gate_submit|order_session_config|order_feedback|lead_lag|signal_csv_writer)' --output-on-failure
ctest --test-dir build/debug -R monitor_ --output-on-failure
./build/debug/monitor/gate_account_tui --dump --live-market-data --width 260 --height 60
./build/debug/tools/lead_lag_replay --config config/strategies/lead_lag_ordi_replay.toml --signals-output /home/liuxiang/tmp/lead_lag_compare/tardis_signal.csv
```

真实 Gate / LeadLag live smoke 需要 `PROBE_KEY` / `PROBE_SECRET`、外网、显式 `--execute`，并在运行前后用 REST 确认 flat / no open orders。

### Evaluation 边界

修改 `evaluation/` 或相关 CMake 边界后运行：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望无命中。

## 下一步建议

### Ack Latency / RTT Probe

1. 先读 `docs/gate_order_session_rtt_probe_design.md` 和 `docs/diagnostic_fields.md`。
2. 若要复现 Ack RTT outlier，使用 private plain all-stage config，并临时设 `ack_rtt_threshold_ns=0`、合适的 `max_logs_per_second`；同时记录 endpoint、owner CPU、send CPU、ack CPU、diagnostic CPU、TCP_INFO 和 socket timestamping 字段。
3. 分析时区分 Ack RTT、send-to-finish 本地闭环和 exchange Ack-to-finish；terminal lifecycle tail 不并入 Ack path。
4. 对 `10ms-20ms` 级 tail，优先看 `write_complete_ns -> ts_rx_software_ns`；若本机 write / runtime / RX 后处理仍是几十微秒，不要归因到 owner thread。
5. 若要确认是否发生在本机 NIC，需要补 hardware timestamp 或对 TCP flow 做 pcap；software timestamping 只能做大段归因。
6. RTT probe 当前只做 measurement，不自动 score / 切换；下一步先补工具内 REST guard 和离线分布 / top tail 阶段拆解脚本，再考虑 `gtc` / `ioc+gtc` live execute。

### LeadLag

1. 先读 `docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_operations_pipeline.md`、`docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_reconcile_design.md` 和 `strategy/lead_lag/README.md`。
2. signal-only 用 `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml`；真实订单 `--execute` 用 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`。
3. 如果用户说 `lead_lag_live_replay_signal_parity <duration>`，按 `docs/lead_lag_live_replay_testing.md` 执行 live signal、DataReader binary record、replay 和 signal CSV 对比，产物写入 `/home/liuxiang/tmp/<run_id>`。
4. IOC partial-fill / decimal filled close 当前不是 active blocker；后续如再次异常，再做 targeted small smoke。
5. account / position realtime feedback 作为 V2 可选能力；V1 继续依赖当前 guardrails 和 REST final check。

### DataReader / Data Session

1. 先读 `docs/data_reader_config.md`、`docs/data_session_config.md` 和 `docs/data_session_shm_communication_design.md`。
2. 长时间 recorder 或实盘并行录制使用临时 `drain` 配置，输出到 `/home/liuxiang/tmp`，观察 `overruns` / `skipped` 和 CPU 抢占。
3. 比较不同 private IP 行情延迟时，必须按 data session 连接记录 endpoint / owner CPU，并按 exchange timestamp、data session ingress、SHM publish / reader 时间分组统计。

### Gate Trading / TUI

1. Gate 交易继续前读 `docs/agent-handoff-gate-trade-architecture.md` 和 `docs/strategy_order_component_model.md`。
2. Gate failure protocol probe 继续前，先基于 Gate 官方协议或 dedicated account 明确可返回最终 error 的请求形态。
3. TUI 下一步优先做 monitor 专用 Gate orders raw parser、REST snapshot、account model 和真实 health sampler；不要把交易系统 `OrderFeedbackEvent` 直接当作 TUI 主事件。

## 给下一个对话的 onboarding 提示

请先在 `/home/liuxiang/dev/aquila` 运行 `git status --short --branch` 和 `git log --oneline -8`，再读 `AGENTS.md`、`README.md`、`docs/project_onboarding_guide.md`、`docs/evaluation_support.md`。当前 branch / ahead / dirty 状态只信 `git status`；公共 order / runtime contract 在 `core/trading/*` + `aquila::core`，Gate runtime adapter 在 `exchange/gate/trading/order_session_runtime_adapter.h`。2026-05-31 已提交 onboarding 和主要专题文档精简，优先把本文件当作入口索引，细节再读对应专题文档。

DataReader / data session：先读 `docs/data_reader_config.md`、`docs/data_session_config.md` 和 `docs/data_session_shm_communication_design.md`。`data_reader_recorder` 已能用 `RealtimeDataReader::Drain()` 从 Gate / Binance `BookTicker` SHM 写 merged replay binary，支持 rotation + manifest replay config；完整 dump 使用临时 `drain` 配置并观察 `overruns` / `skipped` / CPU 抢占。比较 Gate data session 不同 private IP 行情延迟时，`local_ns` 是 data session 接入时刻，需要按连接记录 endpoint / owner CPU，并按 `BookTicker.exchange_ns -> local_ns`、SHM publish / reader 侧时间和 `skipped` / `overruns` 分组统计。

TUI：先读 `docs/tui_onboarding_guide.md` 和 `docs/tui_gate_account_monitor_design.md`。当前 `gate_account_tui --live-market-data` 只读现有 Gate / Binance `BookTicker` SHM；订单、仓位、PnL 和 health 还未接真实账户数据，下一步是 monitor 专用 Gate orders raw parser、REST snapshot 和 account model。

LeadLag / Gate Ack latency：先读 `docs/lead_lag_live_runtime_plan.md`、`docs/lead_lag_live_operations_pipeline.md`、`docs/lead_lag_ack_latency_outlier_analysis.md`、`docs/lead_lag_runtime_latency_improvement_plan.md`、`docs/gate_order_session_rtt_probe_design.md`、`docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_reconcile_design.md` 和 `docs/diagnostic_fields.md`。真实订单 `--execute` 默认用 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`；仓库默认 12-symbol live 配置仍是 `open_slippage=2` / `close_slippage=2` ticks。2026-05-31 已提交 `629629f Fix websocket timestamp probe lifecycle`，WebSocket socket timestamping attribution 默认编译 ON，OFF build 为 no-op；只有 private plain transport 成功 apply `SO_TIMESTAMPING` 后才启动 probe，取消 / 失败发送 / 未完整 write complete 会释放或丢弃 probe。Socket timestamping 字段可把 Ack RTT 拆到 `write_complete -> tx_software -> tx_ack -> rx_software -> ack_receive` software-level 大段；`ts_available=false` 表示缺归因，不要把 0 当真实时间。2026-06-01 8 条 private plain / no TLS RTT probe 半小时测试未复现 `219ms` outlier，`1798` 个 Ack 中 max `18.709ms`，top tail 主要在 `write_complete_ns -> ts_rx_software_ns`，不支持本机 owner thread / read parse / write queue 积压作为主要原因。若要确认 outlier 是否发生在本机 NIC，需要硬件 timestamp 或 pcap。`gate_order_session_rtt_probe` 连接列表在 CSV，默认 12 行配置是 `config/order_session_rtt_probe/gate_order_session_rtt_connections.csv`，8 条 private plain 全阶段配置是 `config/order_session_rtt_probe/gate_order_session_rtt_probe_private8_plain_allstage.toml`，可用 `--connections-file` 覆盖并允许重复 `connect_ip`，不自动 score / 切换。
