# WebSocket Prepared Write Options 使用说明

## 目的

`PreparedWriteArena` 是 WebSocket 写路径的预分配发送缓冲池。调用方先从 arena 获取一个 `PreparedWrite`，把已编码的 WebSocket frame 写入 slot，再提交给 `CriticalSession` 的 pending write queue。

当前已经把 arena 的默认大小收口到 `websocket::DefaultWebSocketOptions`：

```cpp
struct DefaultWebSocketOptions {
  static constexpr size_t kPreparedWriteSlots = 2048;
  static constexpr size_t kPreparedWriteBytes = 4096;
};
```

这一步只改变默认值来源和配置入口，不把 arena 改成静态数组，也不把 arena 大小变成 `BasicWebSocketClient` 的模板参数。`ConnectionConfig` 仍然保存实际运行时值，因此用户可以在构造 client/session 前覆盖。

## 参数含义

| 参数 | 含义 | 默认值 |
| --- | --- | ---: |
| `kPreparedWriteSlots` | `PreparedWriteArena` 的 slot 数量，同时也是业务 pending write queue 的容量。 | `2048` |
| `kPreparedWriteBytes` | 每个 `PreparedWrite` slot 的字节数，用于存放已经编码好的单个 WebSocket frame。 | `4096` |

每个 slot 存放的是编码后的 client frame，不只是业务 payload。需要预留 WebSocket header 和 client mask key 的空间。对于小 payload，额外开销通常是 2 字节基础 header + 4 字节 mask key；较大 payload 会使用 extended length header。

默认 `2048 * 4096` 会为 slot storage 分配约 8 MiB，不含 `PreparedWrite`、free list、pending queue 等元数据。

## 默认用法

直接构造 `ConnectionConfig` 会使用系统默认 options：

```cpp
#include "core/websocket/types.h"

namespace ws = aquila::websocket;

ws::ConnectionConfig config{};
// config.prepared_write_slots == ws::DefaultWebSocketOptions::kPreparedWriteSlots
// config.prepared_write_bytes == ws::DefaultWebSocketOptions::kPreparedWriteBytes
```

这适合不想区分不同链路写路径容量的默认场景。

## 自定义默认值

如果某条链路希望给 prepared write arena 一个固定默认值，可以定义自己的 options struct，并用 `MakeConnectionConfig<OptionsT>()` 创建配置：

```cpp
#include "core/websocket/types.h"

namespace ws = aquila::websocket;

struct TradingWebSocketOptions {
  static constexpr size_t kPreparedWriteSlots = 4096;
  static constexpr size_t kPreparedWriteBytes = 2048;
};

ws::ConnectionConfig config =
    ws::MakeConnectionConfig<TradingWebSocketOptions>();
config.host = "fx-ws.gateio.ws";
config.service = "443";
config.target = "/v4/ws/usdt";
```

`OptionsT` 当前只要求提供：

```cpp
static constexpr size_t kPreparedWriteSlots;
static constexpr size_t kPreparedWriteBytes;
```

## 运行时覆盖

`MakeConnectionConfig<OptionsT>()` 只是填入默认值。最终传给 client/session 的仍是 `ConnectionConfig`，所以可以在构造前按运行环境覆盖：

```cpp
ws::ConnectionConfig config =
    ws::MakeConnectionConfig<TradingWebSocketOptions>();

config.prepared_write_slots = runtime_slots;
config.prepared_write_bytes = runtime_bytes;
```

覆盖必须发生在构造 `BasicWebSocketClient` 或上层 session 之前。client/session 构造后已经持有自己的 `ConnectionConfig` 和 arena，继续修改原始 `config` 对象不会影响已创建实例。

## 如何选择大小

`kPreparedWriteSlots` 应覆盖该连接在一个短时间窗口内可能同时挂起的业务写入数量。它不是吞吐指标，而是 bounded queue 的容量。容量太小会让 `TryAcquirePreparedWrite()` 返回 `nullptr`，上层通常会看到 `SendStatus::kNoPreparedWriteSlot`。

`kPreparedWriteBytes` 应大于该连接可能发送的最大单个业务 frame 的编码后大小。容量太小会导致 encode 失败或提交失败。估算时要包含业务 payload、WebSocket header 和 client mask key。

行情订阅连接通常只发送 subscribe/unsubscribe 这类低频 text frame，实际需求远小于默认值。订单连接会有 order/cancel/replace 等 burst，应该单独根据订单消息大小、最大本地突发和交易所限频来设定。

不要为了“看起来更安全”无限增大。更大的 arena 会增加常驻内存和初始化分配成本，也会掩盖上游写入节奏问题。

## 边界和后续方向

当前设计属于“编译期 options 提供默认值，运行时 config 决定实际值”。它的作用是统一默认值入口，同时保留部署和测试灵活性。

它还不是真正的 compile-time arena policy。`PreparedWriteArena` 仍使用动态分配，`CriticalSession` 的 pending queue 也仍按运行时容量创建。未来如果下单写路径 benchmark 证明有必要，可以再引入类似下面的策略：

```cpp
using PreparedWriteArenaPolicy =
    StaticPreparedWriteArenaPolicy<Slots, Bytes>;
```

那一步会改变对象布局、内存占用和模板实例组合，应该和真实 Gate 下单链路 benchmark 一起评估。
