# Aquila 诊断字段索引

## 目的

本文登记为诊断、延迟分析、运行报告和临时排障新增的字段。它不是完整 report schema；完整 LeadLag CSV
字段说明仍以 `docs/lead_lag_live_report_csv_schema.md` 为准。

维护目标：

- 新增、重命名或删除诊断字段、log key、stats 字段、report CSV 字段时，同步更新本文。
- 临时字段必须写清用途和删除条件，避免长期遗留无归属字段。
- 字段说明按 Aquila 组件分 section，便于后续用字段名反查代码入口和删除依据。
- 性能结论仍必须基于实际测试、benchmark、profile 或 live run 证据，字段本身只提供观测面。

## 字段生命周期

| 生命周期 | 含义 | 删除或保留规则 |
| --- | --- | --- |
| `stable` | 长期可观测性字段，报告、脚本或 runbook 已依赖。 | 删除前必须同步更新 parser、report、文档和历史兼容说明。 |
| `experiment` | 为当前实验或诊断面加入，预计会被验证或调整。 | 实验结束后改为 `stable` 或删除，并更新本文。 |
| `temporary` | 为复现某个问题加入的短期字段。 | 问题关闭或替代观测面落地后删除；删除时在 commit / report 中说明。 |
| `planned` | 已确认需要，但代码尚未落地。 | 实现时把状态改成实际生命周期。 |

## Gate OrderSession

组件入口：

- `exchange/gate/trading/order_session.h`
- `exchange/gate/trading/order_latency_diagnostics.h`
- `exchange/gate/trading/order_session_runtime_adapter.h`
- `tools/gate/strategy_order.cpp`
- `tools/gate/order_session_failure_probe.cpp`

### 连接级字段

这些字段用于区分多条 Gate `OrderSession` / WebSocket 连接的实际路径。连接 active 时低频输出在
`gate_order_session_connected` 中；DNS `resolved_ips` 仍等待 WebSocket 层暴露稳定 resolver 快照。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `order_session_id` | `gate_order_session_connected` / `gate_order_send_ok` / `gate_order_response` / `gate_order_ack_latency_diagnostic` | experiment | 本进程内单调 id | 关联同一条 `OrderSession` 的 send、ack、diagnostic 和 summary。 | 多 session 诊断停止且所有下游 parser 不再依赖。 |
| `endpoint_available` | `gate_order_session_connected` | experiment | `true` / `false` | 表示本地 / 远端 endpoint snapshot 是否可用；不可用时 endpoint 字段为空或 0。 | endpoint snapshot 不再使用时删除。 |
| `local_ip` | `gate_order_session_connected` / report CSV | experiment | IP 文本 | 记录本地 TCP endpoint，用于区分 NAT / source address。 | 被等价 endpoint snapshot 取代。 |
| `local_port` | `gate_order_session_connected` / report CSV | experiment | TCP port | 区分同 remote endpoint 下的不同连接。 | 被等价 endpoint snapshot 取代。 |
| `remote_ip` | `gate_order_session_connected` / report CSV | experiment | IP 文本 | 判断慢 session 是否落到不同 remote IP / gateway。 | 被等价 endpoint snapshot 取代。 |
| `remote_port` | `gate_order_session_connected` / report CSV | experiment | TCP port | 记录远端 TCP endpoint。 | 被等价 endpoint snapshot 取代。 |
| `owner_thread_cpu` | `gate_order_session_connected` | experiment | Linux CPU id，失败为 `-1` | 确认 session 进入 active 时 owner thread 实际运行 CPU。 | 被更完整 thread affinity / sched snapshot 取代。 |
| `resolved_ips` | `gate_order_session_connected` | planned | DNS 结果列表 | 区分 hostname 相同但 DNS / connect 目标不同的情况。 | WebSocket 层无法稳定提供时可留空；若无消费者可删除。 |

### IP discovery JSONL 字段

