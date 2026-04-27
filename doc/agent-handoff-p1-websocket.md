# Agent 接手说明：WebSocket Client 阶段归档

## 这份文件是给谁的

本文件原本用于接手 **Phase 1（WebSocket client 的重连与降级）**。截至 2026-04-27，P0、P1、P2-A、P2-B、P3 均已完成并合入 `main`；已完成的阶段执行 plan 文档已删除。

本文仅作为历史归档入口，不再作为执行计划使用。

当前已完成：

- P0：随机 nonce、背压非致命、冷路径总预算超时。
- P1：重连 / 退避 / 失败分类、`kDegraded` 状态与 degraded evaluator。
- P2-A：mirrored receive ring、FrameCodec 零拷贝、容量 fail-fast、decode direct fast path。
- P2-B：bounded read pump、dedicated control write slot、runtime clock source / cadence。
- P3：构建图核对、README / roadmap / review 文档闭环、debug / release 验证和 benchmark smoke。

当前建议入口：

1. 阶段闭环总览：`doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`
2. 差距处理证据：`doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
3. FrameCodec 接收策略总结：`doc/websocket_frame_codec_receive_strategies.md`
4. P2-A 设计规格：`doc/superpowers/specs/2026-04-25-websocket-client-p2a-framecodec-zero-copy-design.md`
5. P2-B 设计规格：`doc/superpowers/specs/2026-04-26-websocket-client-p2b-hotpath-pacing-design.md`
6. 后续优化 backlog：`doc/websocket_client_future_optimizations.md`
7. 构建、测试、benchmark、probe 指引：`README.md`

> 注意：阶段执行 plan 已按完成状态删除；历史决策以 review 条目、roadmap 摘要、设计规格和 commit 历史为准。

---

## 30 秒速览

- **项目**：crypto HFT 系统，重点是面向 gate.io / binance 的低延迟 C++ WebSocket client
- **当前分支基线**：`main`
- **当前位置**：P0 / P1 / P2-A / P2-B / P3 已完成
- **下一步**：从 `doc/websocket_client_future_optimizations.md` 选择后续优化，或进入新的交易所接入 / 订单链路工作
- **风格**：TDD + 原子提交 + 中英混写（文档中文、代码注释英文、commit message 英文）

---

## 归档说明

旧版 P1 handoff 中的执行步骤已经全部关闭，不再保留在本文中，避免后续接手时误按历史 plan 执行。

如需追溯具体决策：

- P0 / P1 / P2 / P3 的阶段归属和完成状态：看 roadmap。
- G1-G11 的问题描述、处理方式、验证证据：看 review 文档。
- FrameCodec decode / read ring / 第三方策略对比：看 FrameCodec 接收策略总结。
- P2-A / P2-B 的设计约束：看对应 specs。
- 后续仍可做的优化：看 future optimizations。

---

## 后续建议

后续如果继续 WebSocket 方向，优先从 `doc/websocket_client_future_optimizations.md` 中选一个明确条目，先补 benchmark 或最小验证，再做实现。若进入交易所接入、订单链路或风控链路，应重新建立新的计划文档或设计规格，不复用已经完成的 WebSocket 阶段 plan。
