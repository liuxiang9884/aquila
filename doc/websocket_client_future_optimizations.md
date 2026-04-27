# WebSocket Client 未来优化清单

## 文档信息

- 日期：2026-04-26
- 范围：WebSocket client P2 之后的性能、尾延迟和生产可观测性优化
- 前置状态：
  - P2-A：`FrameCodec` mirrored ring 零拷贝 decode 主体已完成。
  - P2-B：bounded read pump、dedicated control write slot、clock source/cadence 已完成。
- 原则：所有性能结论必须通过 benchmark、profile、链路压测或生产观测验证后才能写成定论。

## 优化原则

后续优化默认按下面顺序判断价值：

1. 先看真实生产路径，不只看纯 codec microbenchmark。
2. 先看 P99 / P99.9 和最大毛刺，再看 p50。
3. 不为了平均吞吐引入不可控排队、额外拷贝或长时间独占 loop。
4. 对低频异常路径可以保持清晰和可观测；热路径才值得做复杂快路径。
5. 每个优化都需要同时给出行为测试和对应 benchmark。

## 当前 read 路径

生产 read 路径可以按层拆成：

```text
NIC / kernel TCP receive queue
-> socket fd readable
-> SSL_read()
-> OpenSSL TLS record 解密
-> FrameCodec::WritableSpan()
-> FrameCodec::CommitWritten()
-> FrameCodec::Poll()
-> MessageConsumer::Handle()
-> strategy / market-data parser
```

当前 `session_read_path` benchmark 只覆盖无 TLS 的本地 socketpair 下限路径：

```text
socketpair read syscall
-> CriticalSession::DriveRead()
-> FrameCodec decode
-> MessageConsumer callback
```

它不覆盖真实网卡、TCP 远端收包、TLS 解密、`SSL_pending()` 行为、epoll wakeup 或真实行情解析。

## 当前 write 路径

生产 write 路径可以按层拆成：

```text
order / control event
-> payload serialization
-> FrameCodec::EncodeText() / EncodeBinary() / EncodeControl()
-> PreparedWrite / dedicated control slot
-> CriticalSession::DriveWrite()
-> SSL_write()
-> OpenSSL TLS record 加密
-> kernel socket send buffer
-> NIC
```

当前 P2-B write benchmark 结果：

```text
session_write_path:                                  415 / 433 / 1043 ns
session_write_path_control_slot_full_business_queue:  39 /  40 /   45 ns
```

`session_write_path` 使用本地 socketpair，覆盖：

```text
CommitPreparedWrite()
-> CriticalSession business write queue
-> DriveWrite()
-> LocalFdSocket::WriteSome()
-> socketpair write syscall
```

它没有开启 TLS，不覆盖 `SSL_write()`、TLS encrypt、真实 TCP 发送队列、网卡和交易所远端 ACK 行为。

`session_write_path_control_slot_full_business_queue` 使用 fake socket，覆盖：

```text
business queue full
-> AdvanceHeartbeat()
-> dedicated control slot encode ping
-> DriveWrite()
-> fake WriteSome()
```

该 benchmark 证明 dedicated control slot 的用户态路径很轻，并且业务队列满时 heartbeat ping 可以绕过业务 pending queue。但它不是真实 socket write benchmark，不能代表生产网络发送成本。

## 建议优先级

### 1. 增加 TLS read path benchmark

这是下一步最高优先级。原因是当前 read benchmark 没有开启 TLS，而真实交易所 `wss://` 链路必然经过 OpenSSL。

建议新增 benchmark：

```text
session_read_path_tls_single_frame
session_read_path_tls_coalesced_frames
session_read_path_tls_pending_pump
session_read_path_tls_partial_frame
```

分别测：

- `tls_single_frame`：一个完整 TLS record 内含一个 WebSocket frame，从 `SSL_read()` 到 consumer。
- `tls_coalesced_frames`：一个或多个 TLS record 内含多个完整 WebSocket frame，测 drain 到 consumer 的总延迟。
- `tls_pending_pump`：OpenSSL 内部已有 pending plaintext 时，比较 `max_reads_per_drive = 1` 与 bounded pump。
- `tls_partial_frame`：第一次 `SSL_read()` 只拿到 frame 的一部分，第二次完整后再进入 codec，测半包对尾延迟和 buffer 生命周期的影响。

验证目标：

