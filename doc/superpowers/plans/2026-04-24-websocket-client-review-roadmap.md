# WebSocket Client Review 处置路线图（Review Roadmap）

## 文档信息

- 版本：`v0.3`
- 状态：`P3 收尾中`（P0 / P1 / P2-A / P2-B 已完成，当前 P3）
- 创建日期：`2026-04-24`
- 文档定位：对 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` 中 G1–G11 共 11 条差距的整体分阶段处置计划。本文件是**索引 / 路线图**，不承载具体任务拆分；每个 Phase 对应一份独立的 plan 文档，按需展开。

## 关联文档

- `doc/websocket_client_design_v1.0.md`
- `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
- `doc/superpowers/plans/2026-04-23-websocket-client-minimal-hft.md`
- `doc/superpowers/plans/2026-04-24-websocket-client-p0-production-safety.md`（Phase 0）
- `doc/superpowers/plans/2026-04-24-websocket-client-p1-reconnect-and-degraded.md`（Phase 1）
- `doc/websocket_frame_codec_receive_strategies.md`（Phase 2-A 总结）
- `doc/superpowers/plans/2026-04-26-websocket-client-p2b-hotpath-pacing.md`（Phase 2-B）
- `doc/superpowers/specs/2026-04-26-websocket-client-p2b-hotpath-pacing-design.md`（Phase 2-B 设计）
- `doc/superpowers/plans/2026-04-27-websocket-client-p3-cleanup-validation.md`（Phase 3）
- `doc/websocket_client_future_optimizations.md`（P3 后优化 backlog）

## 目标

覆盖 review 中全部 11 条差距，把当前 `core/websocket` 实现从"骨架跑通"推进到 v1.0 设计所定义的"最小可接受验收"。阶段划分按"做完之后系统能干什么"排序，bug-first，然后补功能缺失，最后做性能/收尾。

## Gap → Phase 分配

| Gap | 主题 | 阶段 | 所属 plan |
|---|---|---|---|
| G6 | 握手 nonce 硬编码 | **P0** | `2026-04-24-websocket-client-p0-production-safety.md` |
| G3 | 背压被当作致命错误 | **P0** | 同上 |
| G5 | 冷路径无超时 | **P0** | 同上 |
| G7 | 无重连 / 退避 / 失败分类 | **P1** | `2026-04-24-websocket-client-p1-reconnect-and-degraded.md` |
| G9 | 缺少 `Degraded` 状态 | **P1** | 同上 |
| G1 | 热路径动态分配 + 多次 memcpy | **P2-A** | `websocket_frame_codec_receive_strategies.md` / `2026-04-25-websocket-client-p2a-framecodec-zero-copy-design.md` |
| G10 | 容量耗尽未显式 fail-fast | **P2-A** | 同上（与 G1 合并设计） |
| G2 | `DriveRead` 单次 `ReadSome` | **P2-B** | `2026-04-26-websocket-client-p2b-hotpath-pacing.md` |
| G4 | 心跳与业务写无协调 | **P2-B** | 同上 |
| G8 | 心跳粒度绑在 spin iteration | **P2-B** | 同上 |
| G11 | 构建图形态核对 | **P3** | `2026-04-27-websocket-client-p3-cleanup-validation.md` |

## Phase 0 — 生产可用性修复（已完成）

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

## Phase 1 — 重连与降级（已完成）

**plan**：`doc/superpowers/plans/2026-04-24-websocket-client-p1-reconnect-and-degraded.md`

**范围**：G7 + G9

**已落地能力**：

