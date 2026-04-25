# aquila WebSocket 方案与 MengRao/websocket 对比说明

## 文档信息

- 状态：`draft`
- 记录位置：`doc/`
- 对比对象：
  - `aquila` 当前讨论中的低延迟高频 WebSocket 方案
  - `third_party/websocket/websocket.h`
  - 上游来源：`git@github.com:MengRao/websocket.git`

## 结论摘要

`third_party/websocket/websocket.h` 更接近一个轻量、单线程、手动 `poll()` 驱动的 `WebSocket` 工具库；它能覆盖基础 `TCP + WebSocket` 握手、收发和简单 server/client 场景，但不是面向高频低延迟系统的连接内核。

和 `aquila` 当前方案相比，它的主要问题不是“协议完全不对”，而是架构边界与执行模型不满足我们的目标：

- 没有 `epoll + 单连接唯一 owner` 的事件驱动内核
- 没有严格有界的 `pending write chain` 和明确的 `fail-fast` 背压语义
- 没有独立 `control plane`、细粒度状态机、退化状态和退避重连
- 没有 `wss/TLS` transport
- 消息交付是直接事件回调，不是 `MessageView + MessageSink` 这种显式边界
- 可观测性只停留在 `last_error` 级别，不满足阶段耗时和链路归因要求

因此，这个三方库适合作为：

- `WebSocket` 基础实现参考
- 某些 header-only 技巧和静态分发风格的参考
- 本地 demo、工具或简单 smoke test 的候选

但不适合作为：

- `aquila` 的 HFT `WebSocket core`
- 交易所公网接入的通用 `ws/wss` 内核
- 需要清晰线程边界、恢复语义和性能证据的生产实现

## 详细差异

### 1. 总体定位不同

`aquila` 的目标是“低延迟、低抖动、可恢复、可观测”的通用 `WebSocket client` 内核，强调 owner 边界、热路径零临时分配、控制面隔离和可验证性能。

`MengRao/websocket` 的实现更偏“能跑的轻量协议库”，核心接口集中在 [SocketTcpConnection](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:38>)、[WSConnection](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:252>)、[WSClient](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:416>)、[WSServer](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:524>)。它没有把热路径、控制面、交付边界和恢复语义拆成独立组件。

### 2. 事件与线程模型不同

`aquila` 方案要求：

- 单连接始终只有唯一 `I/O owner`
- owner 通过 `epoll/eventfd/timerfd` 驱动
- 控制面与消费线程和 `I/O owner` 解耦

而三方库没有 `epoll` reactor；它要求调用方反复调用客户端 [poll()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:516>) 或服务端 [poll()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:547>)，由调用方承担调度责任。这种模型可以简单工作，但它没有把 owner-thread 约束编码进接口，也没有独立控制面。

### 3. 发送路径语义不同

`aquila` 方案要求发送路径满足：

- owner-thread only
- 严格串行
- 部分写可续写
- 内部挂起写链严格有界
- 满了立即失败，不阻塞、不自旋

三方库的发送从 [SocketTcpConnection::write()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:79>) 到 [WSConnection::send()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:263>) 都是直接同步推进。`write()` 在 `EAGAIN` 时会持续 `continue` 重试直到发完，这对 HFT 热路径是明显不合适的：它既没有显式背压，也没有写链高水位，更没有“失败即返回给上层决策”的路径。

### 4. 接收路径与消息交付模型不同

`aquila` 方案要求接收路径在完成最小解析后，交付 `MessageView/span/field-view`，再通过 `MessageSink/callback/poll contract` 把消息交给集成方；是否入队、如何跨线程 handoff 由外部决定。

三方库的接收处理集中在 [WSConnection::handleWSMsg()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:320>)，解析完成后直接调用：

- `handler->onWSSegment(...)`
- `handler->onWSMsg(...)`
- `handler->onWSClose(...)`

这意味着它把“协议推进”和“业务回调”耦合到了同一条读路径里。对于简单程序这是省事的；但对于高频系统，这种设计会让消息交付、业务背压和跨线程边界变得模糊。

### 5. 控制面与恢复能力不同

`aquila` 方案明确要求：

- 细粒度状态机
- 分阶段超时
- 心跳策略
- `Degraded` 状态
- 退避重连
- 按失败原因分类恢复

三方库没有单独的 `control plane`。客户端只有建连/握手超时 [wsConnect()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:426>)，服务端只有新连接和已打开连接超时 [WSServer::init()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:541>)、[WSServer::poll()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:548>)。它没有：

- 心跳调度与 `RTT` 观测
- `Degraded`
- 退避重连
- `DNS/TCP/TLS/WS/Auth` 分阶段状态
- owner command channel

