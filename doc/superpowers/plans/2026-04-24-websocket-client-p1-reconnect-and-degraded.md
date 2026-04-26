# P1：WebSocket Client 重连与降级（G7 + G9）

> **给执行 agent 的说明：** 必备子技能：使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按 task 逐项执行。步骤使用 `- [ ]` 复选框追踪。
>
> **讨论闸门：** 每个 Task 开头都有 "Scope Decision" 块，必须先与人类协作者确认后再进入 Step 1。未确认前不允许开始实现。

## 文档信息

- 版本：`v0.2`（补 HFT 合规要求：benchmark 回归、E2E 夹具、滑动窗口实现、xorshift、锁域约束、整数化）
- 状态：`已完成`
- 创建日期：`2026-04-24`
- 关联文档：
  - `doc/websocket_client_design_v1.0.md`
  - `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
  - `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`
  - `doc/superpowers/plans/2026-04-24-websocket-client-p0-production-safety.md`（前置 Phase）

## 目标

给 `core/websocket` 补齐"能自愈"的能力。P0 解决了"不自杀"——nonce 随机、背压非致命、冷路径有超时——但连接一旦断开，`WebSocketClient::Start()` 仍然立即返回 `false` 就再也不管了。P1 把这一环补上：

- **G7** — 引入按失败原因分类的重连 / 退避循环。`kReconnectBackoff` 真正落地。
- **G9** — 引入 `kDegraded` 作为 `kActive` 与 `kReconnectBackoff` 之间的预警态，给控制面"断链前就能看到"的机会。

## 不在范围内（推到 P2+）

- G1 / G10 零拷贝 FrameCodec 重写
- G2 多轮 `DriveRead`
- G4 / G8 心跳协调、时钟粒度
- 冷路径分阶段超时拆分（P0 已落总预算；分阶段拆分仍在 G5 后续工作清单里）
- 跨 Phase 并行（本 P1 分支不做 P2-A 的任何前期改动）

## 架构原则

- 重连循环落在 `WebSocketClient`（Layer 2）。`CriticalSession`（Layer 1）继续保持"用户驱动 + 纯状态容器"，只负责标记 `should_reconnect_` / `last_error_`。Layer 1 用户自己写 reconnect 的路径保留。
- Degraded 的进入 / 退出决策也落在 Layer 2，不污染热路径。
- 任何新增阈值、退避参数、分类映射都**可配置**但**有安全默认值**，空配置不应阻塞 `Start()`。
- 重连路径不是热路径，允许使用 `std::this_thread::sleep_for`、`std::condition_variable`、必要时分配少量内存；但**进入 active spin 之后**仍要遵守 P0 的热路径零分配约束。

## 延迟 / 确定性原则

- Backoff sleep **不允许 busy-spin**；必须让 CPU 真正休息，否则等于再次做"活着的僵尸"。
- Degraded 判定使用现有 metrics 作为输入，**不新增热路径时间戳采集**（RTT-based 触发器留给 P2-B 做完 G8 后再接）。
- 重连过程中的分配只能发生在冷路径；active spin 期间的 `CriticalSession` 状态复位应走 `Reset()` 而非重构对象，避免重新分配 `read_buffer_storage_` 等大块内存。
- `Stop()` 唤醒统一走 `eventfd + epoll_wait`（详见 Task 1 决策 6）。热路径 spin loop 不触及任何等待原语；`Stop()` 的 `eventfd_write` 是外部线程偶发 syscall，与热路径零内存共享。
- 任何"滑动窗口"类观测量（例如每秒背压丢帧数）必须用**定长环形快照**实现：每 N 个 spin 周期记录 `(now_ns, counter)`，评估时做差分。禁止"每事件 push/pop" 或"每次评估遍历队列"类设计。
- RNG 用于 jitter 等冷路径目的时，**禁止 `std::mt19937`**（状态 ~2.5KB、构造重）；使用内联的 `xorshift64` 或 `splitmix64`（8 字节状态）。

---

## 文件清单

### G7 — 重连 / 退避 / 失败分类
- 修改：`core/websocket/types.h`（在 `ConnectionConfig` 加 reconnect 配置子结构）
- 修改：`core/websocket/critical_session.h`（新增 `Reset()` 方法；清空 pending_writes / codec / awaiting_pong / should_reconnect / last_error）
- 修改：`core/websocket/state_machine.h`（如需要，补充辅助方法；保持薄）
- 修改：`core/websocket/websocket_client.h`（重连循环、分类器、backoff 计算、`kReconnectBackoff` 状态通知、`Stop()` 唤醒机制）
- 修改：`core/websocket/metrics.h`（确认 `reconnects` 在此阶段被驱动）
- 修改：`core/websocket/cold_path_loop.h`（每次重连 `Close/Init` 重建 epoll_fd；新增 `SetInterruptFd(int)` + 把该 fd 加入 epoll 集合，使握手阶段可被 Stop 中断）
- 修改：`test/websocket/critical_session_test.cpp`（Reset 相关 unit test）
- 创建：`test/websocket/reconnect_classifier_test.cpp`（失败分类表驱动测试）
- 创建：`test/websocket/websocket_client_reconnect_test.cpp`（端到端：注入失败 → 观察 backoff / 分类器 / 最终 Stop 的协同）

### G9 — Degraded 状态
- 修改：`core/websocket/types.h`（新增 `ConnectionPhase::kDegraded`、`DegradedThresholds` 结构体挂在 `ConnectionConfig` 下）
- 修改：`core/websocket/websocket_client.h`（Degraded 进入 / 退出判定，状态通知）
- 修改：`core/websocket/metrics.h`（新增 `degraded_enter_count`、`degraded_exit_count`、`degraded_active` 快照字段）
- 修改：`test/websocket/websocket_client_reconnect_test.cpp` 或新建 `degraded_test.cpp`（阈值触发与回落测试）

---

## Task 1：G7 — 重连、退避、失败分类

### Scope Decision（Step 1 之前必须确认）

**决策 1：配置结构（HFT 风格：整数 + 位移，冷路径也避开浮点）**

新增 `ReconnectPolicy` 子结构，挂在 `ConnectionConfig` 下：

```cpp
struct ReconnectPolicy {
  bool enabled = true;
  std::uint32_t initial_backoff_ms = 100;
  std::uint32_t max_backoff_ms = 30'000;
  // Multiplier = 1 << shift_bits (1=x2, 2=x4); 0 means constant backoff.
  std::uint8_t backoff_shift_bits = 1;
  // Jitter ∈ [-jitter_percent, +jitter_percent] applied as integer pct.
  std::uint8_t jitter_percent = 25;
  // 0 = unlimited (until Stop() or fatal-class error).
  std::uint32_t max_attempts = 0;
};
```

Backoff 计算（纯整数）：
```
raw = min(initial_backoff_ms << (attempt * shift_bits), max_backoff_ms)
rand_pct = xorshift64() % (2 * jitter_percent + 1) - jitter_percent
final_ms = raw * (100 + rand_pct) / 100
```

RNG：`WebSocketClient` 持有一个 `std::uint64_t xorshift_state_`，构造时用
`steady_clock::now().time_since_epoch().count() ^ pthread_self()` 播种。

**决策 2：失败原因分类器**

```cpp
enum class FailureClass : std::uint8_t {
  kTransient,   // 走退避重连
  kFatal,       // 停止重试，等待外部介入
};

