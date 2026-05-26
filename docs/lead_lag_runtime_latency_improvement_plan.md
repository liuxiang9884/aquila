# LeadLag Runtime Latency 改进计划

## 目的

本文记录 LeadLag live-orders runtime latency 讨论、待验证假设和后续实现计划。当前阶段只落计划，不开始实现；所有代码、配置和实盘运行变更都必须等本计划讨论完毕后再执行。

相关事实源：

- `docs/lead_lag_ack_latency_outlier_analysis.md`
- `reports/20260525_133511_12pair_live_2ticks/latency.csv`
- `/home/liuxiang/log/lead_lag_strategy_requested_12symbols_live_20260522_20260525_133511.log`
- `/home/liuxiang/tmp/lead_lag_live_12pairs_1h_2ticks_20260525_132912/guarded_live_retry1.stdout`

## 当前状态

- 2026-05-25 的 12-symbol guarded live-orders 1 小时运行正常退出 flat，`signals=58`、`orders=58`、`finished=58`、`filled=0`。
- 本轮只证明 guarded run 和 cancelled-only feedback 闭环正常，不证明 IOC partial-fill / decimal filled close blocker 已复核。
- `latency.csv` 中最大 Ack RTT 是 `219.023ms`，对应 `request_sequence=40`、`local_order_id=288230376151711783`、`symbol=ZEC_USDT`。
- `request_send_local_ns` 与 `ack_local_receive_ns` 都在同一个 strategy / Gate order-session owner thread 里记录；不要把该 outlier 解释为跨线程消息传递延迟，也不要归因到 `gate_order_feedback_session` 处理 Ack 慢。

## Latency 讨论结论

### 现象

目标订单：

| 字段 | 值 |
| --- | --- |
| `request_sequence` | `40` |
| `local_order_id` | `288230376151711783` |
| `symbol` | `ZEC_USDT` |
| `request_send_local_ns` | `1779719172171523913` |
| `ack_local_receive_ns` | `1779719172390546936` |
| `ack_exchange_ns` | `1779719172176961000` |
| `ack_rtt_ns` | `219023023` |

派生指标：

```text
ack_rtt_ns = ack_local_receive_ns - request_send_local_ns
           = 219,023,023 ns

send_to_exchange_ns = ack_exchange_ns - request_send_local_ns
                    = 5,437,087 ns

exchange_to_local_ns = ack_local_receive_ns - ack_exchange_ns
                     = 213,585,936 ns
```

`ack_exchange_ns` 来自 Gate header 的 `x_out_time` / `response_time`。本地机器和 Gate 不是同一时钟，因此不能把 `send_to_exchange_ns` 或 `exchange_to_local_ns` 当作严格单程网络延迟；但同一 run 内的相对分布仍有诊断价值。本轮 `exchange_to_local_ns` 的 p95 约 `4.25ms`，只有 seq40 达到 `213.586ms`。

### 已排除或降级的解释

- **跨线程消息传递**：降级。起点和终点都在同一个 owner thread。
- **feedback session Ack 慢**：排除。Ack 来自 Gate order session submit response，不来自 private `futures.orders` feedback session。
- **仅凭 Gate clock 断言本地调度慢**：不能成立。跨机器 clock 不能直接证明单程路径，但可以作为异常排序线索。

### 仍成立的候选

1. **同一 owner thread 在 send 后没有及时进入 `DriveRead()`**
   - 当前 active loop 顺序是 `BeforeDrive()` / runtime hook -> `DriveWrite()` -> `DriveRead()`。
   - runtime hook 内会 poll order feedback、poll data reader、调用 strategy hook。
   - 如果 send 后下一次 `DriveRead()` 被推迟，就会表现为 `ack_local_receive_ns` 晚。

2. **同一 owner thread 被 OS deschedule**
   - CPU4 在 outlier 所在 10 分钟 sysstat 窗口长期 `idle=0.00`。
   - strategy loop、Gate order session、Gate order feedback session 都绑 CPU4，并且 active spin。
   - 这只能说明调度 / 同核竞争是强候选，不能证明 seq40 的 219ms 就是 deschedule。

3. **socket / TLS / read path 晚返回**
   - Ack 可能在网络或 TLS 层晚到达。
   - 也可能数据已经可读，但 owner thread 晚进入 `SSL_read()`。
   - 现有日志没有 `DriveRead()` 前后时间，无法区分。

4. **frame decode / response dispatch 晚**
   - 当前 `ack_local_receive_ns` 在 `OrderSession::HandleText()` 入口附近记录。
   - 如果 `DriveRead()` 已经读到 bytes，但 delivery / parser 之后才记录 Ack，需要进一步拆 `DriveRead()` 内部。
   - 现有日志不足以验证。

