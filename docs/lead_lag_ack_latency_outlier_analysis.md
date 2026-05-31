# LeadLag Ack Latency Outlier 分析

## 结论

2026-05-25 的 `219.023ms` Ack RTT outlier 发生在同一条 strategy / Gate `OrderSession` owner thread 的 send 与 receive Ack 之间。它不是 `gate_order_feedback_session` 处理 Ack 慢，也不是跨线程消息传递延迟。

当前证据只能说明 CPU4 同期满载、且 strategy / order session / feedback session 当时都绑 CPU4 并 active-spin，因此本地调度或同核竞争是强候选；但原始日志不能证明具体卡在 OS deschedule、runtime hook、socket read / parse、网络路径还是 Gate 侧。

该问题当前是 inactive investigation：等待复现后，用新增 write path、runtime loop、`TCP_INFO` 和 socket timestamping 字段归因。

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

- 等待 Ack RTT outlier 复现，不再基于单次样本改 order session 架构。
- 复现后按 `docs/diagnostic_fields.md` 中的 order write path、runtime loop、`TCP_INFO` 和 socket timestamping 字段归因。
- 多连接 / 多 IP 对照用 `docs/gate_order_session_rtt_probe_design.md` 的 probe，不把 URL 相同当作路径相同的证据。