### 6. Transport 能力不同

`aquila` 方案要求 `ws/wss transport-agnostic`，要能显式承载 `TCP` 和 `TLS`。

三方库只有 plain `TCP`：

- `socket(AF_INET, SOCK_STREAM, 0)`
- `inet_pton`
- `connect`
- `read/send`

见 [SocketTcpConnection::connect()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:47>)。它没有：

- `DNS` 解析阶段
- `TLS` 握手
- `wss`
- 可插拔 transport abstraction

### 7. 握手与协议实现存在工程简化

这个三方库的客户端握手在 [WSClient::wsConnect()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:426>) 中直接使用固定 `Sec-WebSocket-Key`，并在响应校验时直接比固定 `Sec-WebSocket-Accept`。这对 demo 没问题，但不适合我们做通用公网客户端。

客户端发送 mask 时，库里还把 masking-key 固定写成 0，见 [WSConnection::send()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:287>)。这属于“为简单和效率做的工程近似”，不是我们希望放进生产内核的默认行为。

### 8. 内存模型有相似点，但不够完整

这个三方库有两个值得注意的优点：

- 很少使用堆分配，主要依赖固定容量数组
- 主要靠模板静态分发，不依赖虚函数

例如：

- `SocketTcpConnection` 内含固定 `recvbuf_` [websocket.h:160](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:160>)
- `WSServer` 内含固定连接数组 [websocket.h:743](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:743>)

这些点和 `aquila` 的方向并不冲突。但它仍然缺少我们需要的：

- `fixed_buffer_pool`
- `PreparedWrite`
- `PendingWriteChain`
- owner command ring
- metrics/event ring
- 明确的 buffer 生命周期契约

此外它在接收缓冲区上会做 `memcpy` 压缩 [SocketTcpConnection::read()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:118>)，这说明它是“简单固定缓冲区方案”，而不是围绕零拷贝视图和生命周期边界设计的接收模型。

### 9. 可观测性与诊断能力不同

`aquila` 方案要求至少具备：

- `L0/L1` 指标
- 状态转移记录
- 分阶段耗时
- 心跳 `RTT`
- `pending write` 高水位
- 失败原因分类

三方库的诊断几乎只有 `last_error_` 字符串 [SocketTcpConnection](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:161>) 和 server 的 `last_error_` [SocketTcpServer](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:235>)。这对于 demo 足够，但无法支持 HFT 链路归因与尾延迟分析。

## 逐模块映射表

| aquila 模块 | 我们方案中的职责 | 三方库中是否存在 | 对应位置 | 结论 |
| --- | --- | --- | --- | --- |
| `types.h` | 统一定义状态、错误码、返回码、配置 | 部分存在 | [OPCODE 常量](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:244>)、各类布尔/整型成员 | 只有零散常量和状态字段，没有统一的错误码、阶段状态和返回码体系 |
| `message_view.h` | 结构化只读消息视图、生命周期边界 | 没有 | 无直接等价 | 三方库直接把 `payload + len` 回调给 handler，没有独立 `MessageView` |
| `message_sink.h` | `I/O owner -> consumer` 交付契约 | 没有 | [onWSMsg/onWSSegment](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:367>) | 只有直接业务回调，没有显式 sink contract 和 `DeliveryResult` |
| `fixed_buffer_pool.h` | 固定容量 buffer 管理 | 没有 | [recvbuf_](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:160>)、[frame[]](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:409>) | 只有内嵌固定数组，没有独立池化抽象 |
| `prepared_write.h` | 已编码写对象及 ownership | 没有 | 无 | 发送直接在 `send()` 中拼 header 并立刻写 socket |
| `pending_write.h` | 严格有界挂起写链、部分写续写 | 没有 | 无 | 三方库没有写链、没有高水位、没有 `fail-fast` |
| `owner_command.h` / `owner_command_ring.h` | 控制面对 owner 的最小命令注入 | 没有 | 无 | 无 ping/close/reconnect 命令通道，控制动作直接在回调或接口里做 |
| `state_machine.h` | 细粒度连接阶段状态机 | 没有 | [open/send_fin/expire_time 等字段](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:404>) | 只有少量布尔和时间字段，没有显式阶段状态机 |
| `transport.h` | 统一 transport 抽象 | 没有 | [SocketTcpConnection](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:38>) | 只有 TCP 连接类，没有独立 transport interface |
| `tcp_transport.h` | 非阻塞 TCP transport | 有，部分 | [SocketTcpConnection](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:38>) | 可以参考基础 TCP 连接、`TCP_NODELAY`、固定接收缓冲的做法 |
| `tls_transport.h` | `wss/TLS` transport | 没有 | 无 | 无法直接复用，需要单独实现 |
| `handshake.h` | client/server 握手构造和校验 | 有，部分 | [WSClient::wsConnect()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:426>)、[WSServer::handleHttpRequest()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:633>) | 可参考基础 HTTP 头处理和 server 端 `Sec-WebSocket-Accept` 逻辑，但要修掉固定 key 等简化 |
| `frame_codec.h` | frame encode/decode、partial frame 处理 | 有，部分 | [WSConnection::send()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:263>)、[handleWSMsg()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:320>) | 可以参考 opcode、长度字段、mask/unmask、分片处理逻辑，但要改成更清晰的 codec 边界 |
| `io_loop.h` | `epoll/eventfd/timerfd` owner reactor | 没有 | [poll() 轮询](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:516>)、[server poll()](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:547>) | 只有手动轮询，没有 event loop 抽象 |
| `session.h` | 握手推进、frame 组装、串行发送、消息交付 | 有，部分 | [WSConnection](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:252>) | `WSConnection` 最接近 session core，但它把发送、关闭、消息交付、连接状态都揉在一起，边界过粗 |
| `control_plane.h` | 心跳、退化、超时、退避重连 | 没有 | 仅有 timeout 字段 [websocket.h:408](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:408>) | 需要全新实现 |
| `metrics.h` | `L0/L1` 指标与预算化事件记录 | 没有 | 无 | 需要全新实现 |
| `client.h` | 粗粒度 client 生命周期入口 | 有，部分 | [WSClient](</home/liuxiang/dev/aquila/third_party/websocket/websocket.h:416>) | 可参考最外层 client 壳子，但接口语义和状态模型需要重设计 |