这些字段由 `scripts/gate/discover_gate_ws_ips.py` 写入 `candidate_ips.jsonl`，用于为后续 OrderSession RTT probe
准备候选 `connect_ip`。脚本不登录、不下单，也不修改本机 resolver 配置。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `sources` | `candidate_ips.jsonl` | experiment | `dns_system`、`dns_udp_<ip>_<port>`、`history_log`、`candidate_file` 等 | 记录候选 IP 来自 system resolver、explicit UDP resolver、历史连接日志或既有候选文件。 | IP discovery schema 升级并迁移消费者后删除或重命名。 |
| `dns.resolvers` | `candidate_ips.jsonl` | experiment | resolver label 列表，例如 `system`、`1.1.1.1:53` | 记录该 IP 被哪些 resolver 返回。 | 同上。 |
| `dns.resolver_details` | `candidate_ips.jsonl` | experiment | `{kind,address,port,label}` 对象列表 | 区分 system resolver 与 explicit UDP resolver，便于比较不同 DNS 来源的候选集。 | 同上。 |
| `websocket_verify` | `candidate_ips.jsonl` | experiment | `{attempted,ok,remote_ip,local_port,tcp_connect_ns,tls_handshake_ns,websocket_handshake_ns,total_ns,http_status,error}` | 记录 pinned TCP / TLS / WebSocket handshake 验证结果；不代表 private login 成功。 | 同上。 |
| `login_verify` | `candidate_ips.jsonl` | experiment | `{attempted,ok,latency_ns,status,uid,request_id,conn_id,conn_trace_id,error}` | 启用 `--verify-login` 时记录 `futures.login` 验证结果；只有 login 成功的 IP 会写入 `candidate_ips.txt`。 | 同上。 |

### OrderSession RTT probe CSV 字段

这些字段由第一版 `gate_order_session_rtt_probe` 的 sample CSV schema / writer 定义。CSV 采用长表，每行对应一个已提交
order action：GTC open、GTC cancel、可选 GTC safety close、IOC open 或 IOC safety close。当前字段集只保留 RTT
分析必须字段和建议诊断字段，并使用短字段名降低 CSV 宽度；连接级 endpoint / owner CPU 信息不重复写入每行 CSV，后续计划使用
`gate_order_session_rtt_probe_connection` Nova 结构化 log 记录，当前 connection log 尚未落地。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `run` | `order_session_rtt_samples.csv` | experiment | 文本 | 关联同一次 RTT probe run 的连接 log 与 sample CSV。 | RTT probe schema 升级并迁移消费者后重审。 |
| `ip` | `order_session_rtt_samples.csv` | experiment | IP 文本 | 被测 Gate TCP 直连 IP，是分组统计主 key。 | 同上。 |
| `sid` | `order_session_rtt_samples.csv` | experiment | 本进程内单调 id | 关联 sample、连接 endpoint 和底层 order session log。 | 同上。 |
| `connection_generation` | planned connection log | planned | 同一 `connect_ip` 内从 0 递增 | 区分同一个指定 IP 断开重连前后的 `OrderSession`，用于 reconnect RTT 对比。 | 同上。 |
| `connected_at_ns` | planned connection log | planned | 本机 Unix epoch ns | 记录该 generation 建连完成时间。 | 同上。 |
| `round` / `sample` | `order_session_rtt_samples.csv` | experiment | 0-based integer | 支持 round-robin 采样顺序分析，避免按 IP 连续采样造成时间窗口偏差。 | 同上。 |
| `contract` | `order_session_rtt_samples.csv` | experiment | Gate contract，例如 `ZEC_USDT` | 标记本次行情触发 cycle 的交易合约；第一版由 Gate `BookTicker` 行情事件决定，不固定只测一个 symbol。 | 同上。 |
| `qty` | `order_session_rtt_samples.csv` | experiment | Gate wire 文本 | 复核 instrument catalog 最小下单量是否符合预期。 | 同上。 |
| `price` | `order_session_rtt_samples.csv` | experiment | Gate wire 文本 | 当前 action 使用的价格；`open` / `cancel` 行使用对应 open price，`close` 行使用 close order price。 | 同上。 |
| `type` | `order_session_rtt_samples.csv` | experiment | `gtc` / `ioc` | 标记该行归属的 probe leg；`gtc,close` 表示为 GTC leg 触发的 safety close，即使 wire TIF 为 IOC。 | 同上。 |
| `action` | `order_session_rtt_samples.csv` | experiment | `open` / `cancel` / `close` | 将旧的 place/cancel/close 阶段规范成长表 action；place 统一写作 `open`。 | 同上。 |
| `local_id` / `req_seq` | `order_session_rtt_samples.csv` | experiment | id / request sequence | 直接关联 sample CSV、`gate_order_send_ok`、`gate_order_response` 和 feedback log。 | 同上。 |
| `bbo_id` / `bbo_ns` | `order_session_rtt_samples.csv` | experiment | `BookTicker.id` / 本机 Unix epoch ns | 记录当前 action 对应 open order 使用的行情版本；没有行情锚点的 action 填 0。 | 同上。 |
| `send_ns` | `order_session_rtt_samples.csv` | experiment | 本机 Unix epoch ns | 当前 action 实际写 socket 前的本地发送时间，用于直接从 CSV 计算 Ack RTT 与近似上行时间。 | 同上。 |
| `ack_recv_ns` / `ack_ex_ns` / `ack_ex2local_ns` / `ack_rtt_ns` | `order_session_rtt_samples.csv` | experiment | ns / 本机 Unix epoch ns / Gate timestamp ns | 当前 action Ack timing；`ack_ex2local_ns` 受时钟偏移影响，`ack_rtt_ns` 是本地 send 到本地 Ack receive。 | 同上。 |
| `resp_recv_ns` / `resp_ex_ns` / `resp_ex2local_ns` / `resp_rtt_ns` | `order_session_rtt_samples.csv` | experiment | ns / 本机 Unix epoch ns / Gate timestamp ns | 当前 action final response timing；没有 final response 时填 0 / -1。 | 同上。 |
| `status` | `order_session_rtt_samples.csv` | experiment | enum 文本 | 标记当前 action 是 sent、acked、terminal confirmed、rejected、timeout 或 send failed。 | 同上。 |
| `term_fb` | `order_session_rtt_samples.csv` | experiment | enum 文本 | 记录使 action 进入 terminal/invalid 路径的 feedback kind；无 terminal feedback 时为空。 | 同上。 |
| `fill` | `order_session_rtt_samples.csv` | experiment | `true` / `false` | 标记 passive probe 是否意外成交。 | 同上。 |
| `invalid` / `inv_reason` | `order_session_rtt_samples.csv` | experiment | bool / 文本 | 排除 reject、timeout、unexpected fill、safety close timeout 或 run-end REST 需要人工复核的样本。 | 同上。 |
| `rest_guard_phase` / `rest_guard_result` / `rest_guard_json_path` | `order_session_rtt_rest_guard.csv` | planned | enum / 文本 / 路径 | 记录 REST preflight、fatal flatten 和 run-end 整体账户检查结果；REST 不再作为 sample 级 `final_flat` 字段。 | 同上。 |

