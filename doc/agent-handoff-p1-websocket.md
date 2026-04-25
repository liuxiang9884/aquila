# Agent 接手说明：继续 P1（WebSocket 重连 + Degraded）

## 这份文件是给谁的

本仓库 `aquila` 当前正在执行 **Phase 1（WebSocket client 的重连与降级）** 计划。前一个协作 agent 已经：

- 完成 P0 三条差距的修复（随机 nonce / 背压非致命 / 冷路径超时），合并到 `dev`
- 完成 P1 plan 的两次定稿和 8 + 7 = 15 条 Scope Decision 的逐条拍板
- **尚未**写一行 P1 实现代码

你是新接手这项工作的 agent。本文件告诉你：**先读什么、当前状态是什么、下一步具体要做什么、哪些红线不能碰**。

按顺序执行即可。

---

## 30 秒速览

- **项目**：crypto HFT 系统，重点是面向 gate.io / binance 的低延迟 C++ WebSocket client
- **当前分支**：`p1-reconnect-degraded`（从 `dev` 切出）
- **当前位置**：P1 plan 全部决策已定，准备进入 **Task 1（G7 重连）Step 1：写失败测试**
- **风格**：TDD + 原子提交 + 中英混写（文档中文、代码注释英文、commit message 英文）

---

## 必读顺序（按优先级分 3 级）

### T1：动手前**必读**（按序）

1. **`CLAUDE.md` → `AGENTS.md`**
   - 项目级协作约定。涵盖 HFT 设计原则、文档/注释/commit/PR 语言规则、C++ 风格（`magic_enum` / `fmt::format_to_n` / 不加 `std::string`）、原子提交纪律。
   - 特别注意 "高频系统设计和实现原则" 和 "项目级执行规则" 两节。
   - AGENTS.md 硬约束：任何性能结论必须有 benchmark / profile / 运行证据，**不允许凭直觉宣告完成**。

2. **`doc/websocket_client_design_v1.0.md`**
   - WebSocket client 的系统设计基线（Layer 1 / Layer 2、I/O owner 约束、热路径零分配、分阶段超时、Degraded 状态等）。
   - P1 要落地的 `kReconnectBackoff` 和 `kDegraded` 都在这里有规范。

3. **`doc/superpowers/plans/2026-04-24-websocket-client-p1-reconnect-and-degraded.md`（v0.2）**
   - 当前 plan。**你接下来所有的工作按这里走**。
   - 两个 Task（G7 + G9），每个 Task 顶部的 "Scope Decision" 已经全部决定（user 已拍板），不要再重新讨论；如果发现决策有问题，**先暂停**问 user。
   - 最重要的部分：Task 1 "步骤" 和 Task 2 "步骤" 的 Step 1/2/3/4/5 列表。

4. **`git log --oneline dev..HEAD`**
   - 看你这个分支上已经有哪些提交。现在应该全部是 plan 文档提交（4 个 commit）。

### T2：理解现有代码（按需查阅）

5. **`core/websocket/` 全部 header**
   - `types.h`、`runtime_policy.h`、`message_view.h`、`prepared_write.h`、`metrics.h`、`state_machine.h`、`frame_codec.h`、`handshake.h`、`tls_socket.h`、`cold_path_loop.h`、`critical_session.h`、`active_spin_loop.h`、`websocket_client.h`
   - 都是 header-only（除了占位 `aquila_core.cpp`）。看 `websocket_client.h` 和 `critical_session.h` 最重要，因为 P1 主要改这两个。

6. **`test/websocket/` 现有测试**
   - `critical_session_test.cpp` 尤其值得看，P1 的 `ResetClearsPendingAndFlags` 会追加在这里。
   - `cold_path_loop_test.cpp`（P0 新加）是 TLS blackhole 夹具的**雏形**，P1 要把它扩展为能完成 TLS 握手 + 发 101 的完整夹具。

### T3：跳读 / 遇到问题再查

7. **`doc/reviews/2026-04-24-websocket-client-gap-analysis.md`**
   - 11 条差距的出处。G3 / G5 / G6 已经关闭并回填了处理方案块。G7 / G9 是本次要关闭的两条。

8. **`doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`**
   - 5 个 Phase 的总路线图。你只负责 P1；P2-A / P2-B / P3 不要提前动手。

9. **`doc/superpowers/plans/2026-04-24-websocket-client-p0-production-safety.md`**
   - P0 plan。不用执行，但 P0 的 TDD 节奏和 Scope Decision 形式是 P1 的样板。

10. **`doc/project_structure.md`**
    - 目录划分的一阶段范围（gate 行情+交易、binance 行情）。P1 不涉及 `exchange/`。

---

## 当前状态精确快照

### 分支拓扑

```
main: ... → b126266 (G6 随机 nonce)
             ↓
dev:  b126266 → b269586 (G3) → 18c8ac2 (test infra) → 978249e (G5) → d1e50a1 (review 回填)
                                                                           ↓
p1-reconnect-degraded (当前分支):
  d1e50a1 → a9d49d0 (P1 plan v0.1)
         → 2af78a0 (P1 plan v0.2 HFT 合规修订)
         → e41fe28 (决策 6 改为 eventfd)
         → af2d996 (决策 8 补变体 4)
```

