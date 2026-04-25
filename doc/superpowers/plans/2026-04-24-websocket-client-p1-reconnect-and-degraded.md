# P1：WebSocket Client 重连与降级（G7 + G9）

> **给执行 agent 的说明：** 必备子技能：使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按 task 逐项执行。步骤使用 `- [ ]` 复选框追踪。
>
> **讨论闸门：** 每个 Task 开头都有 "Scope Decision" 块，必须先与人类协作者确认后再进入 Step 1。未确认前不允许开始实现。

## 文档信息

- 版本：`v0.1`
- 状态：`待讨论`
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

---

## 文件清单

### G7 — 重连 / 退避 / 失败分类
- 修改：`core/websocket/types.h`（在 `ConnectionConfig` 加 reconnect 配置子结构）
- 修改：`core/websocket/critical_session.h`（新增 `Reset()` 方法；清空 pending_writes / codec / awaiting_pong / should_reconnect / last_error）
- 修改：`core/websocket/state_machine.h`（如需要，补充辅助方法；保持薄）
- 修改：`core/websocket/websocket_client.h`（重连循环、分类器、backoff 计算、`kReconnectBackoff` 状态通知、`Stop()` 唤醒机制）
- 修改：`core/websocket/metrics.h`（确认 `reconnects` 在此阶段被驱动）
- 修改：`core/websocket/cold_path_loop.h`（若需要在连接 Reset 后复用，可能需要一个 `ResetForReconnect()`；但优先让 `ColdPathLoop` 继续"一次性"，每次重连新建一个 epoll_fd 最简单 —— 在 Scope Decision 中确认）
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

**决策 1：配置结构**

新增 `ReconnectPolicy` 子结构，挂在 `ConnectionConfig` 下：

```cpp
struct ReconnectPolicy {
  bool enabled = true;
  std::uint32_t initial_backoff_ms = 100;
  std::uint32_t max_backoff_ms = 30'000;
  double backoff_multiplier = 2.0;
  // Jitter factor applied multiplicatively, uniform in [1 - jitter, 1 + jitter).
  double jitter = 0.25;
  // 0 = unlimited (until Stop() or fatal-class error).
  std::uint32_t max_attempts = 0;
};
```

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

**决策 6：可中断 sleep**

- **选项 A（推荐）**：在 `WebSocketClient` 新增 `std::condition_variable backoff_cv_` + `std::mutex backoff_mtx_`，`Stop()` 里 `notify_all` 唤醒。
- **选项 B**：分片 sleep（例如每 100ms 检查一次 `stop_requested_`）。简单但 `Stop()` 最大延迟 100ms。
- **选项 C**：`eventfd` + epoll_wait。低延迟但引入额外 fd。

P1 选 A。`Stop()` 响应时间从"直到 backoff 结束"缩短到 O(线程唤醒)，符合 v1.0 "不允许在读写热路径里直接触发复杂重连流程，也不允许同步阻塞"的精神。

**决策 7：状态机驱动**

- 成功重连回到 `kActive` 时，`state_machine.Enter(kActive)` 会清 `last_error`（现有行为）。
- 进入 backoff：`state_machine.Fail(<last_error>, kReconnectBackoff)` —— 在 state_machine.h 里 `Fail` 同时设 error 和 phase，刚好符合。
- `metrics_.reconnects++` 放在每次**成功** 从 cold_path 返回之后（表示完成了一次重连闭环），而不是进入 backoff 时。

**决策 8：测试策略**

1. **分类器表驱动**（`reconnect_classifier_test.cpp`）：对每个 `ConnectionError` 枚举值断言其 `FailureClass`。magic_enum 遍历 + expected table。
2. **CriticalSession Reset**（`critical_session_test.cpp` 追加）：触发一次 backpressure→`Reset()`→再喂一条正常帧，验证状态干净。
3. **WebSocketClient 重连端到端**（`websocket_client_reconnect_test.cpp`）：这个较复杂，需要一个可编程行为的 FakeSocket + FakeColdPath 或类似。**开放问题**：是否引入新的测试替身？

**开放问题**：对 Layer 2 的端到端测试最难的是 `TlsSocket` 不好替换。考虑：
- 把 `WebSocketClient` 模板化 `template <typename TlsSocketT, typename ColdPathT>` —— 侵入性大。
- 写一个本地 TLS echo server（openssl 自签证书启动一个 mini server）—— 代码多但测试真实度高。
- 只对 Classify 和 Backoff 算法做单元测试，端到端等 P3 长稳压测覆盖。

**默认提案**：P1 只测 Classify + Backoff 算法 + `CriticalSession::Reset`；端到端 reconnect 测试留给 P3（用 P3 的长稳 / 压测手段，避免把 WebSocketClient 模板化）。

### 步骤

- [ ] **Step 1**：写失败测试
  - `reconnect_classifier_test.cpp`：对全量 `ConnectionError` 枚举断言 `FailureClass`
  - `critical_session_test.cpp` 追加 `ResetClearsPendingAndFlags`
  - 新增 `backoff_compute_test.cpp`（如果 Backoff 是独立 free function）或在 classifier test 同文件内
- [ ] **Step 2**：跑测试，预期 FAIL
- [ ] **Step 3**：实现
  - `types.h`：`ReconnectPolicy`、`ConnectionConfig::reconnect`
  - 新建（或放进 `websocket_client.h` 的 anonymous namespace）：`Classify` + `ComputeBackoffMs(attempt, policy, rng)`
  - `critical_session.h`：`Reset()`
  - `websocket_client.h`：重连循环、`backoff_cv_`、`Stop()` 唤醒
