# WebSocket Client P2-B 热路径节奏设计

## 文档信息

- 状态：`draft`
- 日期：`2026-04-26`
- 分支：`p2b-hotpath-pacing`
- 范围：G2 `DriveRead` 节奏、G4 heartbeat/control write 协调、G8 clock cadence
- 前置：P1 重连 / Degraded 已完成；P2-A FrameCodec mirrored ring decode 主体已完成

## 目标

P2-B 解决 P2-A 之后仍然存在的热路径节奏问题：

1. `CriticalSession::DriveRead()` 当前每轮只调用一次 `ReadSome()`，TLS 内部或内核中已经可读的后续 record 可能要等下一轮 spin。
2. heartbeat ping / auto-pong 当前和业务写共用 pending queue，业务队列满时 control frame 会失败，且 ping 可能被未开始发送的业务帧拖延。
3. `ActiveSpinLoop`、heartbeat 和 degraded evaluation 的 clock cadence 仍然分散，存在重复取时和策略不可比较的问题。

本阶段不改变 P2-A 的 FrameCodec decode 主体，不引入跨线程队列，不把热路径改成 epoll 或阻塞式等待。

## G2：Read Pump 设计

### 方案

在 `CriticalSession::DriveRead()` 中引入 **bounded read pump**：

```cpp
for read_op in 0..max_reads_per_drive:
  writable = codec.WritableSpan()
  if writable empty -> capacity metric, stop

  n = tls_socket.ReadSome(writable)
  if n > 0:
    codec.CommitWritten(n)
    drain codec.Poll() until NeedMore/error
    if should_reconnect -> stop
    if should_continue_reading() -> continue
    stop

  if EAGAIN -> stop
  else -> reconnect
```

新增配置建议：

```cpp
std::uint32_t max_reads_per_drive = 1;
bool read_until_would_block = false;
```

语义：

- `max_reads_per_drive = 1` 保持当前行为。
- `read_until_would_block = false` 时，只有 socket 明确报告已有 buffered plaintext 时才继续读；这条路径不为了探测空 socket 额外打一发 EAGAIN。
- `read_until_would_block = true` 时，只要上一轮 `ReadSome()` 成功且未达到预算，就继续读到 EAGAIN 或预算耗尽；这能 drain 多个 TLS record，但单帧常见路径会多一次可能返回 EAGAIN 的 read。

`TlsSocket` 增加：

```cpp
size_t PendingReadableBytes() const noexcept;
```

Linux / OpenSSL 下实现为 `SSL_pending(ssl_)`。测试 fake socket 和 benchmark local socket 提供同名方法，便于模板代码统一调用。

### 默认值取舍

默认保持 `max_reads_per_drive = 1`、`read_until_would_block = false`，避免默认路径产生额外 syscall。生产环境如果确认行情 burst 下 TLS record 连续到达，可以通过 benchmark 后把 `max_reads_per_drive` 调到 `4` 或 `8`，并按场景决定是否打开 `read_until_would_block`。

### 验收

- 不影响当前单帧 read path 的 p50。
- 在模拟多 TLS record / 多 read chunk 的 benchmark 中，bounded pump 能减少 drain 完所有消息所需的 `DriveRead()` 次数。
- `read_until_would_block = false` 时，无 pending plaintext 不做第二次 read。

## G4：Control Frame 写协调

### 方案

为 heartbeat ping / auto-pong 增加一个连接本地的 **dedicated control slot**，不再占用业务 `PreparedWriteArena` slot。

核心状态：

```cpp
std::array<std::byte, 131> control_write_storage_;
PreparedWrite control_write_;
bool control_write_pending_;
std::uint64_t control_queued_ns_;
```

`131` 字节来自客户端 control frame 最大编码长度：

```text
2 byte header + 4 byte client mask + 125 byte control payload
```

发送顺序：

1. 如果业务 front write 已经部分发送，先完成该业务 frame，避免 WebSocket frame 字节交错。
2. 如果没有部分业务 frame 且 control slot pending，优先发送 control frame。
3. control frame 发送完成后，继续发送业务 pending queue。

入队语义：

- heartbeat ping 和 auto-pong 都尝试占用 dedicated control slot。
- control slot 忙时，递增 `control_frame_enqueue_failures`，不重连。
- 业务队列满不再影响 ping / pong 入队。

指标：

```cpp
std::uint64_t heartbeat_ping_send_delay_ns;
std::uint64_t heartbeat_ping_send_delay_max_ns;
```

