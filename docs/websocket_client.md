# Aquila WebSocket Client

本文是 `core/websocket` 的当前架构、配置、对照 benchmark 和后续优化入口。所有性能、tail、吞吐和稳定性结论必须来自
benchmark、profile、live probe 或生产观测。

## 当前选择

- Cold path：DNS/TCP/TLS/WebSocket handshake 与 reconnect/backoff。
- Hot path：单 owner thread 驱动 `CriticalSession::DriveRead/DriveWrite`、heartbeat 和 runtime hook。
- Read codec：mirrored-ring、direct one-frame delivery、borrowed `MessageView`、bounded capacity/degraded。
- Write：fixed-capacity `PreparedWriteArena`、RFC6455 client masking、mask-key pool、8-byte XOR、partial write。
- Control plane：dedicated control slot，业务 queue 满时 ping/pong 不被阻塞。
- Runtime：bounded read/write budget、clock cadence、`WriteFlushMode::kTryFlushOne`。

Frame decode 主体已冻结。除非 profile 指向 codec，优化优先级是 exchange parser/serialization/signing、TLS/socket I/O、runtime loop 和
真实 request-to-Ack path。

## Read path

```text
socket/TLS ReadSome
  -> FrameCodec::WritableSpan
  -> CommitWritten
  -> Poll / MessageView
  -> message handler
```

Mirrored ring 让跨物理尾部的 frame 在虚拟地址上连续，避免 compact/memmove；payload view 只在约定借用窗口有效。Capacity exhaustion、
protocol error 和 degraded 有显式结果，不能覆盖未消费数据。

Runtime config：

```toml
[data_session.websocket.read_path]
max_reads_per_drive = 8
read_until_would_block = false
```

参数是预算而非最优常数。太小会让 burst 留到下一轮，太大会让单次 drive 占用 owner 更久；`read_until_would_block=true`
可能改善 drain，也可能增加一次 EAGAIN read。必须在相同 TLS/payload/runtime 条件下验证。

## Receive strategy 对照

| 模型 | Payload | Boundary 行为 | 适用 |
| --- | --- | --- | --- |
| Mirrored ring | borrowed zero-copy view | 无 compact | HFT production read path |
| Linear carry buffer | borrowed view | 可能 compact/memmove | 简单工具、测试、非关键连接 |
| Owning accumulator | copied string/message | framework 管理 | 通用异步服务 |
| Bare parser | caller-owned bytes | 不管理 partial/lifetime | Parser 下限对照 |

`third_party/websocket::handleWSMsg` 只是 bare parser，不负责 socket/TLS buffer、partial frame、capacity、payload lifetime 或 session state，
不能直接替换 production `FrameCodec`。`QueuedFrameCodec` 位于 `evaluation/websocket/queued_frame_codec.h`，只用于 parse-ahead 对照。

## Write path

```text
payload serialization
  -> frame encode + client mask
  -> PreparedWriteArena slot
  -> CommitPreparedWrite
  -> DriveWrite
  -> send/SSL_write
```

Encoded frame 为单段 header/mask/masked-payload。`kTryFlushOne` 只尝试一次，不 busy-loop。Arena 的 slot/bytes/message capacity、
business write budget 和 flush policy 在 `DefaultWebSocketOptions` 或 session config 中显式设置；容量不足必须返回错误/degraded，
不能动态扩容热路径。

第三方 zero-mask/no-XOR write 不符合 client masking 要求，只能作为协议工作量下限。生产对照必须包含随机 mask 与 XOR。

## Runtime 与连接边界

- Active loop 每固定 spin cadence 检查时钟；不因单次 clock microbenchmark 直接切 coarse/TSC。
- Read callback 与 strategy hook 共用 owner 时，read/write budget 必须防止任一方向长期独占。
- Heartbeat/reconnect 属于 session control state，不由业务 handler临时发送。
- 多路行情选择在 `docs/market_data_fusion.md` 完成，策略不直接消费 N 条 WebSocket。
- 扩大 connection count 前先验证 CPU、IRQ、source-level tail 和交易所限额；多连接不等于 N 倍预算。

## 代表 benchmark

2026-04-28 release、`taskset -c 2` 本地 microbenchmark，单位 ns，格式 p50/p99/p99.9：

Read：

| Case | Result | 边界 |
| --- | --- | --- |
| bare parser | 2/2/2 | parser-only 下限 |
| Aquila direct poll | 5/6/17 | production codec core |
| Aquila feed+poll | 7/8/25 | 含 feed |
| Drogon-style feed | 18/19/145 | linear/copy model |
| Linear compact pressure | 1239/2277/2463 | boundary tail |
| Large payload boundary | 2440/3449/4345 | linear/copy pressure |

Pinned 2026-04-26 多轮中 contiguous decode median 为 4/5/12ns；session read path 为 396/433/3902ns。Ns 级 p99.9
容易混入调度噪声，必须结合多轮和环境记录。

Plain write：

| Payload | Aquila | Drogon-style | Third-party compliant |
| --- | --- | --- | --- |
| 64B | 422/445/1549 | 445/935/1413 | 1183/1258/9105 |
| 256B | 454/476/891 | 587/1097/2902 | 1334/1448/5910 |
| 1024B | 523/558/811 | 1128/1652/4565 | 1836/1954/10545 |
| 4096B | 712/779/1836 | 3168/4148/12479 | 3849/4603/12862 |

这些结果比较本地用户态 codec/buffer/write 构造，不包含真实网络、TLS、kernel/NIC、远端 exchange Ack。TLS、mixed read/write、
runtime loopback 和 cold handshake 使用对应 benchmark target 单独测量。

## 代码、测试与 benchmark

```text
core/websocket/frame_codec.h
core/websocket/critical_session.h
core/websocket/prepared_write.h
core/websocket/types.h
core/websocket/runtime_policy.h
benchmark/websocket/third_party_frame_codec_comparison_benchmark.cpp
benchmark/websocket/session_read_path_benchmark.cpp
benchmark/websocket/session_write_path_benchmark.cpp
benchmark/websocket/session_tls_write_path_benchmark.cpp
benchmark/websocket/session_mixed_path_benchmark.cpp
benchmark/websocket/runtime_loopback_benchmark.cpp
```

验证：

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
taskset -c 16 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 16 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 16 ./build/release/benchmark/websocket/session_mixed_path_benchmark
```

Benchmark 必须记录 commit、CPU/affinity、scheduler、MHz/governor、kernel、OpenSSL、payload、TLS/plain、repetitions 和
p50/p99/p99.9/max。当前 VM/未隔离 CPU 的 tail 解释见 `docs/lead_lag_latency_analysis.md` 的环境边界。

## 后续优化入口

只有 evidence 指向具体阶段时才推进：

- TLS read/write 与 session mixed-path tail。
- Exchange JSON/SBE parser、order serialization/signature 与 request-to-Ack。
- Kernel RX/TX timestamping、TCP_INFO、IRQ/CPU isolation。
- Prepared-write capacity 与 read/write budget A/B。
- Long-run reconnect/heartbeat/degraded observability。

不重新引入 queued parse-ahead、owning payload 或无界 buffer；如需改变这些已锁定选择，必须有行为测试和 profile/benchmark 证据。