FailureClass Classify(ConnectionError error) noexcept;
```

默认映射：

| `ConnectionError` | `FailureClass` | 备注 |
|---|---|---|
| `kNone` | — | 不会走分类器 |
| `kResolveFailure` | **`kFatal`** | DNS 解析失败通常是配置错 |
| `kSocketError` | `kTransient` | |
| `kConnectTimeout` | `kTransient` | |
| `kTlsFailure` | `kTransient` | 证书过期一类也会映射进来；此 P1 阶段不区分 |
| `kHandshakeFailure` | **`kFatal`** | WS handshake 失败通常是 URL / 协议版本问题 |
| `kProtocolError` | `kTransient` | 偶发帧错误允许重试 |
| `kHeartbeatTimeout` | `kTransient` | |
| `kPeerClosed` | `kTransient` | |
| `kConsumerFatal` | **`kFatal`** | 上层消费者显式声明不再能处理，必须人工介入 |

**开放问题**：`kTlsFailure` 是否该细分？默认先归 Transient；以后如果出现"证书过期后无限重试"这种症状再拆。

**决策 3：重连循环落位**

在 `WebSocketClient::Start()` 内部循环：

```
loop:
  run cold path → active spin
  if stop_requested: return ok
  if core.should_reconnect:
    classify core.last_error
    if kFatal: return fail, phase=kClosed
    compute backoff ms
    state=kReconnectBackoff; notify
    interruptible_sleep(backoff_ms)
    if stop_requested: return ok
    core.Reset()
    tls_socket_.Close()  # cold_path_loop_ 新建一个还是复用？决策 4
    continue