只在 heartbeat ping 真正开始写出时记录 `now - control_queued_ns_`。取时发生在“control ping pending 且本轮首次写出”的低频分支，不污染常规业务写路径。

### 取舍

control slot 不允许打断已经部分发送的业务 frame。这牺牲极端情况下的 ping 最短延迟，但保持 WebSocket frame 边界正确，也避免在 `DriveWrite()` 中引入复杂的 scatter/gather 或 frame split 状态。

### 验收

- 业务 pending queue 满时，heartbeat ping 仍可入 dedicated control slot。
- 未开始发送的业务 frame 前，control frame 可以优先发送。
- 已部分发送的业务 frame 不被 control frame 字节打断。
- auto-pong 在业务 queue 满时不再因为业务 slot 耗尽失败。

## G8：Clock Cadence 设计

### 方案

先做 **clock source 抽象 + benchmark + 取时复用**，不直接上 TSC。

新增：

```cpp
enum class ClockSource : std::uint8_t {
  kSteady,
  kMonotonic,
  kMonotonicCoarse,
};

struct RuntimePolicy {
  ClockSource clock_source = ClockSource::kSteady;
};

std::uint64_t NowNs(ClockSource source) noexcept;
```

实现：

- `kSteady`：保持当前 `std::chrono::steady_clock` 语义。
- `kMonotonic`：Linux 下 `clock_gettime(CLOCK_MONOTONIC)`。
- `kMonotonicCoarse`：Linux 下 `clock_gettime(CLOCK_MONOTONIC_COARSE)`。
- 非 Linux 或 syscall 不可用时回退到 `steady_clock`。

`ActiveSpinLoop` 用 `runtime_policy.clock_source` 取时，并支持 session 提供两个可选方法：

```cpp
std::uint32_t ClockCheckInterval(std::uint32_t default_interval) const noexcept;
void AdvanceClock(std::uint64_t now_ns,
                  std::uint32_t elapsed_iterations) noexcept;
```

普通 session 仍然只需要实现现有 `AdvanceHeartbeat(now_ns)`。`WebSocketClient::RuntimeSession` 实现上述两个方法：

- `ClockCheckInterval()` 返回 heartbeat clock interval 与 degraded evaluation interval 的较小值。
- `AdvanceClock()` 用同一个 `now_ns` 调用 `core.AdvanceHeartbeat(now_ns)`，并按 `elapsed_iterations` 累计 degraded evaluation interval。

这样 degraded evaluation 不再在 `RuntimeSession::DriveRead()` 内单独调用 `steady_clock::now()`，但 `evaluation_interval_iterations` 仍保持“spin iteration 数”的语义。

不在 P2-B 引入 calibrated TSC。原因：

- TSC 需要频率校准、跨核稳定性和虚拟化环境判断。
- 当前项目还没有 clock calibration 基础设施。
- P2-B 先用 benchmark 判断 `steady_clock` / `clock_gettime` 是否已经足够。

### 验收

- 新增 clock benchmark 比较三种 source 的 p50 / p99 / p99.9。
- `active_spin_benchmark` 比较默认 source 前后结果。
- degraded evaluation 不再在 `RuntimeSession::DriveRead()` 内独立调用 `steady_clock::now()`，且 `evaluation_interval_iterations` 的 iteration 语义保持不变。

## 测试与 Benchmark

必须新增或扩展：

- `websocket_critical_session_test`
  - bounded read pump 多 chunk 同轮 drain
  - 无 pending 时不额外 read
  - control slot 在业务 queue 满时仍可发送 ping / pong
  - partially-written business frame 不被 control frame 打断
- `websocket_types_test`
  - 新配置字段默认值
  - `ClockSource` enum 可见
- `session_read_path_benchmark`
  - 单帧 legacy path
  - 多 read chunk / burst path
  - `read_until_would_block` variant
- `session_write_path_benchmark`
  - business write baseline
  - heartbeat control slot under full business queue
- 新增 `clock_source_benchmark`
  - `steady_clock`
  - `CLOCK_MONOTONIC`
  - `CLOCK_MONOTONIC_COARSE`

## 性能判断原则

- 默认配置不能让单帧 read/write p50 明显退化。
- 优化是否成立看 p50 / p99 / p99.9，不只看平均值。
- 如果 bounded read pump 只改善 synthetic burst，但真实 session benchmark 无收益，默认保持 legacy 行为，只保留可配置能力。
- 如果 `CLOCK_MONOTONIC_COARSE` p50 更低但粒度不足以支撑 heartbeat/degraded 判断，不能作为默认 source。