- [ ] **Step 4**：跑测试 + live probe + 手动黑洞端口模拟（`cold_path_loop_test` 已验证，可以复用思路让 websocket_client 连黑洞，观察重连打点）
- [ ] **Step 5**：提交（可能拆成 3 个原子提交：types+classify、critical_session.Reset、websocket_client.reconnect_loop）

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

**不在 P1 范围**：
- 心跳 RTT 分布（需要 G8 的时钟改造）
- owner-thread 交付延迟（需要 P2-B 的观测点）

**决策 3：退出条件**

所有触发因子**连续** `recover_ticks` 轮回落到阈值以下（默认 16 轮，约 2× hold_ticks，避免震荡）。

**决策 4：阈值配置**

```cpp
struct DegradedThresholds {
  double high_watermark_ratio = 0.8;
  std::uint32_t high_watermark_hold_ticks = 8;
  std::uint32_t recover_ticks = 16;
  std::uint32_t backpressure_drops_per_second = 10;
  std::uint32_t awaiting_pong_timeout_ms = 3000;
};
```

挂在 `ConnectionConfig::degraded`，默认启用。设 `high_watermark_ratio = 0.0` 或类似可以关闭。

**决策 5：判定落位**

在 `WebSocketClient` 层的 spin loop 之前 / 之后的某个节奏点做判定。提案：`RuntimeSession` 里每 N 次 spin 调用一次 `EvaluateDegraded()`。N 可以复用 `spin_iterations_before_clock_check`（P0 已有）。

**决策 6：状态通知**

- 进入 / 退出 Degraded 都通过现有的 `StateHandler` 通知 `kDegraded` / `kActive`。
- 新增 metrics：
  - `degraded_enter_count` / `degraded_exit_count`：累计
  - `degraded_active`（0/1）：当前是否在 Degraded（方便外部快照）

**决策 7：测试策略**

`degraded_test.cpp`（或 `websocket_client_reconnect_test.cpp` 追加部分）：

- 直接用一个 `DegradedEvaluator`（把判定逻辑提取成 free 结构体或类，单元可测）
- 注入 metrics 快照 + prepared_write_high_watermark 等参数，断言：(a) 各触发因子单独达到阈值 → 进入；(b) 多触发因子同时达到 → 进入；(c) 全部回落 `recover_ticks` 轮后 → 退出；(d) 震荡场景（时好时坏）不会频繁切换

**开放问题**：`DegradedEvaluator` 提出来后，WebSocketClient 持有它的实例。单元测试只测 Evaluator 类；端到端 Degraded 行为（触发→通知→metrics 更新）留到 P3 长稳压测。

### 步骤

- [ ] **Step 1**：写失败测试（`degraded_evaluator_test.cpp`）
- [ ] **Step 2**：FAIL
- [ ] **Step 3**：实现
  - `types.h`：`kDegraded`、`DegradedThresholds`
  - 新建 `core/websocket/degraded_evaluator.h`（header-only，纯函数 / 轻状态）
  - `metrics.h`：新增计数器
  - `websocket_client.h`：集成 evaluator，放到 spin 节奏点
- [ ] **Step 4**：全量测试通过；手动场景：`prepared_write_slots = 4`，业务写 4 条但不 DriveWrite，心跳 tick 多轮，观察进入 Degraded
- [ ] **Step 5**：提交（建议拆 2-3 个原子提交）

---

## 三个（两个）Task 完成后的最终验证

- [ ] `./build.sh debug && ctest --test-dir build/debug --output-on-failure -R "websocket_"` 全通过
- [ ] live probe 对 Gate 仍能成功握手 + active spin
- [ ] 手动黑洞重连场景：`websocket_probe --host 127.0.0.1 --port <blackhole>`（或 iptables 模拟），观察指数退避 + 可中断
- [ ] 更新 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`：G7 和 G9 补上"处理方案 / 关联提交 / 验证证据 / 确认日期"块
- [ ] README 不强制更新；若引入了新的配置字段，可在 P3 一次性补文档

## 规范覆盖检查

- G7 覆盖 v1.0 "重连决策在控制面完成，真正的断开、重建连接、重新认证动作由 I/O owner 执行"、"重连策略必须支持退避和抖动"、"对认证失败、配置错误、协议错误，应允许控制面进入暂停重试或人工介入状态"。
- G9 覆盖 v1.0 "Degraded 不是已经断开，而是连接仍在，但健康度或时延特征已经异常"、"让控制面可以先降级、限流或准备重连，而不是等到彻底断链才反应"。
- 仍未关闭：v1.0 要求的细粒度 RTT-based Degraded 触发（待 P2-B）。

## Placeholder 扫描

- Task 1 决策 2 的"TLS 是否细分"、决策 8 的"是否引入模板替身"为开放问题。
- Task 2 决策 2 的触发因子 **不含** RTT-based，已明确推到 P2-B。

## 执行注意

- 即便进入 `CriticalSession::Reset()` 实现阶段，不要顺手重写 codec（G1 范围）。
- `backoff_cv_` 仅用于唤醒 `Stop()`；不要把它变成"通用事件通道"，热路径不允许触及。
- 如果某个 Task 的 Step 3 净改动超过约 150 行（P1 两个 Task 本身比 P0 大，但每个 Task 内部仍应收敛），停下来重新讨论 scope。
- 引入新的配置字段时，**必须**给出安全默认值；空配置应与"关闭该功能"等价，不应把现有 caller 推入未初始化行为。
