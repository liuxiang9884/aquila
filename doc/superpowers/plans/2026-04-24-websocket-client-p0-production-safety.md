# P0：WebSocket Client 生产可用性修复（G6 + G3 + G5）

> **给执行 agent 的说明：** 必备子技能：使用 `superpowers:subagent-driven-development`（推荐）或 `superpowers:executing-plans` 按 task 逐项执行。步骤使用 `- [ ]` 复选框追踪进度。
>
> **讨论闸门：** 每个 Task 开头都有 "Scope Decision" 块，必须先与人类协作者确认后再进入 Step 1。未确认前不允许开始实现。

## 文档信息

- 版本：`v0.1`
- 状态：`待讨论`
- 创建日期：`2026-04-24`
- 关联文档：
  - `doc/websocket_client_design_v1.0.md`
  - `doc/superpowers/plans/2026-04-23-websocket-client-minimal-hft.md`
  - `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`

## 目标

修复阻挡 `core/websocket` 在生产环境无人值守运行的三条短板：

- **G6** — 握手 `Sec-WebSocket-Key` 硬编码（违反 RFC 6455，服务端任何时候都可能拒绝）。
- **G3** — 消费者返回 `kBackpressured` 或控制帧入队失败时直接断链。任何短暂的消费者卡顿 = 客户端死亡。`EnqueueControlFrame` 心跳路径上有同样的 bug。
- **G5** — 冷路径使用 `epoll_wait(..., -1)`，DNS / TCP / TLS / WS 任意阶段卡死时进程会变成无声僵尸。

## 不在范围内（推到 P1+）

- G7 重连 / 退避 / 失败分类
- G9 `Degraded` 状态
- G1 / G10 零拷贝 frame codec 重写
- G2 多轮 `DriveRead`
- G4 / G8 心跳协调、时钟粒度
- G11 构建图相关收尾（已确认与 plan 一致）
- 冷路径分阶段超时拆分（本 P0 只加单一总预算；分阶段拆分作为后续工作）

## 架构原则

这是 bug 修复，不是重新设计。改动面要小。除非真的必需，不引入新抽象、新线程或新公共 API。如果某个 Task 的实现超过预估规模，停下来重新讨论。

## 延迟 / 确定性原则

- G6 的 nonce 生成只在冷路径（每次新建连接）执行，绝不进入热路径。
- G3 修复在热路径上只能新增一次计数器自增和一个早 return；不允许新增分配、锁或额外分支。
- G5 的超时机制完全位于冷路径，热路径不受影响。

---

## 文件清单

### G6 — 随机 nonce
- 修改：`core/websocket/handshake.h`
- 修改：`core/websocket/cold_path_loop.h`
- 修改：`test/websocket/handshake_test.cpp`
- 可能创建：单独的 nonce 生成测试文件（在 Task 1 决策中确认）

### G3 — 背压语义
- 修改：`core/websocket/types.h`（新增 `ConnectionError::kConsumerFatal`，保留 `kBackpressured` 作为非致命 `DeliveryResult`）
- 修改：`core/websocket/metrics.h`（新增 `consumer_backpressure_drops` 与 `control_frame_enqueue_failures` 计数器）
- 修改：`core/websocket/critical_session.h`（拆分 fatal 与背压路径；控制帧失败降级为计数器）
- 修改：`test/websocket/critical_session_test.cpp`（新增或调整用例）

### G5 — 冷路径总预算超时
- 修改：`core/websocket/types.h`（在 `ConnectionConfig` 加 `cold_path_total_timeout_ms`，默认 `10000`）
- 修改：`core/websocket/cold_path_loop.h`（带 deadline 的 `epoll_wait` + ReadSome / WriteSome 循环）
- 修改：`test/websocket/CMakeLists.txt`，并新增 `cold_path_loop_test.cpp` 用于驱动超时路径（具体测试夹具在 Task 3 中决定）

---

## Task 1：G6 — 每次连接生成随机 `Sec-WebSocket-Key`

### Scope Decision（Step 1 之前必须确认）

