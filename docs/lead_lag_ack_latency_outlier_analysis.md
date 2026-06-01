# LeadLag Ack Latency Outlier 分析

## 结论

2026-05-25 的 `219.023ms` Ack RTT outlier 发生在同一条 strategy / Gate `OrderSession` owner thread 的 send 与 receive Ack 之间。它不是 `gate_order_feedback_session` 处理 Ack 慢，也不是跨线程消息传递延迟。

当前证据只能说明 CPU4 同期满载、且 strategy / order session / feedback session 当时都绑 CPU4 并 active-spin，因此本地调度或同核竞争是强候选；但原始日志不能证明具体卡在 OS deschedule、runtime hook、socket read / parse、网络路径还是 Gate 侧。

该问题当前仍未复现到 `219ms` 量级。2026-06-01 的 8 条 private plain RTT probe 与 no TLS pcap
已能把 `10ms+` Ack tail 定位到 request 出现在本机 pcap 后、Gate submit Ack response 回到本机 pcap 前；
未支持本机 owner thread 调度、读解析、本机写队列积压或本机发送侧 TCP retrans 作为主要原因。

## 触发样本

来源：

- Report：`reports/20260525_133511_12pair_live_2ticks/`
- Latency CSV：`reports/20260525_133511_12pair_live_2ticks/latency.csv`
- Strategy log：`/home/liuxiang/log/lead_lag_strategy_requested_12symbols_live_20260522_20260525_133511.log`
- Guard stdout：`/home/liuxiang/tmp/lead_lag_live_12pairs_1h_2ticks_20260525_132912/guarded_live_retry1.stdout`

run 概况：

| 指标 | 值 |
| --- | ---: |
| `signals` | `58` |
| `orders` | `58` |
| `finished` | `58` |
| `filled` | `0` |
| final state | `normal_exit_flat` |

最大 Ack RTT：

| 字段 | 值 |
| --- | --- |
| `local_order_id` | `288230376151711783` |
| `request_sequence` | `40` |
| `symbol` | `ZEC_USDT` |
| `action` | `kOpenShort` |
| `side` | `kSell` |
| `status` | `kCancelled` |
| `request_send_local_ns` | `1779719172171523913` |
| `ack_local_receive_ns` | `1779719172390546936` |
| `ack_rtt_ns` | `219023023` |

```text
ack_rtt_ns = ack_local_receive_ns - request_send_local_ns
           = 219,023,023 ns
           = 219.023 ms
```

58 笔订单分布：

| metric | value |
| --- | ---: |
| min | `3.638ms` |
| p50 | `5.628ms` |
| p90 | `7.098ms` |
| p95 | `10.845ms` |
| max | `219.023ms` |

## 字段口径

Ack 链路：

```text
lead_lag_strategy process
  -> Gate OrderSession WebSocket
  -> futures.order_place
  <- Gate submit Ack response
  -> gate_order_response kind=kAck
  -> lead_lag_order_response kind=kAck
```

`gate_order_feedback_session` 订阅 private `futures.orders`，只负责 accepted、filled、cancelled、continuity lost 等后续状态，不返回 submit Ack。

本轮 outlier 的两个主时间点都在同一 owner thread：

- `request_send_local_ns`：请求进入 WebSocket 发送路径前的本地时间。
- `ack_local_receive_ns`：同一 order session runtime 线程读到并解析 Ack response 后的本地时间。

`ack_exchange_ns` / `exchange_to_local_ns` 只能作为辅助线索。Gate 与本机是不同机器、不同 clock，不能用跨机器时间差直接证明 Gate 处理耗时或单程网络延迟。

## 已确认事实

1. CPU4 在 outlier 所在 10 分钟 sysstat 窗口满载。
2. 当时 strategy loop、Gate order session 和 Gate order feedback session 都绑 CPU4。
3. strategy runtime 是 active spin，线程仍是普通 `SCHED_OTHER`；多个 active-spin 进程绑同一 CPU 时会由 CFS 分时。
4. 原始日志无法继续拆分 send -> Ack receive 中间阶段。

