# `core/websocket` 现状与 v1.0 设计 / 实施计划差距分析

## 文档信息

- 版本：`v0.1`
- 状态：`待讨论`
- 创建日期：`2026-04-24`
- 记录位置：`doc/reviews/`
- 关联文档：
  - `doc/websocket_client_design_v1.0.md`
  - `doc/superpowers/plans/2026-04-23-websocket-client-minimal-hft.md`
  - `doc/project_structure.md`

## 文档定位

本文件用于沉淀对当前 `core/websocket` 实现的首轮代码审查结论，逐条记录与 v1.0 设计、实施计划（plan）存在的偏差，作为后续逐条讨论、排序和立项的依据。**本文件不做方案定稿，仅做问题登记。**

每条记录包含：
- 当前实现状态（引自 `core/websocket/*.h`）
- 与 v1.0 设计 / plan 的差距
- 讨论点与可能的后续动作方向

后续每确定一条的处理方案（修、延后、放弃），应在条目末尾补充：处理方案、关联 PR / 提交、验证证据。

---

## 审查范围

- `core/websocket/types.h`
- `core/websocket/runtime_policy.h`
- `core/websocket/message_view.h`
- `core/websocket/prepared_write.h`
- `core/websocket/metrics.h`
- `core/websocket/state_machine.h`
- `core/websocket/frame_codec.h`
- `core/websocket/handshake.h`
- `core/websocket/tls_socket.h`
- `core/websocket/cold_path_loop.h`
- `core/websocket/critical_session.h`
- `core/websocket/active_spin_loop.h`
- `core/websocket/websocket_client.h`
- `core/websocket/CMakeLists.txt`

## 模块总览（现状）

| 文件 | 作用 | 形态 |
|---|---|---|
| `types.h` | 公共枚举 + `ConnectionConfig` | 基础类型 |
| `runtime_policy.h` | `RuntimePolicy` + `ApplyRuntimePolicy` + `PrefaultThreadStack` | inline 实现 |
| `message_view.h` | `MessageView` + C 风格 `MessageHandler`/`MessageConsumer` | 纯 POD |
| `prepared_write.h` | `PreparedWriteArena`（连接本地写槽池） | 完整实现（LIFO free-list） |
| `metrics.h` | `Metrics` 计数器聚合体 | 纯 POD |
| `state_machine.h` | `StateMachine`（phase + last_error） | 极薄 |
| `frame_codec.h` | mask 编码 + 拆帧解码 + ready queue | 完整实现 |
| `handshake.h` | `BuildClientHandshake` + `ValidateServerHandshake`（SHA-1 + Base64） | 完整实现 |
| `tls_socket.h` | 同步 + 非阻塞 OpenSSL 封装 | 完整实现 |
| `cold_path_loop.h` | 冷路径 `RunUntilActive`（DNS→TCP→TLS→WS） | 阻塞式 `epoll_wait` |
| `critical_session.h` | 用户驱动核心：`DriveRead / DriveWrite / AdvanceHeartbeat` | 完整实现 |
| `active_spin_loop.h` | 默认 busy-spin driver | 完整实现 |
| `websocket_client.h` | Layer 2 薄包装：准备 runtime → 冷路径 → 热路径 | 完整实现 |

## 与 plan 一致的设计选择（作为基线）

- Layer 1 / Layer 2 双层：`CriticalSession<TlsSocketT>`（模板化、用户驱动）+ `WebSocketClient`（薄包装 + spin loop）
- 连接本地预分配：`PreparedWriteArena` 一次性分配 slot storage 数组；`CriticalSession` 持有 `read_buffer_storage_` 和 `pending_writes_[]`
- Google C++ Style 命名（`PascalCase`、`snake_case`、`_` 后缀、`k` 前缀）
- C 风格回调接口（`MessageHandler = fn ptr`）避免 `std::function`
- `CriticalSession` 是 header-only 模板，测试可注入 `FakeTlsSocket`
- 冷路径 `ColdPathLoop` 只在初始连接阶段使用 epoll；热路径完全 spin-based