- 找出真实 read path 中 `SSL_read()`、TLS decrypt、codec、consumer 各自占比。
- 判断 `max_reads_per_drive` 和 `read_until_would_block` 的生产推荐值。
- 验证 bounded pump 是否真的改善 TLS burst 下的 P99 / P99.9，而不是只改善 synthetic socket benchmark。

风险：

- 本地 TLS benchmark 容易变成 loopback TLS 测试，仍不等价于真实交易所链路。
- 需要固定 CPU、OpenSSL 版本、cipher、证书和 buffer 配置，否则结果不稳定。

### 2. 增加 consumer / 行情解析 benchmark

当前 read benchmark 的 consumer 只做计数：

```text
++messages
bytes += payload.size()
```

真实生产 consumer 可能会做：

```text
JSON parse
symbol lookup
sequence check
order book update
trade / depth event dispatch
strategy callback
```

建议新增：

```text
session_read_path_with_market_payload
session_read_path_with_minimal_json_parse
session_read_path_with_order_book_update
```

输入应使用真实交易所样本，例如 depth、trade、bookTicker、order update 等消息。

验证目标：

- 判断 WebSocket frame decode 是否仍是瓶颈。
- 如果 JSON / 行情对象更新才是主成本，后续优化应转向 parser、字段访问、symbol 映射和 order book 数据结构。

风险：

- 真实 payload 样本需要版本化，避免 benchmark 输入随手改动导致结果不可比。
- 不同交易所消息结构差异较大，需要避免把单一交易所结论泛化。

### 3. 调整 read pump 生产参数

P2-B 已增加：

```cpp
std::uint32_t max_reads_per_drive = 1;
bool read_until_would_block = false;
```

后续应基于 TLS benchmark 和真实链路压测给出推荐值，例如：

```text
默认低延迟路径：
  max_reads_per_drive = 1
  read_until_would_block = false

TLS burst 行情路径：
  max_reads_per_drive = 4 或 8
  read_until_would_block = false
  仅在 SSL_pending() 表示已有 plaintext 时继续 read

吞吐优先回放 / 压测路径：
  max_reads_per_drive = 8 或 16
  read_until_would_block = true
  读到 EAGAIN 或预算耗尽
```

验证目标：

- bounded pump 改善 burst 下消息 drain 时间。
- 不让 read loop 长时间独占 CPU，影响 write、heartbeat 或 strategy。
- 明确不同交易所和不同网络环境下的推荐配置。

风险：

- `read_until_would_block = true` 可能在单帧常见路径多打一发返回 `EAGAIN` 的 read。
- `max_reads_per_drive` 太大可能提升吞吐但增加调度不公平和尾延迟。

### 4. TLS 层优化

候选项：

- 比较 `SSL_read()` 与 `SSL_read_ex()` 的成本和错误处理分支。
- 针对 `SSL_pending()` 做更细的 pump 策略，例如 pending plaintext 为 0 时不探测，pending 非 0 时按预算 drain。
- 固定并记录 OpenSSL 版本、cipher suite、AES-NI 支持状态。
- 评估 TLS record 大小、交易所 burst 模式和 coalesced frame 对 read latency 的影响。

验证目标：

- 找出 TLS decrypt 在总 read path 中的比例。
- 确认 TLS pending pump 对 P99 / P99.9 是否有稳定收益。

风险：

- TLS 行为依赖 OpenSSL、CPU、cipher 和对端发送方式。
- 任何 TLS 配置调整都不能牺牲证书校验和连接安全性。

### 5. kernel / socket / 部署层优化

候选项：

- recv thread CPU pinning。
- IRQ / RSS 亲和性与应用线程绑定。
- NUMA placement，避免 socket read、codec 和 strategy 跨 NUMA。
- socket receive buffer 调整，避免 burst 下 kernel drop 或排队过深。
- 评估 `SO_BUSY_POLL`，但只在专用机器和明确收益下启用。
- 生产环境记录 CPU governor、C-state、scheduler policy、NIC queue、IRQ 分布。

验证目标：

- 降低 wakeup、调度、cache migration 带来的 P99 / P99.9 抖动。
- 建立可复现的低延迟部署基线。

风险：

- 强依赖机器和内核配置，本地 benchmark 不能直接代表生产。
- busy polling 会增加 CPU 占用和功耗。

#### 5.1 private `ws://` live probe 后的网络路径优化记录

2026-04-27 对 `ws://fxws-private.gateapi.io:80/v4/ws/usdt` 做两条
private plain WebSocket 同行情订阅对比时，出现了毫秒级到达差。后续检查说明，
这类差异不能直接归因到 `FrameCodec` 或用户态 WebSocket decode。