运行 `git log --oneline dev..HEAD` 确认一致。

### P1 plan 决策摘要（避免回退讨论）

**Task 1（G7）已定决策**：
1. `ReconnectPolicy`：纯整数（`backoff_shift_bits` / `jitter_percent`），**不用** `double`
2. `Classify` 默认映射表：`kResolveFailure` / `kHandshakeFailure` / `kConsumerFatal` → Fatal；其余 → Transient
3. 重连循环在 `WebSocketClient::Start()` 内部
4. `ColdPathLoop` 每次重连重建 epoll_fd（选 A）
5. `CriticalSession::Reset()` 复位 pending_writes + codec + awaiting_pong + last_error（不清 metrics）
6. **`eventfd` + `epoll_wait`** 统一唤醒（冷路径 + backoff sleep 共用），**不要** `std::mutex` + `condition_variable`
7. state_machine 驱动：`Fail(err, kReconnectBackoff)` 进入 backoff；`reconnects++` 在成功重连之后
8. 测试：classifier / backoff（固定 xorshift 种子） / Reset 单元测试 + **TLS blackhole 夹具 E2E 4 个变体**

**Task 2（G9）已定决策**：
1. `kDegraded` phase 插在 `kActive` 和 `kReconnectBackoff` 之间
2. P1 阶段只用三个触发因子（写队列水位 / 背压丢帧 1s 滑窗 / pong 挂起时长）；**不引入 RTT 触发器**（留 P2-B）
3. 退出：全部回落连续 `recover_ticks`（16）轮
4. `DegradedThresholds` 结构体整数化
5. 判定落在 `RuntimeSession`，cadence 可配置（默认复用 `spin_iterations_before_clock_check`，但留独立字段）
6. 状态机通知 `kDegraded` / `kActive` 切换；新增 `degraded_enter_count` / `degraded_exit_count` / `degraded_active`
7. 测试：`DegradedEvaluator` 单元测试（6 个用例） + E2E（复用 TLS blackhole 夹具模拟 pong 延迟）

**硬约束**（反复出现）：
- 滑动窗口**必须**用 16 slot 定长环形快照（`BackpressureWindow`）。不允许 `std::deque`、`std::vector`、每事件 push/pop
- RNG 用 `xorshift64`，**不允许** `std::mt19937`
- `backoff_*` 锁域仅限冷路径；热路径 spin loop 绝不触及
- 对 Degraded / reconnect 性能无回归的**必须**跑 benchmark 证明，不允许"看起来应该没问题"

---

## 下一步具体操作（从这里开始做事）

### 第 0 步：确认环境

```bash
cd /home/liuxiang/dev/aquila
git status                  # 应该 clean，on branch p1-reconnect-degraded
git log --oneline dev..HEAD # 4 个 plan 相关提交
./build.sh debug            # 应成功
ctest --test-dir build/debug -R "websocket_" # 9/9 pass
```

任何一步不符合预期 → 停下问 user，不要自行修复。

### 第 1 步：进入 Task 1（G7）Step 1 —— 写失败测试

对应 plan 文件第 ~200 行附近的 "Task 1 > 步骤 > Step 1"。要新建 / 修改的文件：

- 新建 `test/websocket/reconnect_classifier_test.cpp`
  - 对 `ConnectionError` 所有枚举值断言 `FailureClass`。用 `magic_enum::enum_values<ConnectionError>()` 遍历 + 一张 expected table。
- 新建 `test/websocket/backoff_compute_test.cpp`
  - 用固定 xorshift 种子，断言基线值 + jitter 范围 + `shift_bits = 0` 恒等于 `initial_backoff_ms`
- 修改 `test/websocket/critical_session_test.cpp`
  - 追加 `ResetClearsPendingAndFlags`：触发背压或 fatal → `Reset()` → 喂新帧 → 验证 pending_writes 空 / awaiting_pong false / last_error kNone / arena free count 回满
- 新建 `test/websocket/tls_blackhole_server.h`（头文件，夹具）
  - OpenSSL 自签证书 + `SSL_CTX` + accept 循环的小框架。**可以参考 `test/websocket/cold_path_loop_test.cpp` 里的 `BlackholeTcpServer` 作为 TCP 层骨架**，在其之上加 TLS 握手和 101 响应。
  - 这个夹具有点工作量（~100 行），但一次投入 P1 全部 4 个 E2E 变体都复用。
- 新建 `test/websocket/websocket_client_reconnect_test.cpp`
  - 4 个变体的骨架，**本步先写期望 API**（例如 `WebSocketClient.Start()` 返回值、metrics 快照、状态回调捕获），编译期和运行期都必须 FAIL。

同步更新 `test/websocket/CMakeLists.txt`：用已有的 `add_websocket_gtest(...)` 函数注册 3 个新 exe。若 E2E 测试运行时间长，加 `set_tests_properties(websocket_client_reconnect_test PROPERTIES TIMEOUT 30)`。

### 第 2 步：运行测试，确认 FAIL