---

## 差距条目

### G1：热路径仍有动态分配与多次 memcpy（`FrameCodec`）

- **现状**
  - `FrameCodec::Feed` 先把入站字节 `std::vector::insert` 进 `inbound_bytes_`
  - 解析一帧后再 `decoded_payload_.resize(N)` 并逐字节 memcpy
  - `inbound_bytes_.erase(begin, begin+N)` 做 O(N) 搬移
  - 多帧并发到达时 `CaptureReadyFrame` 再次 `std::vector<std::byte>` 堆拷贝 + move 进 `std::deque`
- **差距**
  - 违反 v1.0 `热路径禁止临时分配`、`优先交付零拷贝视图` 的硬约束
  - 每条消息最少 2–3 次 memcpy
- **讨论方向**
  - 是否改为：环形接收 buffer + 原地 frame view（`std::span<const std::byte>` 指向接收 buffer）
  - 是否保留 ready queue；如果保留，改为定长节点 + 直接引用接收 buffer
  - mask 字段对入站客户端不存在，服务器→客户端默认 unmasked，可不做解掩码

### G2：`DriveRead` 单次 `ReadSome`，多条消息被 spin 轮数拉开

- **现状**
  - `DriveRead` 调用一次 `tls_socket_.ReadSome`，然后循环 `codec_.Feed({})` 直到无新帧
  - 不主动再次 `ReadSome` 填充 buffer
- **差距**
  - `SSL_read` 的内部 record 缓冲可能还有未读数据；需要等下一轮 spin 才能处理
  - 尾延迟会被 spin 节奏、心跳打点节奏牵制
- **讨论方向**
  - 在 `DriveRead` 中加入"有界多轮 `ReadSome`"预算（例如每次最多 N 次或 M 字节）
  - 或保持当前语义，但记录 L1 观测点用于评估影响
  - 需要 benchmark 证据支撑取舍

### G3：背压语义与 v1.0 的 `fail-fast` 错位

- **现状**
  ```
  if (result == DeliveryResult::kFatal ||
      result == DeliveryResult::kBackpressured) {
    TriggerReconnect(ConnectionError::kSocketError);
  }
  ```
- **差距**
  - `kBackpressured` 被当作致命错误直接重连
  - `kBackpressured` 和 `kFatal` 都映射到同一个 `kSocketError`，无法归因
  - v1.0 把背压视为合法状态，应由控制面降级 / 限流，而非切链
- **讨论方向**
  - 新增 `ConnectionError::kBackpressure` / `kConsumerFatal` 或走独立计数器
  - 讨论背压触发时的行为：丢消息？断开并重连？计入 degraded？
  - 考虑在 `Metrics` 里新增 `backpressure_events`
- 处理方案：fix — `kBackpressured` 丢帧并计入 `consumer_backpressure_drops`，不重连；`kFatal` 走新的 `ConnectionError::kConsumerFatal` 触发重连；控制帧（自动 pong、心跳 ping）入队失败同样非致命，计入 `control_frame_enqueue_failures` 并 skip 本轮（心跳不更新 `awaiting_pong_`/`last_ping_ns_`，真正死链路仍由心跳超时路径捕获）。
- 关联提交：`b269586`
- 验证证据：`websocket_critical_session_test` 新增 4 个聚焦用例（`BackpressuredConsumerDropsFramesWithoutReconnect`、`FatalConsumerTriggersReconnectWithConsumerFatal`、`AutoPongEnqueueFailureSkipsWithoutReconnect`、`HeartbeatPingEnqueueFailureSkipsTickWithoutReconnect`）全部通过。
- 确认日期：2026-04-24

### G4：心跳与业务发送共用 `pending_writes_`，无优先级协调

- **现状**
  - `AdvanceHeartbeat` 通过 `EnqueueControlFrame(kPing, {})` 把 ping 塞进同一条队列