当时的证据：

- 两条 TCP 连接都落到同一个 peer：`10.0.1.154:80`。
- 本机源地址为 `10.0.1.103`，出接口为 `enp55s0`。
- 两条连接 local port 不同，因此五元组不同，仍可能被 RSS 分到不同 RX queue。
- 网卡驱动为 `ena`，`enp55s0` 有 8 个 combined queue。
- `enp55s0-Tx-Rx-*` IRQ 当时分别绑定到
  `CPU5 / CPU7 / CPU9 / CPU11 / CPU16 / CPU21 / CPU26 / CPU28`。
- 测试中的两个 WebSocket owner thread 绑定到 `CPU2 / CPU3`。
- `rx-* rps_cpus` 和 `tx-* xps_cpus` 都是 `0`，说明没有启用 RPS / XPS
  把包软件重定向到 owner thread 所在 CPU。

因此该环境下的实际路径更接近：

```text
NIC RX queue IRQ CPU
-> kernel TCP receive / softirq
-> wakeup owner thread on CPU2 / CPU3
-> recv / frame decode / consumer timestamp
```

这会引入跨核 wakeup、cache migration 和调度时序差异。即使两条连接的 host、
port、target 和订阅完全相同，也可能因为 local port 导致 RSS queue 不同，从而
经过不同 IRQ CPU。

建议的优化顺序：

1. **先改测试绑核，不改系统配置**：不要默认绑 `CPU2 / CPU3`。优先把 owner
   thread 绑到实际 RX IRQ CPU 上做对照，例如 `CPU11 / CPU26`、
   `CPU7 / CPU28`、`CPU5 / CPU9`。
2. **确认每条 flow 的 RX queue**：记录连接的 local port、remote peer、`ss -tinp`
   TCP 统计，并在连接运行期间采样 `/proc/interrupts` 增量，推断该 flow 更可能
   落在哪个 `enp55s0-Tx-Rx-*` queue。
3. **让 owner thread 靠近 RX IRQ CPU**：如果能稳定识别目标 flow 的 RX queue，
   优先把对应 WebSocket owner thread 绑到该 IRQ CPU，或至少绑到同 NUMA、
   同 cache locality 更好的 CPU。
4. **生产上优先单行情源连接 + 进程内 fanout**：多个策略消费同一行情时，优先用
   一条 private WS 主连接做 decode，再通过本进程内 SPSC / ring fanout 分发给策略。
   避免多个独立 TCP/WebSocket flow 引入不同交易所 fanout、RSS queue 和 wakeup
   时序。
5. **再考虑需要 root 的系统配置**：固定 IRQ affinity，确认或关闭 `irqbalance`
   对关键 IRQ 的迁移；按机器情况评估 RPS / XPS、GRO / LRO、CPU governor、
   C-state、scheduler policy 和 isolated CPU。
6. **最后评估 busy polling**：`SO_BUSY_POLL` / `SO_PREFER_BUSY_POLL` 只适合专用
   机器和明确收益场景，必须用 live probe 或 loopback benchmark 验证 P99 / P99.9
   真的改善，不能只看平均值。

推荐补一个网络诊断模式或独立工具，启动连接后自动打印：

- local address / port 与 remote address / port。
- owner thread CPU 和 affinity policy。
- `SO_INCOMING_CPU`，如果当前内核支持。
- 网卡接口、driver、queue 数、IRQ affinity。
- 运行前后的 `/proc/interrupts` 增量。
- RPS / XPS / GRO / LRO / busy poll 相关配置。

这类信息应与 live latency compare 结果一起记录，否则 `public vs private`、
`ws vs wss` 或“双 private WS”测试很容易把网络路径差异误判为 codec / TLS 成本。

#### 5.2 多 feed 方案选择

围绕同一份交易所行情，目前有三种候选形态：

**方案 A：单 private WS 主连接 + 进程内 fanout**

```text
private WS
-> WebSocket owner thread
-> frame decode / message parse
-> canonical market data event
-> SPSC / ring fanout
-> strategy A / strategy B / strategy C
```

特点：

- 外部只有一条 TCP/WebSocket flow，交易所 fanout、RSS queue、socket wakeup 时序最少。
- 策略看到同一条 canonical stream，顺序一致，debug 和回放简单。
- 内部 fanout 的延迟和 backpressure 可以由本进程控制并 benchmark。
- 缺点是只能吃到单一路外部连接的到达时间，无法利用双连接中偶尔更快的另一条。