### 请求与 Ack 字段

这些字段已经在 Gate order submit / cancel 路径中使用，主要来自 `gate_order_send_ok`、
`gate_order_response` 和 `gate_order_ack_latency_diagnostic`。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `request_sequence` | log / report CSV | stable | `OrderSession` 内请求序号 | 关联 send log、Gate response 和 diagnostic window。 | 不能删除；除非替换 correlation key 并迁移所有 parser。 |
| `encoded_request_id` | log / report CSV | stable | Gate WS payload request id | 排查 request id 编码、Gate response correlation。 | 同 request correlation 迁移。 |
| `local_order_id` | log / report CSV | stable | Aquila 本地订单 id | 关联策略、订单管理、Gate response 和 feedback。 | 不能删除。 |
| `request_send_local_ns` | log / report CSV | stable | 本机 Unix epoch ns | Ack RTT 本地闭环起点。 | 不能删除；性能报告依赖。 |
| `local_receive_ns` | `gate_order_response` | stable | 本机 Unix epoch ns | Ack / result 本地接收时间，用于本地 RTT。 | 不能删除；性能报告依赖。 |
| `exchange_ns` | `gate_order_response` | stable | Gate timestamp ns | 交易所 timestamp 诊断，不可直接当单程网络延迟。 | 只有 Gate 不再提供该字段时删除。 |
| `exchange_to_local_ns` | log / report CSV | stable | ns，受时钟偏移影响 | 辅助观察 exchange timestamp 到本地 receive 的差值。 | 若被更清晰的 clock-offset 模型取代可删除。 |
| `ack_rtt_ns` | diagnostic / report CSV | stable | ns | 本地发送到本地收到 Ack 的主指标。 | 不能删除；latency report 主字段。 |
| `inflight` | `gate_order_send_ok` | stable | 当前 inflight 数量 | 观察 request map pressure。 | 若被 per-session summary 全面取代，可评估删除。 |
| `latency_diagnostic_inflight_at_send` | diagnostic / report CSV | experiment | 当前 inflight 数量 | Ack outlier 时确认发送瞬间是否存在排队压力。 | Ack outlier 诊断结束且无下游依赖后可删除。 |
| `send_cpu` | `gate_order_send_ok` / report CSV | experiment | Linux CPU id，失败为 `-1` | 确认发送时 owner thread 运行 CPU。 | 被完整 sched trace 或 thread sample 取代。 |
| `ack_cpu` | `gate_order_response` / report CSV | experiment | Linux CPU id，失败为 `-1` | 确认 Ack / submit result 本地处理时 owner thread 运行 CPU；用于对比 send CPU 和 owner CPU。 | 被完整 sched trace 或 thread sample 取代。 |