## Latency 诊断改进计划

### 已定稿边界

第一阶段采用 **Gate `OrderSession` 专用 Ack latency diagnostic**，不先做通用 WebSocket loop observer。

理由：

- 当前问题只针对 Gate submit Ack 的 `request_send_local_ns -> ack_local_receive_ns`，诊断状态天然绑定 `local_order_id`、`request_sequence`、`ack_rtt_ns` 和 `inflight_count`。
- 通用 WebSocket observer 需要额外抽象 hook、状态对象和回调接口，当前阶段成本高于收益。
- 专用诊断可以只在订单 send 后 arm，正常无订单时不取时、不打日志。
- 如果下一轮证据证明该模式对其它 WebSocket session 也有价值，再抽成通用 loop observer。

执行顺序：

1. Phase 1：实现 Gate `OrderSession` 专用 Ack latency diagnostic。
2. Phase 2：根据 live 证据决定是否抽象为通用 WebSocket loop observer。

### 设计目标

- 只诊断同一 owner thread 内 `request_send_local_ns -> ack_local_receive_ns` 的时间分布。
- 不引入跨线程解释，不把 feedback session 纳入 Ack path。
- 不在每轮 active loop 打日志。
- 只在订单发送后 arm 一个轻量诊断窗口，Ack 到达或超时后解除。
- 每个 `request_sequence` 每类异常最多打一条日志，并保留进程级 rate limit。
- 正常无订单时不调用额外取时函数。

### 建议日志形态

新增一类低频异常日志，例如：

```text
gate_order_latency_diag reason=<reason>
  local_order_id=<id>
  request_sequence=<seq>
  request_send_local_ns=<ns>
  ack_local_receive_ns=<ns>
  ack_exchange_ns=<ns>
  ack_rtt_ns=<ns>
  send_to_first_after_hook_ns=<ns>
  send_to_first_drive_read_ns=<ns>
  drive_read_duration_ns=<ns>
  max_observed_drive_read_duration_ns=<ns>
  inflight=<count>
```

`reason` 第一版只保留少量枚举文本：

- `ack_rtt_threshold`
- `send_to_drive_read_threshold`
- `drive_read_duration_threshold`
- `diagnostic_timeout`

### 计时点

第一版必需新增 3 个计时点：

1. `after_runtime_hook_ns`
   - 位置：`BasicWebSocketClient::RuntimeSession::BeforeDrive()` 调用 runtime hook 之后。
   - 目的：看下单后本线程是否至少完成过一次 runtime hook。

2. `before_drive_read_ns`
   - 位置：active loop 的 `DriveWrite()` 之后、`DriveRead()` 之前。
   - 目的：计算 `send_to_first_drive_read_ns`。

3. `after_drive_read_ns`
   - 位置：active loop 的 `DriveRead()` 返回之后。
   - 目的：计算 `drive_read_duration_ns = after_drive_read_ns - before_drive_read_ns`。

如果后续必须精确拆 `runtime_hook_duration_ns`，再追加第 4 个计时点：

```text
before_runtime_hook_ns
```

第一版不默认添加第 4 个计时点，避免在还没有证据前扩大热路径取时面。

### 触发阈值

第一版阈值：

```text
ack_rtt_ns > 20ms
send_to_first_drive_read_ns > 3ms
drive_read_duration_ns > 1ms
diagnostic_window_age_ns > 250ms
```

日志限制：

- 每个 `request_sequence` 对每个 reason 最多输出 1 次。
- 进程级 rate limit 初始建议为每秒最多 10 条。
- 正常 run 中预期日志量接近 0；按 2026-05-25 run 的 58 单分布，`ack_rtt_ns > 20ms` 只会触发 seq40 一笔。

### 推荐实现落点

后续进入实现前，再按代码现状细化接口。当前建议优先看这些文件：

| 文件 | 用途 |
| --- | --- |
| `core/websocket/active_spin_loop.h` | active loop 顺序：`BeforeDrive()`、`DriveWrite()`、`DriveRead()`。 |
| `core/websocket/websocket_client.h` | `RuntimeSession::BeforeDrive()` 调用 `RunRuntimeHook()`，适合作为 loop 边界观测入口。 |
| `exchange/gate/trading/order_session.h` | 记录 `request_send_local_ns`、处理 Gate submit Ack、输出 `gate_order_response`。 |
| `exchange/gate/trading/order_session_runtime_adapter.h` | runtime adapter 把 `OrderResponse` 传给 trading runtime。 |
| `core/trading/trading_runtime.h` | runtime hook 内 poll feedback、poll data reader、调用 strategy hooks。 |
| `tools/lead_lag/live_strategy.cpp` | LeadLag live runner 使用 `TradingRuntimeDiagnostics`，可决定是否打印 summary。 |

