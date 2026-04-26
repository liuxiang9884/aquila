# WebSocket 接收缓冲与 Frame Codec 实现策略对比

## 文档信息

- 状态：`accepted`（FrameCodec decode 主体已收口）
- 记录位置：`doc/`
- 适用范围：`WebSocket frame receive codec`、接收缓冲、payload 生命周期与低延迟路径设计
- 背景：本文总结当前讨论过的三类完整消息接收/解析方法，并说明 `third_party/websocket` 这类 parser-only 实现为什么不能单独承担生产 receive codec。

## 总结

完整的 WebSocket 接收实现至少要解决四件事：

1. 从 socket/TLS read 接收字节。
2. 保存未完成 frame。
3. 从 coalesced bytes 中逐帧解析。
4. 定义 payload 生命周期和 capacity/error 语义。

当前可比较的完整方法有三类：

| 名称 | 核心思想 | 代表实现 | 当前定位 |
| --- | --- | --- | --- |
| Mirrored Ring Zero-Copy Codec | mirrored receive ring + 零拷贝 `MessageView` | `aquila::websocket::FrameCodec` | 生产主路径 |
| Linear Carry Buffer Codec | 线性 `head/tail` buffer + 必要时 compact | benchmark-only `LinearFrameCodec`、大量简单 socket parser adapter | 简单、快，但尾部整理有毛刺 |
| Owning Message Accumulator Codec | 框架 buffer + payload 拷贝到 owning message | Drogon / Trantor 风格 | 生命周期简单，但拷贝成本高 |

`third_party/websocket` 的 `handleWSMsg(ptr, size)` 不是第四种完整 receive codec。它只是 parser：调用方必须自己提供输入 buffer、partial frame 保存、capacity 策略和 payload 生命周期。

## 方法一：Mirrored Ring Zero-Copy Codec

### 命名

建议命名为 **Mirrored Ring Zero-Copy Codec**。

含义：

- `Mirrored Ring`：底层 receive buffer 是双映射环形缓冲区。
- `Zero-Copy`：解析完成后，payload 不复制，`MessageView::payload` 直接指向 receive ring。
- `Codec`：它不是裸 parser，而是包含接收缓冲、partial frame、协议校验、生命周期和错误状态的完整接收 codec。

### 实现描述

核心状态：

```cpp
MirroredBuffer receive_ring;
uint64_t consume_abs; // 上层已经释放生命周期的位置
uint64_t parse_abs;   // 下一个待解析 frame 的起点
uint64_t write_abs;   // 已写入字节末尾
```

`MirroredBuffer` 把同一段物理内存映射到连续的两段虚拟地址：

```text
virtual memory:
[ ring physical bytes ][ same ring physical bytes again ]
```

这样即使 frame 跨过物理 ring 尾部，`Ptr(parse_abs)` 后面在虚拟地址上仍然是连续的。

接收流程：

```cpp
span<byte> writable = codec.WritableSpan();
ssize_t n = SSL_read(ssl, writable.data(), writable.size());
codec.CommitWritten(n);

for (;;) {
  DecodeResult decoded = codec.Poll();
  if (decoded.status == kMessageReady) {
    consumer(decoded.view);
    continue;
  }
  if (decoded.status == kNeedMore) {
    break;
  }
  handle_error(decoded.status);
  break;
}
```

解析伪代码：

```cpp
DecodeResult Poll() {
  ReleaseDeliveredFrame();

  if (protocol_error_pending) return ProtocolError();
  if (capacity_error_pending) return CapacityExceeded();

  available = write_abs - parse_abs;
  if (available < 2) return NeedMore();

  ptr = Ptr(parse_abs);
  header = ParseServerFrameHeader(ptr, available);
  if (header.need_more) return NeedMore();
  if (header.protocol_error) return ProtocolError();
  if (header.payload_length > max_payload_bytes) return ProtocolError();

  total = header.header_bytes + header.payload_length;
  if (available < total) return NeedMore();
  if (total > receive_ring.capacity()) return CapacityExceeded();

  frame_start = parse_abs;
  payload_start = frame_start + header.header_bytes;
  frame_end = frame_start + total;

  parse_abs = frame_end;
  delivered_pending = true;
  delivered_frame_end_abs = frame_end;

  return MessageReady({
      .kind = header.kind,
      .payload = span(Ptr(payload_start), header.payload_length),
      .sequence = next_sequence++,
      .fin = true,
  });
}
```