```

**决策 4：`ColdPathLoop` 复用策略**

- **选项 A（推荐）**：保持现在的 `ColdPathLoop` 为成员，调用之间只 `Close` 掉 epoll_fd 并重建。`epoll_create1` 每次重连 ~μs 级，可以接受。
- **选项 B**：新加 `ColdPathLoop::ResetForReconnect()` 保留 epoll_fd，手动 `epoll_ctl(DEL)` 掉旧的 socket fd。优点零分配，缺点多一个状态面。

P1 选 A，代码变更最小。P2 性能收敛阶段若 benchmark 表明 epoll 重建有尾延迟问题，再换 B。

**决策 5：`CriticalSession::Reset()`**

```cpp
void Reset() noexcept;  // 复位 pending_writes_、codec_、awaiting_pong_、last_ping_ns_、should_reconnect_、last_error_、metrics_ 不动
```

- 不重新分配 `read_buffer_storage_` / `pending_writes_`。
- `prepared_write_arena_` 由外部持有，Reset 时如果 pending_writes_ 里还挂着节点，必须先一个个 `Release` 回 arena。
- `Metrics` 不在 Reset 中清零，`reconnects` 计数由 WebSocketClient 层在每次成功重连前 `++`。

**决策 6：统一用 eventfd 中断，冷路径 + backoff 共用一个唤醒机制**

选 **C（eventfd + epoll_wait）**，而不是 `std::mutex` + `std::condition_variable`。原因：

- 代码库冷路径已经用 epoll；mutex/cv 是第二种"等待 + 唤醒"机制，没必要。
- eventfd 只是一个 `int` 成员，Stop 线程写它是 syscall，**不触及 `WebSocketClient` 任何内存字段** —— 零 false sharing 风险，无需维护 `alignas(64)` 布局约束。
- 顺手修复一个现存问题：当前冷路径握手期间 `Stop()` 要等 `cold_path_total_timeout_ms`（10s）才能返回；接入 eventfd 后变成 ~μs 响应。

**实现要点**：

```cpp
// WebSocketClient
int wakeup_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);  // ctor
// Stop():
stop_requested_.store(true, std::memory_order_release);
const std::uint64_t one = 1;
::write(wakeup_fd_, &one, sizeof(one));

// ColdPathLoop
void SetInterruptFd(int fd) noexcept;  // WebSocketClient 在 Start() 前调用一次
// WaitForSocket 内部把 wakeup_fd 加入 epoll 集合；若它 ready 则返回 WaitOutcome::kInterrupted