## 可以参考的部分

虽然不能直接当成 `aquila` 的 HFT 内核，这个三方库仍有一些值得参考的点：

- 尽量少用虚函数，主要靠模板和静态分发
- header-only、小实现面，便于快速读懂协议处理
- `TCP_NODELAY` 的基础 socket 细节
- `WebSocket` 基础 frame encode/decode 思路
- 服务端 `Sec-WebSocket-Accept` 计算和 HTTP 升级头处理

## 不建议直接沿用的部分

- `poll()` 驱动而非 `epoll + owner reactor`
- 同步阻塞式写完全部 payload 的发送语义
- `EAGAIN` 时持续自旋重试
- 直接业务回调而不是 `MessageView + MessageSink`
- 固定 `Sec-WebSocket-Key`
- mask key 固定为 0
- 无 `TLS/wss`
- 无控制面、无恢复编排、无指标体系

## FrameCodec 对比后的优化建议

2026-04-25 的 `third_party_frame_codec_comparison_benchmark` 显示，在只比较“内存中已有完整单帧、解析后回调”的极窄路径时，三方库的 `handleWSMsg()` pinned p50/p99/p99.9 为 `2/2/2ns`；当前 `aquila` 的 `aquila_direct_poll_decode` 为 `12/13/30ns`，`aquila_feed_decode` 为 `16/20/112ns`，`aquila_direct_poll_mirrored_boundary` 为 `21/22/230ns`。

同日新增 coalesced 多帧 benchmark，模拟一次 read 中包含 16 个 `"tick"` text frame，并按每 frame 归一化计时。release pinned 结果：

| benchmark | 场景 | p50/p99/p99.9 |
| --- | --- | --- |
| `third_party_handle_ws_msg` | 单帧，已有完整 frame | `2/2/2ns` |
| `aquila_direct_poll_decode` | 单帧，预填 read ring 后只计 `Poll()` | `13/14/158ns` |
| `third_party_coalesced_drain` | 16 帧 coalesced，循环调用 `handleWSMsg()` drain | `2/3/12ns` |
| `aquila_coalesced_feed_drain` | 16 帧 coalesced，`Feed()` + drain | `14/22/24ns` |
| `aquila_coalesced_direct_poll_drain` | 16 帧 coalesced，预填 read ring 后只计 `Poll()` drain | `14/23/25ns` |

这组多帧数据说明：即使主动循环 drain 三方 parser，它的 parser-only 成本仍显著更低；`aquila` 的 coalesced direct/feed 两组接近，说明在 16 个小 frame 的场景中，主要成本不在把 coalesced bytes 复制进 mirrored ring，而在当前 `Poll()` 的 ready metadata ring 往返、release 分支和 `MessageView` 生命周期维护。

这组数字不能直接说明三方库整体更适合作为生产内核，因为它没有覆盖 `aquila` 当前保留的生命周期、容量、降级和状态边界；但它指出了一个明确优化点：**单帧已完整到达的常见路径不需要先写入 ready metadata ring 再读出，可以直接返回 `MessageView`。**