- **差距**
  - 业务消息在排队时，ping 被拖延 → 心跳 RTT 被业务吞吐污染
  - 无法区分"真链路问题"与"自家发送阻塞"
  - v1.0 要求"心跳应与正常写链协调，避免因心跳插队破坏发送顺序语义"，这里既没有协调也没有独立观测
- **讨论方向**
  - 独立的 control-frame slot / fast-path（插队或并行）
  - 至少区分 metrics：ping 入队到实际 sendto 的耗时
  - 若选择"不插队"，需要记录心跳排队延迟，并在超时判定上把排队时间扣除，避免误判

### G5：冷路径无分阶段超时

- **现状**
  - `ColdPathLoop::WaitForSocket` 使用 `epoll_wait(epoll_fd_, &ready, 1, -1)` 无限阻塞
  - `RunUntilActive` 中 DNS → TCP connect → TLS handshake → WS handshake → 读响应 都可能永久等待
  - `ConnectionError::kConnectTimeout` 枚举存在但无任何引用
- **差距**
  - 与 v1.0 "不同阶段必须有独立超时，不允许只用一个笼统的连接超时覆盖全部过程" 直接冲突
- **讨论方向**
  - 分别定义 `dns_timeout_ms` / `tcp_connect_timeout_ms` / `tls_handshake_timeout_ms` / `ws_handshake_timeout_ms` 到 `ConnectionConfig`
  - `WaitForSocket` 换成带超时的 epoll_wait，或改用 `timerfd`
  - 超时映射到对应的 `ConnectionError`，落到 state_machine
- 处理方案：fix（部分） — 新增单一 `ConnectionConfig::cold_path_total_timeout_ms`（默认 10s），`ColdPathLoop::RunUntilActive` 在入口记录 deadline，向下传到 `WaitForSocket`（带 remaining_ms 的 epoll_wait）、`WriteAll`、响应读循环；超时映射到 `ConnectionError::kConnectTimeout` 并保留当时的 phase（`kTlsHandshaking` / `kWsHandshaking`）。分 DNS / TCP / TLS / WS 四个独立 timer 推迟到后续 Phase；同步 `getaddrinfo` 受系统 resolver 控制不可中断，文档中留有 TODO。
- 关联提交：`978249e`
- 验证证据：新增 `websocket_cold_path_loop_test`（带本地 blackhole TCP server）在 `cold_path_total_timeout_ms = 200` 下约 210ms 内触发 `kConnectTimeout` + `kTlsHandshaking`；live probe 连接 `wss://fx-ws.gateio.ws/v4/ws/usdt` 仍能成功进入 active spin。
- 确认日期：2026-04-24

### G6：握手 `Sec-WebSocket-Key` 硬编码

- **现状**
  ```
  static constexpr std::string_view kClientKey = "dGhlIHNhbXBsZSBub25jZQ==";
  ```
- **差距**
  - 每次握手使用同一个固定 nonce，违反 RFC 6455（每连接应随机 16 字节）
  - Gate 宽松可忽略，但仍是协议违规；服务端一旦抽查即断连
- **讨论方向**
  - 冷路径启动时 `RAND_bytes(16)` + base64 生成；由 `handshake.h` 提供工具函数
  - 响应校验逻辑（`ValidateServerHandshake`）已支持动态 key，只需生成端同步更新
- 处理方案：fix — 在 `handshake.h` 新增 `GenerateClientKey(std::span<char>)` helper（`RAND_bytes(16)` + `EVP_EncodeBlock`）；`ColdPathLoop` 持有 `std::array<char, 32>` 成员，每次 `RunUntilActive` 重新生成并同时传入 `BuildClientHandshake` / `ValidateServerHandshake`，删除原静态 `kClientKey` 常量。
- 关联提交：`b126266`
- 验证证据：新增 `websocket_handshake_test.GenerateClientKeyProducesUniqueBase64Keys`（两次调用输出 24 字节、不相等、base64 反解 16 字节）；live probe 连接 `wss://fx-ws.gateio.ws/v4/ws/usdt` 握手仍成功。
- 确认日期：2026-04-24

### G7：`WebSocketClient` 没有任何重连 / 退避 / 恢复逻辑