该方案适合作为生产 baseline。多个策略消费同一行情时，不应默认让每个策略各自连一条 WS。

**方案 B：双 private WS fastest-win arbiter**

```text
WSA ----\
        -> FeedArbiter -> canonical market data stream -> fanout -> strategies
WSB ----/
```

特点：

- 理论首包延迟是 `min(WSA, WSB) + arbiter cost`，可以吃到两路里先到的合法 update。
- 可以容忍一路偶发抖动、断线或交易所侧单 flow 卡顿。
- 必须在策略之前做统一 `FeedArbiter`，不建议每个策略自己同时消费 WSA / WSB。
- `FeedArbiter` 必须处理去重、sequence 顺序、gap、stale source、重连后重新加入和 source health。
- 对 order book delta，不能因为 seq+1 比 seq 更早就先应用，必须保持 sequence 连续。

该方案适合极致低延迟和高可用行情，但实现复杂度明显高于单连接。

**方案 C：warm-up feed selection**

```text
启动 / 交易前 warmup：
WSA + WSB 同时订阅同一行情
-> 采样 30s / 60s / N 条 update
-> 统计 matched、p50、p99、pending/gap、health
-> selected=public/private

交易中：
selected primary -> strategy / canonical stream
secondary        -> standby monitor
```

特点：

- 比随机选一条单 WS 更稳，可以在交易前选出当前环境下更快、更健康的一路。
- 比 fastest-win 简单，不需要每条 update 都做仲裁。
- secondary 不建议关闭，应保持 warm standby，用于 health monitor 和快速 failover。
- 切换必须有 hysteresis，不能因为单个 update 更快就频繁切换。
- 对快照型行情如 `book_ticker` 实现较简单；对 order book delta 切换时必须确认 sequence 连续。

推荐落地顺序：

1. 先在 `websocket_latency_compare` 加 warmup selection 输出，只推荐 primary，不接交易。
2. warmup 同时开 WSA / WSB，订阅相同 channel / contract，采样 30s 或 60s。
3. 输出 `selected=public/private` 和 `reason=p50/p99/gap/health`。
4. 后续再把该逻辑抽成 `FeedSelector`，交易中采用 primary publish、secondary monitor。
5. 最后再评估是否升级到 fastest-win `FeedArbiter`。

### 6. `CriticalSession::DriveRead()` 微优化

当前流程：

```text
WritableSpan()
-> ReadSome()
-> CommitWritten()
-> DrainDecodedMessages()
-> ShouldContinueReadPump()
```

候选项：

- 为“单 read、单完整 data frame、consumer accepted”的常见路径做 session-level fast path。
- 让 `DrainDecodedMessages()` 返回 drain 数、最后状态和是否需要 reconnect，减少重复状态判断。
- hot/cold metrics 分离，减少每条消息对共享 cache line 的写入。
- 对 capacity / protocol error 等低频路径做 cold path 隔离。

验证目标：

- `session_read_path` p50 不退化。
- TLS benchmark 和真实 consumer benchmark 中 P99 / P99.9 不退化。

风险：

- 当前 read path 已经较短，盲目微优化可能只改变分支布局，收益不稳定。
- session-level fast path 会增加代码分叉，必须保持错误路径和 control frame 语义一致。

### 7. `FrameCodec` decode 微优化

当前 fast path 覆盖：

```text
server text / binary
FIN = 1
RSV = 0
unmasked
payload len < 126 或 126/16-bit length
非 control
非 fragmented
```

候选项：

- 评估 `payload len == 127` 的大 payload data frame 是否需要专门 fast path。
- 把 `BuildDirectMessage()` 中的常见检查专门化，减少小帧分支。
- 评估 `DecodeResult` / `MessageView` 返回方式是否还能减少复制或寄存器压力。
- 对 ping / pong / close 的 generic path 做更清晰的 cold path 标记。

验证目标：

- `frame_codec_decode_contiguous`、`frame_codec_decode_coalesced_drain`、`frame_codec_decode_mirrored_boundary` 不退化。
- 与 third-party linear / Drogon style benchmark 保持可比。

风险：

- 纯 codec p50 收益可能很小。
- 之前 codec 微调已经出现过 p99 波动，必须用多次 benchmark 和 pinned CPU 验证。

### 8. 写路径和 control frame 后续优化

P2-B 已增加 dedicated control write slot。当前 benchmark 表明 control slot 用户态路径成本很低，因此 write 侧后续优化不应优先改 control slot 结构，而应先补齐 TLS、client masking、真实订单 payload 和 syscall 成本的证据。