- G7：`ReconnectPolicy`、失败分类器、xorshift jitter、`kReconnectBackoff`、eventfd 可中断 Stop、`CriticalSession::Reset()`。
- G9：`kDegraded`、`DegradedThresholds`、16-slot backpressure window、degraded metrics、状态切换通知。
- 测试和 benchmark 证据已回填到 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` 的 G7 / G9 条目。

---

## Phase 2 — 热路径与心跳（已完成，拆两个 plan）

### Phase 2-A：FrameCodec 零拷贝 + 容量 fail-fast（已完成）

**文档**：

- `doc/websocket_frame_codec_receive_strategies.md`
- `doc/superpowers/specs/2026-04-25-websocket-client-p2a-framecodec-zero-copy-design.md`

**范围**：G1 + G10

**为什么合并**：G1 要改的接收 buffer / ready queue 结构正是 G10 所说"容量耗尽是否显式 fail-fast"的触发点；分开改会两次推翻同一套接口，浪费测试。

**已落地能力**：

- 接收 buffer 改为 Linux mirrored receive ring，payload 通过 `MessageView::payload` 零拷贝借用 ring 内存。
- `CriticalSession::DriveRead` 直接读入 `FrameCodec::WritableSpan()`，生产路径不再需要外部 read buffer + `Feed()` copy。
- `FrameCodec` 默认路径收口为 direct one-frame delivery；`QueuedFrameCodec` 保留为 parse-ahead / ready queue 对照实现。
- 容量耗尽返回 `kCapacityExceeded`，递增 `frame_codec_capacity_exhaustions` 并进入 degraded 观测，不再静默扩容。
- 单元测试、session read path benchmark、第三方 codec 对比 benchmark 已记录在相关文档和 review G1 / G10 条目中。

### Phase 2-B：热路径节奏（读循环 + 心跳 + 时钟）（已完成）

**plan**：`doc/superpowers/plans/2026-04-26-websocket-client-p2b-hotpath-pacing.md`

**范围**：G2 + G4 + G8

**为什么合并**：三条都是 spin loop 中"读 / 写 / 时钟"节奏问题，分开改会互相干扰彼此的 benchmark 基线。

**已落地能力**：

- G2：`ConnectionConfig::max_reads_per_drive` / `read_until_would_block`，`CriticalSession::DriveRead()` bounded read pump，`TlsSocket::PendingReadableBytes()` 基于 `SSL_pending()`。
- G4：dedicated control write slot，业务 queue 满时 heartbeat ping / auto-pong 不再消耗业务 `PreparedWriteArena` slot；partial business frame 不被 control frame 打断。
- G8：`ClockSource` 与 `runtime_clock.h`，`ActiveSpinLoop` 支持 runtime clock source，`RuntimeSession` 复用 loop clock 做 heartbeat 和 degraded evaluation。
- 验证证据已回填到 review G2 / G4 / G8；P3 之后的性能优化候选项记录在 `doc/websocket_client_future_optimizations.md`。

---

## Phase 3 — 收尾（当前）

**plan**：`doc/superpowers/plans/2026-04-27-websocket-client-p3-cleanup-validation.md`

**范围**：
- G11 构建图核对（已基本核实 `aquila_core` 为 STATIC，与最初 plan 一致）
- v1.0 "最小可接受验收" 的长稳运行证据补齐
- README 补本仓库实际的构建、运行、benchmark 指引（plan 文档要求但当前 README 未更新）
- 把 review 文档中 G1–G11 的"处理方案 / 关联提交 / 验证证据 / 确认日期"块逐条补齐（review 末尾已预留模板）

**产出证据**：

- G11 构建图已核对并补齐 P2 新增 websocket headers 到 `core/websocket/CMakeLists.txt` 与 `core/aquila_core.cpp`。
- `README.md` 已补当前构建、测试、benchmark、live probe 和长稳验证指引。
- `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` 已回填 G11 处理方案和验证证据。

**P3 验证证据（2026-04-27）**：

- `./build.sh debug`：通过。
- `ctest --test-dir build/debug -R websocket_ --output-on-failure`：14/14 通过。
- `./build.sh release`：通过。
- `ctest --test-dir build/release -R websocket_ --output-on-failure`：14/14 通过。
- release benchmark smoke（`taskset -c 2`）：
  - `session_read_path`：p50/p99/p99.9 = 409/449/2566 ns。
  - `session_read_path_burst_single_read`：79/109/1230 ns。
  - `session_read_path_burst_bounded_pump`：78/100/1276 ns。
  - `session_write_path`：419/446/1207 ns。
  - `session_write_path_control_slot_full_business_queue`：39/41/46 ns。
  - `active_spin`：42/43/44 ns。
  - `clock_source_steady`：52/54/56 ns。
  - `clock_source_monotonic`：51/54/123 ns。
  - `clock_source_monotonic_coarse`：34/35/35 ns。
  - `runtime_loopback`：3465160/3788280/3800920 ns。
- live probe：本轮未执行外网 live smoke；4h live 长稳保留为合并 `main` 前按环境执行的 release gate。

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
- P1 的 `kDegraded` / `kConsumerFatal` 行为影响了 P2 的 fail-fast 分流
- P2-A 的接收 buffer 布局决定了 P2-B 的 `DriveRead` 预算语义

## 一句话取舍

当前位置是"骨架跑通但不可运营"；P0 让它**不自杀**，P1 让它**能自愈**，P2 让它**够快**，P3 让它**可交付**。

## 下一步

- [x] P0：关闭 G6 / G3 / G5
- [x] P1：关闭 G7 / G9
- [x] P2-A：关闭 G1 / G10
- [x] P2-B：关闭 G2 / G4 / G8
- [x] P3：关闭 G11，补本地验证证据和最终交付文档（4h live 长稳保留为合并 `main` 前 release gate）
