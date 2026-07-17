# Bitget Fast Fill 时间诊断实施计划

## 目标

在不改变 authoritative 订单状态、反馈 SHM ABI 或交易决策的前提下，为 Bitget UTA 私有链路补齐可离线关联的时间证据：

- `OrderFeedbackSession` 在现有 `order` subscription 之外 best-effort 订阅 `fast-fill`。
- 原样记录 `fast-fill` 的逐笔成交身份、价格、数量、maker/taker 和交易所时间字段。
- 补录 Place Ack 的 `cTime`、顶层 `ts`，以及 order push 的顶层 `ts`、`createdTime`、`updatedTime`、
  `orderStatus`、`cancelReason` 和消息排序字段。
- 为下单发送、底层 write completion 和 Ack receive 同时记录 realtime 与 monotonic 时间。
- 保留现有 BBO recorder，由离线 report 按 `clientOid` / `orderId` / signal data index 合并证据。

## 非目标

- 不从 `fast-fill` 构造或发布 `OrderFeedbackEvent`。
- 不让 `fast-fill` 进入 `OrderManager`、策略、风控、恢复、reconcile 或任何交易状态机。
- 不修改 `OrderFeedbackEvent`、OrderGateway SHM 或其他通用 SHM ABI。
- 不在本轮实现 `execId` 去重、跨流累计成交量、最快 feedback 路径或双路状态合并。
- 不新增 `publicTrade`、`books50` recorder，不修改现有 BBO 二进制格式。
- 不把 `execTime` 解释为 order ingress，也不对零成交 IOC 作“漏撮合”归因。

## 已锁定决策

1. 现有 `order` topic 仍是唯一 authoritative feedback；`Ready()` 只由 login 与 `order` subscription 决定。
2. `fast-fill` 使用同一 private connection 的独立 subscription 状态；普通订阅或解析错误只进入 diagnostics，
   不发布 continuity lost，不阻断 order feedback。认证失效类错误仍按整个 private connection 的既有规则重连。
3. `fast-fill` handler 只依赖独立 parser 与 structured logger，不 include 或构造 `OrderFeedbackEvent`。
4. 原始协议字段、`connection_generation`、本地 message sequence 和 batch index 必须保留；派生 ns 值只能附加。
5. realtime 用于与 Bitget `cTime` / `ts` / `execTime` 和 BBO 对齐；本机 duration 只使用 monotonic。
6. write completion 必须来自底层 WebSocket 完成整帧 write 的时点；若连接在完成前中断，则明确记录为 unavailable，
   不用 `SendText` 返回时间替代。

## 影响边界

- `core/websocket`：为调用方拥有的长生命周期 `WritePathDiagnostics` 增加显式的 until-complete 生命周期，
  并记录 completion monotonic 时间。默认调用语义保持不变。
- `exchange/bitget/trading/order_session.h`：请求 correlation 持有 write diagnostics 和 paired clocks，仅扩充日志，
  不改变 response handler 或 gateway event contract。
- `exchange/bitget/trading/order_feedback_parser.h`：识别 `fast-fill` envelope，并向 session 暴露 order 原始诊断记录；
  现有 event 映射与 continuity 语义不变。
- `exchange/bitget/trading/fast_fill_parser.h`：新增独立、无交易 event 依赖的 parser。
- `exchange/bitget/trading/order_feedback_session.h`：维护非门控 fast-fill subscription、解析统计和日志。
- `docs/diagnostic_fields.md`、`docs/bitget_trading.md`：成为完成后的长期 contract 事实源。

## 实施步骤

1. 先补 parser/session/core WebSocket 测试，证明当前缺少 fast-fill dispatch、原始 order 时间字段和 deferred
   write completion。
2. 实现独立 fast-fill record/parser，覆盖合法 payload、缺字段、错误 topic 和时间溢出。
3. 扩展 order parser 的 envelope 分类与原始诊断 sink；保持原有单参数 event sink API 可用。
4. 扩展 FeedbackSession 的第二 subscription 状态与日志，验证 fast-fill 失败不改变 `Ready()`、不发布 feedback
   或 continuity lost。
5. 为 WebSocket write diagnostics 增加显式 until-complete 生命周期；Bitget correlation 在 send 前建立并保证指针
   生命周期覆盖 pending write，失败时回滚。
6. 扩充 Place/order/fast-fill structured log，并同步 stats、probe summary 和诊断字段文档。
7. 完成 focused build/test、WebSocket write-path benchmark、Bitget 回归、完整测试和最终 diff review。

## 验证策略

- 单元测试：
  - fast-fill 字段完整映射、毫秒到纳秒边界和非法 payload。
  - order parser 保留顶层 `ts`、`createdTime`、原始状态/原因、batch index。
  - FeedbackSession 分别确认 order/fast-fill ACK；fast-fill 数据不产生 `OrderFeedbackEvent`。
  - fast-fill subscription/parse failure 不回退 authoritative `Ready()`，认证失效除外。
  - pending/partial WebSocket write 在后续 drive 完成时才填充 write-complete paired clocks。
  - Bitget Ack 日志关联数据在同步和 deferred write 下均保持有效。
- 回归：
  - Bitget trading focused tests。
  - WebSocket focused tests。
  - Release build 与完整 `ctest`。
  - evaluation 边界检查。
- 性能：
  - 修改前后运行 WebSocket session write-path benchmark；不在没有 fresh 数据时声明无回归。

## 回滚

- 回滚本 feature branch 即恢复单一 `order` subscription 与原日志 schema。
- `fast-fill` 不写 SHM、不改变订单状态，因此回滚不需要迁移 persistent state。
- 如果 live 只读验证发现 fast-fill subscription 不稳定，可只撤销该 best-effort subscription；order feedback contract
  仍保持不变。

## 剩余风险

- Bitget 文档没有为零成交 IOC 提供 request ingress 或 match-attempt 时间戳；本实现无法补出该事实。
- `execTime` 为毫秒级成交时间，只能用于已成交样本的离线经验区间。
- 同一连接的 `fast-fill` 处理仍消耗少量 owner-thread CPU；必须通过测试和后续运行证据观察，不能预先宣称零影响。
- 若未来让 `fast-fill` 进入交易状态，必须另开 L3 设计并解决 `execId` 去重、乱序、漏消息、重连、reconcile 和双路一致性。