#### 8.1 增加 TLS write path benchmark

这是 write 侧最高优先级。真实 `wss://` 发送路径必然经过 `SSL_write()` 和 TLS encrypt，当前 `session_write_path` 没覆盖这部分。

建议新增 benchmark：

```text
session_write_path_tls_single_frame
session_write_path_tls_burst_business_queue
session_write_path_tls_control_slot_full_business_queue
session_write_path_tls_partial_write
```

分别测：

- `tls_single_frame`：一个业务 frame 从 prepared write 到 `SSL_write()` 完成。
- `tls_burst_business_queue`：多个业务 frame 连续 pending，测 write queue drain 和 TLS send buffer 行为。
- `tls_control_slot_full_business_queue`：业务 queue 满时，heartbeat ping / auto-pong 经过 dedicated control slot 的真实 TLS write 成本。
- `tls_partial_write`：`SSL_write()` 或底层 socket 只写出部分 frame 时，测 partial write 状态恢复和后续 drain 成本。

验证目标：

- 判断 write path 主成本在 syscall、TLS encrypt、masking、queue 还是 payload serialization。
- 确认 dedicated control slot 在 TLS 路径下仍然不被业务 queue 满阻塞。
- 验证 partial business frame 不被 control frame 打断。

风险：

- loopback TLS benchmark 仍不等价于真实交易所链路。
- TLS benchmark 需要固定 OpenSSL 版本、cipher、CPU affinity 和证书配置。

#### 8.2 优先评估 client masking / random mask 生成成本

WebSocket client 发出的 frame 必须 masked。当前 `FrameCodec::EncodeFrame()` 每帧调用 `RAND_bytes()` 生成 4-byte masking key。

低频发送时这没有问题；高频下单、撤单或批量请求时，`RAND_bytes()` 可能成为 write 热路径成本和尾延迟来源。

候选方案：

```text
初始化或冷路径批量生成 masking keys
-> per-session mask key ring
-> 热路径从 ring 取 4 bytes
-> 低水位时在冷路径补充
-> 耗尽时 fail-fast 或回退 RAND_bytes()
```

验证目标：

- 用 benchmark / profile 证明 `RAND_bytes()` 在 write path 中占比明显。
- 比较 per-frame `RAND_bytes()` 与 mask key pool 的 p50 / P99 / P99.9。
- 验证 masking key 不固定、不复用到违反协议语义。

风险：

- WebSocket client masking key 不能固定为 0。
- mask key pool 需要容量、低水位、耗尽策略和可观测指标。
- 不允许为了性能破坏 RFC 6455 对 client masking 的基本要求。

#### 8.3 增加真实订单 payload benchmark

当前 write benchmark 使用固定测试 payload，不覆盖真实交易所下单、撤单、签名和序列化。

建议新增：

```text
session_write_path_order_new
session_write_path_order_cancel
session_write_path_order_batch
```

测完整路径：

```text
order object
-> JSON / protocol payload serialization
-> timestamp / nonce / signature
-> FrameCodec encode
-> CommitPreparedWrite()
-> DriveWrite()
```

验证目标：

- 判断 write path 瓶颈是否其实在 JSON serialization、HMAC/signature、时间戳、nonce 或字段格式化。
- 避免只优化 WebSocket frame write，却忽略真实下单路径主成本。

风险：

- 不同交易所签名和 payload 格式差异较大，benchmark 输入需要按交易所版本化。
- 如果 benchmark 不包含签名和序列化，就不能代表真实交易链路。

#### 8.4 评估 `sendmsg` / scatter-gather

当前 prepared write 存储完整 encoded frame：

```text
[header + mask + masked payload]
```

因此业务写是单段 buffer，普通 `WriteSome()` 已经适合 syscall。只有当后续想减少 encode copy、把 header/mask 和 payload 分离时，才需要评估 `sendmsg` / `writev`。

候选方向：

```text
iov[0] = websocket header + masking key
iov[1] = masked payload buffer
```

但 WebSocket client payload 必须 masked，不能直接把原始 payload 作为第二段发送。因此 scatter-gather 是否有收益，取决于是否能减少现有 prepared write storage 的拷贝或构造成本。

验证目标：

- 证明 scatter-gather 减少了实际 copy 或降低了 P99 / P99.9。
- 确认 syscall 参数复杂度没有抵消收益。

风险：