```bash
./build.sh debug 2>&1 | tail -20  # 应该编译失败，因为 FailureClass / ReconnectPolicy / Reset 还不存在
```

预期 FAIL 的原因应该是"符号不存在"（类 / 枚举 / 方法还没写）。如果是其他类型的 FAIL（比如测试已经跑但断言错了），意味着你漏掉了什么，回头检查。

### 第 3 步：进入实现（Task 1 Step 3）

严格按 plan Task 1 Step 3 列出的文件顺序改。建议顺序：

1. `types.h`：加 `ReconnectPolicy` + `ConnectionConfig::reconnect`
2. 新建 `core/websocket/reconnect_classifier.h`（header-only，`Classify` + `ComputeBackoffMs` + `BackoffRng`）
   - 或者放在 `websocket_client.h` 的 anonymous namespace 里 —— 你自己判断：独立 header 测试更清爽；放一起文件更少。P0 的 `handshake.h::GenerateClientKey` 是放独立功能 header 的例子。
3. `critical_session.h`：加 `Reset()`
4. `websocket_client.h`：eventfd 管理 + 重连循环 + backoff sleep + `Stop()` 写 eventfd
5. `cold_path_loop.h`：加 `SetInterruptFd()` + epoll 集合加入 wakeup_fd + `WaitOutcome::kInterrupted`
6. `metrics.h`：确保 `reconnects` 被驱动；G9 的新字段这一步**先不加**（留给 Task 2）

**每改完一个文件就先 build 一次**，避免一次改完编译爆炸难排查。

### 第 4 步：跑测试 + live probe + benchmark 回归

```bash
./build.sh debug
ctest --test-dir build/debug --output-on-failure -R "websocket_"

# live probe（需要网络）
timeout 15 build/debug/tools/websocket_probe --host fx-ws.gateio.ws --port 443 --target /v4/ws/usdt --tls
# exit code 143 = 被 SIGTERM，说明握手成功并进入 active spin

# benchmark 回归（AGENTS.md 硬约束）
./build.sh release
# 对 session_read_path_benchmark / session_write_path_benchmark /
# active_spin_benchmark / frame_codec_benchmark / prepared_write_benchmark
# 在 p1-reconnect-degraded 和 dev（作为基线）分别跑，对比 p50/p99/p99.9
# 记录结果，P1 结束时回填进 review 文档的 G7 验证证据块
```

### 第 5 步：原子提交

建议按 plan Task 1 Step 5 的切分提交。每个原子提交 ≤ 150 行净改动。commit message **英文**，格式参考 `git log --oneline dev..HEAD` 前面的风格（`core: ...` / `test: ...` / `docs: ...`）。

### 然后：进入 Task 2（G9）

Task 1 完成并提交后，同分支继续 Task 2，同样 Step 1→5 走一遍。Task 2 能**复用** Task 1 的 TLS blackhole 夹具。

---

## 绝不能忘的红线

1. **文档中文，代码注释英文，commit message 英文，PR 正文中文** —— AGENTS.md "默认约定"
2. **原子提交**：不要把多个无关改动塞一个 commit —— AGENTS.md "项目级执行规则"
3. **热路径零分配 / 零系统调用 / 零锁** —— 见 `doc/websocket_client_design_v1.0.md`
4. **冷路径 / 热路径区分**：backoff sleep、`Reset()`、TLS handshake 都是冷路径；`DriveRead` / `DriveWrite` / `AdvanceHeartbeat` 是热路径
5. **任何性能结论必须 benchmark 证明**。"应该不影响"不算证据
6. **TLS blackhole 夹具端到端测试必须在 P1 内完成**，不允许"留到 P3 再说"
7. **滑动窗口必须用 16 slot 定长环形快照**；**RNG 必须是 xorshift64**；**Stop 唤醒必须是 eventfd**
8. **决策已定不再回退讨论**。如果发现决策有问题，先暂停写代码、问 user
9. **遇到大于 plan 预估规模（单 Task Step 3 净改动 > 150 行）**，停下问 user，不要硬塞
10. **不要改 `main` 分支**。当前分支是 `p1-reconnect-degraded`，合并走 `dev`

---

## 迷路时去哪找

| 情况 | 去查 |
|---|---|
| 不确定项目约定 | `AGENTS.md` |
| 不确定系统设计意图 | `doc/websocket_client_design_v1.0.md` |
| 不确定 P1 具体该做什么 | `doc/superpowers/plans/2026-04-24-websocket-client-p1-reconnect-and-degraded.md` |
| 不确定某条差距 (G1~G11) 是什么 | `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` |
| 不确定其他 Phase 的范围 | `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md` |
| 看不懂 P0 为什么那样做 | `doc/superpowers/plans/2026-04-24-websocket-client-p0-production-safety.md` + `git log --oneline dev` |
| 热路径 / 冷路径概念 | 本仓库没有专门文档；直接读 `core/websocket/critical_session.h`（热）和 `cold_path_loop.h`（冷）对比 |

遇到 plan 里没覆盖的决策（例如新发现一个 edge case 需要选择）→ **停下来问 user**，不要自己决定。已经花了大量时间做的决策（例如决策 6 eventfd）不要轻易回退。
