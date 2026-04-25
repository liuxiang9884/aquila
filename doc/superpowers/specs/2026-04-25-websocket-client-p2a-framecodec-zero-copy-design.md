# WebSocket Client P2-A FrameCodec Mirrored Ring Design

## 文档信息

- 日期：2026-04-25
- 状态：设计规格；user 已确认按最低延迟优先的 mirrored ring 方案推进
- 范围：P2-A，覆盖 `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` 中的 G1 与 G10
- 关联文档：
  - `doc/websocket_client_design_v1.0.md`
  - `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
  - `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`

## 目标

P2-A 的目标是把当前 `FrameCodec` 从“`std::vector` 聚合 + payload 拷贝 + `std::deque` ready queue”改成初始化阶段确定容量、热路径无隐式扩容、入站消息以连续 `std::span<const std::byte>` 零拷贝交付的实现。

最低延迟优先级下，主方案采用 **mirrored ring buffer**：

```text
virtual address:
[ receive ring ][ receive ring alias ]
```

两段虚拟地址映射同一块物理内存。这样即使 payload 在物理 ring 尾部跨界，在虚拟地址中仍是一段连续内存，可以继续用单个 `MessageView::payload` 表达。

## 关闭的差距

- G1：去掉 `inbound_bytes_.insert`、`decoded_payload_.resize`、`inbound_bytes_.erase`、`ReadyFrame::payload.assign` 这几类热路径复制 / 移动。
- G10：容量耗尽时显式暴露为可观测事件，不再依赖 `std::vector` / `std::deque` 的容量自恢复或隐式堆分配。

## 不在范围内

- 不改变 `MessageConsumer` 的回调形式。
- 不把 `MessageView::payload` 改成双段 view。
- 不做 P2-B 的有界多轮 `DriveRead`、心跳优先级、时钟粒度改造。
- 不新增 WebSocket fragmentation 支持；当前 `fin == false` 仍按协议错误处理。
- 不把 text / binary payload 解析成 JSON、结构化行情对象或字符串。

## 当前接口语义

`MessageView::payload` 是对已解码 WebSocket payload 的只读借用视图：

```cpp
struct MessageView {
  PayloadKind kind{PayloadKind::kBinary};
  std::span<const std::byte> payload{};
  std::uint64_t sequence{0};
  bool fin{true};
};
```

P2-A 保持这个接口不变，并明确生命周期约束：

- `payload` 只在当前 `MessageConsumer::Handle(const MessageView&)` 调用内有效。
- consumer 如果要跨回调保存 payload，必须自己复制。
- `FrameCodec::Feed`、`FrameCodec::Poll`、`FrameCodec::Reset` 或 `CriticalSession::DriveRead` 的下一次推进可以复用内部 buffer。

这个约束匹配当前使用方式：`CriticalSession` 在收到 `DecodeStatus::kMessageReady` 后立即同步调用 consumer，随后才继续 drain 下一帧。

## 设计决策

### 决策 1：保留单 `std::span<const std::byte>`

P2-A 保留单段连续 payload view，不引入 `{first, second}` 双段 view。

原因：

- 当前 consumer 和测试都围绕单段连续内存设计；改成双段 view 会把复杂度扩散到所有下游 parser。
- 大部分交易所 WebSocket payload 是小型 text/binary message，消费者通常会直接做 JSON / binary parser，连续内存是最低分支成本的输入。
- mirrored ring 能把物理跨界 payload 映射成虚拟连续区间，不需要把 ring 边界暴露给 consumer。

### 决策 2：mirrored ring 是主路径，不做边界线性化

普通 ring 的边界问题：

```text
physical ring:
[ payload head ... ][ ... payload tail ]
```

mirrored ring 的虚拟视图：

```text
virtual view:
[ ... payload tail ][ payload head ... ]
```

只要 `frame_size <= receive_capacity`，从 `base + (payload_abs % receive_capacity)` 开始的 `payload_size` 字节在虚拟地址里总是连续。因此：

- 常规 payload：`MessageView::payload` 指向 receive ring。
- 跨物理尾部 payload：`MessageView::payload` 仍指向 receive ring 的 mirrored 虚拟连续区间。
- 不需要预分配线性化缓冲。
- 不需要 edge copy metric；边界本身不是性能异常。

### 决策 3：socket/TLS 直接读入 codec writable span

当前路径是：

```text
TLS read -> CriticalSession::read_buffer_ -> FrameCodec::Feed -> codec storage
```

P2-A 改为：

```text
TLS read -> FrameCodec::WritableSpan -> FrameCodec::CommitWritten -> FrameCodec::Poll
```

`FrameCodec::WritableSpan()` 返回 mirrored ring 当前可写连续区间。`CriticalSession::DriveRead` 直接把这个 span 传给 `TlsSocket::ReadSome`，不再维护单独的 read buffer。

`Feed(bytes)` 保留给测试、冷路径和兼容调用；它只把外部 bytes 复制到 mirrored ring，不触发动态分配。

### 决策 4：ready queue 只保存 metadata

当前 `ready_frames_` 通过 `std::deque<ReadyFrame>` 保存 payload 拷贝。P2-A 改成固定容量 `ReadyFrame` ring，只保存 metadata：

```cpp
struct ReadyFrame {
  PayloadKind kind{PayloadKind::kBinary};
  std::uint64_t sequence{0};
  bool fin{true};
  std::uint64_t frame_abs{0};
  std::uint32_t frame_size{0};
  std::uint64_t payload_abs{0};
  std::uint32_t payload_size{0};
};
```

ready ring 不拥有 payload，也不分配 payload。交付时把 `payload_abs` 转成 mirrored ring 上的连续 `std::span`。

后续 direct delivery 优化后，默认 `Poll()` 路径不再把每个完整 frame 先写入 ready ring，而是每次直接解析并交付一帧。ready ring 保留为 future batch / parse-ahead fallback；`ready_frame_slots == 0` 不再阻止单帧或 repeated `Poll()` 多帧交付。

### 决策 5：容量耗尽显式 fail-fast，并接入 Degraded

P2-A 新增 codec 资源耗尽状态，不把它伪装成协议错误。

```cpp
enum class DecodeStatus : uint8_t {
  kNeedMore,
  kMessageReady,
  kProtocolError,
  kCapacityExceeded,
};
```

容量耗尽包括：

- receive ring 没有空间接收下一段入站数据。
- mirrored buffer 初始化失败，codec 不可用。
- future batch / parse-ahead fallback 启用时，ready frame ring 已满且无法继续保留已解析 frame。

payload 长度超过 WebSocket / 配置上限不归为内部容量耗尽，仍属于协议或配置层错误，按 `kProtocolError` / reconnect 路径处理。

新增指标：

```cpp
std::uint64_t frame_codec_capacity_exhaustions{0};
std::uint64_t frame_codec_ready_high_watermark{0};
```

`DegradedSample` 增加 `frame_codec_capacity_exhaustions`。`DegradedThresholds` 增加 1s 窗口阈值：

```cpp
std::uint32_t frame_codec_capacity_events_per_second = 1;
```

默认值为 1，表示只要 1 秒窗口内出现 codec 容量耗尽，就进入 `kDegraded`。

## 目标结构

P2-A 后的 `FrameCodec` 仍保持 header-only 使用方式，但把 Linux mirrored mapping 封装到独立 RAII helper：

```cpp
class MirroredBuffer {
 public:
  bool Init(size_t requested_capacity) noexcept;
  void Reset() noexcept;
  std::byte* data() noexcept;
  size_t capacity() const noexcept;
};
```

`FrameCodec` 的目标接口：

```cpp
class FrameCodec {
 public:
  explicit FrameCodec(size_t max_payload_bytes) noexcept;
  FrameCodec(size_t max_payload_bytes,
             size_t receive_buffer_bytes,
             size_t ready_frame_slots) noexcept;