### 验证计划

实现前不运行 live-orders。实现后最小验证顺序应为：

1. 构建和单元测试：

```bash
./build.sh debug
ctest --test-dir build/debug -R '(websocket_|gate_order|order_session|lead_lag)' --output-on-failure
git diff --check
```

2. 本地或 dry-run 验证：

```bash
./build/debug/tools/gate_demo_strategy --config config/strategies/demo_strategy.toml
```

3. 下一轮 live 验证必须同时采集：

```bash
pidstat -wt -p <strategy_pid>,<feedback_pid> 1
sar -u -P 4 1
sar -q 1
sar -w 1
```

4. 如权限允许，在异常窗口短时间采集：

```bash
perf sched record -p <strategy_pid> -- sleep <duration>
perf sched latency
```

## 独立问题：order session inflight 泄漏风险

本轮日志中每笔订单都有 `gate_order_response_ignored reason=unknown_kind`，且 `OrderSession` 在 Ack 时不清理 `request_id_to_local_order_id_`，只在 final result / error 处理路径清理。当前 `inflight` 随 request sequence 递增，例如 seq40 发送时 `inflight=39`。

这不是 219ms Ack RTT 的直接解释，但它是长时间 live-orders 运行的独立稳定性风险：

- request map capacity 当前默认 `16384`，短 run 不会触顶。
- 长时间运行或高频下单时，若 final result 持续被解析为 `unknown_kind`，最终可能触发 `kInflightFull`。
- 该问题需要单独分析 Gate final submit response 的实际 payload shape、parser profile 和清理时机；不要把它和 Ack RTT 根因混在同一个修复里。

后续讨论项：

1. 是否先修 parser / cleanup，再做 latency instrumentation。
2. Ack 后是否允许保留 request correlation 直到 final result。
3. 如果 final result 可能长期无法按预期解析，是否需要按 Ack + private `futures.orders` feedback 清理 correlation。

## CPU 拆分讨论入口

已定稿：下一轮 live run 前先避免核心行情和交易链路的明显同核竞争。当前机器 `CPU 0-31` 全在 `NUMA node0`，`Thread(s) per core = 1`，因此下面的核心绑核方案满足“不同 core、同一 NUMA”要求。

核心行情和交易链路：

| CPU | 组件 | 说明 |
| --- | --- | --- |
| CPU2 | Gate market data session | Gate `BookTicker` producer。 |
| CPU3 | Binance market data session | Binance `BookTicker` producer。 |
| CPU4 | LeadLag strategy + Gate `OrderSession` owner thread | Ack path 的 `request_send_local_ns` 和 `ack_local_receive_ns` 都在该线程。strategy `DataReader` 也在 runtime hook 内由该线程调用。 |
| CPU6 | Gate `OrderFeedbackSession` | 从 CPU4 拆出，避免和 Ack path 的 owner thread active-spin 竞争。 |
| CPU5 | log backend | 可保留在同 NUMA，但不算交易热链路。 |

非核心辅助进程：

- `data_reader_recorder`、TUI、report、guard、shell 不纳入“核心链路同 NUMA”硬约束。
- 这些辅助进程不得绑定到 CPU2 / CPU3 / CPU4 / CPU6。
- 如需运行 recorder 或 TUI，优先放到 CPU7+ 或其它非核心 core；只要不抢核心 core 即可。

执行顺序：

1. Phase 1A：实现 Gate `OrderSession` 专用 Ack latency diagnostic。
2. Phase 1B：整理 live run affinity profile，把 `gate_order_feedback_session` 从 CPU4 移到 CPU6，保留 Gate / Binance data session 在 CPU2 / CPU3，strategy + order owner 在 CPU4。
3. Phase 1C：live 验证报告中明确记录 `diagnostic_enabled=true` 和 `affinity_split=true`。如需严格归因，可先短跑 `diagnostic_only`，再短跑 `diagnostic + split_cpu`。

## 待继续讨论

在开始执行前，至少还需要讨论并定稿：

1. `inflight` 泄漏风险是否排在 latency instrumentation 前面。
2. 下一轮 live smoke 的范围：只做 signal / dry-run、单 symbol 小额真实订单，还是 12-symbol guarded smoke。
3. report 侧是否新增 latency outlier 字段，例如 `send_to_first_drive_read_ns`、`drive_read_duration_ns`、`diag_reason`。

## 当前执行边界

- 本文落地后仍不开始实现。
- 继续讨论上述未定项，并把结论追加到本文。
- 等所有讨论项定稿后，再拆成具体实现任务、测试任务和提交顺序。
- 最终计划定稿前，不启动新的无人值守真实订单长跑。
