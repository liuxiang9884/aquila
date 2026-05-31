# LeadLag Runtime Latency 当前计划

## 目的

本文是 LeadLag live-orders latency 的当前 runbook：记录已经落地的诊断能力、仍未证明的问题、下一轮测试必须采集什么。历史样本细节见：

- `docs/lead_lag_ack_latency_outlier_analysis.md`
- `docs/gate_order_session_rtt_probe_design.md`
- `docs/diagnostic_fields.md`
- `docs/lead_lag_live_operations_pipeline.md`
- `docs/lead_lag_live_report_csv_schema.md`

## 当前判断

- 2026-05-25 12-symbol guarded live-orders 1 小时 run 正常 flat，最大 Ack RTT `219.023ms`。
- 该 outlier 在同一 strategy / Gate order-session owner thread 的 send 与 receive Ack 之间，不是 feedback session 处理 Ack 慢。
- 当时 CPU4 长时间满载，strategy / order session / feedback session 都绑 CPU4 并 active-spin；本地调度或同核竞争是强候选，但没有证明。
- 2026-05-26 拆核 30 分钟 run 正常 flat，最大 Ack RTT `6.738ms`，没有复现 219ms outlier。
- 2026-05-26 最大 send-to-finish `45.977ms`，其中 Gate `exchange_lifecycle_ns` `37.336ms`；这是 terminal lifecycle 诊断面，不是 Ack path。
- 当前不继续凭单次样本调整架构，等待复现后用新增字段归因。

## 已落地能力

Ack diagnostic：

- `ack_rtt_threshold_ns` 默认 `20000000`。
- 短期诊断可设 `0` 做每 Ack 采样。
- `max_logs_per_second` 控制 diagnostic log 限流。
- `enable_tcp_info=true` 时采样 Linux `TCP_INFO`，默认不建议长期生产开启。

Order write path：

- order encode
- WebSocket frame encode
- enqueue
- write pump
- `send()` / `SSL_write()`
- write complete
- socket send queue / notsent

Runtime loop：

- send 后首次 runtime hook
- send 后首次 `DriveRead()`
- `DriveRead()` duration
- max loop gap
- Ack 前 loop iterations
- owner / send / ack / diagnostic CPU

Socket timestamping：

- 编译开关：`AQUILA_ENABLE_SOCKET_TIMESTAMPING_ATTRIBUTION`，默认 ON。
- runtime config 开启且 private plain transport 成功 apply `SO_TIMESTAMPING` 后才启动 probe。
- 字段可拆到 `write_complete -> tx_software -> tx_ack -> rx_software -> ack_receive` software-level 大段。
- TLS 或 apply 失败时 `ts_available=false`；不要把 0 当真实时间。

Report / CSV：

- `order_detail.csv` / `latency.csv` 已包含 Ack diagnostic 和 exchange lifecycle 字段。
- 跨时钟域的 `bbo_to_strategy_ns` / `trigger_to_request_send_ns` 已在 report 中置空并写 warning。
- 字段说明集中在 `docs/diagnostic_fields.md`。

## 诊断边界

必须分清三类延迟：

| 名称 | 口径 | 用途 |
| --- | --- | --- |
| Ack RTT | `request_send_local_ns -> ack_local_receive_ns` | 下单 submit Ack path 主指标。 |
| send-to-finish | `request_send_local_ns -> order_finished_local_ns` | 本地完整订单闭环，包含 terminal feedback。 |
| exchange lifecycle | `finish_exchange_ns - ack_exchange_ns` | Gate submit Ack 到 private order terminal update 的交易所侧相对间隔。 |

不要用跨机器 clock 推导单程网络延迟。`ack_exchange_ns` 和 `finish_exchange_ns` 只能作为 Gate 侧相对时间或辅助上下文。

## 下一轮 live run 要求

如果目标是复核 Ack latency：

1. 按 `docs/lead_lag_live_operations_pipeline.md` 启动 guarded live run。
2. 使用 affinity profile `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml`，并在 report 中记录实际 split 状态。
3. 明确 Gate data / order / feedback endpoint，记录 private/public、TLS/plain、remote endpoint、local port、owner CPU。
4. 若需要每 Ack 采样，临时设置：

   ```toml
   [order_session.diagnostics]
   ack_rtt_threshold_ns = 0
   max_logs_per_second = 100
   enable_tcp_info = true
   ```

5. private plain 全阶段归因使用 `config/order_sessions/gate_order_session_rtt_probe_allstage.toml` 或等价临时配置。
6. 临时配置、日志、调度采样和 scratch 输出写入 `/home/liuxiang/tmp/<run_id>/`。

同步采集调度证据：

```bash
pidstat -wt -p <strategy_pid>,<feedback_pid> 1
sar -u -P 2,3,4,5,6 1
sar -q 1
sar -w 1
```

如权限允许，异常窗口补：

```bash
perf sched record -p <strategy_pid> -- sleep <duration>
perf sched latency
```

## Outlier 分析顺序

Ack RTT outlier：

1. 看 `send_to_first_drive_read_ns`、`max_loop_gap_ns` 和调度采样，判断 owner thread 是否没及时读。
2. 看 `drive_read_duration_ns`，判断 socket read / frame decode / dispatch 是否慢。
3. 看 write path 字段，判断本机发送路径是否慢。
4. 看 socket queue / `TCP_INFO`，判断本机 TCP 状态是否异常。
5. 看 socket timestamping 分段，判断延迟更接近 TX 前、TCP ACK、业务 Ack 回程还是用户态读取。
6. 按 session / connection 分组，避免把多连接全局聚合掩盖掉。

Terminal lifecycle tail：

1. 确认 `ack_rtt_ns` 是否正常。
2. 看 `exchange_lifecycle_ns`、`ack_to_finish_local_ns` 和 `send_to_finish_local_ns` 是否同步变大。
3. 按 symbol、order role、`finish_as`、remote endpoint、local port、`order_session_id` 分组。
4. 不把 terminal lifecycle 归入 Ack path。

## RTT Probe 关系

`gate_order_session_rtt_probe` 用于多 connection / 多 IP 的真实 order Ack RTT 采样。当前它是 measurement-only：

- 连接列表在 CSV。
- 允许重复 `connect_ip`。
- live execute 当前只支持 IOC。
- sample CSV 保留全阶段诊断字段。
- 不自动 score / 切换。

使用 probe 得出的结论必须按 session / group / ip / symbol / time window 分组；同一 URL 不代表同一 remote IP、同一 gateway shard、同一 per-connection queue 或同一 owner thread 状态。

## 执行边界

- 只改文档不需要重新构建 C++。
- 修改 latency diagnostic、order session、feedback parser、runtime loop 或 config parser 后，至少运行对应 C++ / Python 单测和 `git diff --check`。
- 新增、重命名或删除诊断字段、log key、stats 字段、report CSV 字段时，必须同步更新 `docs/diagnostic_fields.md`。
