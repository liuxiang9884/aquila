# WebSocket Client 优化与配置说明

## 范围

本文合并原 WebSocket 未来优化清单和 prepared write options 说明，作为继续
`core/websocket` 性能、尾延迟、生产可观测性和写路径容量配置时的入口文档。

当前前置状态：

1. `FrameCodec` mirrored ring 零拷贝 decode 主体已完成并冻结。
2. bounded read pump、dedicated control write slot、clock cadence、business write budget 已落地。
3. client mask key pool、8-byte chunk XOR、`WriteFlushMode::kTryFlushOne` 已落地。
4. `DefaultWebSocketOptions` 统一了 clock source 和 prepared write arena 默认值。

原则：所有性能、延迟、吞吐和稳定性结论必须来自 benchmark、profile、live probe 或生产观测。

## 当前结论

1. 不应继续只抠 `FrameCodec` p50；除非真实 profile 指向 frame decode，否则后续优化应转向 TLS read/write、真实行情 parser、订单 payload 和 runtime loop。
2. 不应直接把默认 clock source 改成 coarse / TSC；active loop 当前每 4096 次 spin 才检查一次时钟，coarse 的单次取时收益不足以抵消粒度风险。
3. 不应让策略直接消费多条 WebSocket 并自行选择最快数据；多路行情应先收敛成一条 canonical market data stream。
4. 不应未经 live 证据就增加大量 WebSocket 连接；先验证 2 路 primary / standby，再讨论 N=3 / N=4。
5. 通用 WebSocket write encode 近期不再作为主要瓶颈；真实下单链路应优先测 JSON serialization、签名、timestamp、TLS/socket write 和 exchange ack。

## Read Path

生产 read path 按层拆分为：

```text
NIC / kernel TCP receive queue
-> socket fd readable
-> SSL_read()
-> OpenSSL TLS record decrypt
-> FrameCodec::WritableSpan()
-> FrameCodec::CommitWritten()
-> FrameCodec::Poll()
-> exchange parser / strategy consumer
```

当前 `session_read_path_benchmark` 是无 TLS 的本地 socketpair 下限路径，不覆盖真实网卡、TCP 远端收包、TLS 解密、`SSL_pending()`、epoll wakeup 或真实行情解析。

后续优先补：

1. `session_read_path_tls_single_frame`
2. `session_read_path_tls_coalesced_frames`
3. `session_read_path_tls_pending_pump`
4. `session_read_path_tls_partial_frame`
5. `session_read_path_with_market_payload`
6. `session_read_path_with_minimal_json_parse`

目标是区分 `SSL_read()`、TLS decrypt、codec、consumer parser 和 symbol lookup 的成本占比，并用证据决定 `max_reads_per_drive`、`read_until_would_block` 的生产推荐值。

## Live Feed Selection

双 private WS 的毫秒级到达差不能简单归因到 codec、TLS 或 owner thread 绑核。生产形态应先选出一条 canonical stream，而不是让策略直接消费多路行情。

推荐下一步：

```text
WSA + WSB warmup
-> selected primary
-> primary canonical stream
-> secondary standby monitor
-> strategy fanout
```

验收标准：

1. warmup 输出 `selected`、`reason`、health、gap、p50、p99、`SO_INCOMING_CPU`。
2. 交易中策略只消费一条 canonical stream。
3. secondary 持续 monitor，不直接进入策略。
4. 切换必须有 hysteresis，避免 feed flapping。

生产观测至少记录 remote peer、local port、NIC、IRQ/RSS/RPS/XPS、owner CPU、NUMA、socket buffer、OpenSSL、kernel、交易所 endpoint 和连接限额。

## Write Path

生产 write path 按层拆分为：

```text
order / control event
-> payload serialization
-> FrameCodec::EncodeText() / EncodeBinary() / EncodeControl()
-> PreparedWrite / dedicated control slot
-> CriticalSession::DriveWrite()
-> SSL_write()
-> OpenSSL TLS record encrypt
-> kernel socket send buffer
-> NIC
```

business write 当前有两种提交模式：