- [ ] 在 `handshake.h` 新增 `GenerateClientKey(std::span<char> output)` helper，内部用 `RAND_bytes` 生成 16 字节，base64 写入 `output`，返回指向 `output` 的 `std::string_view`。`output` 由调用方持有。
- [ ] `ColdPathLoop` 持有固定大小的 storage 成员（`std::array<char, 32>` 足够装 24 字节的 base64），每次 `RunUntilActive` 重新生成一次 key。
- [ ] 不改动 `WebSocketClient` / `CriticalSession` 的公共 API。
- [ ] 测试方式：单元测试连续调用 `GenerateClientKey` 两次，断言：(a) 都成功；(b) 输出不同；(c) 长度为 24；(d) base64 反解后是 16 字节。

### 步骤

- [ ] **Step 1**：在 `test/websocket/handshake_test.cpp` 新增 `GenerateClientKey` 的失败测试（如果增长过大可以拆出新文件）。
- [ ] **Step 2**：运行测试 — 预期 FAIL，因为 `GenerateClientKey` 还不存在。
- [ ] **Step 3**：在 `handshake.h` 实现 `GenerateClientKey`；让 `ColdPathLoop` 在 `RunUntilActive` 中调用；删除原 `kClientKey` 常量。
- [ ] **Step 4**：运行所有 websocket 测试 — 预期 PASS。运行 `tools/websocket_probe` 连接 `wss://fx-ws.gateio.ws/v4/ws/usdt` — 预期握手仍然成功。
- [ ] **Step 5**：提交。

---

## Task 2：G3 — 背压不再是致命错误

### Scope Decision（Step 1 之前必须确认）

当前代码：

```cpp
if (result == DeliveryResult::kFatal ||
    result == DeliveryResult::kBackpressured) {
  TriggerReconnect(ConnectionError::kSocketError);
}
```

拆分提案：

| Consumer 返回值 | 行为 | 计数器 |
|---|---|---|
| `kAccepted` | 正常路径 | `++rx_messages` |
| `kBackpressured` | **丢弃此条消息，不断链** | `++consumer_backpressure_drops` |
| `kFatal` | 触发重连，使用新的 `ConnectionError::kConsumerFatal` | （走原重连路径） |

控制帧（`AdvanceHeartbeat` 心跳和自动 pong 回应路径上的 `EnqueueControlFrame` 失败）：

- [ ] P0 中按**非致命**处理：`++control_frame_enqueue_failures`，跳过本轮心跳（**不**置位 `awaiting_pong_`），不断链。真正死链路仍然会被心跳超时路径捕获。
- [ ] **待讨论的开放问题：** 自动 pong 回应失败是否走相同规则？这是一项对端可见的协议义务，但比直接断链危害小。**默认提案：是，走相同规则。**

需要确认的决策：

- [ ] `kBackpressured` 语义 = 丢弃 + 计数。（替代方案：保留并重试 — 需要 codec 支持 backpressure，改动较大，**P0 拒绝**。）
- [ ] 新增 `ConnectionError::kConsumerFatal`（P0 中暂不驱动差异化的重连行为，仅为 G7 留 hook）。
- [ ] 新增 metrics `consumer_backpressure_drops` 与 `control_frame_enqueue_failures`。

### 步骤

- [ ] **Step 1**：在 `test/websocket/critical_session_test.cpp` 新增失败测试，覆盖：(a) consumer 返回 `kBackpressured` → 不重连，计数器 +1，本帧被丢弃，下一条解码出来的帧仍能正常交付；(b) consumer 返回 `kFatal` → 触发重连，error 为 `kConsumerFatal`。
- [ ] **Step 2**：运行测试 — 预期 FAIL。
- [ ] **Step 3**：按 Scope Decision 修改 `types.h`、`metrics.h`、`critical_session.h`。
- [ ] **Step 4**：运行所有 websocket 测试 — 预期 PASS。
- [ ] **Step 5**：提交。

---

## Task 3：G5 — 冷路径总预算超时