payload 生命周期：

```text
Poll() 返回 MessageView
  -> payload 借用 receive_ring 内存
下一次 Poll()/WritableSpan()/Feed()
  -> ReleaseDeliveredFrame()
  -> consume_abs 推进
  -> 旧 payload 对应空间可被后续 read 覆盖
```

这要求 consumer 同步消费，或者在跨线程/异步持有前主动复制。

### FrameCodec decode 主体定稿

当前生产 `FrameCodec` decode 主体采用 **mirrored receive ring + direct one-frame delivery + shared data-frame fast parser**。

主路径职责划分如下：

1. `WritableSpan()` / `CommitWritten()` 让 socket/TLS read 直接写入 codec owned mirrored ring。生产主路径不需要先读到外部 buffer 后再 `Feed()`。
2. `Poll()` 每次直接从 `parse_abs_` 解析并交付一帧，不经过 ready metadata queue。
3. `TryParseFastServerDataFrameHeader()` 是 text/binary data frame 的共享 fast primitive，`FrameCodec` 和 `ParseServerFrameHeader()` 都复用它。
4. `ParseServerFrameHeader()` 继续负责 generic header 解析和 WebSocket 协议结构校验。
5. `FrameCodec` / `QueuedFrameCodec` 各自负责 `max_payload_bytes` 这类 codec 配置上限，parser 不持有业务容量配置。

当前 fast primitive 的命中条件：

```text
FIN=1
RSV=0
server inbound unmasked
opcode=text 或 binary
payload length 使用 7-bit 或 16-bit 编码，也就是 <= 65535
```

命中后，`FrameCodec::ParseOneFrameDirect()` 直接调用 `BuildDirectMessage()` 构造 `MessageView`。如果未命中，例如 control frame、fragmentation、RSV、masked inbound、unknown opcode 或 64-bit payload length，则回落到 `ParseServerFrameHeader()` 的 generic path。

不再继续优化 decode 主体的原因：

- 小帧 direct decode 已稳定在低 ns 级，继续追逐 `1ns` 级差异容易被 code layout 和测量噪声主导。
- session read path 已进入约 `400ns` 级，真实链路更可能受 socket/TLS/session dispatch 影响。
- compact pressure 和 large payload boundary 场景已经证明 mirrored ring 的尾延迟价值。
- `QueuedFrameCodec` 保留为 parse-ahead / ready queue 对照路径，不污染生产 `FrameCodec` 主路径。

### FrameCodec 配置建议

`FrameCodec` 相关容量参数不是越大越好。高频行情主路径更关注确定性和尾延迟，所以配置目标是：足够容纳交易所合法消息和短 burst，同时让异常大包、消费停滞或容量估计错误尽快暴露。

相关字段：

- `max_frame_payload_bytes`：单个 WebSocket frame payload 上限。超过该值返回 `kProtocolError`，保持断链 / 重连语义。它应该贴近交易所协议允许的最大合法消息，而不是按机器内存随意放大。
- `frame_buffer_bytes`：mirrored receive ring 的 requested capacity。`FrameCodec` 构造时会先保证它至少能容纳 `max_frame_payload_bytes + 14` 字节，随后 `MirroredBuffer` 再按 page / power-of-two 对齐得到实际容量。
- `read_buffer_bytes`：保留给 legacy 或外部 buffer adapter 路径。当前 `CriticalSession` 生产读路径是 `WritableSpan()` -> socket/TLS read -> `CommitWritten()` -> `Poll()`，不会先读入 `read_buffer_bytes` 再复制进 codec。
- `degraded.frame_codec_capacity_events_per_second`：容量耗尽事件进入 degraded 的阈值。默认 `1` 是刻意严格的设置；生产中一次容量耗尽就说明 bounded receive storage 无法安全推进，应当作为降级信号观察，而不是静默扩容。