```text
queued:
  CommitPreparedWrite(write)
  -> pending_writes_
  -> next DriveWrite()

try-flush-one:
  CommitPreparedWrite(write, WriteFlushMode::kTryFlushOne)
  -> pending_writes_
  -> at most one immediate WriteSome()/SSL_write()
```

`kTryFlushOne` 用于 read callback / strategy callback 中的下单或撤单热路径。它不会 busy loop，不会 drain 整个 queue，EAGAIN / partial write 仍保留 pending 给正常 write pump，且不会绕过已有 pending business 或 pending control frame。

当前 benchmark 证据边界：

| benchmark | 当前结论 | 范围限制 |
| --- | --- | --- |
| `session_write_path_benchmark` | 本地 socketpair 下 `CommitPreparedWrite()` 到 `DriveWrite()` 的路径已可测。 | 不含 TLS、真实 TCP、网卡、交易所 ACK。 |
| `session_tls_write_path_benchmark` | local TLS single-frame write 基线已补齐，TLS write 成本不可忽略。 | 仍是本地 loopback，不代表公网或 private link。 |
| `session_write_path_control_slot_full_business_queue` | dedicated control slot 用户态路径很轻，业务队列满时 heartbeat ping 可绕过业务 pending queue。 | fake socket，不代表真实发送成本。 |
| `gate_order_session_benchmark` | 已覆盖 `StrategyContext` -> `OrderManager` -> Gate `OrderSession` -> WebSocket encode -> fake transport / local socketpair `send()`。 | 不含 TLS、真实 TCP、网卡、交易所 ACK；p99 / p999 需按 cold / warm 口径解读，详见 `doc/agent-handoff-gate-trade-architecture.md`。 |

历史 local TLS single-frame write 基线显示，p50 约为 plain local socket 的两倍以上；这个结论只限本地 loopback TLS，不代表交易所公网或 private link。

后续优先补：

1. TLS burst / control / partial write benchmark。
2. Gate order / cancel payload 的 TLS 发送 benchmark 或 live probe。
3. 签名、timestamp、JSON serialization、`kTryFlushOne` 和 TLS / socket 配置的组合 benchmark。
4. 如果 syscall / TLS 是主成本，先调 socket/TLS 配置，再考虑 queue 微优化。

## Prepared Write 配置

`PreparedWriteArena` 是 WebSocket 写路径的预分配发送缓冲池。调用方从 arena 获取一个 `PreparedWrite`，写入已经编码好的 WebSocket frame，再提交给 `CriticalSession` 的 pending write queue。

当前默认值来自 `websocket::DefaultWebSocketOptions`：

```cpp
struct DefaultWebSocketOptions {
  static constexpr ClockSource kClockSource = ClockSource::kSteady;
  static constexpr size_t kPreparedWriteSlots = 2048;
  static constexpr size_t kPreparedWriteBytes = 4096;
};
```

| 参数 | 含义 | 默认值 |
| --- | --- | ---: |
| `kPreparedWriteSlots` | `PreparedWriteArena` 的 slot 数量，同时也是业务 pending write queue 容量。 | `2048` |
| `kPreparedWriteBytes` | 每个 slot 的字节数，用于存放编码后的单个 WebSocket frame。 | `4096` |

每个 slot 存放的是 encoded client frame，不只是业务 payload。需要预留 WebSocket header 和 client mask key；小 payload 通常需要 2 字节基础 header + 4 字节 mask key，较大 payload 还会使用 extended length header。

默认 `2048 * 4096` 会为 slot storage 分配约 8 MiB，不含 `PreparedWrite`、free list 和 pending queue 元数据。

### 默认与自定义

直接构造 `ConnectionConfig` 会使用系统默认 options：

```cpp
#include "core/websocket/types.h"

namespace ws = aquila::websocket;

ws::ConnectionConfig config{};
```

如果某条链路需要固定默认值，可以定义自己的 options struct，并用
`MakeConnectionConfig<OptionsT>()` 创建配置。建议继承 `DefaultWebSocketOptions`，只覆盖当前链路需要调整的字段：

