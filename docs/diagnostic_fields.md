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

这些字段用于区分多条 Gate `OrderSession` / WebSocket 连接的实际路径。当前第一批连接级字段为 planned，
后续实现时应优先输出在低频 log `gate_order_session_connected` 中。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `order_session_id` | log / stats / CSV | planned | 本进程内单调 id 或配置 id | 关联同一条 `OrderSession` 的 send、ack、diagnostic 和 summary。 | 多 session 诊断停止且所有下游 parser 不再依赖。 |
| `local_ip` | `gate_order_session_connected` | planned | IP 文本 | 记录本地 TCP endpoint，用于区分 NAT / source address。 | 被等价 endpoint snapshot 取代。 |
| `local_port` | `gate_order_session_connected` | planned | TCP port | 区分同 remote endpoint 下的不同连接。 | 被等价 endpoint snapshot 取代。 |
| `remote_ip` | `gate_order_session_connected` | planned | IP 文本 | 判断慢 session 是否落到不同 remote IP / gateway。 | 被等价 endpoint snapshot 取代。 |
| `remote_port` | `gate_order_session_connected` | planned | TCP port | 记录远端 TCP endpoint。 | 被等价 endpoint snapshot 取代。 |
| `owner_thread_cpu` | `gate_order_session_connected` / summary | planned | Linux CPU id，失败为 `-1` | 确认 session owner thread 实际运行 CPU。 | 被更完整 thread affinity / sched snapshot 取代。 |
| `resolved_ips` | `gate_order_session_connected` | planned | DNS 结果列表 | 区分 hostname 相同但 DNS / connect 目标不同的情况。 | WebSocket 层无法稳定提供时可留空；若无消费者可删除。 |

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

### 计划中的 CPU / TCP_INFO 字段

这些字段用于解释多个 `OrderSession` Ack RTT 不同的问题。采集点应限制在 send、ack、diagnostic 或 summary，
不要放入无订单 hot loop。

| 字段 | 表面 | 状态 | 单位 / 取值 | 用途 | 删除条件 |
| --- | --- | --- | --- | --- | --- |
| `send_cpu` | send log / report CSV | planned | Linux CPU id，失败为 `-1` | 确认发送时 owner thread 运行 CPU。 | 被完整 sched trace 或 thread sample 取代。 |
| `ack_cpu` | response log / report CSV | planned | Linux CPU id，失败为 `-1` | 确认 Ack 处理时 owner thread 运行 CPU。 | 同上。 |
| `tcp_info_rtt_us` | response / diagnostic / summary | planned | microseconds | kernel TCP RTT 估计，用于区分网络 RTT 与本地调度。 | TCP_INFO 不可用或被外部采样取代。 |
| `tcp_info_rttvar_us` | response / diagnostic / summary | planned | microseconds | kernel TCP RTT variance 估计。 | 同上。 |
| `tcp_info_retrans` | response / diagnostic / summary | planned | counter | 当前 TCP retrans 相关字段，字段语义需按 Linux `tcp_info` 明确。 | 同上。 |
| `tcp_info_total_retrans` | response / diagnostic / summary | planned | counter | 连接累计重传数。 | 同上。 |
| `tcp_info_unacked` | response / diagnostic / summary | planned | packet count | 当前未确认 packet 数。 | 同上。 |
| `tcp_info_snd_cwnd` | response / diagnostic / summary | planned | packet count | TCP send congestion window。 | 同上。 |

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
