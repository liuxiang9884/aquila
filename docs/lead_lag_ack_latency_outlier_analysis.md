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

## 2026-05-26 拆核 30 分钟 run 补充证据

2026-05-26 的 12-symbol guarded live-orders 30 分钟 run 已生成报告：

- Report 目录：`reports/20260526_043440_12pair_live_30m/`
- Run 目录：`/home/liuxiang/tmp/20260526_043440_12pair_live_30m/`
- 策略与 order session owner 使用 CPU4，Gate order feedback 使用 CPU6，Gate / Binance data session 分别使用 CPU2 / CPU3，log backend 使用 CPU5。

本轮最终 `normal_exit_flat`，`signals=10`、`orders=10`、`finished=10`、`filled=0`，全部 terminal `kCancelled`。未出现 `gate_order_ack_latency_diagnostic`、`ContinuityLost`、`needs_reconcile` 或 `manual_intervention`。

Ack RTT 概况：

| metric | value |
| --- | ---: |
| min | `3.016ms` |
| p50 | `3.193ms` |
| avg | `3.890ms` |
| p95 | `6.738ms` |
| max | `6.738ms` |

这轮没有复现 `219ms` 级别 Ack RTT outlier。最大 send-to-finish 出现在：

| 字段 | 值 |
| --- | --- |
| `local_order_id` | `288230376151711749` |
| `exchange_order_id` | `51509920985043349` |
| `symbol` | `DASH_USDT` |
| `action` | `kOpenShort` |
| `status` | `kCancelled` |
| `finish_reason` | `kImmediateOrCancel` |
| `ack_rtt_ns` | `6738050` |
| `send_to_finish_local_ns` | `45976983` |
| `exchange_lifecycle_ns` | `37336000` |

该订单的 exchange timestamp 显示：

```text
ack_exchange_ns    = 1779771255433664000
finish_exchange_ns = 1779771255471000000
exchange_lifecycle_ns = 37,336,000 ns = 37.336 ms
```

本地时间闭环显示：

```text
request_send_local_ns   = 1779771255428674541
ack_local_receive_ns    = 1779771255435412591
order_finished_local_ns = 1779771255474651524
ack_rtt_ns              = 6.738 ms
send_to_finish_local_ns = 45.977 ms
ack_to_finish_local_ns  = 39.239 ms
```

结论：这不是 Ack path outlier，而是 IOC submit Ack 后到 Gate private order terminal update 的 lifecycle 延迟。由于 `exchange_lifecycle_ns` 只使用 Gate exchange timestamp，可用于观察 Gate 侧 Ack 到终态 update 的相对间隔；但它不说明本地和交易所之间的单程网络延迟，也不应和 `ack_rtt_ns` 混为一个指标。后续报告需要同时保留 Ack RTT、send-to-finish 本地闭环和 exchange Ack-to-finish，避免把交易所终态生命周期延迟误判成 Ack receive 延迟。

截至 2026-05-28，`exchange_lifecycle_ns` / terminal lifecycle outlier 的当前候选假设是 Gate 交易所内部订单队列或 IOC terminal lifecycle 延迟；该判断仍未被复现样本证明，只作为后续分析标签。若后续 live run 再出现 Ack RTT 正常但 `exchange_lifecycle_ns` 或 `ack_to_finish_local_ns` 明显偏高，应优先按交易所侧 terminal lifecycle 方向分析，并记录 symbol、order type、`finish_as`、remote endpoint 和 session 维度信息。

## 后续验证建议

拆 CPU、Gate `OrderSession` 专用 Ack latency diagnostic、affinity profile overlay、report diagnostic 字段和 exchange Ack-to-finish 字段已经落地。2026-05-26 的 30 分钟拆核 run 没有复现 Ack RTT outlier，因此 2026-05-25 的 `219.023ms` Ack RTT 当前处于 inactive investigation 状态：不继续凭单次样本推断根因，等待后续复现后再结合新 diagnostic / 调度证据归因。

后续 live run 如果继续复核 Ack latency，应并行采集：

```bash
pidstat -wt -p <strategy_pid>,<feedback_pid> 1
sar -u -P 2,3,4,5,6 1
sar -q 1
sar -w 1
```

如需要更强证据，短窗口运行：

```bash
perf sched record -p <strategy_pid> -- sleep <duration>
perf sched latency
```

若环境权限不足，至少保留：

```bash
sar -u -P 2,3,4,5,6 1
sar -q 1
sar -w 1
```

10 分钟 sysstat 只能证明趋势，不能解释 200ms 单点 outlier。

复现 Ack RTT outlier 时优先看：

1. `send_to_first_drive_read_ns` 是否显著增大：判断 owner thread 是否在 send 后迟迟未进入 read。
2. `drive_read_duration_ns` / `max_observed_drive_read_duration_ns` 是否显著增大：判断 socket / TLS / frame decode 路径是否耗时。
3. `pidstat` / `perf sched` 是否显示 strategy owner thread 被 deschedule。
4. `exchange_lifecycle_ns` 是否同步变大：若只是 exchange Ack-to-finish 变大，应归入 Gate terminal lifecycle，而不是 Ack RTT。

复现 terminal lifecycle outlier 时优先看：

1. `ack_rtt_ns` 是否仍处于常态范围，避免把 Ack path 与 terminal lifecycle 混在一起。
2. `exchange_lifecycle_ns`、`ack_to_finish_local_ns` 和 `send_to_finish_local_ns` 是否同步变大。
3. `finish_as`、status、symbol、order role、IOC 参数和是否集中在特定合约或下单形态。
4. remote endpoint、local port、`order_session_id` 和可选 `TCP_INFO`，判断是否存在连接维度聚集。

当前仍未完成的是 Ack RTT outlier 和 terminal lifecycle outlier 的复现与归因。按 2026-05-27 当前接手决策，IOC partial-fill / decimal filled close 不再作为当前阶段 active blocker；后续若 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再恢复 targeted small smoke 复查。