// Backoff sleep（在 WebSocketClient 内部）
// 直接 epoll_wait(own_epoll_fd_, ..., remaining_backoff_ms)，集合里只有 wakeup_fd。
// 若返回事件 → stop 触发；若返回 0 → 超时，backoff 结束。
```

**语义落位**：
- Stop 中断不映射到任何 `ConnectionError`（Stop 不是错误）。
- 冷路径被中断 → `state_machine.Enter(ConnectionPhase::kClosing)` → `RunUntilActive` 返回 `false`（"非错误"语义）。
- `WebSocketClient::Start()` 看到 `stop_requested_ == true` + 当前 phase 为 `kClosing` → 正常走 Stop 分支返回 `true`。
- **不新增 `ConnectionError::kStopped`**，保持 `ConnectionError` 枚举只含真正的错误。

**一次性消耗 wakeup_fd 的计数**：
- eventfd 的计数器会累加；每次从 epoll 检测到 wakeup_fd ready 时，读一次把计数清零，避免重复触发。
- `Stop()` 可能被多次调用；幂等（计数再+1 无副作用）。

**决策 7：状态机驱动**

- 成功重连回到 `kActive` 时，`state_machine.Enter(kActive)` 会清 `last_error`（现有行为）。
- 进入 backoff：`state_machine.Fail(<last_error>, kReconnectBackoff)` —— 在 state_machine.h 里 `Fail` 同时设 error 和 phase，刚好符合。
- `metrics_.reconnects++` 放在每次**成功** 从 cold_path 返回之后（表示完成了一次重连闭环），而不是进入 backoff 时。

**决策 8：测试策略（P1 内必须有 E2E 证据，不能推到 P3）**

1. **分类器表驱动**（`reconnect_classifier_test.cpp`）：对每个 `ConnectionError` 枚举值断言其 `FailureClass`。magic_enum 遍历 + expected table。
2. **Backoff 算法**（`backoff_compute_test.cpp`）：
   - `attempt=0..N` 下 `ComputeBackoffMs` 的基线值（不加 jitter）满足 `initial_backoff_ms << (attempt * shift_bits)` 上限 `max_backoff_ms`
   - jitter 范围 `[raw*(100-j)/100, raw*(100+j)/100]`（用 fixed xorshift seed 让测试可复现）
   - `backoff_shift_bits = 0` 时恒等于 `initial_backoff_ms`
3. **CriticalSession Reset**（`critical_session_test.cpp` 追加）：触发一次 backpressure→`Reset()`→再喂一条正常帧，验证 pending_writes 为空、`awaiting_pong_ == false`、`last_error_ == kNone`、arena free count 回满。
4. **`Stop()` 唤醒 backoff**（在下面 4 合并）：`Start()` 进入 backoff → 外部线程调用 `Stop()` → `Start()` 在 O(线程唤醒) 时间内返回。
5. **端到端重连**（`websocket_client_reconnect_test.cpp` + 新增 `test/websocket/tls_blackhole_server.h`）：
   - 夹具：本地 TLS 服务器，用 OpenSSL 自签证书 / 自签 CA；接受 TCP → 完成 TLS 握手 → 读取 HTTP → 返回 101 Switching Protocols → 立刻 close。
   - **变体 1 — 稳态断链重连**：`WebSocketClient::Start()` 连该夹具 → active → 被关 → 重连退避 → 再次连上 → 再次被关 → 观察到 `metrics.reconnects >= 2` 且状态回调序列包含 `kActive → kReconnectBackoff → kActive`。
   - **变体 2 — 指数退避**：夹具对前 N 次连接直接拒绝（例如 TLS 握手超时），观察 backoff 确实按指数增长；最后一次接受后进入 `kActive`。
   - **变体 3 — max_attempts 上限**：配置 `max_attempts = 3` → 第 3 次仍失败 → `Start()` 返回 false 且 `phase = kClosed`。
   - **变体 4 — Stop 中断冷路径握手**（决策 6 选 eventfd 之后新增）：夹具 accept TCP 后**不发 TLS 字节**，客户端在 TLS handshake 阶段卡住；外部线程调 `Stop()` → `Start()` 在 ~μs 内返回 true，`phase = kClosing`，不计入 `reconnects`。

**为什么必须 P1 内做 E2E**：AGENTS.md "先跑证据，再宣称完成" 明确禁止"算法对 → 假设集成也对"。TLS 夹具代码一次性投入（~100 行 boilerplate），比 P3 压测便宜得多，且可复用给 P3。

**不做的事**：
- 不把 `WebSocketClient` 模板化（保持 `TlsSocket` 直接成员，避免侵入式重构）。
- 不做跨机器 / 真实网络的 reconnect 压测（P3 负责）。

### 步骤

- [x] **Step 1**：写失败测试
  - `reconnect_classifier_test.cpp`：对全量 `ConnectionError` 枚举断言 `FailureClass`
  - `backoff_compute_test.cpp`：基线值 + jitter 范围 + `shift_bits=0` 恒定
  - `critical_session_test.cpp` 追加 `ResetClearsPendingAndFlags`
  - `test/websocket/tls_blackhole_server.h`（TLS 夹具）+ `websocket_client_reconnect_test.cpp`（E2E）
- [x] **Step 2**：跑测试，预期 FAIL
- [x] **Step 3**：实现
  - `types.h`：`ReconnectPolicy`、`ConnectionConfig::reconnect`
  - 新建（或放进 `websocket_client.h` 的 anonymous namespace）：`Classify` + `ComputeBackoffMs(attempt, policy, rng)`
  - `critical_session.h`：`Reset()`
  - `websocket_client.h`：重连循环、`backoff_cv_`、`Stop()` 唤醒
- [x] **Step 4**：跑测试 + live probe + **benchmark 回归**（详见最终验证清单）
- [x] **Step 5**：提交（建议拆成多个原子提交：types+classify、backoff、critical_session.Reset、tls_blackhole 夹具、websocket_client.reconnect_loop；如果某一项净改动 > 150 行，单独立一个提交）

---

## Task 2：G9 — Degraded 状态

### Scope Decision（Step 1 之前必须确认）

**决策 1：Phase 位置**

新增 `ConnectionPhase::kDegraded`，在 `kActive` 和 `kReconnectBackoff` 之间。枚举顺序：

```
kDisconnected, kResolving, kTcpConnecting, kTlsHandshaking,
kWsHandshaking, kActive, kDegraded, kReconnectBackoff, kClosing, kClosed
```

**决策 2：触发因子（P1 阶段用哪些）**

P1 阶段只使用 P0 已经有的观测量，不引入新的时间戳采集：

| 触发因子 | 使用的 metric / 状态 | 默认阈值 |
|---|---|---|
| 写队列积压 | `pending_count / prepared_write_slots` ≥ `high_watermark_ratio` 持续 `hold_ticks` 轮 | 0.8 / 8 轮 |
| 背压丢帧 | 滑动窗口内 `consumer_backpressure_drops` 增量 ≥ 阈值 | 10 / 每秒 |
| 心跳挂起 | `awaiting_pong_` 持续 > `awaiting_pong_timeout_degraded_ms` | 3000ms |

**"滑动窗口"的强制实现方式**（HFT 合规，不允许其他写法）：

```cpp
struct BackpressureWindow {
  static constexpr size_t kSlots = 16;  // 2^4, fixed
  std::uint64_t counter_snapshot[kSlots]{};
  std::uint64_t timestamp_ns[kSlots]{};
  std::uint32_t write_index = 0;
  // On each evaluation tick: snapshot (now_ns, metrics.consumer_backpressure_drops)
  // into write_index, advance. To read "drops in last 1s", find the oldest slot
  // with timestamp >= now_ns - 1e9 and compute counter delta.
};
```

总内存 = 16 * 16 字节 = 256 字节，定长、零堆分配。禁止改为 `std::deque`、`std::vector`、每事件 push/pop、或任何变长结构。

**不在 P1 范围**：
- 心跳 RTT 分布（需要 G8 的时钟改造）
- owner-thread 交付延迟（需要 P2-B 的观测点）

**决策 3：退出条件**

所有触发因子**连续** `recover_ticks` 轮回落到阈值以下（默认 16 轮，约 2× hold_ticks，避免震荡）。

**决策 4：阈值配置（整数化）**

```cpp
struct DegradedThresholds {
  // High watermark expressed as percent to avoid double on the hot path.
  std::uint8_t high_watermark_percent = 80;  // 0 disables this trigger
  std::uint32_t high_watermark_hold_ticks = 8;
  std::uint32_t recover_ticks = 16;
  std::uint32_t backpressure_drops_per_second = 10;  // 0 disables
  std::uint32_t awaiting_pong_timeout_ms = 3000;     // 0 disables
  // Cadence at which the spin loop invokes the evaluator. Defaults to
  // RuntimePolicy::spin_iterations_before_clock_check for convenience;
  // override only if evidence shows coupling with the clock tick hurts.
  std::uint32_t evaluation_interval_iterations = 0;
};
```

挂在 `ConnectionConfig::degraded`，默认启用。所有触发因子单独可关（阈值设 0）。

**决策 5：判定落位与节奏解耦**

- 判定落在 `WebSocketClient::RuntimeSession` 内部，每 `evaluation_interval_iterations` 次 spin 调一次 `evaluator_.Evaluate(...)`。
- 默认复用 `RuntimePolicy::spin_iterations_before_clock_check` 的节奏，但**保留独立字段**（`evaluation_interval_iterations`）用以后续解耦。如果两种节奏后面证明需要差异化（例如 Degraded 评估应更稀疏），只改配置不改代码。
- Evaluator 成本必须 < 100ns/次（纯计数器比较 + 滑动窗口差分）；Step 4 benchmark 需验证此假设。

**决策 6：状态通知**

- 进入 / 退出 Degraded 都通过现有的 `StateHandler` 通知 `kDegraded` / `kActive`。
- 新增 metrics：
  - `degraded_enter_count` / `degraded_exit_count`：累计
  - `degraded_active`（0/1）：当前是否在 Degraded（方便外部快照）

**决策 7：测试策略（含 E2E，不推 P3）**

1. **`degraded_evaluator_test.cpp`**（纯单元）：
   - (a) 三个触发因子各自单独达到阈值 → 进入
   - (b) 多触发同时 → 只进入一次、`degraded_enter_count` += 1
   - (c) 全部回落 `recover_ticks` 轮后 → 退出
   - (d) 震荡（在触发 / 回落之间反复）下不会频繁切换（通过 hold/recover 计数器验证）
   - (e) 滑动窗口快照：喂入 `(timestamp, counter)` 序列，验证窗口覆盖 1s 内的差分
   - (f) 阈值设 0 → 对应触发器禁用
2. **E2E**：复用 Task 1 的 `tls_blackhole_server.h` 夹具，配置为 "accept 后故意不回应心跳"（让 `awaiting_pong_` 超过阈值）→ 观察 WebSocketClient 状态从 `kActive → kDegraded`，`degraded_active == 1`；再让夹具继续拖 → 超过 `heartbeat_timeout_ms` → 断链 → 进入 `kReconnectBackoff`。

### 步骤

- [x] **Step 1**：写失败测试（`degraded_evaluator_test.cpp`）
- [x] **Step 2**：FAIL
- [x] **Step 3**：实现
  - `types.h`：`kDegraded`、`DegradedThresholds`
  - 新建 `core/websocket/degraded_evaluator.h`（header-only，纯函数 / 轻状态）
  - `metrics.h`：新增计数器
  - `websocket_client.h`：集成 evaluator，放到 spin 节奏点
- [x] **Step 4**：全量测试通过；手动场景：`prepared_write_slots = 4`，业务写 4 条但不 DriveWrite，心跳 tick 多轮，观察进入 Degraded
- [x] **Step 5**：提交（建议拆 2-3 个原子提交）

---

## 两个 Task 完成后的最终验证

- [x] `./build.sh debug && ctest --test-dir build/debug --output-on-failure -R "websocket_"` 全通过
- [x] live probe 对 Gate 仍能成功握手 + active spin
- [x] TLS blackhole 夹具的端到端 reconnect 测试通过（Task 1 决策 8 和 Task 2 决策 7 的 E2E 用例）
- [x] **Benchmark 回归（AGENTS.md 硬约束）**：以 `main` 分支（P1 合入前）为基线，跑 `session_read_path_benchmark`、`session_write_path_benchmark`、`active_spin_benchmark`、`frame_codec_benchmark`、`prepared_write_benchmark`；对比 P1 分支结果，确认 p50 / p99 / p99.9 无可观测回归。若出现回归，停下来定位而不是宣告完成。
  - 记录在 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` 的 G7 / G9 验证证据块中，附具体数值
  - benchmark 必须标注：CPU id、是否开 affinity、是否启用 `mlockall`、运行时长、样本数