- 实现复杂度高于当前单段 prepared write。
- masking 仍然需要对 payload 做 XOR，不能绕过。

#### 8.5 保持 dedicated control slot 简洁

当前 control slot benchmark 已经很轻：

```text
session_write_path_control_slot_full_business_queue: 39 / 40 / 45 ns
```

后续优先做可观测性，而不是复杂化发送状态机：

- control frame enqueue delay 分布。
- heartbeat ping send delay p50 / P99 / max。
- control slot busy count。
- auto-pong enqueue failure count。

不建议为了更低 ping 延迟去打断 partial business frame。WebSocket frame 字节不能交错，打断 partial frame 会显著增加状态机复杂度和协议风险。

#### 8.6 `CriticalSession` business write queue 微优化

候选项：

- 评估 business write queue 的 hot/cold metrics 分离。
- 如果 `prepared_write_slots` 总是 power-of-two，评估 modulo 改 mask。
- 评估 `pending_writes_` 是否需要固定容量版本，减少初始化期动态分配和指针间接。
- 拆分 write error / reconnect cold path，保持成功写出路径更短。

验证目标：

- 业务 queue 满时 ping / pong 不被阻塞。
- partial business frame 不被 control frame 打断。
- control frame 延迟指标可用于 degraded 或告警。
- `session_write_path` 和 TLS write benchmark p50 / P99 / P99.9 不退化。

风险：

- WebSocket frame 字节不能交错，任何优先级优化都必须保持 frame boundary。
- 当前 `session_write_path` 主要包含 socket write syscall，queue 微优化可能收益很小。
- 过早拆分 hot/cold path 会增加代码分叉和维护成本。

### 9. clock / spin loop 后续优化

P2-B 已增加：

```cpp
ClockSource::kSteady
ClockSource::kMonotonic
ClockSource::kMonotonicCoarse
```

当前 benchmark 结果：

```text
active_spin:                  42 / 44 / 45 ns
clock_source_steady:          52 / 54 / 56 ns
clock_source_monotonic:       51 / 52 / 60 ns
clock_source_monotonic_coarse:32 / 34 / 56 ns
```

现有 `active_spin` benchmark 只测一个 fake session 一轮退出，覆盖面很窄。它可以证明 loop skeleton 没有明显退化，但不能代表生产 read/write 混合压力下的节奏。

#### 9.1 增加 active spin mixed benchmark

建议新增：

```text
active_spin_idle_many_iterations
active_spin_clock_check_hit
active_spin_read_ready
active_spin_write_ready
active_spin_mixed_read_write
active_spin_stop_requested
active_spin_yield_mode
```

分别测：

- `idle_many_iterations`：没有 read/write 工作时，loop、`pause` 和 clock cadence 的成本。
- `clock_check_hit`：刚好触发 clock check 时的成本。
- `read_ready`：每轮都有 read 工作时，active spin loop 对 read path 的额外调度成本。
- `write_ready`：每轮都有 write 工作时，active spin loop 对 write path 的额外调度成本。
- `mixed_read_write`：read 和 write 同时 ready，比较不同调度顺序。
- `stop_requested`：外部 stop flag 触发退出时的 atomic 和退出成本。
- `yield_mode`：`active_spin = false` 时的 yield 模式成本，用于非 HFT 或降级运行环境。

验证目标：

- 确认 loop 优化不是只优化 fake session 的一轮退出。
- 建立 read/write 混合压力下的 P50 / P99 / P99.9 基线。

#### 9.2 减少每轮 atomic stop 检查

当前 `RuntimeSession` 的热循环中可能多次读取 `stop_requested`：

```text
DriveWrite()     -> stop_requested.load()
DriveRead()      -> stop_requested.load()
AdvanceClock()   -> stop_requested.load()
ShouldReconnect()-> stop_requested.load(memory_order_acquire)
```

前三个 `load()` 未显式指定 memory order，默认是 `seq_cst`。在 active spin 热循环里，这可能是不必要的成本。

候选方向：

```text
每轮 loop 只读取一次 stop flag
或 DriveRead / DriveWrite / AdvanceClock 使用 memory_order_relaxed
退出边界 ShouldReconnect 保留 memory_order_acquire
```

验证目标：

- `active_spin_mixed_read_write` 和 `active_spin_stop_requested` 不退化。
- stop 请求仍然能及时退出。
- 不破坏 `Stop()` 的 release/acquire 可见性假设。

风险：

- atomic memory order 修改必须明确线程可见性语义，不能只为性能改弱同步。
- 如果 stop 还承担其他状态发布语义，需要保留 acquire 边界。

