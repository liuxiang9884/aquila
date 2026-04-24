# WebSocket Client Review 处置路线图（Review Roadmap）

## 文档信息

- 版本：`v0.1`
- 状态：`待讨论`
- 创建日期：`2026-04-24`
- 文档定位：对 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` 中 G1–G11 共 11 条差距的整体分阶段处置计划。本文件是**索引 / 路线图**，不承载具体任务拆分；每个 Phase 对应一份独立的 plan 文档，按需展开。

## 关联文档

- `doc/websocket_client_design_v1.0.md`
- `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
- `doc/superpowers/plans/2026-04-23-websocket-client-minimal-hft.md`
- `doc/superpowers/plans/2026-04-24-websocket-client-p0-production-safety.md`（Phase 0）

## 目标

覆盖 review 中全部 11 条差距，把当前 `core/websocket` 实现从"骨架跑通"推进到 v1.0 设计所定义的"最小可接受验收"。阶段划分按"做完之后系统能干什么"排序，bug-first，然后补功能缺失，最后做性能/收尾。

## Gap → Phase 分配

| Gap | 主题 | 阶段 | 所属 plan |
|---|---|---|---|
| G6 | 握手 nonce 硬编码 | **P0** | `2026-04-24-websocket-client-p0-production-safety.md` |
| G3 | 背压被当作致命错误 | **P0** | 同上 |
| G5 | 冷路径无超时 | **P0** | 同上 |
| G7 | 无重连 / 退避 / 失败分类 | **P1** | 待建 |
| G9 | 缺少 `Degraded` 状态 | **P1** | 同上 |
| G1 | 热路径动态分配 + 多次 memcpy | **P2** | 待建（P2-A：FrameCodec 重写） |
| G10 | 容量耗尽未显式 fail-fast | **P2** | 同上（与 G1 合并设计） |
| G2 | `DriveRead` 单次 `ReadSome` | **P2** | 待建（P2-B：热路径节奏） |
| G4 | 心跳与业务写无协调 | **P2** | 同上 |
| G8 | 心跳粒度绑在 spin iteration | **P2** | 同上 |
| G11 | 构建图形态核对 | **P3** | 待建（或合并进 P2 收尾） |

## Phase 0 — 生产可用性修复（已建）

**plan**：`2026-04-24-websocket-client-p0-production-safety.md`

**范围**：G6（随机 nonce）、G3（背压 ≠ 致命）、G5（冷路径总预算超时）。

**为什么先做**：这三条在当前实现中都是"跑就出问题"的 bug：
- G6 是 RFC 违规，服务端抽查就断
- G3 意味着任何 consumer 短暂卡顿都会断链，当前还没有重连，就是永久死亡
- G5 意味着 DNS/TLS 一卡，进程变僵尸

**产出证据**：
- handshake 随机 nonce 单元测试（uniqueness + base64 长度 + 16 字节 round-trip）
- backpressure 不断链 + fatal 重连的拆分测试
- 冷路径黑洞超时测试
- Gate live probe 回归

---

## Phase 1 — 重连与降级（待建）

**拟建 plan**：`doc/superpowers/plans/2026-04-24-websocket-client-p1-reconnect-and-degraded.md`

**范围**：G7 + G9

**核心决策（Phase 1 plan 中逐条确认）**：

1. **重连策略落位**（G7）
   - 重连循环放在 `WebSocketClient`（Layer 2），`CriticalSession`（Layer 1）保持用户驱动、纯状态容器。
   - 退避算法：指数 + 抖动（底线 100ms、上限可配、`jitter ∈ [0.5, 1.5)`）。
   - 按 `ConnectionError` 分两类：
     - 网络抖动类（`kSocketError`、`kPeerClosed`、`kHeartbeatTimeout`、`kConnectTimeout`、`kTlsFailure`、`kProtocolError`）→ 指数退避重连
     - 配置 / 认证 / 消费者致命类（`kHandshakeFailure`、`kResolveFailure`、P0 新增的 `kConsumerFatal`）→ 暂停重试，要求外部明确指令
   - `ConnectionPhase::kReconnectBackoff` 真正落地，对外通过 `StateHandler` 通知。
   - `Metrics.reconnects` 计数在此阶段首次有效。

2. **Degraded 状态**（G9）
   - 新增 `ConnectionPhase::kDegraded`，位于 `kActive` 与 `kReconnecting` 之间。
   - 进入条件（阈值全部可配，默认值在 plan 中讨论）：
     - `prepared_write_high_watermark` 持续超过某比例（例如 80% 连续 N 轮）
     - 心跳 RTT 超过阈值（依赖 P2 的 RTT 观测，本阶段可先只记录 `awaiting_pong_` 持续时长）
     - `consumer_backpressure_drops` 在滑动窗口内超阈值
   - 退出条件：所有触发因子连续 M 轮回落。
   - 新增 metrics：`degraded_enter_count`、`degraded_exit_count`。
   - Degraded 不等于断链；只对外通知，让外部决策限流 / 降级 / 继续观察。

**依赖**：P0 完成（G3 背压降级后才能在 Degraded 判定中引用新计数器；G5 超时后 `kConnectTimeout` 才真正落盘）。

