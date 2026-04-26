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

P2-B 已增加 dedicated control write slot。后续候选项：

- control slot 支持 close frame 和 heartbeat ping 的优先级区分。
- 记录 control slot 排队时间分布，而不只记录最大值。
- 评估 `sendmsg` / scatter-gather，避免 header 和 payload 已分离时的额外拼接。
- 评估 business write queue 的 hot/cold metrics 分离。

验证目标：

- 业务 queue 满时 ping / pong 不被阻塞。
- partial business frame 不被 control frame 打断。
- control frame 延迟指标可用于 degraded 或告警。

风险：

- WebSocket frame 字节不能交错，任何优先级优化都必须保持 frame boundary。
- scatter-gather 可能减少 copy，但增加 syscall 参数复杂度，收益要实测。

### 9. clock / spin loop 后续优化

P2-B 已增加：

```cpp
ClockSource::kSteady
ClockSource::kMonotonic
ClockSource::kMonotonicCoarse
```

候选项：

- 基于 benchmark 决定生产默认 clock source，不直接假设 `CLOCK_MONOTONIC_COARSE` 更优。
- 增加 calibrated TSC，但必须先实现频率校准、跨核稳定性检查和虚拟化环境检测。
- 将 heartbeat、degraded、stats flush 等低频 clock check 进一步合并，减少重复取时。
- 为 runtime loop 增加 clock drift / backward jump 观测。

验证目标：

- `active_spin_benchmark` 不退化。
- clock source benchmark 在目标机器上稳定。
- heartbeat 和 degraded 的语义不因 coarse clock 粒度变差。

风险：

- TSC 优化复杂度高，跨核和虚拟化风险大。
- coarse clock 成本低但粒度粗，不能默认用于需要精确延迟度量的路径。

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

建议按下面顺序推进：

1. **TLS read path benchmark**：先把 `SSL_read()`、TLS decrypt 和 `SSL_pending()` 纳入基准。
2. **真实 consumer benchmark**：加入交易所行情 payload 和最小行情解析，确认 codec 是否仍是瓶颈。
3. **read pump 参数实验**：基于 TLS benchmark 比较 `max_reads_per_drive`、`read_until_would_block` 的组合。
4. **生产部署基线**：记录 CPU affinity、IRQ/RSS、NUMA、socket buffer、OpenSSL 和 kernel 配置。
5. **CriticalSession read loop 微优化**：只有当 benchmark 证明 session 逻辑占比明显时再做。
6. **FrameCodec 微优化**：保持小步修改，每次只改一个分支，并重跑 codec + session benchmark。
7. **write/control 和 clock 后续优化**：在 read path 真实瓶颈确认后，再按 P99 风险继续推进。

当前最重要的结论是：后续 read path 优化不应继续只抠 `FrameCodec`，而应先把 TLS、bounded read pump、consumer 解析放进同一套 benchmark，找真实生产热路径的最大成本。
