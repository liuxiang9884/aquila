# LeadLag Latency 分析与复现

本文统一 LeadLag submit-path benchmark、Gate Ack RTT 和 terminal lifecycle 的当前证据与归因方法。诊断字段定义见
`docs/diagnostic_fields.md`，Gate probe 操作见 `docs/gate_order_session_rtt_probe_design.md`。

## 指标口径

| 指标 | 口径 | 含义 |
| --- | --- | --- |
| Ack RTT | `request_send_local_ns -> ack_local_receive_ns` | 同一 OrderSession owner 的 submit Ack 闭环 |
| Ack uplink | request send/write complete -> Gate `x_in` | 跨时钟只作分段辅助，需 pcap/socket timestamp 对齐 |
| Gate process | `x_in -> x_out` | Gate header 同时钟域处理段 |
| Ack downlink | Gate `x_out` -> local receive | 跨时钟需 pcap/socket timestamp 对齐 |
| send-to-finish | request send -> local terminal feedback | 完整订单生命周期 |
| exchange lifecycle | Gate Ack exchange time -> terminal feedback exchange time | Gate 侧相对生命周期，不是 Ack path |

不能用不同机器 clock 的绝对差推导单程网络延迟。

## 已确认 live 证据

2026-05-25 12-symbol guarded run 正常 flat，58 orders、0 fills；Ack RTT p50 5.628ms、p95 10.845ms、max
219.023ms。Outlier 位于同一 strategy/OrderSession owner 的 send 与 receive Ack 之间，不是 feedback session 处理慢。当时 strategy、
order session、feedback active-spin 同绑 CPU4，调度/同核竞争是候选，但原日志无法定位具体阶段。

2026-05-26 拆核 30-minute run 正常 flat，max Ack RTT 6.738ms，未复现 219ms。Max send-to-finish 45.977ms，其中
exchange lifecycle 37.336ms；它属于 terminal lifecycle，不是 Ack tail。

2026-06-01 8-session private plain RTT probe：1798 Ack、invalid=0、fill=0、feedback routed 1798/1798。Socket timestamping、
TCP_INFO 和 no-TLS pcap 将 `10ms+` tail 定位到 request 出现在本机 pcap 后、业务 Ack response 回到本机 pcap 前。Request 到 remote
TCP ACK 与 request 到 WebSocket Ack 相等，peer 用业务 response piggyback TCP ACK；pcap receive 到 software RX/user callback 为微秒级。
证据不支持 owner deschedule、read/parse、local write queue、send-side retrans 为这些样本的主因，剩余段覆盖 kernel/NIC/private link/
Gate edge/Gate process/return path。若 Gate `x_in->x_out` duration share 接近 1 且 residual <1ms，优先归因 Gate processing。

219ms 单样本仍未复现，不据此修改 OrderSession 架构。

## Component benchmark 环境边界

当前主机是 KVM/AWS kernel；CPU 未 isolation/nohz_full/rcu_nocbs，irqbalance 运行，benchmark CPU 仍承受 IRQ、softirq、RCU、timer 和
host vCPU deschedule。2026-06-06 `OpenSignalSubmitPath` p50 1.567us、p99 11.265us、max 114.511ms；同期 CPU16 有 LOC/TIMER/
RCU/NET_RX 增量。Max 不可能代表稳定纯计算成本。

因此当前环境中 median/p50 更接近组件路径；p99/p99.9/max 必须连同 CPU、MHz/governor、isolation、IRQ/softirq delta、load 和后台任务报告。
Refactor 前后 p99 基本持平，只能说明未观察到回归，不能宣称确定优化收益。

## 已落地诊断层

- Ack threshold/rate limit：`ack_rtt_threshold_ns`、`max_logs_per_second`。
- Write path：order/frame encode、enqueue、write pump、send/SSL_write、write complete、send queue/notsent。
- Runtime loop：first hook/DriveRead、DriveRead duration、max loop gap、iterations、owner/send/ack CPU。
- TCP_INFO：cwnd、rtt、retrans、notsent 等；仅诊断时开启。
- Socket timestamping：`write_complete -> tx_software -> tx_ack -> rx_software -> ack_receive`，TLS/apply failure 时 unavailable。
- Pcap alignment：request/Ack frame、TCP ACK、Gate `x_in/x_out` 与 residual。
- Report：`order_detail.csv`/`latency.csv` 保存 Ack 与 lifecycle 分段；字段见 diagnostic index。

## 复现 Pipeline

按 `docs/lead_lag_live_operations.md` 或独立 RTT probe 创建隔离 run。记录 endpoint/private/public、TLS/plain、remote IP/local port、
session id、owner CPU、actual affinity 和 binary commit。需要每 Ack 采样时使用 scratch config：

```toml
[order_session.diagnostics]
ack_rtt_threshold_ns = 0
max_logs_per_second = 100
enable_tcp_info = true
```

Private plain 全阶段使用 `config/order_sessions/gate_order_session_rtt_probe_allstage.toml` 或等价临时配置。同步采集：

```bash
pidstat -wt -p <strategy_pid>,<feedback_pid> 1
sar -u -P <cpu_list> 1
sar -q 1
sar -w 1
```

有权限时用 `perf sched`；pcap 输出和 scratch config 写入 `/home/liuxiang/tmp/<run_id>/`。

## Outlier 分析顺序

Ack：

1. `max_loop_gap/send_to_first_drive_read` 与 scheduler evidence。
2. `drive_read_duration` 与 parse/dispatch。
3. Encode/write/send complete。
4. Socket queue/TCP_INFO/retrans。
5. Socket timestamp stages。
6. Pcap request/TCP ACK/business Ack 与 Gate `x_in/x_out`。
7. 按 session/IP/symbol/time window 分组。

Terminal：先确认 Ack 正常，再比较 exchange lifecycle、Ack-to-finish 和 send-to-finish；按 symbol/order role/finish_as/endpoint/session 分组，
不得把 terminal tail 归入 Ack path。

## 当前边界与下一步

- 复现 `>10ms` private plain tail 时优先运行 `scripts/gate/diagnostics/analyze_order_session_rtt_pcap.py`。
- 严肃代码级 tail benchmark 需要 isolated/dedicated CPU、迁移 IRQ、固定 frequency 并避免 build/replay/pcap heavy workload 并发。
- 性能结论必须来自新鲜 benchmark/profile/live evidence；单次 outlier 只作为调查线索。
- 新增、重命名或删除诊断字段时同步 `docs/diagnostic_fields.md`。