### 分阶段 Ack latency diagnostic 字段

这些字段由 `OrderAckLatencyDiagnostics` 只在订单后的诊断窗口内采集，正常无订单时不应输出。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `latency_diagnostic_reason` | diagnostic / report CSV | experiment | `kAckRttThreshold`、`kSendToDriveReadThreshold`、`kDriveReadDurationThreshold`、`kDiagnosticTimeout` | 表示触发诊断输出的原因。 | Ack outlier 分析结束且 report 不再使用时删除。 |
| `latency_diagnostic_ack_rtt_ns` | diagnostic / report CSV | experiment | ns | diagnostic 记录到的 Ack RTT。 | 同上。 |
| `send_to_first_after_hook_ns` | diagnostic / report CSV | experiment | ns | 判断订单发送后 runtime hook 是否及时返回。 | 若 runtime hook 不再是疑点可删除。 |
| `send_to_first_drive_read_ns` | diagnostic / report CSV | experiment | ns | 判断 owner thread 是否及时进入 `DriveRead()`。 | 若被更直接 scheduler trace 取代可删除。 |
| `drive_read_duration_ns` | diagnostic / report CSV | experiment | ns | 记录触发时单次 `DriveRead()` 耗时。 | 若 read path tail 已通过其他 profile 覆盖可删除。 |
| `max_observed_drive_read_duration_ns` | diagnostic / report CSV | experiment | ns | 诊断窗口内最大 `DriveRead()` 耗时。 | 同上。 |
| `diagnostic_cpu` | `gate_order_ack_latency_diagnostic` / report CSV | experiment | Linux CPU id，失败为 `-1` | 记录触发 latency diagnostic log 时 owner thread 所在 CPU；Ack RTT 阈值触发时通常等价于 Ack 处理 CPU。 | 被完整 sched trace 或 thread sample 取代。 |

### TCP_INFO 字段

这些字段用于解释多个 `OrderSession` Ack RTT 不同的问题。采集点限制在 `gate_order_response` 和
`gate_order_ack_latency_diagnostic`，由 `order_session.diagnostics.enable_tcp_info` 显式打开；默认关闭时仍输出
`tcp_info_requested=false` / `tcp_info_available=false`，但不调用 `getsockopt(TCP_INFO)`。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `tcp_info_requested` | response / diagnostic | experiment | `true` / `false` | 标记本条 log 是否启用了 TCP_INFO 采集，区分“未请求”和“请求但不可用”。 | TCP_INFO 诊断删除时同步删除。 |
| `tcp_info_available` | response / diagnostic / report CSV | experiment | `true` / `false` | 标记本次 `TCP_INFO` snapshot 是否成功。 | 同上。 |
| `tcp_info_rtt_us` | response / diagnostic / report CSV | experiment | microseconds | kernel TCP RTT 估计，用于区分网络 RTT 与本地调度。 | TCP_INFO 不可用或被外部采样取代。 |
| `tcp_info_rttvar_us` | response / diagnostic / report CSV | experiment | microseconds | kernel TCP RTT variance 估计。 | 同上。 |
| `tcp_info_retrans` | response / diagnostic / report CSV | experiment | counter | Linux `tcp_info.tcpi_retrans`。 | 同上。 |
| `tcp_info_total_retrans` | response / diagnostic / report CSV | experiment | counter | Linux `tcp_info.tcpi_total_retrans`，连接累计重传数。 | 同上。 |
| `tcp_info_unacked` | response / diagnostic / report CSV | experiment | packet count | Linux `tcp_info.tcpi_unacked`，当前未确认 packet 数。 | 同上。 |
| `tcp_info_snd_cwnd` | response / diagnostic / report CSV | experiment | packet count | Linux `tcp_info.tcpi_snd_cwnd`，TCP send congestion window。 | 同上。 |

