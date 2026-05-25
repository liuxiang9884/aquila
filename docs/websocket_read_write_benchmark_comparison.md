# WebSocket Read / Write 实现与 Benchmark 对比

## 文档信息

- 日期：2026-04-28
- 范围：比较 `aquila`、Drogon-style、`third_party/websocket` 在 WebSocket read / write 路径上的实现差异和 benchmark 结果。
- 目标：给后续接手者一个可快速判断的基线，说明哪些结果可以直接比较，哪些结果只是 microbenchmark 下限。

## 对比对象

本文里的三类实现含义如下：

| 名称 | 含义 |
| --- | --- |
| `aquila` | 当前自研 `core/websocket` 路径：`FrameCodec` + `CriticalSession` + `PreparedWriteArena`。 |
| `drogon-style` | benchmark 中模拟 Drogon / trantor 风格的线性 buffer / payload copy / connection send 行为；不是完整 Drogon event loop benchmark。 |
| `third_party/websocket` | `third_party/websocket/websocket.h` 的轻量 WebSocket 风格：线性接收 buffer、直接 callback、plain TCP、无 TLS。 |

所有数值均为 release build、`taskset -c 2`、本地 microbenchmark。它们可以比较用户态 codec / buffer / write 构造成本，但不能代表真实交易所网络、TLS、网卡、内核调度和交易所远端 ACK 延迟。

## Read 实现差异

### `aquila`

read 主体是：

```text
socket / TLS read
-> FrameCodec::WritableSpan()
-> CommitWritten()
-> Poll()
-> MessageView
-> consumer
```

关键设计：

- receive buffer 使用 mirrored ring，跨物理尾部时仍能向下游交付连续 payload view。
- 默认 `FrameCodec` 使用 direct one-frame delivery，不再经过 ready metadata ring。
- `MessageView` 保留 payload 生命周期、kind、sequence、fin 等结构化边界。
- capacity exhaustion 有明确返回值和 degraded 指标，不静默覆盖。
- `QueuedFrameCodec` 保留在 `evaluation/websocket/queued_frame_codec.h`，用于 ready queue / parse-ahead 对照，但不在默认热路径。

代价：

- 比裸 parser 多了生命周期、容量、状态和 mirrored cursor 管理。
- microbenchmark 中 p50 不一定能达到最简 parser 的 `2ns` 下限。

### `drogon-style`

benchmark 中的 Drogon-style 模拟重点是：

```text
linear buffer
-> frame decode
-> payload copy / std::string-like delivery
-> callback
```

关键特征：

- WebSocket frame codec 和 socket buffer 细节由库内部隐藏。
- 应用通常拿到完整 message 字符串或 buffer view。
- 线性 buffer 在空间不足或头部已消费时可能 compact / move。
- 对通用服务端 / API 服务友好，但低延迟系统难以控制 payload 生命周期和 read/write pump 预算。

### `third_party/websocket`

`third_party/websocket` read 路径更接近裸 parser：

```text
recvbuf_
-> handleWSMsg()
-> onWSMsg(payload, len)
```

关键特征：

- 线性固定接收 buffer。
- 解析完成后直接调用 handler。
- 没有 `MessageView`、degraded、session 状态、TLS、control plane。
- buffer head/tail 空间不够时需要 compact / memmove。

它的 read microbenchmark 很快，但这是“协议 parser 下限”，不是完整生产连接内核。

## 实际 Read Pump 机制对照

上面的 `drogon-style` / `third_party/websocket` benchmark 主要比较 codec / buffer
成本，不等于完整 event loop 行为。配置 `read_path` 时，需要区分真实实现里的 socket
read pump 机制。

### `third_party/websocket.h`

`third_party/websocket/websocket.h` 是显式 `poll()` 模型。client `poll()` 每次调用：

```text
poll()
  -> conn.read()
  -> ::read() 一次
  -> handleWSMsg() 解析当前 recvbuf 中已有 frame
```

也就是说，它没有内部 `max_reads_per_drive`。如果 socket 中已有多批数据但一次
`::read()` 没取完，后续数据需要等外层再次调用 `poll()`。因此它的尾延迟不仅取决于
parser 成本，也取决于外层 `poll()` 频率和单次 `::read()` 能取到多少数据。

优点是路径很薄，单连接、plain TCP、紧循环 polling 时开销下限很低。缺点是没有 TLS、
运行策略、read budget、degraded、重连和指标边界，不适合作为生产行情连接内核。

### Drogon / Trantor plain path

Drogon 底层 plain TCP 路径由 trantor `Channel` readable event 触发：

```text
epoll readable
  -> TcpConnectionImpl::readCallback()
  -> MsgBuffer::readFd()
  -> ::readv() 一次
  -> 上层 parser / callback 消费 MsgBuffer
```

`MsgBuffer::readFd()` 使用两段 iovec：一段写入内部 buffer，一段写入栈上的临时
`extBuffer`。这对通用网络库很方便，可以减少一次 buffer 扩容前的额外 syscall，但业务层
无法配置“本轮最多读几次”。如果还有数据留在 socket 中，通常要等下一次 readable event
或 event loop 再次调度。