一个容易踩到的细节是默认值：

```cpp
frame_buffer_bytes = 1 MiB;
max_frame_payload_bytes = 1 MiB;
```

因为最大 WebSocket frame header 是 `14` 字节，实际 required capacity 是 `1 MiB + 14`，再经过 power-of-two 对齐后，默认 receive ring 实际会变成 `2 MiB`。如果希望实际 ring 严格保持 `1 MiB` 量级，可以把 `max_frame_payload_bytes` 设为 `1 MiB - 14`；如果必须接受完整 `1 MiB` payload，应显式接受 `2 MiB` ring 的内存和 cache footprint。

推荐起点：

| 场景 | `max_frame_payload_bytes` | `frame_buffer_bytes` | 容量事件阈值 | 说明 |
| --- | --- | --- | --- | --- |
| 高频增量行情 | `64 KiB` 或 `128 KiB` | `256 KiB` 或 `512 KiB` | `1` | 适合常规 order book / trade / ticker 增量消息，优先控制 footprint 和异常暴露速度。 |
| 混合行情与偶发快照 | `256 KiB` 或 `512 KiB` | `512 KiB` 或 `1 MiB` | `1` | 适合同一连接上存在较大订阅响应、批量 diff 或偶发 snapshot 的场景。 |
| 大快照 / replay / 未知上游 | `1 MiB - 14` 搭配 `1 MiB` ring，或 `1 MiB` payload 搭配 `2 MiB` ring | 按左侧选择 | 生产 `1`，离线 replay 可放宽 | 适合还没有完成协议画像或需要接收大消息的阶段；上线前应回收为更贴近真实上限的配置。 |

调参顺序：

1. 先用真实交易所频道样本确认最大合法 payload，而不是用 benchmark 小帧估计。
2. 设定 `max_frame_payload_bytes` 为“协议合法最大值 + 明确余量”；超过后应视为协议或订阅异常。
3. 设定 `frame_buffer_bytes` 至少覆盖 `max_frame_payload_bytes + 14`，再按 burst、partial frame 和 cache footprint 做权衡。
4. 线上目标是 `frame_codec_capacity_exhaustions == 0`。一旦出现容量事件，先确认是否有合法大包、consumer 停滞或配置过小，再决定是否扩大容量。

### 代表实现

- `aquila::websocket::FrameCodec`：当前生产主路径。
- `aquila::websocket::QueuedFrameCodec`：同样使用 mirrored ring，但额外做 parse-ahead ready queue，仅作为实验/对照。
- 通用 WebSocket 库中很少直接采用这种模型；它更多见于自研低延迟网络栈、共享内存队列或需要消除环尾拷贝的专用 buffer。对本仓库来说，明确代表就是 `aquila` 的 mirrored receive ring。

### 优点

- 没有 compact/memmove。
- 跨 ring 尾部的 frame 和 payload 仍是虚拟地址连续区间。
- partial frame 可以留在 ring 中等待下一次 read。
- coalesced 多帧可以 repeated `Poll()` drain。
- payload 零拷贝交付。
- capacity/error 语义可以在 codec 边界内统一建模。
- 对大 payload、partial payload、buffer boundary 压力场景更稳定。

### 缺点

- 实现复杂度高于线性 buffer。
- 依赖 OS 虚拟内存能力，例如 Linux `memfd_create` + `mmap` 双映射。
- 初始化失败需要明确 fallback 或 fail-fast 策略。
- `MessageView` 是借用视图，异步持有需要上层复制或 handoff 约束。
- 对极小完整帧，裸 parser 或简单线性 buffer 的 p50 可能更低。

## 方法二：Linear Carry Buffer Codec

### 命名

建议命名为 **Linear Carry Buffer Codec**。

含义：

- `Linear`：内部 receive buffer 是普通连续数组。
- `Carry Buffer`：未完成 frame 的 prefix 会保留在 buffer 中，等待下一次 read 的 suffix。
- `Codec`：它在 parser 外面补齐了输入缓冲、partial frame 和 drain 逻辑。