- [x] 更新 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`：G7 和 G9 补上"处理方案 / 关联提交 / 验证证据 / 确认日期"块
- [x] README 不强制更新；若引入了新的配置字段，可在 P3 一次性补文档

## 规范覆盖检查

- G7 覆盖 v1.0 "重连决策在控制面完成，真正的断开、重建连接、重新认证动作由 I/O owner 执行"、"重连策略必须支持退避和抖动"、"对认证失败、配置错误、协议错误，应允许控制面进入暂停重试或人工介入状态"。
- G9 覆盖 v1.0 "Degraded 不是已经断开，而是连接仍在，但健康度或时延特征已经异常"、"让控制面可以先降级、限流或准备重连，而不是等到彻底断链才反应"。
- 仍未关闭：v1.0 要求的细粒度 RTT-based Degraded 触发（待 P2-B）。

## Placeholder 扫描

- Task 1 决策 2 的"TLS 是否细分"为唯一开放问题（默认归 Transient）；决策 8 的"是否引入模板替身"已解决（不引入，走 TLS blackhole 夹具）。
- Task 2 决策 2 的触发因子 **不含** RTT-based，已明确推到 P2-B。

## 执行注意

- 即便进入 `CriticalSession::Reset()` 实现阶段，不要顺手重写 codec（G1 范围）。
- `backoff_cv_` 仅用于唤醒 `Stop()`；不要把它变成"通用事件通道"，热路径不允许触及。
- 如果某个 Task 的 Step 3 净改动超过约 150 行（P1 两个 Task 本身比 P0 大，但每个 Task 内部仍应收敛），停下来重新讨论 scope。
- 引入新的配置字段时，**必须**给出安全默认值；空配置应与"关闭该功能"等价，不应把现有 caller 推入未初始化行为。