### Drogon / Trantor TLS path

Drogon TLS 路径分两层：

```text
epoll readable
  -> readv() 读 encrypted bytes 到 MsgBuffer
  -> BIO_write() 写入 OpenSSL rbio
  -> processApplicationData()
  -> while (true) SSL_read(...)
  -> message callback
```

因此它在 TLS application data 层会循环 `SSL_read()`，直到 OpenSSL 读不出更多应用层
数据或遇到错误。这有利于吞吐和通用服务场景，因为一次 lower read 后会尽量 drain 已解密
数据；但业务层没有预算参数，burst 下可能在同一个 event loop callback 中停留更久。

Sirius Gate websocket 基于 Drogon `WebSocketClient`，Sirius 自己看到的是 Drogon 已经
组装后的 `std::string&& message` callback，因此也没有 read budget 控制。

### `aquila` bounded read pump

`aquila` 当前运行期 read path 是显式预算模型：

```text
DriveRead()
  -> TransportSocketT::ReadSome()
       TLS:   SSL_read()
       plain: recv()
  -> FrameCodec::CommitWritten()
  -> DrainDecodedMessages()
  -> 根据 read_path 决定是否继续读
```

对应配置：

```toml
[data_session.websocket.read_path]
max_reads_per_drive = 8
read_until_would_block = false
```

`max_reads_per_drive` 控制一次 `DriveRead()` 最多调用几次 `ReadSome()`。`read_until_would_block`
控制在预算内是否更积极地读到 `EAGAIN` / would-block。

这个设计的目的不是保证某个固定参数最快，而是把 third-party 实现中隐含或不可调的策略变成
可测、可配置的 runtime 参数：

- 值太小，burst 时可能 drain 不干净，后续消息留到下一轮。
- 值太大，单次 read drive 可能占用线程更久，增加同线程任务的尾延迟。
- `read_until_would_block = true` 可能提升 drain 能力，也可能额外打一发返回 `EAGAIN`
  的 read。

因此 production 推荐值必须用同一机器、同一 payload、同一 TLS 条件下的 benchmark 或
live probe 证明。当前示例中的 `max_reads_per_drive = 8`、`read_until_would_block = false`
只是沿用现有 Gate / Binance probe 的保守设置，不应写成性能最优结论。

## Read Benchmark

命令：

```bash
taskset -c 2 ./build/release/benchmark/websocket/third_party_frame_codec_comparison_benchmark \
  --benchmark_filter='third_party_handle_ws_msg|aquila_feed_decode|aquila_direct_poll_decode|aquila_coalesced_feed_drain|aquila_coalesced_direct_poll_drain|third_party_coalesced_drain|drogon_feed_decode|drogon_direct_poll_decode|drogon_coalesced_feed_drain|drogon_coalesced_direct_poll_drain|drogon_compact_pressure_feed_decode|drogon_burst_feed_drain|drogon_large_payload_boundary_feed_decode'
```

2026-04-28 代表结果，单位 ns：

| Benchmark | 场景 | p50 / p99 / p99.9 |
| --- | --- | --- |
| `third_party_handle_ws_msg` | 单帧 parser-only | `2 / 2 / 2` |
| `aquila_feed_decode` | 单帧 `Feed()` + `Poll()` | `7 / 8 / 25` |
| `aquila_direct_poll_decode` | 预填 mirrored ring，只计 `Poll()` | `5 / 6 / 17` |
| `drogon_feed_decode` | 单帧 Drogon-style feed | `18 / 19 / 145` |
| `drogon_direct_poll_decode` | 单帧 Drogon-style direct poll | `13 / 13 / 147` |
| `third_party_coalesced_drain` | 16 帧 coalesced drain | `3 / 3 / 1967` |
| `aquila_coalesced_feed_drain` | 16 帧 `Feed()` + drain | `3 / 4 / 15` |
| `aquila_coalesced_direct_poll_drain` | 16 帧 direct poll drain | `5 / 6 / 3426` |
| `drogon_coalesced_feed_drain` | 16 帧 Drogon-style feed drain | `19 / 28 / 3031` |
| `drogon_coalesced_direct_poll_drain` | 16 帧 Drogon-style direct drain | `13 / 21 / 25` |
| `drogon_compact_pressure_feed_decode` | 线性 buffer compact 压力 | `1239 / 2277 / 2463` |
| `drogon_burst_feed_drain` | 128 帧 burst，按帧归一 | `17 / 18 / 19` |
| `drogon_large_payload_boundary_feed_decode` | 大 payload + boundary 压力 | `2440 / 3449 / 4345` |

解读：

- `third_party_handle_ws_msg` 是裸 parser 下限，不能和完整 `CriticalSession` 等价。
- `aquila` 在单帧 direct poll 上比裸 parser 慢几 ns，但保留生产边界。
- `aquila_coalesced_feed_drain` 在本轮 p50 / p99 已接近 `third_party_coalesced_drain`。
- Drogon-style 在 compact / large payload boundary 压力下出现微秒级成本，说明线性 buffer 的整理成本会进入尾延迟。
- p99.9 在 ns 级 microbenchmark 中容易受调度噪声影响，应结合多轮结果和 p50 / p99 判断趋势。