更准确的描述：

```text
request_send_local_ns
  -> owner thread / runtime hook / socket write-read / network / Gate submit Ack
ack_local_receive_ns
```

不能直接断言：

- Ack 被另一个 session 或另一个线程处理慢。
- Gate 已在 5ms 内处理完，剩余 213ms 一定是本地调度。

## 后续证据

2026-05-26 拆核 30 分钟 run：

- Report：`reports/20260526_043440_12pair_live_30m/`
- strategy / order owner：CPU4
- Gate order feedback：CPU6
- Gate / Binance data session：CPU2 / CPU3
- log backend：CPU5

结果：

| 指标 | 值 |
| --- | ---: |
| `signals` | `10` |
| `orders` | `10` |
| `filled` | `0` |
| max Ack RTT | `6.738ms` |
| max send-to-finish | `45.977ms` |
| corresponding `exchange_lifecycle_ns` | `37.336ms` |

这轮没有复现 `219ms` Ack RTT outlier。最大 send-to-finish 属于 IOC submit Ack 后到 Gate private order terminal update 的 lifecycle tail，不是 Ack path outlier。

2026-06-01 8 条 private plain RTT probe：

- Run 目录：`/home/liuxiang/tmp/gate_order_session_rtt_probe/20260601_011001_gate_rtt_private8_plain_30m_alllogs/`
- 临时配置：`/home/liuxiang/tmp/20260601_011001_gate_rtt_private8_plain_30m_alllogs/configs/`
- 连接：8 条 `fxws-private.gateapi.io:80` / `connect_ip=10.0.1.154` / `enable_tls=false`，worker CPU `6-13`。
- 节奏：`order_session_interval_us=1000000`，`cycle_cooldown_us=1000000`，`duration_sec=1800`。
- 诊断：`ack_rtt_threshold_ns=0`，每 Ack 写 sample CSV；`TCP_INFO` 与 software socket timestamping 全开。
- 样本：`1798` Ack，`diag_rows=1798`，`ts_stage_rows=1798`，`invalid=0`，`fill=0`，feedback routed `1798/1798`。
- 安全检查：run 后 12 个目标合约 open orders 为空；全账户 futures positions `size != 0` 为 0，`pending_orders` 为 0。

Ack RTT 分布：

| metric | value |
| --- | ---: |
| p50 | `0.613ms` |
| p95 | `0.842ms` |
| p99 | `2.632ms` |
| max | `18.709ms` |

最大样本：

| 字段 | 值 |
| --- | --- |
| session | `private-03` |
| contract | `PROVE_USDT` |
| local_order_id | `504403158265502382` |
| request_sequence | `215` |
| ack_rtt_ns | `18708540` |
| send_to_drive_read_ns | `29417` |
| drive_read_duration_ns | `21433` |
| max_runtime_loop_gap_ns | `51248` |
| write_complete_to_rx_software_ns | `18669441` |
| rx_software_to_ack_receive_ns | `26948` |
| tcp_info_rtt_us | `2687` |
| tcp_info_rttvar_us | `4648` |
| tcp_info_retrans / total_retrans / unacked | `0 / 0 / 0` |
| tcp_notsent_bytes | `0` |

最大样本的阶段拆解图：

```text
request_send_local_ns
  |
  | submit / encode / enqueue / send() / write_complete
  | 几微秒到十几微秒；未见本机写队列积压
  v
write_complete_ns
  |
  | 18.669ms  <-- 本轮最大样本的主要 tail
  | 覆盖本机 kernel / NIC、private link、Gate edge / app、回程到本机 kernel 的 software-level 大段
  v
ts_rx_software_ns
  |
  | 0.027ms
  | kernel 已拿到 Ack packet 后，用户态 `recvmsg()` / parse / ack callback
  v
ack_local_receive_ns
```