- **现状**
  ```
  if (core_.ShouldReconnect()) {
    state_machine_.Fail(core_.LastError(), ConnectionPhase::kClosed);
    ...
    return false;
  }
  ```
  - `Start()` 触发 reconnect 后直接返回 `false`
  - `ConnectionPhase::kReconnectBackoff` 枚举存在但无任何状态真正进入
- **差距**
  - plan 明确要求 "session-local heartbeat, timeout, degrade, and reconnect logic inside the core"
  - 当前只实现了 heartbeat，reconnect / backoff / degraded 全缺
- **讨论方向**
  - 重连策略落在哪里：`WebSocketClient` 层还是 `CriticalSession` 层
  - 退避算法：固定间隔？指数 + 抖动？
  - 失败原因分类处理（认证/配置错误 vs 网络抖动）
  - 是否区分 `kReconnectBackoff` 状态给外部观测
- 处理方案：fix — 在 `ConnectionConfig` 增加整数化 `ReconnectPolicy`，新增 `FailureClass` 分类器和 `xorshift64` 退避 jitter；`WebSocketClient::Start()` 内部循环执行冷路径、active spin、失败分类、`kReconnectBackoff` 通知和可中断 backoff。`Stop()` 统一写 `eventfd`，`ColdPathLoop` 把该 fd 加入 `epoll_wait`，可中断 TLS 握手与 backoff sleep。`CriticalSession::Reset()` 释放 pending writes、复位 codec / heartbeat flags / last error，但不清 metrics。另补充 `SIGPIPE` 屏蔽，避免 TLS 握手期间 peer close 触发进程终止，使其回到普通 TLS/socket 错误路径。
- 验证证据：新增 `websocket_reconnect_classifier_test`、`websocket_backoff_compute_test`、`websocket_client_reconnect_test`，并扩展 `websocket_critical_session_test.ResetClearsPendingAndFlags`；debug 与 release 下 `ctest --test-dir build/<type> -R websocket_ --output-on-failure` 均为 12/12 通过。live probe 连接 `wss://fx-ws.gateio.ws/v4/ws/usdt` 输出 `state=kActive` 后由 `timeout` 结束。release benchmark 与 `dev@d1e50a1` 对比：`session_read_path` p50/p99/p99.9 = 440/474/815ns vs 442/473/3929ns；`session_write_path` 复跑值 = 411/438/647ns vs 407/431/934ns；`active_spin` = 42/43/44ns vs 42/43/44ns；`frame_codec` = 379/529/573ns vs 379/542/938ns；`prepared_write` = 2/2/8ns vs 2/2/3ns。

### G8：心跳超时分辨率绑在 spin `iteration_budget` 上

- **现状**
  ```
  ++iterations_since_clock;
  if (iterations_since_clock >= iteration_budget) {
    const auto now = std::chrono::steady_clock::now()...;
    session.AdvanceHeartbeat(...);
  }
  ```
- **差距**
  - 心跳 timeout 判定精度 ≈ 4096 轮 spin 的耗时
  - 空闲时单轮很快（ns 级），但消息处理变重时心跳判定被拖延
  - `std::chrono::steady_clock` 对低延迟路径不算最优
- **讨论方向**
  - 是否改为 `clock_gettime(CLOCK_MONOTONIC_COARSE)` 或 `rdtsc` + 频率校准
  - 是否用 `timerfd_create` 在 spin loop 读 fd 代替轮询打点
  - 评估 clock 调用开销 benchmark

### G9：`ConnectionPhase` 缺少 `Degraded`

- **现状**：`Disconnected / Resolving / TcpConnecting / TlsHandshaking / WsHandshaking / Active / ReconnectBackoff / Closing / Closed`
- **差距**
  - v1.0 明确要求 `Degraded`：连接还在，但 RTT / 写链高水位 / 交付滞后等指标触发预警
  - 当前 `heartbeat_timeouts` 和 `prepared_write_high_watermark` 超阈值时连接不会进入预警状态，只能在断链后观察
  - plan 也不要求 `Authenticating`（Gate 公共频道无认证），可以不补