### 应保留的机制

下列机制不应为了追逐 `2ns` 解析数字直接删除：

- `MessageView`：它是 codec 与 consumer 之间的结构化边界，保留 `payload/span`、`kind`、`sequence` 和 `fin`，避免把裸 `opcode + pointer + len` 泄漏给下游。
- mirrored receive ring：它保证跨物理尾部的 payload 仍能以单段 `std::span<const std::byte>` 交付，不把双段 payload 复杂度扩散到 parser。
- cursor 与交付生命周期：`consume_abs_ / parse_abs_ / write_abs_` 和 delivered-frame release 语义用于保证零拷贝 payload 在同步回调期间有效，并防止未交付 payload 被覆盖。
- capacity / degraded 指标：`kCapacityExceeded`、`frame_codec_capacity_exhaustions` 和 degraded 窗口是生产链路的资源耗尽观测点，不能退化成静默丢弃或普通协议错误。
- 协议校验：mask、payload 上限、control frame 长度、`fin` 等检查仍应留在 codec 边界内。
- fixed ready metadata ring：它仍用于 coalesced 多帧、上一帧尚未 release、或需要批量 drain 的 fallback 场景。

### 推荐的快路径

在 `FrameCodec::Poll()` 中新增 **single-frame direct delivery fast path**，只覆盖最常见且最容易证明正确的情形：

1. ready ring 当前为空，且没有上一条 delivered frame 等待 release。
2. receive ring 中从 `parse_abs_` 开始至少包含一个完整 frame header。
3. header 通过现有协议检查，payload 长度不超过配置上限，且完整 frame 已在 ring 中可见。
4. 当前可见数据只覆盖这一条完整 frame；如果同一次读取中 coalesced 了多条 frame，则走现有 ready ring fallback。
5. 直接用 `payload_abs` 和 `payload_size` 构造 `MessageView`，返回 `kMessageReady`，并按当前生命周期规则延后释放 receive ring 空间。

这条快路径可以从单帧主路径移除：

- `QueueReadyFrame()` 的 metadata 写入
- `DrainReadyFrame()` 的 metadata 读取
- ready ring 的 `head/count` 更新
- ready high-watermark 更新分支

但它不移除 `MessageView`、协议检查、capacity/degraded、mirrored ring 和 payload 生命周期约束。

### 方案比较

| 方案 | 主路径延迟 | 正确性 / 生产边界 | 结论 |
| --- | --- | --- | --- |
| 直接采用三方库 `handleWSMsg()` 风格 | 最低，benchmark 为 `2ns` 量级 | 缺少 `MessageView`、capacity/degraded、ready fallback、TLS/session 边界和完整恢复语义 | 只适合作为 parser 下限参考 |
| 当前 `aquila` ready ring 路径 | 已经很低，direct poll 约 `12ns` 量级 | 生产边界完整，支持 coalesced frame 和容量观测 | 保守可靠，但单帧路径仍有可减掉的 metadata 往返 |
| 普通 ring + 边界线性化 copy | 初始化简单 | 跨 ring 尾部时引入 copy 或双段 parser 分支 | 不符合 P2-A 低延迟目标 |
| 双段 payload view | 避免 mirrored mmap | 把边界处理推给所有下游 parser | 不建议作为当前主线 |
| single-frame direct delivery + ready ring fallback | 接近 parser 下限，同时保留生产边界 | 只优化常见单帧路径，多帧和复杂场景沿用现有机制 | 当前最优后续方案 |

### 验证要求

实现该快路径时，应至少补充：

- 单帧完整到达时可以不占用 ready ring 的回归测试。
- coalesced 多帧仍按 sequence 顺序交付并走 ready fallback 的测试。
- `ready_frame_slots == 0` 或容量受限场景的行为说明，避免无意放宽已有 capacity 语义。
- `frame_codec_benchmark` 与 `third_party_frame_codec_comparison_benchmark` 的 release pinned 对比，记录 p50/p99/p99.9。
- `session_read_path_benchmark` 复跑，确认 session 读路径没有因快路径分支引入尾延迟回归。

## 建议结论

对 `aquila` 来说，`MengRao/websocket` 更适合作为“协议层和轻量实现参考”，而不是“直接集成的连接内核”。

推荐策略是：

1. 借鉴其中 `frame encode/decode`、握手头处理和模板静态分发的思路
2. 保留我们当前方案中的 owner 边界、control plane、`pending write chain`、`MessageView/MessageSink`、`ws/wss transport` 分层
3. 不直接把这个三方库当作生产接入层塞进 `aquila`