#### 9.3 把 active spin / yield 分支移出循环

当前每轮都有：

```cpp
if (runtime_policy_.active_spin) {
  CpuRelax();
} else {
  std::this_thread::yield();
}
```

生产 HFT 路径通常固定 `active_spin = true`。可以考虑进入 loop 前选择：

```text
RunActive()
RunYield()
```

或在模板 / helper 中把 idle action 固化，避免每轮判断。

验证目标：

- `active_spin_idle_many_iterations` 不退化。
- `yield_mode` 仍然行为正确。

风险：

- 收益可能很小，只有在 benchmark 证明分支成本明显时才值得改。

#### 9.4 读写调度顺序可配置或自适应

当前顺序固定：

```text
DriveWrite()
DriveRead()
```

这对尽快发送订单、pong 或 heartbeat 有利，但在行情主链路上，read latency 可能更希望：

```text
DriveRead()
DriveWrite()
```

候选策略：

```text
kWriteFirst
kReadFirst
kControlWriteFirstThenRead
kAdaptive
```

其中 `kControlWriteFirstThenRead` 可以保留 control frame 优先级，同时避免普通 business write 长时间推迟 read。

验证目标：

- 在 `active_spin_mixed_read_write` 中比较 read-first / write-first 对 read-to-consumer 和 write latency 的影响。
- 在 control pending 场景确认 ping / pong 不被业务写和 read burst 饿死。

风险：

- 读写顺序是交易系统策略选择，不存在全局最优。
- 写优先可能降低下单延迟，读优先可能降低行情延迟，需要按连接类型和交易所通道区分配置。

#### 9.5 为 business write 增加预算，避免饿死 read

`DriveRead()` 已有 `max_reads_per_drive`，但 `DriveWrite()` 当前会尽量 drain pending queue，直到 EAGAIN 或队列空。

如果业务 write queue 很长且 socket 持续可写，可能出现：

```text
一轮 loop 写太久
-> read 被推迟
-> 行情处理 P99 / P99.9 变差
```

候选配置：

```cpp
std::uint32_t max_writes_per_drive;
std::uint32_t max_write_bytes_per_drive;
```

原则：

- control frame 不受普通 business write budget 限制。
- partial business frame 仍然必须按 WebSocket frame boundary 完成，不能和 control frame 交错。
- write budget 应按连接角色配置：行情连接偏 read，交易连接可能偏 write。

验证目标：

- mixed read/write benchmark 中 read tail latency 改善。
- order burst benchmark 中 write latency 不出现不可接受退化。
- control frame 仍然能在业务队列满时及时发送。

风险：

- write budget 太小会增加订单发送排队。
- 如果 partial frame 很大，仍可能占用一轮较长时间；需要和 payload size limit 一起评估。

#### 9.6 只在 idle iteration 执行 `CpuRelax()`

当前每轮末尾都会执行 `CpuRelax()` 或 `yield()`，即使这一轮刚处理了 read/write 工作。

可考虑让 session 返回是否做了实际工作：

```text
did_write = DriveWrite()
did_read = DriveRead()
if (!did_write && !did_read) {
  CpuRelax()
}
```

收益：

- burst 期间少一次 `pause`。
- idle 时仍保留 `pause`，降低 SMT 竞争和功耗。

风险：

- 需要修改 `DriveRead()` / `DriveWrite()` 返回值或增加 wrapper，接口影响比普通微优化大。
- 如果工作判断不准确，可能造成 idle loop 过热或 burst path 退化。

#### 9.7 clock source 和 TSC

候选项：

- 基于 benchmark 决定生产默认 clock source，不直接假设 `CLOCK_MONOTONIC_COARSE` 更优。
- 增加 calibrated TSC，但必须先实现频率校准、跨核稳定性检查和虚拟化环境检测。
- 将 heartbeat、degraded、stats flush 等低频 clock check 进一步合并，减少重复取时。
- 为 runtime loop 增加 clock drift / backward jump 观测。

当前不建议直接把默认 clock source 改成 `CLOCK_MONOTONIC_COARSE`。它单次调用更快，但 active loop 默认 4096 次才取一次时，摊薄收益很小，而且 coarse 粒度不适合精细延迟判断。

验证目标：

- `active_spin_benchmark` 不退化。
- clock source benchmark 在目标机器上稳定。
- heartbeat 和 degraded 的语义不因 coarse clock 粒度变差。
- read/write mixed benchmark 中 clock cadence 不引入尾延迟毛刺。