  void Reset() noexcept;

  std::span<std::byte> WritableSpan() noexcept;
  void CommitWritten(size_t bytes) noexcept;
  DecodeResult Feed(std::span<const std::byte> bytes) noexcept;
  DecodeResult Poll() noexcept;

  EncodeResult EncodeText(std::span<const std::byte> payload,
                          std::span<std::byte> output) noexcept;
  EncodeResult EncodeBinary(std::span<const std::byte> payload,
                            std::span<std::byte> output) noexcept;
  EncodeResult EncodeControl(PayloadKind kind,
                             std::span<const std::byte> payload,
                             std::span<std::byte> output) noexcept;
};
```

内部 cursor 使用绝对递增值：

```cpp
std::uint64_t consume_abs_;
std::uint64_t parse_abs_;
std::uint64_t write_abs_;
```

不变量：

```text
consume_abs_ <= parse_abs_ <= write_abs_
write_abs_ - consume_abs_ <= receive_capacity
```

## 数据流

### 常规连续 payload

```text
TLS read
  -> FrameCodec::WritableSpan
  -> FrameCodec::CommitWritten
  -> parse header
  -> ReadyFrame{payload_abs, payload_size}
  -> MessageView::payload = span(mirrored_base + payload_abs % capacity, payload_size)
  -> MessageConsumer::Handle(view)