### 实现描述

核心状态：

```cpp
vector<byte> buffer;
size_t head; // 未消费数据起点
size_t tail; // 已写入数据末尾
```

接收流程：

```cpp
span<byte> writable = codec.WritableSpan();
ssize_t n = SSL_read(ssl, writable.data(), writable.size());
codec.CommitWritten(n);

while (true) {
  DecodeResult decoded = codec.Poll();
  if (decoded.status == kMessageReady) {
    consumer(decoded.view);
    continue;
  }
  if (decoded.status == kNeedMore) break;
  handle_error(decoded.status);
  break;
}
```

写入空间管理伪代码：

```cpp
span<byte> WritableSpan() {
  ReleaseDeliveredFrame();

  if (tail == buffer.size()) {
    CompactIfNeeded();
  }

  if (tail == buffer.size()) {
    capacity_error_pending = true;
    return {};
  }

  return span(buffer.data() + tail, buffer.size() - tail);
}

void CompactIfNeeded() {
  if (head == 0) return;

  remaining = tail - head;
  memmove(buffer.data(), buffer.data() + head, remaining);
  head = 0;
  tail = remaining;
}
```

解析伪代码：

```cpp
DecodeResult Poll() {
  ReleaseDeliveredFrame();

  available = tail - head;
  if (available < 2) return NeedMore();

  ptr = buffer.data() + head;
  header = ParseServerFrameHeader(ptr, available);
  if (header.need_more) return NeedMore();
  if (header.protocol_error) return ProtocolError();
  if (header.payload_length > max_payload_bytes) return ProtocolError();

  total = header.header_bytes + header.payload_length;
  if (available < total) return NeedMore();

  payload = span(buffer.data() + head + header.header_bytes,
                 header.payload_length);

  delivered_pending = true;
  delivered_frame_end = head + total;

  return MessageReady({.payload = payload, ...});
}

void ReleaseDeliveredFrame() {
  if (!delivered_pending) return;

  head = delivered_frame_end;
  delivered_pending = false;

  if (head == tail) {
    head = 0;
    tail = 0;
  }
}
```

### 代表实现

- `benchmark/websocket/third_party_frame_codec_comparison_benchmark.cpp` 中的 benchmark-only `LinearFrameCodec`。
- Boost.Beast 的 `flat_buffer` / `basic_flat_buffer` 属于线性动态缓冲风格：对上提供连续 readable bytes，消费后通过 `consume()` 推进读位置。
- Asio `streambuf` / DynamicBuffer 风格常被用于这种线性 carry buffer 模型，调用方 append 网络字节，parser 从 readable 区间消费。
- 简单 TCP/WebSocket parser adapter 常用这种模型：`std::vector` 或固定数组 + `head/tail` + compact。
- 如果生产中使用 `third_party/websocket` 的 `handleWSMsg(ptr, size)`，最容易补上的外层 buffer 也是这种 Linear Carry Buffer。

注意：`third_party/websocket` 自身没有这个 buffer 层；它只是 parser。这里的代表是“使用第三方 parser 时最常见的外层补法”，不是该第三方库内建策略。

### 优点

- 实现简单。
- 小帧、完整帧、连续 frame 场景非常快。
- 不依赖 OS 双映射能力，移植性好。
- 便于接入 parser-only 第三方库。
- 内存模型容易调试。

### 缺点

- 当 `tail` 接近 buffer 末尾且仍有 partial frame 未完成时，需要 compact/memmove。
- compact 成本和 remaining bytes 成正比，可能制造尾延迟毛刺。
- 如果不 compact，则需要双段 parser 或额外拷贝跨尾部数据。
- payload view 指向 linear buffer，下一次 compact/read 可能改变或覆盖底层内存，生命周期约束要非常明确。
- buffer 配得很大可以降低 compact 频率，但不能从模型上消除 compact。

## 方法三：Owning Message Accumulator Codec

### 命名

建议命名为 **Owning Message Accumulator Codec**。

含义：