**产出证据**：
- 重连按失败分类分流的测试（注入不同 `ConnectionError`，观察是否进入退避或暂停状态）
- 退避抖动合理性测试（连续重连不会同步尖峰）
- Degraded 进入 / 退出的单元测试（阈值触发 + 回落）
- 长稳模拟：注入周期性断链，确认长稳运行下重连次数和状态机路径符合预期

---

## Phase 2 — 热路径与心跳（待建，拆两个 plan）

### Phase 2-A：FrameCodec 零拷贝 + 容量 fail-fast

**拟建 plan**：`doc/superpowers/plans/2026-04-24-websocket-client-p2a-framecodec-zero-copy.md`

**范围**：G1 + G10

**为什么合并**：G1 要改的接收 buffer / ready queue 结构正是 G10 所说"容量耗尽是否显式 fail-fast"的触发点；分开改会两次推翻同一套接口，浪费测试。

**核心决策**：
- 接收 buffer 换成"环形字节 buffer + 定长 `ReadyFrame` ring"，`MessageView::payload` 直接指向环形 buffer 中的帧区间。
- 客户端入站 frame **无需解掩码**（RFC 6455 服务器→客户端默认 unmasked），省一轮 XOR。
- 长消息跨越环形 buffer 边界时的处理策略：拒收并进入 `Degraded`，或有限次允许单次拷贝（需讨论）。
- ready queue 定长；满了 fail-fast 进入 `Degraded`。
- `FrameCodec::Feed` 输入环形 buffer 的 `span` 而非重新入队。

**依赖**：P1 的 `Degraded` 状态落地后，容量 fail-fast 才能映射到合理的状态转移。

**产出证据**：
- 单元测试：跨环形 buffer 边界的帧、多帧突发、超长帧拒收
- 微基准：`frame_codec_benchmark` 的 p50 / p99 / p99.9 需比 P0 基线显著下降（具体阈值在 plan 中定）

### Phase 2-B：热路径节奏（读循环 + 心跳 + 时钟）

**拟建 plan**：`doc/superpowers/plans/2026-04-24-websocket-client-p2b-hotpath-pacing.md`

**范围**：G2 + G4 + G8

**为什么合并**：三条都是 spin loop 中"读 / 写 / 时钟"节奏问题，分开改会互相干扰彼此的 benchmark 基线。

**核心决策**：
- **G2**：`DriveRead` 引入有界多轮 `ReadSome`，预算按字节或次数（二选一，benchmark 支撑）。
- **G4**：独立的 control-frame slot / 快路径，使心跳 ping 不被业务写排队拖延；`Metrics` 新增 ping 入队到 sendto 的耗时。
- **G8**：时钟打点从 spin iteration 解耦，换成 `clock_gettime(CLOCK_MONOTONIC_COARSE)` 或带校准的 `rdtsc`，具体选型需 benchmark 对比。

**依赖**：P2-A 完成（FrameCodec 新节奏下再测 `DriveRead` 预算才有意义）。

**产出证据**：
- 链路基准：`active_spin_benchmark` 在不同预算下的 p99.9 变化
- 心跳 RTT 分布（业务写空载 vs 满载）
- 时钟调用开销 micro-benchmark

---

## Phase 3 — 收尾（待建）

**拟建 plan**：`doc/superpowers/plans/2026-04-24-websocket-client-p3-cleanup.md`（或合并进 P2-B 的尾声）

**范围**：
- G11 构建图核对（已基本核实 `aquila_core` 为 STATIC，与最初 plan 一致）
- v1.0 "最小可接受验收" 的长稳运行证据补齐
- README 补本仓库实际的构建、运行、benchmark 指引（plan 文档要求但当前 README 未更新）
- 把 review 文档中 G1–G11 的"处理方案 / 关联提交 / 验证证据 / 确认日期"块逐条补齐（review 末尾已预留模板）

**产出证据**：
- 长稳运行（≥ 4h）内存与水位稳定性
- p99.9 长时段分布
- review 文档完成闭环

---

## 阶段间依赖图

```text
P0 (G6 / G3 / G5)
  │
  ▼
P1 (G7 / G9)
  │
  ├──▶ P2-A (G1 / G10)         <- 依赖 P1 Degraded 落地
  │         │
  │         ▼
  └──▶ P2-B (G2 / G4 / G8)      <- 依赖 P2-A FrameCodec 节奏稳定
              │
              ▼
            P3 (G11 + 验收)
```

横向上，同一 Phase 内部的任务如果彼此独立可以并行（例如 P1 的 G7 和 G9 大部分改动可以切分），但**不建议跨 Phase 并行**，因为：
- P1 的 `kDegraded` / `kConsumerFatal` 行为会影响 P2 的 fail-fast 分流
- P2-A 的接收 buffer 布局会决定 P2-B 的 `DriveRead` 预算语义

## 一句话取舍

当前位置是"骨架跑通但不可运营"；P0 让它**不自杀**，P1 让它**能自愈**，P2 让它**够快**，P3 让它**可交付**。

## 下一步

- [ ] 按 P0 plan 的 Scope Decision 闸门逐条过：Task 1 (G6) → Task 2 (G3) → Task 3 (G5)
- [ ] P0 三个 Task 完成并验证后，立项 P1 plan 细稿（G7 策略与 G9 阈值）
- [ ] P1 完成验收后，立项 P2-A
- [ ] P2-A 完成后，立项 P2-B
- [ ] 最后立项 P3，关闭 review 文档
