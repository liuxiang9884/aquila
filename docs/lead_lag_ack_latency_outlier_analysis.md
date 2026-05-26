# LeadLag Ack Latency Outlier 分析

## 背景

2026-05-25 的 12-symbol guarded live-orders 1 小时运行已生成报告：

- Report 目录：`reports/20260525_133511_12pair_live_2ticks/`
- Latency CSV：`reports/20260525_133511_12pair_live_2ticks/latency.csv`
- Strategy log：`/home/liuxiang/log/lead_lag_strategy_requested_12symbols_live_20260522_20260525_133511.log`
- Guard stdout：`/home/liuxiang/tmp/lead_lag_live_12pairs_1h_2ticks_20260525_132912/guarded_live_retry1.stdout`

本轮真实订单运行最终 `normal_exit_flat`，`signals=58`、`orders=58`、`finished=58`、`filled=0`，全部 terminal `kCancelled`。因此该 run 只能证明 guarded run 和 cancelled-only feedback 闭环正常，不证明 IOC partial-fill / decimal filled close blocker 已复核。

本文只分析这轮 run 中 Ack RTT 的最大 outlier。

## 问题现象

`latency.csv` 中最高 Ack RTT：

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

计算方式：

```text
ack_rtt_ns = ack_local_receive_ns - request_send_local_ns
           = 219,023,023 ns
           = 219.023 ms
```

这笔订单的相邻高延迟排序：

| rank | `ack_rtt_ns` | symbol | local order |
| --- | ---: | --- | --- |
| 1 | `219.023ms` | `ZEC_USDT` | `288230376151711783` |
| 2 | `17.006ms` | `ZEC_USDT` | `288230376151711784` |
| 3 | `10.845ms` | `ZEC_USDT` | `288230376151711777` |
| 4 | `10.837ms` | `ZEC_USDT` | `288230376151711751` |
| 5 | `9.430ms` | `INJ_USDT` | `288230376151711760` |

整轮 58 个订单的 Ack RTT 概况：

| metric | value |
| --- | ---: |
| min | `3.638ms` |
| p50 | `5.628ms` |
| p90 | `7.098ms` |
| p95 | `10.845ms` |
| max | `219.023ms` |

## 字段口径

`Ack` 来自 **Gate order session**，不是 `gate_order_feedback_session`。

链路：

```text
lead_lag_strategy process
  -> Gate OrderSession WebSocket
  -> futures.order_place
  <- Gate submit Ack response
  -> gate_order_response kind=kAck
  -> lead_lag_order_response kind=kAck
```

`gate_order_feedback_session` 是独立进程，订阅 private `futures.orders`，负责后续 `kAccepted`、`kFilled`、`kCancelled`、`ContinuityLost` 等 terminal / state feedback。它不返回 submit Ack。

本轮 outlier 的 `request_send_local_ns` 和 `ack_local_receive_ns` 是同一个 strategy / order-session runtime 线程上的两个本地时间点：

- `request_send_local_ns`：order session 发送路径记录下单请求送入 WebSocket 发送路径的本地时间。
- `ack_local_receive_ns`：同一个 order session runtime 线程读到并解析 Gate submit Ack response 后记录的本地时间。

因此 `219ms` 不是跨线程消息传递延迟；它表示同一条 runtime 线程在 send 后到处理 Ack response 之间经过的本地时间。

## 关键日志

Strategy log 中这笔订单：

```text
14:26:12.171528286 gate_order_send_ok local_order_id=288230376151711783 request_sequence=40 request_send_local_ns=1779719172171523913
14:26:12.390548908 gate_order_response kind=kAck local_order_id=288230376151711783 local_receive_ns=1779719172390546936 exchange_ns=1779719172176961000 exchange_to_local_ns=213585936
14:26:12.390549359 lead_lag_order_response kind=kAck local_order_id=288230376151711783 ack_rtt_ns=219023023
14:26:12.462550722 lead_lag_order_feedback kind=kCancelled local_order_id=288230376151711783 ...
14:26:12.462552016 lead_lag_order_finished local_order_id=288230376151711783 ... ack_rtt_ns=219023023
```

相邻订单对照：

```text
ENA_USDT local_order_id=288230376151711782 ack_rtt_ns=5317203
ZEC_USDT local_order_id=288230376151711783 ack_rtt_ns=219023023
ZEC_USDT local_order_id=288230376151711784 ack_rtt_ns=17006394
```

`ack_exchange_ns` / `exchange_to_local_ns` 只能作为诊断辅助。因为本地机器和 Gate 是不同机器、不同 clock，不能把 `ack_exchange_ns - request_send_local_ns` 直接当作精确单程延迟或 Gate 处理耗时。更可靠的主指标仍然是同一机器本地时钟计算的 `ack_rtt_ns`。

## 已确认事实

### 1. CPU4 在 outlier 前后处于满载

`sar -u -P 4 -f /var/log/sysstat/sa25` 在 outlier 所在窗口显示：

```text
14:20:10 CPU4 user=54.90 system=45.10 idle=0.00
14:30:04 CPU4 user=54.88 system=45.12 idle=0.00
```

系统 run queue / context switch 同期：

```text
14:20:10 runq-sz=6 plist-sz=701 ldavg-1=6.10 cswch/s=59554.19
14:30:04 runq-sz=6 plist-sz=703 ldavg-1=6.00 cswch/s=60317.34
```

这些是 10 分钟粒度采样，不能证明 14:26:12 的 219ms 正好由调度造成，但能证明 outlier 所在区间 CPU4 长时间满载。

### 2. 多个关键组件绑在 CPU4

相关配置：