Top tail 样本均呈现同一形态：encode / enqueue / send / write complete 是微秒级，
`write_complete -> rx_software` 是 `12ms-18ms` 级，`rx_software -> ack_receive` 是十几到几十微秒。

因此这轮证据支持：

- 排除：本机 owner thread 长时间 deschedule、`DriveRead()` / Ack parse 慢、本机 write queue 积压、`tcp_notsent` backlog、明显 TCP retrans。
- 指向：请求完整写入本机 socket 后，到业务 Ack packet 进入本机 kernel 前的大段，可能覆盖本机内核 / NIC、private link、Gate edge、Gate 应用处理和回程路径。
- 限制：当前是 software timestamping，不能严格证明 packet leaves / returns NIC，也不能把剩余大段拆成 Gate 应用处理或网络单向延迟。若需要确认 NIC 边界，需要 hardware timestamp 或 pcap。

2026-06-01 8 条 private plain RTT probe + no TLS pcap：

- Run 目录：`/home/liuxiang/tmp/gate_order_session_rtt_probe/20260601_021256_gate_rtt_private8_plain_30m_pcap/`
- 临时配置：`/home/liuxiang/tmp/20260601_021256_gate_rtt_private8_plain_30m_pcap/configs/`
- pcap：`/home/liuxiang/tmp/20260601_021256_gate_rtt_private8_plain_30m_pcap/pcap/gate_private_plain_10.0.1.154_tcp80.pcap`
- pcap 对齐输出：`/home/liuxiang/tmp/20260601_021256_gate_rtt_private8_plain_30m_pcap/pcap_alignment_top25.csv`
- 路由：`10.0.1.154 dev enp55s0 src 10.0.1.103`。
- NIC timestamp 能力：`enp55s0` 只有 software TX/RX/system clock，`PTP Hardware Clock: none`，无 hardware TX/RX timestamp。
- tcpdump：`22409` packets captured / received by filter，`0` packets dropped by kernel。
- 样本：`1798` Ack，feedback routed `1798/1798`，`invalid=0`，`fill=0`。
- 安全检查：run 前后 12 个目标合约 open orders 为空；全账户 futures positions `size != 0` 为 0，`pending_orders` 为 0。

Ack RTT 分布：

| metric | value |
| --- | ---: |
| p50 | `0.632ms` |
| p95 | `0.879ms` |
| p99 | `4.465ms` |
| max | `25.921ms` |
| `>5ms` | `16 / 1798` |
| `>10ms` | `12 / 1798` |

`>5ms` tail 按 session 分布：

| session | count |
| --- | ---: |
| `private-00` | `1` |
| `private-01` | `6` |
| `private-02` | `2` |
| `private-03` | `1` |
| `private-04` | `1` |
| `private-05` | `4` |
| `private-06` | `1` |
| `private-07` | `0` |

最大样本：

| 字段 | 值 |
| --- | --- |
| session | `private-02` |
| contract | `SUI_USDT` |
| local_order_id | `504403158265495818` |
| request_sequence | `10` |
| ack_rtt_ns | `25921194` |
| tcp_info_rtt_us | `3755` |
| tcp_info_rttvar_us | `6570` |
| tcp_info_retrans / total_retrans / unacked | `0 / 0 / 0` |
| tcp_notsent_bytes | `0` |
| pcap request source port | `57842` |

最大样本的 pcap 阶段拆解：

```text
write_complete_ns
  |
  | -5.572us
  | 本机 pcap 已看到 request packet；负值来自 pcap 微秒精度 / 抓包点与软件时间戳误差
  v
pcap request packet
  |
  | 25.895ms  <-- 本轮最大样本的主要 tail
  | 覆盖去程网络、Gate edge TCP stack、Gate app / order path、回程网络
  v
pcap remote TCP ACK + WebSocket Ack response
  |
  | 0.095us 到 ts_rx_software，23.191us 到 ack_receive
  | Ack packet 回到本机 pcap 后，本机 RX / 用户态读取 / parse / callback 是微秒级
  v
ack_local_receive_ns
```

Top tail 的 pcap 形态一致：