风险：

- TSC 优化复杂度高，跨核和虚拟化风险大。
- coarse clock 成本低但粒度粗，不能默认用于需要精确延迟度量的路径。
- 过早加入 `likely` / `unlikely` 或 TSC 可能增加复杂度，但没有生产收益证据。

### 10. 可观测性和回归基线

候选项：

- 为 read path 增加分层 latency counters：
  - read syscall / `SSL_read`
  - codec drain
  - consumer handle
  - total read-to-consumer
- 增加 benchmark 输出机器信息：
  - CPU
  - kernel
  - OpenSSL
  - affinity
  - scheduler
  - governor
- 增加固定 benchmark filter 的脚本或 CTest label，避免手工漏跑。
- 为 P99 / P99.9 设置回归阈值，但阈值需要先收集多次稳定样本。

验证目标：

- 每次优化能明确证明收益或无收益。
- 发现退化时能定位到 read、TLS、codec、consumer 或 runtime loop 的具体层。

风险：

- 热路径打点本身可能扰动延迟。
- 生产打点需要可采样、可关闭，不能默认每帧重度记录。

## 推荐执行顺序

建议按下面顺序推进 read 侧：

1. **TLS read path benchmark**：先把 `SSL_read()`、TLS decrypt 和 `SSL_pending()` 纳入基准。
2. **真实 consumer benchmark**：加入交易所行情 payload 和最小行情解析，确认 codec 是否仍是瓶颈。
3. **read pump 参数实验**：基于 TLS benchmark 比较 `max_reads_per_drive`、`read_until_would_block` 的组合。
4. **网络路径基线**：记录 remote peer、local port、NIC、IRQ/RSS/RPS/XPS、owner CPU 和 `/proc/interrupts` 增量。
5. **生产部署基线**：记录 CPU affinity、IRQ/RSS、NUMA、socket buffer、OpenSSL 和 kernel 配置。
6. **CriticalSession read loop 微优化**：只有当 benchmark 证明 session 逻辑占比明显时再做。
7. **FrameCodec 微优化**：保持小步修改，每次只改一个分支，并重跑 codec + session benchmark。
8. **write/control 和 clock 后续优化**：在 read path 真实瓶颈确认后，再按 P99 风险继续推进。

建议按下面顺序推进 write 侧：

1. **TLS write path benchmark**：先把 `SSL_write()`、TLS encrypt、partial write 纳入基准。
2. **真实订单 payload benchmark**：加入下单、撤单、签名、序列化，确认 WebSocket write 是否是瓶颈。
3. **masking / RNG profile**：确认 `RAND_bytes()` 和 payload XOR 在 write path 的占比。
4. **mask key pool 实验**：只有当 RNG 成本明显时，再实现 per-session mask key ring。
5. **socket/TLS 配置实验**：如果 syscall/TLS 是主成本，优先调 socket/TLS 配置，而不是 queue 微优化。
6. **business write queue 微优化**：最后再考虑 modulo、metrics、hot/cold path、固定容量等细节。

建议按下面顺序推进 active spin loop：

1. **mixed benchmark**：先补 idle、clock hit、read-ready、write-ready、mixed read/write、stop、yield 模式。
2. **atomic stop 检查优化**：减少每轮重复 atomic load，明确 relaxed / acquire 边界。
3. **active/yield 分支外提**：只有 benchmark 证明有收益时再拆 `RunActive()` / `RunYield()`。
4. **读写顺序策略实验**：比较 write-first、read-first、control-first-then-read。
5. **write budget 实验**：防止 business write burst 饿死 read。
6. **idle-only pause 实验**：让 read/write 返回 did-work 后，只在 idle iteration `CpuRelax()`。
7. **clock / TSC 后续实验**：最后再考虑 calibrated TSC 或更激进的 branch layout。

当前最重要的结论是：后续 read path 优化不应继续只抠 `FrameCodec`，而应先把 TLS、bounded read pump、consumer 解析放进同一套 benchmark，找真实生产热路径的最大成本。

write path 同理：后续不应只抠 `CriticalSession` pending queue，而应先把 TLS、client masking/RNG、真实订单序列化和签名纳入 benchmark，确认真实下单链路的最大成本。

active spin loop 同理：后续不应只抠单轮 fake session 的 42ns，而应先用 read/write mixed benchmark 证明 atomic、调度顺序、write budget 和 idle pause 策略对 P99 / P99.9 的真实影响。