## Gate OrderFeedbackSession

组件入口：

- `exchange/gate/trading/order_feedback_session.h`
- `exchange/gate/trading/order_feedback_parser.h`
- `core/trading/order_feedback_event.h`
- `core/trading/order_feedback_shm.h`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `local_order_id` | feedback event / log / report CSV | stable | Aquila 本地订单 id | 关联 strategy order、Gate feedback 和 OrderManager 状态。 | 不能删除。 |
| `exchange_order_id` | feedback event / log / report CSV | stable | Gate order id | 对账 REST / private feedback / submit response。 | 不能删除。 |
| `accepted_exchange_ns` | feedback event / report CSV | stable | Gate timestamp ns | 交易所接受订单时间。 | Gate 字段不可用或 schema 变更时重审。 |
| `finish_exchange_ns` | feedback event / report CSV | stable | Gate timestamp ns | 交易所订单终态时间。 | 同上。 |
| `cumulative_filled_quantity` | feedback event / report CSV | stable | contract quantity | 判断 filled / partial filled / terminal 行为。 | 不能删除。 |
| `average_fill_price` | feedback event / report CSV | stable | price | 计算成交价、PnL 和 slippage。 | 不能删除。 |
| `finish_reason` | feedback event / report CSV | stable | Gate / Aquila reason 文本或枚举 | 区分 filled、cancelled、partial-cancel terminal。 | 不能删除。 |
| `continuity_lost` / `kContinuityLost` | SHM control event / log | stable | control event | 标记 private feedback 连续性丢失，触发 stop-and-flat / handoff。 | 不能删除。 |

## TradingRuntime / Runtime Affinity

组件入口：