### Scope Decision（Step 1 之前必须确认）

- [ ] 在 `ConnectionConfig` 新增单一字段 `uint32_t cold_path_total_timeout_ms = 10000`。分阶段（DNS / TCP / TLS / WS 各自独立预算）拆分**推迟** — v1.0 设计要求分阶段，但当下的 liveness 修复只需要一个总上限。
- [ ] `ColdPathLoop::RunUntilActive` 在入口记录 deadline（`steady_clock::now() + timeout`），并向下传递到：
  - `WaitForSocket` → `epoll_wait(epoll_fd_, &ready, 1, remaining_ms)`
  - 响应读取循环（每次 `WaitForSocket` 前重新计算剩余预算）
  - WS 握手的 `WriteAll` 循环（同上）
- [ ] 超时失败映射到现有的 `ConnectionError::kConnectTimeout`（无需新增 error code，该 code 当前未被使用）。
- [ ] **待讨论的开放问题：** `OpenAndConnect` 中的同步 `getaddrinfo` 单独可能就超过总预算且无法中断。**默认提案：P0 接受这个限制** — 实际中 `getaddrinfo` 受 `/etc/resolv.conf` 的 `timeout` / `attempts` 约束，不会无限阻塞；先文档化，后续再讨论是否需要把 DNS 移到独立 worker。
- [ ] 测试方式：新增 `cold_path_loop_test.cpp`，使用一个 TCP server，`accept()` 后不发任何响应（黑洞），让 `RunUntilActive` 用 `cold_path_total_timeout_ms = 200` 连接，断言：返回 `false`，`state_machine.last_error()` 为 `kConnectTimeout`。（替代方案：连未监听的端口 — 但 `ECONNREFUSED` 会立即返回，无法触发超时路径。）**测试夹具的具体形态在 Step 1 中讨论。**

### 步骤

- [ ] **Step 1**：决定测试夹具方案（黑洞 TCP server 或其他），新增失败测试。
- [ ] **Step 2**：运行测试 — 预期 FAIL 或 HANG（ctest 应配置有界 `TIMEOUT` 兜底）。
- [ ] **Step 3**：在 `cold_path_loop.h` 中实现带 deadline 的循环。改动面尽量小。
- [ ] **Step 4**：运行所有 websocket 测试 — 预期 PASS，且新超时测试在 `cold_path_total_timeout_ms + ε` 内完成。
- [ ] **Step 5**：提交。

---

## 三个 Task 完成后的最终验证

- [ ] 跑全量 websocket 测试：`./build.sh debug && ctest --test-dir build/debug --output-on-failure -R "websocket_"`
- [ ] 跑 live probe，确认随机 nonce 后 Gate 握手仍然成功：`build/debug/tools/websocket_probe --host fx-ws.gateio.ws --port 443 --target /v4/ws/usdt --tls --cpu 2`
- [ ] 更新 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`：在 G6、G3、G5 末尾追加"处理方案 / 关联提交 / 验证证据 / 确认日期"块。
- [ ] P0 不需要改 README 或设计文档；`websocket_client_design_v1.0.md` 已经规定了这些行为，我们只是补齐落地。

## 规范覆盖检查

- G6 关闭 RFC 6455 §4.1 关于每次连接生成随机 `Sec-WebSocket-Key` 的义务。
- G3 对齐 v1.0 "背压视为合法状态" 与 "失败语义清晰" 两条硬约束。
- G5 部分关闭 v1.0 "不同阶段必须有独立超时" — 总预算是底线；分阶段拆分作为 G5 的 P1 后续工作。

## Placeholder 扫描

- 本 plan 中的两处 `TBD` 是 Task 1 和 Task 3 的 Scope Decision 闸门，必须在 Step 1 前解决。

## 执行注意

- 即便改到了相同文件，也**不要**在本 plan 内动 G7 重连工作。
- **不要**新增线程、`std::function`、热路径上的堆分配。
- 如果某个 Task 的 Step 3 净改动超过约 80 行，停下来重新讨论 scope。