- **讨论方向**
  - 是否先加 `kDegraded` 状态 + 进入/退出条件（阈值可配置）
  - 是否对应新增 metrics：`degraded_enter_count` / `degraded_exit_count`

### G10：构造路径与热路径的分配责任混淆

- **现状**
  - `CriticalSession` 构造用 `std::make_unique` 分配 `pending_writes_[]` 和 `read_buffer_storage_` → 冷路径，OK
  - `FrameCodec` 只在 ctor 里 `reserve`，但 `inbound_bytes_.insert` / `decoded_payload_.resize` / `ready_frames_.push_back` 在运行时仍可能触发 `std::vector` 扩容或每次构造/析构 allocator traits
- **差距**
  - 运行时依赖 `std::vector` 容量自恢复，没显式 fail-fast
  - 即使 capacity 足够，`resize` 也会触发值初始化循环
- **讨论方向**
  - 是否改用定长 array buffer + 手工 cursor（确保 header-only 约束下可行）
  - `ready_frames_` 是否改为定长 ring buffer
  - 容量耗尽时是否显式拒收 + 进 degraded / 触发重连

### G11：构建图疑问（待核实）

- **现状**
  - `core/websocket/CMakeLists.txt` 只 `target_sources PRIVATE *.h`
  - 头文件全部 inline 实现，没有 `.cpp`
  - 尚未确认 `core/CMakeLists.txt` 的实际形态（plan 要求 `add_library(aquila_core STATIC aquila_core.cpp)`）
- **讨论方向**
  - 确认 `aquila_core` 当前是 STATIC 空库还是 INTERFACE；若是 STATIC，`aquila_core.cpp` 应存在且至少占位
  - 是否需要改成 INTERFACE（纯 header-only 时更合适）；切换成本、对下游链接的影响
  - 按 plan 要求最终是 STATIC，还是根据现状调整计划

---

## 小结

**基础架构已经搭起来了**（Layer 1 + Layer 2 + cold/hot path 分离 + prepared-write 池），但离 v1.0 "最小可接受验收" 还有明显差距，主要集中在：

- 热路径多次 memcpy + `std::vector` 动态操作（`FrameCodec`）
- 背压被错误地等同致命错误
- 心跳与业务写无协调 / 无独立观测
- 冷路径无分阶段超时
- 完全没有重连 / 退避 / degraded 状态
- 握手 nonce 硬编码
- 时钟粒度绑在 spin `iteration_budget` 上
- 构建图形态待核实

**P0 阶段进展**（2026-04-24）：G6、G3、G5 已关闭，对应路线图 `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md` 的 Phase 0。同步发现并修复了测试注册链路的陈旧状态（顶层缺 `enable_testing()` + `add_websocket_gtest` 未调用 `add_test`，见 commit `18c8ac2`），这一点不属于 G1–G11 的范围，但对后续验收证据可信度是前提。

## 讨论顺序建议

下列顺序仅供讨论起点，最终由讨论结果决定：

1. **G5 分阶段超时** + **G7 重连策略**：这两项缺失最基础，先定框架
2. **G1 零拷贝 / 内存布局**：决定后续 `FrameCodec` 大改，影响测试
3. **G4 心跳协调** + **G8 时钟 / 打点粒度**：心跳一组策略
4. **G3 背压语义** + **G9 Degraded 状态** + **G10 容量耗尽语义**：错误/降级一组策略
5. **G6 握手 nonce**：小闭环，可单独修
6. **G2 `DriveRead` 多轮读取**：基于 benchmark 结果再定
7. **G11 构建图**：与上面工作穿插核对

## 条目跟踪模板

每条后续确认处理方案时，补充以下结构：

```
- 处理方案：<fix / defer / drop + 一句话说明>
- 关联提交 / PR：<sha 或 PR 链接>
- 验证证据：<测试/benchmark/日志>
- 确认日期：<YYYY-MM-DD>
```