## Write 实现差异

### `aquila`

write 主体是：

```text
payload serialization
-> FrameCodec::EncodeText() / EncodeBinary()
-> PreparedWriteArena slot
-> CommitPreparedWrite()
-> DriveWrite()
-> send() / SSL_write()
```

关键设计：

- client frame 按 RFC 6455 正确写 mask key 并 XOR payload。
- mask key 使用 per-codec pool，避免每帧 hot path 调 `RAND_bytes()`。
- payload masking 使用 8-byte chunk XOR，尾部逐字节处理。
- encoded frame 是单段 `[header][mask][masked payload]`，plain socket 和 TLS 都可一次 `WriteSome()`。
- `kTryFlushOne` 可在策略 callback / read callback 中立即尝试一次发送，不 busy loop。
- control frame 有 dedicated slot，业务 queue 满时 ping / pong 不被业务 pending queue 阻塞。

### `drogon-style`

benchmark 中的 Drogon-style write 模拟：

```text
应用 payload
-> 库内部构造 frame / buffer
-> connection send buffer
-> socket write
```

它的优势是通用易用；代价是应用很难控制 frame buffer 生命周期、partial write 细节和每轮 write budget。

### `third_party/websocket`

`third_party/websocket` 原实现风格：

```text
header send
payload send
```

重要区别：

- 原实现的 client mask key 固定为 0，且 payload 不做真实 XOR；这不符合 RFC 6455 对 client frame masking 的要求。
- 因此 benchmark 中分成两类：
  - `third_party_websocket_style_*`：模拟原始 zero-mask / no-XOR，速度快但不适合生产对比。
  - `third_party_websocket_compliant_style_*`：补齐随机 mask + XOR，才是协议合规对照。

## Write Benchmark

命令：

```bash
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark \
  --benchmark_filter='session_write_path_with_encode_plain|drogon_style_write_path_plain|third_party_websocket_style_write_path_plain|third_party_websocket_compliant_style_write_path_plain'
```

2026-04-28 代表结果，单位 ns：

| Payload | `aquila` | `drogon-style` | `third_party` zero-mask | `third_party` compliant |
| --- | --- | --- | --- | --- |
| `64B` | `422 / 445 / 1549` | `445 / 935 / 1413` | `736 / 766 / 1720` | `1183 / 1258 / 9105` |
| `256B` | `454 / 476 / 891` | `587 / 1097 / 2902` | `760 / 808 / 4476` | `1334 / 1448 / 5910` |
| `1024B` | `523 / 558 / 811` | `1128 / 1652 / 4565` | `792 / 840 / 1120` | `1836 / 1954 / 10545` |
| `4096B` | `712 / 779 / 1836` | `3168 / 4148 / 12479` | `852 / 918 / 9163` | `3849 / 4603 / 12862` |

表格格式均为：

```text
p50 / p99 / p99.9
```

解读：

- `third_party` zero-mask 行不能作为生产合规 WebSocket client 参考，只能看作“跳过 masking 的下限”。
- 合规 masking 后，`third_party` 在所有 payload size 上都明显慢于 `aquila` 当前实现。
- `aquila` 的 8-byte XOR 优化对 256B 以上 payload 收益明显，4096B p50 已降到 `712ns`。
- Drogon-style 大 payload 成本明显高于 `aquila`，主要来自更通用的 buffer / copy 模型。
- 这组 benchmark 不含 TLS。TLS write 基线在 `docs/websocket_client_future_optimizations.md` 中记录：local TLS single-frame write p50 约为 plain local socket 的两倍以上。

## 设计结论

1. `third_party/websocket` 适合做协议 parser / lightweight implementation 参考，不适合作为 `aquila` 生产 WebSocket 内核。
2. Drogon-style 对工程开发友好，但 read/write buffer、callback、event loop 和 payload 生命周期不可控，不适合作为最低延迟路径。
3. `aquila` 当前 read path 选择保留 mirrored ring、`MessageView`、capacity/degraded 和 direct delivery，是用几 ns parser 成本换生产边界。
4. `aquila` 当前 write path 在合规 client masking 前提下已经明显优于两个对照路径；短期不应继续只抠通用 frame encode。
5. 后续更有价值的 benchmark 应转向真实 Gate 订单 payload：JSON serialization、timestamp、签名、`Encode*()`、`kTryFlushOne`、plain/TLS socket write。

## 相关文件

- `core/websocket/frame_codec.h`
- `core/websocket/critical_session.h`
- `core/websocket/prepared_write.h`
- `core/websocket/types.h`
- `benchmark/websocket/third_party_frame_codec_comparison_benchmark.cpp`
- `benchmark/websocket/session_write_path_benchmark.cpp`
- `docs/websocket_frame_codec_receive_strategies.md`
- `docs/websocket_client_future_optimizations.md`