- `core/trading/trading_runtime.h`
- `exchange/gate/trading/order_session_runtime_adapter.h`
- `scripts/lead_lag/run_live_with_guard.py`
- `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `affinity_profile` | guard stdout / report | experiment | config path 或 profile name | 记录本轮是否使用 runtime affinity overlay。 | affinity pipeline 稳定后可改为 stable。 |
| `affinity_core_path` | guard stdout / report | experiment | generated config path | 记录 core-path 临时配置位置，便于复现。 | 如果生成配置不再保留可删除。 |
| `owner_thread_cpu` | log / summary | planned | Linux CPU id | 对齐 `OrderSession` owner thread 和 runtime hook 执行 CPU。 | 被完整 scheduler trace 取代。 |
| `poll_calls` / `empty_polls` | runtime diagnostics | stable | counter | 观察 DataReader / scheduler 空转行为。 | 若 runtime diagnostics schema 重构再迁移。 |

## LeadLag Strategy

组件入口：

- `strategy/lead_lag/strategy.h`
- `strategy/lead_lag/execution_state.h`
- `strategy/lead_lag/signal.h`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `trigger_ticker_id` | strategy log / report CSV | stable | market data ticker id | 关联 signal、order intent 和 submitted order。 | 不能删除；真实订单模式不写 per-signal CSV 时依赖它关联。 |
| `trigger_exchange` | strategy log / report CSV | stable | `kGate` / `kBinance` 等 | 表示触发信号的行情来源，不表示实际下单交易所。 | 不能删除；若新增 `order_exchange` 也应保留。 |
| `trigger_symbol_id` | strategy log / report CSV | stable | internal symbol id | 关联触发行情 symbol。 | 不能删除。 |
| `signal_role` | strategy log / report CSV | stable | `kLead` / `kLag` | 区分 pair role。 | 不能删除。 |
| `order_role` | strategy log / report CSV | stable | `entry` / `exit` | 关联 position open / close。 | 不能删除。 |
| `position_id` | strategy log / report CSV | stable | strategy position id | position.csv 配对主键之一。 | 不能删除。 |
| `position_event` | strategy log / report CSV | stable | strategy event enum | 判断 entry / exit submit 状态。 | 不能删除。 |
| `entry_local_order_id` | strategy log / report CSV | stable | local order id | 将 exit 订单关联回 entry。 | 不能删除。 |
| `order_finished_local_ns` | strategy log / report CSV | stable | 本机 Unix epoch ns | send-to-finish / ack-to-finish 本地闭环终点。 | 不能删除。 |
| `ack_rtt_ns` | strategy log / report CSV | stable | ns | 本地 Ack RTT 主指标。 | 不能删除。 |
| `exchange_lifecycle_ns` | strategy log / report CSV | experiment | ns | Gate exchange Ack 到 terminal update 的交易所侧 lifecycle 诊断，不解释 Ack RTT。 | 若 Gate timestamp 语义不稳定或更好 lifecycle 字段落地可重审。 |

## LeadLag Report / Analyzer

组件入口：

- `scripts/lead_lag/analyze_order_detail.py`
- `scripts/lead_lag/generate_live_report.py`
- `docs/lead_lag_live_report_csv_schema.md`

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `order_session_id` | `order_detail.csv` / `latency.csv` | experiment | 本进程内单调 id | 将订单行关联回 Gate `OrderSession`。 | Gate OrderSession 多连接诊断删除后同步删除。 |
| `owner_thread_cpu` | `order_detail.csv` / `latency.csv` | experiment | Linux CPU id，失败为 `-1` | 将 session active 时 owner CPU 合并进 report。 | 同上。 |
| `local_ip` / `local_port` | `order_detail.csv` / `latency.csv` | experiment | TCP endpoint | 将本地 TCP endpoint 合并进 report。 | endpoint 诊断删除后同步删除。 |
| `remote_ip` / `remote_port` | `order_detail.csv` / `latency.csv` | experiment | TCP endpoint | 将远端 TCP endpoint 合并进 report。 | endpoint 诊断删除后同步删除。 |
| `send_cpu` / `ack_cpu` / `diagnostic_cpu` | `order_detail.csv` / `latency.csv` | experiment | Linux CPU id，失败为 `-1`；多 diagnostic CPU 用 `;` 合并 | 将下单发送、Ack 处理和 diagnostic 输出时的 owner CPU 合并进 report。 | CPU 诊断删除后同步删除。 |
| `tcp_info_*` | `order_detail.csv` / `latency.csv` | experiment | 见 Gate OrderSession section | 将 `gate_order_response` / `gate_order_ack_latency_diagnostic` 的 TCP_INFO snapshot 合并进 report；数值字段多次出现取最大值。 | TCP_INFO 诊断删除后同步删除。 |
| `latency_diagnostic_*` | `order_detail.csv` / `latency.csv` | experiment | 见 Gate OrderSession section | 将 `gate_order_ack_latency_diagnostic` 合并进 report。 | Gate OrderSession diagnostic 删除后同步删除。 |
| `send_to_ack_local_ns` | `latency.csv` | stable | ns | 本地 send 到 Ack receive。 | 不能删除；等价于主 Ack RTT 校验字段。 |
| `send_to_finish_local_ns` | `latency.csv` | stable | ns | 本地 send 到策略终态处理完成。 | 不能删除；用于区分 Ack path 与 terminal lifecycle。 |
| `ack_to_finish_local_ns` | `latency.csv` | stable | ns | 本地 Ack receive 到策略终态处理完成。 | 不能删除。 |
| `ack_exchange_to_local_ns` | `latency.csv` | stable | ns，受时钟偏移影响 | 辅助定位 exchange timestamp 与本地 receive 差值。 | 被 clock-offset 校正模型取代时重审。 |
| `exchange_lifecycle_ns` | `latency.csv` | experiment | ns | Gate exchange Ack 到 terminal update 的相对间隔。 | 同 LeadLag Strategy。 |
| `warnings` | CSV | stable | `;` 分隔文本 | 标记缺字段、异常 join、数量不一致等分析问题。 | 不能删除；新增 warning 值需更新 CSV schema。 |
| `order_exchange` | CSV | planned | exchange enum | 区分 signal source (`trigger_exchange`) 与实际下单交易所。 | 实现后按使用情况决定 stable / experiment。 |

## 删除字段流程

删除或重命名字段前，至少检查：

```bash
rg '<field_name>' docs strategy exchange core tools scripts test benchmark
rg '<log_event_or_csv_column>' reports scripts docs
```

如果字段出现在历史 report、CSV schema 或 parser 中，删除提交必须说明兼容策略。对 hot path 字段，删除也需要确认不会移除仍在使用的诊断证据。