- `Owning Message`：解析完成后返回拥有独立生命周期的消息对象，例如 `std::string`。
- `Accumulator`：payload 会 append/copy 到消息累积区。
- `Codec`：框架 buffer 负责 read bytes，parser 负责把完整消息组装成 owning payload。

### 实现描述

核心状态：

```cpp
MsgBuffer input_buffer;
std::string message;
PayloadKind message_kind;
bool got_all;
```

接收流程：

```cpp
input_buffer.append(read_bytes);

for (;;) {
  result = Poll();
  if (result.message_ready) {
    consumer(result.message); // owning string
    continue;
  }
  if (result.need_more) break;
  handle_error(result.status);
  break;
}
```

解析伪代码：

```cpp
DecodeResult Poll() {
  got_all = false;

  while (input_buffer.readableBytes() >= 2) {
    first = input_buffer[0];
    second = input_buffer[1];

    header = ParseHeaderFromMsgBuffer(input_buffer);
    if (header.need_more) return NeedMore();
    if (header.protocol_error) return ProtocolError();

    if (input_buffer.readableBytes() < header.total_frame_bytes) {
      return NeedMore();
    }

    raw_payload = input_buffer.peek() + header.header_bytes;

    if (header.masked) {
      old_size = message.size();
      message.resize(old_size + header.payload_length);
      for (i = 0; i < header.payload_length; ++i) {
        message[old_size + i] = raw_payload[i] ^ mask[i & 3];
      }
    } else {
      message.append(raw_payload, header.payload_length);
    }

    input_buffer.retrieve(header.total_frame_bytes);

    if (header.fin) {
      owning_message = std::move(message);
      message.clear();
      return MessageReady(owning_message);
    }
  }

  return NeedMore();
}
```

payload 生命周期：

```text
payload 被复制到 owning string
callback/consumer 可以安全持有 string
下一次 read 不会覆盖已交付消息
```

### 代表实现

- Drogon WebSocket：基于 `trantor::MsgBuffer` 管理输入 bytes，解析后将 payload 累积到 `std::string`，再交付给上层。
- WebSocket++ 这类面向通用应用的框架也常以 owning message object / payload string 的方式交付完整消息。
- 许多通用 WebSocket 框架会采用类似模式：内部 buffer 管理输入，完整消息以 owning object 或框架 message 类型交付。

### 优点

- payload 生命周期最简单，上层可以安全持有消息对象。
- partial frame 和 fragmentation 处理更自然，可以持续 append 到同一个 message。
- callback 可以跨出 codec 生命周期，不依赖 receive buffer 借用约束。
- 对普通应用、后台服务、聊天、RPC 类 workload 更友好。
- 容易与框架事件模型、线程池和异步回调结合。

### 缺点

- payload 必须复制到 owning message，热路径多一次 copy/append。
- 大 payload 成本明显。
- 高频小消息下，`std::string` 管理、容量增长、拷贝和 cache 写入都会增加成本。
- 如果 callback 后还要转成业务结构，可能出现多层复制。
- 对 HFT 行情接收，生命周期简单性通常不值得交换主路径延迟和尾延迟。

## Parser-Only：third_party/websocket 的定位

`third_party/websocket` 的 `handleWSMsg(ptr, size)` 应单独归类为 **Bare Frame Parser**，不是完整 receive codec。

它的典型形态：

```cpp
remaining = connection.handleWSMsg(handler, ptr, size);
```

它负责：

- 从传入的连续内存解析一帧。
- 在解析完成时调用 `handler->onWSMsg(...)`。
- 返回还有多少 bytes 没被消费。

它不负责：

- socket/TLS read buffer。
- partial frame 保存。
- compact/memmove 策略。
- mirrored ring。
- payload 生命周期。
- capacity exceeded。
- `DecodeResult` / `MessageView` / session drain 语义。

如果生产中使用它，必须外面再包一层：

```text
socket/TLS read
  -> Linear Carry Buffer 或 Mirrored Ring
  -> third_party handleWSMsg(ptr, available)
  -> callback 捕获 payload
  -> 映射成业务处理或 DecodeResult
```