```

这条路径没有 payload 拷贝、没有 ready queue payload 拷贝、没有 `vector::erase` 搬移。

### 跨物理尾部 payload

```text
physical ring:
[ payload head ... ][ ... payload tail ]

mirrored virtual view:
[ ... payload tail ][ payload head ... ]
```

交付仍然是：

```cpp
view.payload = std::span<const std::byte>(
    base + (payload_abs % receive_capacity),
    payload_size);
```

没有边界 copy，也不需要双段 view。

### 容量耗尽

```text
WritableSpan / Feed / Poll detects no bounded storage can safely hold progress
  -> DecodeStatus::kCapacityExceeded
  -> ++Metrics::frame_codec_capacity_exhaustions
  -> DegradedEvaluator 1s window observes the event
  -> ConnectionPhase::kDegraded notification
```

容量耗尽不再静默扩容，不再通过 allocator 行为“碰运气”。

## 错误处理

- WebSocket 协议错误：继续返回 `kProtocolError`，`CriticalSession` 映射到 `ConnectionError::kProtocolError`。
- payload 超过配置上限：返回 `kProtocolError`，保持现有断链 / 重连语义。
- 内部 bounded storage 耗尽：返回 `kCapacityExceeded`，递增指标，进入 degraded，不立即把 peer 当作坏连接。
- consumer backpressure：保持 P0/P1 语义，丢帧并递增 `consumer_backpressure_drops`。
- consumer fatal：保持现有 `ConnectionError::kConsumerFatal`。

## 配置

`ConnectionConfig` 增加两个字段：

```cpp
size_t max_frame_payload_bytes = size_t{1} << 20;
size_t ready_frame_slots = 1024;
```

保留现有 `frame_buffer_bytes`，语义收敛为 receive mirrored ring 的 requested capacity。`max_frame_payload_bytes` 是单帧 payload 上限。`FrameCodec` 构造时保证实际 mirrored capacity 至少能容纳 `max_frame_payload_bytes + 14` 字节。

## 测试策略

单元测试覆盖：

- 多个 coalesced server frame 在一次 `Feed` 后按 sequence 顺序 drain。
- `WritableSpan` + `CommitWritten` 可以交付跨物理尾部的 frame，payload 内容连续正确。
- `ready_frame_slots == 0` 时 coalesced 多帧仍可通过 repeated `Poll()` direct delivery 交付。
- payload 超过 `max_frame_payload_bytes` 返回 `kProtocolError`。
- receive ring 初始化失败或容量不足返回 `kCapacityExceeded`。
- `Reset()` 清空 ring cursor、ready ring、pending error 和 sequence。
- masked inbound server frame 仍返回 `kProtocolError`。
- control frame payload 超过 125 bytes 仍返回 `kProtocolError`。

集成测试覆盖：

- `CriticalSession::DriveRead` 直接读入 codec writable span。
- `CriticalSession::DriveRead` 收到 codec capacity exhausted 时不把连接映射为协议错误，而是递增 `frame_codec_capacity_exhaustions`。
- `WebSocketClient` 在 codec 容量耗尽事件进入 1s 窗口后通知 `kDegraded`。
- 已有 backpressure / reconnect / heartbeat 测试保持通过。

Benchmark 覆盖：

- `frame_codec_benchmark` 分成 encode、decode contiguous、decode mirrored-boundary 三组样本。
- `session_read_path_benchmark` 继续作为端到端读路径回归基准。
- P2-A 完成时必须和合入前 `dev` 基线对比 p50 / p99 / p99.9；不能只引用单次运行。

## 验收标准

- `FrameCodec` 热路径不再使用 `std::vector::insert`、`std::vector::erase`、`std::deque::push_back` 或 payload-owning `ReadyFrame`。
- `CriticalSession::DriveRead` 不再通过单独 read buffer 把数据 copy 进 codec。
- 常规 payload 和跨物理尾部 payload 均以单 `std::span` 零拷贝交付。
- 容量耗尽有独立状态和指标，并能驱动 `kDegraded`。
- debug / release 下 `ctest --test-dir build/<type> -R websocket_ --output-on-failure` 全部通过。
- release benchmark 与 P2-A 基线对比后，在 review 文档 G1 / G10 处理方案块中记录具体数值。

## 设计取舍总结

P2-A 选择 mirrored ring，是因为它在性能模型上最干净：主路径没有堆分配、没有 payload copy、没有 linear buffer compact、没有双段 parser 分支。代价是 Linux 平台相关的初始化复杂度更高，且 buffer capacity 需要按 page 对齐。这个成本全部在冷路径承担，不污染 WebSocket 收包热路径。

## 已实现优化：direct one-frame delivery

P2-A 完成后的第三方对比 benchmark 显示，三方库在“完整单帧已经在内存中，只做 frame header 解析并回调”的极窄场景下能做到 `2ns` 量级；优化前 `aquila` direct poll decode 约为 `13ns` 量级。差距主要来自 `aquila` 保留的 ready metadata ring、`MessageView` 构造、payload 生命周期和容量/降级边界。

direct one-frame delivery 已将默认 `Poll()` 路径调整为：

1. release 上一次 delivered frame。
2. 如果 ready ring 有历史 queued frame，先 drain。
3. 从 `parse_abs_` 开始解析一条完整 frame。
4. 继续执行现有协议检查、payload 上限检查和 mirrored ring span 构造。
5. 直接返回 `MessageView`，跳过 `QueueReadyFrame()` / `DrainReadyFrame()` 的 metadata 往返。
6. coalesced 多帧由上层 repeated `Poll()` drain，每次交付一帧。

该优化没有删除 `MessageView`、协议检查、capacity/degraded、mirrored cursor 和 payload 生命周期约束。ready ring 保留为 future batch / parse-ahead fallback，不再是默认 decode 主路径。release pinned 结果显示：`aquila_direct_poll_decode` p50/p99/p99.9 从 `13/14/158ns` 改善到 `6/6/24ns`，`aquila_coalesced_feed_drain` 从 `14/22/24ns` 改善到 `8/12/17ns`。详细对比记录在 `doc/websocket_third_party_comparison.md` 的“FrameCodec 对比和 direct delivery 优化记录”章节。