```cpp
#include "core/websocket/types.h"

namespace ws = aquila::websocket;

struct TradingWebSocketOptions : ws::DefaultWebSocketOptions {
  static constexpr size_t kPreparedWriteSlots = 4096;
  static constexpr size_t kPreparedWriteBytes = 2048;
};

ws::ConnectionConfig config =
    ws::MakeConnectionConfig<TradingWebSocketOptions>();
```

`OptionsT` 当前要求提供：

```cpp
static constexpr aquila::websocket::ClockSource kClockSource;
static constexpr size_t kPreparedWriteSlots;
static constexpr size_t kPreparedWriteBytes;
```

`kClockSource` 会写入 `config.runtime_policy.clock_source`。如果只想调整 prepared write 大小，继承 `DefaultWebSocketOptions` 即可沿用默认时钟。

`MakeConnectionConfig<OptionsT>()` 只是填默认值。最终传给 client/session 的仍是 `ConnectionConfig`，所以也可以在构造前按运行环境覆盖：

```cpp
config.prepared_write_slots = runtime_slots;
config.prepared_write_bytes = runtime_bytes;
```

覆盖必须发生在构造 `BasicWebSocketClient` 或上层 session 之前。client/session 构造后已经持有自己的 `ConnectionConfig` 和 arena，继续修改原始 `config` 对象不会影响已创建实例。

### 容量选择

`kPreparedWriteSlots` 应覆盖该连接在短时间窗口内可能同时挂起的业务写入数量。它不是吞吐指标，而是 bounded queue 容量；容量太小会让 `TryAcquirePreparedWrite()` 返回 `nullptr`，上层通常会看到 `SendStatus::kNoPreparedWriteSlot`。

`kPreparedWriteBytes` 应大于该连接可能发送的最大单个业务 frame 的编码后大小。容量太小会导致 encode 或提交失败。估算时必须包含业务 payload、WebSocket header 和 client mask key。

行情订阅连接通常只发送 subscribe/unsubscribe 这类低频 text frame，实际需求远小于默认值。订单连接会有 order/cancel/replace 等 burst，应该根据订单消息大小、最大本地突发和交易所限频单独设定。

不要为了“看起来更安全”无限增大。更大的 arena 会增加常驻内存和初始化分配成本，也会掩盖上游写入节奏问题。

当前设计仍是“编译期 options 提供默认值，运行时 config 决定实际值”。它还不是真正的 compile-time arena policy；`PreparedWriteArena` 仍使用动态分配，`CriticalSession` 的 pending queue 也仍按运行时容量创建。如果真实下单链路 benchmark 证明有必要，再讨论静态 arena policy。

## Active Spin Loop

当前 runtime loop 已有以下边界：

1. `ShouldReconnect()` 保留 acquire；read/write hot method 不再重复 load stop flag。
2. `max_business_writes_per_drive = 1` 是默认 pacing，`0` 表示 legacy unbounded。
3. mixed read/write benchmark 已证明 unbounded business write 会线性推迟 read 交付。

后续只有在 benchmark 证明收益时再做：

1. 拆 `RunActive()` / `RunYield()`。
2. 比较 write-first、read-first、control-first-then-read。
3. 让 read/write 返回 did-work 后，只在 idle iteration `CpuRelax()`。
4. 评估 calibrated TSC 或更激进的 branch layout。

## 验证入口

WebSocket 回归测试：

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
ctest --test-dir build/release -R websocket_ --output-on-failure
```

核心 benchmark：

```bash
taskset -c 2 ./build/release/benchmark/websocket/frame_codec_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_mixed_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_tls_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/runtime_loopback_benchmark
```

live 工具：

```bash
./build/release/tools/websocket_latency_compare --contract BTC_USDT --duration-ms 60000
./build/release/tools/websocket_probe --host fx-ws.gateio.ws --port 443 --target /v4/ws/usdt --tls --cpu 2
```

## 相关文档

- `doc/websocket_frame_codec_receive_strategies.md`
- `doc/websocket_read_write_benchmark_comparison.md`
- `doc/data_session_config.md`