所以它适合作为 parser 下限参考，不适合直接替换 `aquila::websocket::FrameCodec`。

## 三种方法的优缺点对比

| 维度 | Mirrored Ring Zero-Copy Codec | Linear Carry Buffer Codec | Owning Message Accumulator Codec |
| --- | --- | --- | --- |
| 输入 buffer | 双映射 ring | 连续数组或 `std::vector` | 框架 buffer，如 `MsgBuffer` |
| partial frame | 留在 ring 中 | 留在 `[head, tail)` | 留在框架 buffer / message accumulator |
| 跨 buffer 尾部 | 虚拟地址连续，无需复制 | 需要 compact、双段解析或边界拷贝 | 由框架 buffer 处理，payload 仍会复制 |
| payload 交付 | 借用 `span` / `MessageView` | 借用 linear buffer | owning `std::string` / message |
| payload copy | 无 | 无，除非上层要异步持有 | 有 |
| compact/memmove | 无 | 有可能 | 框架内部可能整理，payload 层一定 copy |
| 小帧 p50 | 很低 | 通常最低或接近最低 | 较高 |
| 大 payload / boundary | 稳定 | 容易触发 compact 毛刺 | 拷贝成本高 |
| 生命周期复杂度 | 中等，需要明确借用窗口 | 中等，compact 会增加风险 | 最简单 |
| 实现复杂度 | 高 | 低 | 中等，依赖框架 |
| 移植性 | 依赖 OS 虚拟内存能力 | 高 | 取决于框架 |
| HFT 适配性 | 高 | 中，需控制 compact 尾延迟 | 低到中，取决于延迟要求 |
| 普通服务适配性 | 中 | 高 | 高 |

## 场景选择建议

### 低延迟行情主路径

优先选择 **Mirrored Ring Zero-Copy Codec**。

原因：

- 不把 compact/memmove 留在高峰时刻。
- payload 可以零拷贝交付。
- partial frame、大 payload、跨尾部数据都在同一个模型内处理。
- capacity/error/degraded 可以接入生产 session。

### 简单工具、测试程序、非关键连接

可以选择 **Linear Carry Buffer Codec**。

原因：

- 实现快。
- 小帧连续场景足够快。
- 依赖少，容易 debug。

但要明确：

- buffer 满或尾部空间不足时会 compact。
- benchmark 需要覆盖 compact pressure 和 large payload boundary。

### 普通 WebSocket 应用或框架集成

可以选择 **Owning Message Accumulator Codec**。

原因：

- 消息生命周期简单。
- 上层可以自由保存和跨线程转发。
- 与通用异步框架更匹配。

但对 HFT 行情主路径要谨慎，因为 owning payload copy 会直接增加主路径成本。

## 当前 benchmark 观察

当前 release pinned benchmark（2026-04-26，`taskset -c 2`）显示：

- 小帧 parser-only 下限：`third_party_handle_ws_msg` 为 `2/2/2ns`。
- 完整生产 codec 主路径：`frame_codec_decode_contiguous` 多轮 median 为 `4/5/12ns`，单次完整 benchmark 为 `5/5/6ns`。
- 生产式 direct poll 对比：`aquila_direct_poll_decode` 为 `5/5/7ns`。
- session read path：`396/433/3902ns`。
- 线性 buffer 在小帧 coalesced/burst 场景 p50 很强。
- compact pressure 和 large payload boundary 场景下，mirrored ring 明显优于 linear 和 Drogon style。
- `QueuedFrameCodec` parse-ahead ready queue 明显慢于 direct `FrameCodec`，因此只保留为实验/对照路径。

结论：

```text
生产主路径：Mirrored Ring Zero-Copy Codec
实验对照：Linear Carry Buffer Codec、QueuedFrameCodec
框架参考：Owning Message Accumulator Codec / Drogon style
parser 下限参考：third_party/websocket handleWSMsg
```

FrameCodec decode 主体到此冻结：除非真实链路 profile 明确指向 frame decode，否则后续优化应优先转向 session read path、write path、exchange message parser 和 runtime integration。