- `write_complete -> pcap request` 是同一时刻附近，差值在几微秒内，pcap 微秒精度下可出现小负值。
- `pcap request -> remote TCP ACK` 与 `pcap request -> WebSocket Ack response` 相等；第一个 ACK request 的远端包就是携带业务 Ack response 的包，说明 peer 没有先单独 ACK request，而是等业务响应一起 piggyback。
- `Ack response pcap -> ts_rx_software / ack_receive` 是微秒级，不支持本机 RX 后处理作为 tail。
- top tail 的 `tcp_info_retrans=0`、`tcp_info_total_retrans=0`、`tcp_info_unacked=0`、`tcp_notsent_bytes=0`；pcap 中也没有支持 top tail 的 payload 重传证据。

因此这轮 pcap 证据把归因进一步收窄为：

- 排除：本机用户态 write / read / parse / callback、本机 TCP socket send queue、本机发送侧重传。
- 指向：request 已经出现在本机抓包点之后，到 Gate submit Ack response 回到本机抓包点之前的链路大段。
- 限制：由于 `enp55s0` 不支持 hardware timestamp，pcap 仍不能严格证明 packet 已经离开 / 返回物理 NIC，也不能把剩余大段拆成 private link 去程、Gate edge / app 处理或回程。

## 复现时怎么分析

优先看同一时钟域字段：

1. `send_to_first_drive_read_ns` 是否变大：owner thread 是否迟迟未进入 read。
2. `drive_read_duration_ns` / `max_observed_drive_read_duration_ns` 是否变大：socket read / frame decode / dispatch 是否慢。
3. write path 字段是否变大：encode、frame encode、enqueue、write pump、`send()` / `SSL_write()`。
4. `socket_sendq` / `tcp_notsent` / `TCP_INFO` 是否异常。
5. socket timestamping 是否可用，并按 `write_complete -> tx_software -> tx_ack -> rx_software -> ack_receive` 分段。
6. `pidstat` / `sar` / `perf sched` 是否显示 owner thread 被 deschedule 或 CPU 饱和。

同时必须区分：

- Ack RTT：`request_send_local_ns -> ack_local_receive_ns`。
- 本地 send-to-finish：`request_send_local_ns -> order_finished_local_ns`。
- Gate terminal lifecycle：`finish_exchange_ns - ack_exchange_ns`。

如果 Ack RTT 正常但 `exchange_lifecycle_ns` 或 `ack_to_finish_local_ns` 大，应按 Gate terminal lifecycle 分析，不并入 Ack path。

## 采样建议

复现 Ack latency 时并行采集：

```bash
pidstat -wt -p <strategy_pid>,<feedback_pid> 1
sar -u -P 2,3,4,5,6 1
sar -q 1
sar -w 1
```

如权限允许，短窗口补：

```bash
perf sched record -p <strategy_pid> -- sleep <duration>
perf sched latency
```

10 分钟 sysstat 只能证明趋势，不能解释 200ms 单点 outlier。

## 当前下一步

- `219ms` Ack RTT outlier 仍未复现，不基于 2026-05-25 单次样本改 order session 架构。
- 后续复现 Ack tail 时，继续按 `docs/diagnostic_fields.md` 中的 order write path、runtime loop、`TCP_INFO` 和 socket timestamping 字段归因。
- 对 `10ms-30ms` 级 private plain tail，若 no TLS pcap 显示 `pcap request -> WebSocket Ack response` 占主导，且 `Ack response pcap -> ack_receive` 为微秒级，则不要把原因归到本机 owner thread、read parse 或本机 TCP socket 路径。
- 若要继续拆 private link 去程、Gate edge / app 处理和回程，需要 hardware timestamp、链路侧 / Gate 侧证据或多端抓包；当前 `enp55s0` 不支持 hardware timestamp。
- 多连接 / 多 IP 对照用 `docs/gate_order_session_rtt_probe_design.md` 的 probe，不把 URL 相同当作路径相同的证据。