```toml
# config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml
[strategy.loop]
idle_policy = "spin"
bind_cpu_id = 4

# config/order_sessions/gate_order_session.toml
[order_session.websocket.execution_policy]
bind_cpu_id = 4

# config/order_feedback/gate_order_feedback_session.toml
[order_feedback_session.websocket.execution_policy]
bind_cpu_id = 4
```

feedback session 启动日志确认：

```text
gate_order_feedback_session ... bind_cpu_id=4
```

需要注意：`gate_order_feedback_session` 不处理 Ack，但会和 strategy / order session runtime 竞争同一个 CPU。

### 3. Strategy runtime 是 active spin

Strategy summary：

```text
loop_iterations=4069517740
idle_iterations=4068183208
```

这说明 strategy runtime 在 1 小时内大量空转轮询。当前 `RuntimePolicy` 只设置 affinity / mlock / prefault，没有设置 `SCHED_FIFO` / `SCHED_RR`；线程仍是普通 `SCHED_OTHER`，多个 active-spin 进程绑同一 CPU 时会由 CFS 分时。

### 4. 仍不能证明具体卡在哪一段

现有日志只能定位到：

```text
request_send_local_ns
  -> 同一 runtime 线程下一次读到并解析 Ack
ack_local_receive_ns
```

无法继续拆分为：

- OS deschedule / run queue waiting。
- 同一线程在 runtime hook / data reader / strategy logic 中忙，导致 `DriveRead()` 晚执行。
- socket / TLS read 返回晚。
- frame decode / response dispatch 晚。
- 网络路径或 Gate 下发路径抖动。

其中“同一线程在 runtime hook / data reader / strategy logic 中忙”尤其需要单独验证。当前 active spin loop 每轮大致顺序是：

```text
BeforeDrive() / RuntimeHook
DriveWrite()
DriveRead()
```

`RuntimeHook` 内会轮询 order feedback、data reader，并调用 strategy hooks。若这里偶发耗时过长，也会推迟 `DriveRead()`，表现为 Ack 本地 receive 时间变晚。

## 当前判断

更准确的结论是：

> 这笔 `219ms` Ack RTT outlier 发生在同一条 order session runtime 线程的 send 与 receive Ack 之间。CPU4 同期满载，且 strategy/order session/feedback session 都绑在 CPU4 并 active spin，因此本地调度或同核竞争是强候选；但现有日志还不能区分 OS deschedule、runtime hook 内部耗时、socket/TLS read 延迟或网络路径抖动。

不应表述为：

> Ack 被另一个 session 或另一个线程处理慢了。

也不应仅凭 `ack_exchange_ns` 表述为：

> Gate 5ms 内处理完，剩下 213ms 一定是本地调度。

`ack_exchange_ns` 可作为辅助线索，但跨机器 clock 不能直接证明单程耗时。

## 下一轮验证建议

### 1. 拆 CPU

下一轮 live run 前先避免明显同核竞争：

- strategy / order session runtime 保持 CPU4。
- `gate_order_feedback_session` 改到 CPU6 或 CPU7。
- recorder 不使用 CPU4；已经存在的长期 recorder 若继续运行，应固定到非交易 CPU。
- data session 保持 Gate CPU2、Binance CPU3。
- log backend CPU5 可保留。

期望：如果 p95/p99 明显下降，说明同核竞争是主要因素之一。

### 2. 加 runtime 分段时间戳

在 WebSocket active loop / order session runtime 增加低频异常诊断，不在每轮热路径打印日志，只在超过阈值时输出。建议字段：

```text
loop_iteration_start_ns
before_runtime_hook_ns
after_runtime_hook_ns
before_drive_write_ns
after_drive_write_ns
before_drive_read_ns
after_drive_read_ns
runtime_hook_duration_ns
drive_read_duration_ns
loop_gap_ns
last_order_send_local_ns
last_order_send_to_next_drive_read_ns
```

触发条件：

```text
runtime_hook_duration_ns > 1ms
drive_read_duration_ns > 1ms
last_order_send_to_next_drive_read_ns > 3ms
ack_rtt_ns > 20ms
```

这能区分：

- `runtime_hook_duration_ns` 很大：data reader / strategy / feedback polling 推迟 read。
- `before_drive_read_ns - request_send_local_ns` 很大，但 hook 不大：线程可能被 deschedule 或 socket read 未及时返回。
- `drive_read_duration_ns` 很大：socket / TLS / frame decode 路径需要继续拆。

### 3. 运行期采集调度证据

live run 时并行采集：

```bash
pidstat -wt -p <strategy_pid>,<feedback_pid> 1
```

如需要更强证据，短窗口运行：

```bash
perf sched record -p <strategy_pid> -- sleep <duration>
perf sched latency
```

若环境权限不足，至少保留：

```bash
sar -u -P 4 1
sar -q 1
sar -w 1
```

10 分钟 sysstat 只能证明趋势，不能解释 200ms 单点 outlier。

### 4. 报告中保留 outlier 上下文

后续 report 中建议新增或派生：

```text
ack_outlier_rank
ack_rtt_bucket
send_to_next_drive_read_ns
runtime_hook_max_ns_near_order
cpu_id_strategy
cpu_id_order_session
cpu_id_feedback_session
perf_sched_available
```

其中新增字段需要先通过日志或 runtime diagnostics 产生；当前系统没有这些字段。

## 下一步优先级

1. 先拆 CPU，避免 strategy / order session / feedback session 全部绑 CPU4。
2. 增加 order session active loop 的异常分段诊断，尤其是 `send_to_next_drive_read_ns` 和 `runtime_hook_duration_ns`。
3. 下一轮 live run 同时跑 1 秒粒度 `pidstat` / `sar`，必要时短窗口 `perf sched`。
4. 生成 report 时把 Ack RTT outlier 与调度证据一起保存，避免只凭单个 `ack_rtt_ns` 猜原因。
