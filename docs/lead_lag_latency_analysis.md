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

## 2026-07-22 Bitget 46-symbol cold submit benchmark

本轮针对 `20260722_052013_bitget_combined46_n6_hs_fanout1_24h` 的
`signal decision -> strategy request timestamp` 实盘 P50 `6.925us` 建立 cold / warm
对照。benchmark 从同一份 46-pair LeadLag 配置加载参数，强制 `fanout=1`；每个 cold
sample 在目标 BAS submit 前执行 64 次全 pair、双边 non-triggering sweep，共 5,888 次
行情更新。正式 Release 结果固定 CPU 16，每个 case 跑五个独立进程、每进程 1,024 个
有效 sample；下表是五个进程各自分位数的中位数：

| Case | decision -> request P50 | P95 | P99 | P50 组间范围 |
| --- | ---: | ---: | ---: | ---: |
| cold、INFO、完整 stage timestamp | 5.850us | 7.932us | 28.448us | 5.665–6.009us |
| cold、INFO、只保留端点 | 4.943us | 6.786us | 26.802us | 4.804–5.118us |
| cold、INFO 关闭、完整 stage timestamp | 4.058us | 5.398us | 13.972us | 3.862–4.211us |
| warm、INFO、完整 stage timestamp | 1.239us | 1.544us | 1.830us | 1.220–1.292us |

同日删除成功路径中字段重复的 `lead_lag_order_intent` INFO 后，使用未修改的 parent
binary 与 candidate binary 重新做五组交错 paired endpoint-only A/B；每组仍为 1,024
samples，workload 和 CPU 不变：

| Endpoint | Parent group median | Candidate group median | 改善 | 同向组数 |
| --- | ---: | ---: | ---: | ---: |
| decision -> request P50 | 4.939us | 4.285us | 0.654us / 13.2% | 5/5 |
| decision -> request P95 | 6.915us | 5.917us | 0.998us / 14.4% | 5/5 |
| decision -> request P99 | 29.227us | 25.844us | 3.383us / 11.6% | 5/5 |

parent P50 的组间 MAD 为 21ns，P50 收益超过 `2 × MAD`，也超过 2% / 5 cycles
门槛。成功结果仍由 `lead_lag_order_submitted` 记录，所有
`lead_lag_order_intent_rejected`、recovery 和 report 事实源保持不变；strategy 与 report
回归测试通过。

两个 software prefetch 候选使用日志删除后的固定 binary 作为 parent，分别重新跑五组
paired endpoint-only A/B，均回退，因此没有进入生产代码：

| 候选 | Parent P50 | Candidate P50 | P50 方向 | Parent P95 | Candidate P95 | 结论 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| triggered 后预取 reservation bitset / slot | 4.193us | 4.282us | 5/5 回退 | 5.621us | 5.905us | 拒绝 |
| global-risk scan 对后续 runtime 做 distance=4 prefetch | 4.128us | 4.425us | 5/5 回退 | 5.580us | 5.788us | 拒绝 |

不同候选的 parent 来自各自同轮 paired groups，绝对值不能跨轮直接比较；同轮方向足以证明
额外 prefetch 指令 / cache 干扰没有换来 endpoint 收益。没有继续盲调其他 distance，也没有
引入增量 risk totals。

完整 stage case 的 cold INFO P50 分段如下。各 counter 独立取 P50，不能机械相加成总
P50，但可以用于成本排序：

| 阶段 | P50 | 主要工作 |
| --- | ---: | --- |
| decision -> signal-triggered log done | 1.222us | `lead_lag_signal_triggered` 的同步 frontend log call 与 benchmark observer |
| signal log done -> price prepared | 1.245us | parallel / drift guard、symbol/instrument 读取和 order price preparation |
| price -> freshness checked | 0.194us | 可选 signal-decision log 分支与 freshness guard |
| freshness -> quantity prepared | 0.167us | quantity、min/max 和 decimal preparation |
| quantity -> routes selected | 0.346us | gateway route state refresh 与单 route selection |
| routes selected -> risk checked | 0.710us | order notional、global open risk scan / check |
| risk -> order-intent log done（改前） | 0.622us | 已删除的 `lead_lag_order_intent` frontend log call 与 observer |
| intent log -> `PlaceOrder()` begin（改前） | 0.494us | execution group / parent id、fixed risk slot 与 child request 准备 |
| `PlaceOrder()` begin -> request timestamp | 0.632us | request/symbol copy、OrderPool create、gateway route precheck 和 command timestamp |
| request timestamp -> `PlaceOrder()` return | 0.469us | route table record、SHM `TryPush`、order status / send timestamp 回写 |

归因边界：

- 改动前 cold INFO 的完整 stage P50 比 INFO 关闭高 1.792us。两个 log call 可直接对齐的增量约
  1.378us，其中 signal-triggered 约 0.937us、order-intent 约 0.441us；剩余差值分布在
  log 后的工作集扰动和独立分位数误差中。Quill 是异步 logger，这里测到的是 frontend
  格式参数复制 / queue enqueue 及其缓存影响，不是 file sink 同步落盘。
- INFO 关闭后 cold P50 仍为 4.058us，因此日志不是全部原因。相同 INFO 下，cold stage
  P50 是 warm 的 4.72 倍；确定性行情 churn 证明主要差异来自 submit 代码、pair state、
  risk / gateway 数据在持续行情后的 cold instruction/data working set。
- fixed risk slot acquire 的 cold INFO P50 为 0.240us，只占完整 stage 总 P50 的约 4%，
  不是本次 `6+us` 的主瓶颈。
- 完整 stage timestamp 相对 endpoint-only 增加约 0.907us（15.5%），所以总路径应优先看
  endpoint-only，完整 stage 只用于定位。benchmark 没有修改 production binary 或新增
  live timestamp。
- 本机 `kernel.perf_event_paranoid=4`，没有 PMU cache-miss 计数，因此当前证据能确认
  cold/warm working-set 效应，不能进一步声称具体是 L1I、L1D、L2 或某个单一函数 miss。
  P99 仍受本机非隔离 scheduler / IRQ 影响，不用来声明稳定代码成本。

原始 breakdown JSON 位于
`/home/liuxiang/tmp/lead_lag_cold_submit_breakdown_20260722_1024/`；日志删除的 fresh paired
JSON 位于
`/home/liuxiang/tmp/lead_lag_submit_log_risk_prefetch_20260722/intent_removal/`；失败的
prefetch 候选证据分别位于同一根目录下的 `risk_reservation_prefetch/` 和
`risk_runtime_prefetch_distance4/`。运行入口是
`BM_LeadLagSubmitPathBreakdownOrderGatewayBitget46Fanout1Churn`、`Warm` 和
`EndpointOnly`；`AQUILA_LEAD_LAG_BENCHMARK_LOG_LEVEL=critical` 用于 INFO 关闭对照。

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
