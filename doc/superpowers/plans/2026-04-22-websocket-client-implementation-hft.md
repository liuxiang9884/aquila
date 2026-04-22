# Low-Latency WebSocket Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `core/websocket` 下实现一个面向 Linux x86_64 的低延迟 C++20 WebSocket client 内核，满足单连接唯一 owner、严格串行发送、控制面隔离、热路径零临时分配、细粒度状态机和可证据化验证的高频系统约束。

**Architecture:** 实现按“连接数据面”和“冷路径控制面”拆分：`transport + frame codec + session + pending write chain + fixed buffer pools` 组成热路径；`state machine + owner command ring + control plane + metrics` 组成冷路径。`core/websocket` 不内建业务 `inbound/outbound queue`，只提供 owner-thread `try_send`、消息交付契约和最小命令注入机制；跨线程提交、业务排队和下游背压都留给集成方。

**Tech Stack:** C++20, Linux `epoll/eventfd/timerfd`, non-blocking sockets, OpenSSL, CMake, `ctest`, `socketpair`/loopback test harness，手写 microbenchmark（二进制独立可执行，不引入额外 benchmark 框架依赖）。

---

## File Map

### Build And Entry Points

- Modify: `CMakeLists.txt`
  - 顶层接入 `core` 和 `benchmark` 子目录。
- Create: `core/CMakeLists.txt`
  - 注册 `core/websocket` 子模块。
- Create: `core/websocket/CMakeLists.txt`
  - 定义 `aquila_websocket` 静态库和依赖。
- Modify: `test/CMakeLists.txt`
  - 新增 `test/websocket` 子目录和各 websocket 测试注册。
- Create: `test/websocket/CMakeLists.txt`
  - 组织 websocket 单测与环回集成测试。
- Modify: `tools/CMakeLists.txt`
  - 新增 `websocket_probe`。
- Modify: `benchmark/CMakeLists.txt`
  - 接入 `benchmark/websocket`。
- Create: `benchmark/websocket/CMakeLists.txt`
  - 组织 websocket microbenchmark。

### Core Library

- Create: `core/websocket/types.h`
  - 基础类型、连接配置、错误码、状态码、返回码。
- Create: `core/websocket/message_view.h`
  - 零拷贝消息视图及生命周期约束。
- Create: `core/websocket/message_sink.h`
  - owner-thread 交付契约与 `DeliveryResult`。
- Create: `core/websocket/fixed_buffer_pool.h`
  - 固定大小 buffer 池，热路径无扩容。
- Create: `core/websocket/prepared_write.h`
  - 已编码写请求及其 buffer ownership。
- Create: `core/websocket/pending_write.h`
  - 单连接严格有界挂起写链。
- Create: `core/websocket/owner_command.h`
  - owner 命令类型定义。
- Create: `core/websocket/owner_command_ring.h`
  - 固定容量 owner 命令 ring，支持最小 coalescing。
- Create: `core/websocket/state_machine.h`
  - 细粒度阶段状态机。
- Create: `core/websocket/transport.h`
  - transport 抽象。
- Create: `core/websocket/tcp_transport.h`
  - 非阻塞 TCP transport。
- Create: `core/websocket/tls_transport.h`
  - OpenSSL transport 适配层。
- Create: `core/websocket/handshake.h`
  - 客户端握手请求构造与服务端响应校验。
- Create: `core/websocket/frame_codec.h`
  - frame encode/decode，caller-provided buffer，支持部分帧解析。
- Create: `core/websocket/io_loop.h`
  - `epoll/eventfd/timerfd` owner loop。
- Create: `core/websocket/metrics.h`
  - `L0/L1` 指标和预算化事件记录。
- Create: `core/websocket/control_plane.h`
  - 心跳、阶段超时、退化与重连退避。
- Create: `core/websocket/session.h`
  - owner-thread session 内核、串行发送、读写推进、消息交付。
- Create: `core/websocket/client.h`
  - 粗粒度生命周期入口和控制面编排。

### Tests

- Create: `test/websocket/types_test.cpp`
- Create: `test/websocket/fixed_buffer_pool_test.cpp`
- Create: `test/websocket/pending_write_test.cpp`
- Create: `test/websocket/owner_command_ring_test.cpp`
- Create: `test/websocket/state_machine_test.cpp`
- Create: `test/websocket/handshake_test.cpp`
- Create: `test/websocket/frame_codec_test.cpp`
- Create: `test/websocket/io_loop_test.cpp`
- Create: `test/websocket/session_test.cpp`
- Create: `test/websocket/tcp_transport_test.cpp`
- Create: `test/websocket/control_plane_test.cpp`
- Create: `test/websocket/tls_transport_test.cpp`
- Create: `test/websocket/loopback_integration_test.cpp`

### Tools And Benchmarks

- Create: `tools/websocket_probe.cpp`
  - 本地或远端 `ws/wss` smoke probe，不作为性能依据。
- Create: `benchmark/websocket/pending_write_benchmark.cpp`
- Create: `benchmark/websocket/frame_codec_benchmark.cpp`
- Create: `benchmark/websocket/session_owner_benchmark.cpp`

## Task 1: Scaffold The Module And Harden The Public Contracts

**Files:**
- Modify: `CMakeLists.txt`
- Create: `core/CMakeLists.txt`
- Create: `core/websocket/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Create: `test/websocket/CMakeLists.txt`
- Create: `core/websocket/types.h`
- Create: `core/websocket/message_view.h`
- Create: `core/websocket/message_sink.h`
- Create: `test/websocket/types_test.cpp`

- [ ] **Step 1: Write the failing public-contract smoke test**

```cpp
#include "core/websocket/message_sink.h"
#include "core/websocket/message_view.h"
#include "core/websocket/types.h"

int main() {
    using namespace aquila::websocket;
    static_assert(static_cast<int>(ConnectionPhase::Disconnected) == 0);

    MessageView view{};
    struct NoopSink final : MessageSink {
        DeliveryResult handle(const MessageView&) noexcept override {
            return DeliveryResult::Accepted;
        }
    } sink;

    if (view.payload.data() != nullptr) return 1;
    if (sink.handle(view) != DeliveryResult::Accepted) return 1;
    return SendStatus::PendingFull != SendStatus::Ok ? 0 : 1;
}
```

- [ ] **Step 2: Run build to verify it fails**

Run: `./build.sh debug`
Expected: compile failure because websocket module and headers do not exist.

- [ ] **Step 3: Add module wiring and minimal owner-bound public types**

```cpp
// core/websocket/types.h
namespace aquila::websocket {
enum class ConnectionState : uint8_t { Disconnected, Connecting, Active, Degraded, Closing };
enum class ConnectionPhase : uint8_t {
    Disconnected,
    Resolving,
    TCPConnecting,
    TLSHandshaking,
    WsHandshaking,
    Authenticating,
    Active,
    Degraded,
    Reconnecting,
    Closing,
    Closed
};
enum class ConnectionError : uint8_t {
    None,
    InvalidTransition,
    SocketError,
    ConnectTimeout,
    TLSFailure,
    HandshakeFailure,
    ProtocolError,
    HeartbeatTimeout,
    PeerClosed
};
enum class PayloadKind : uint8_t { Text, Binary, Ping, Pong, Close };
enum class SendStatus : uint8_t { Ok, NotActive, PendingFull, PayloadTooLarge, EncodeFailed };
enum class DeliveryResult : uint8_t { Accepted, Backpressured, Fatal };

struct ConnectionConfig {
    std::string host;
    std::string service;
    std::string target;
    bool enable_tls{false};
    size_t read_buffer_bytes{64 * 1024};
    size_t frame_buffer_bytes{64 * 1024};
    size_t pending_write_capacity{1024};
    size_t owner_command_capacity{128};
    size_t max_frame_payload_bytes{64 * 1024};
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/message_view.h
namespace aquila::websocket {
struct MessageView {
    PayloadKind kind{PayloadKind::Binary};
    std::span<const std::byte> payload{};
    uint64_t sequence{0};
    bool fin{true};
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/message_sink.h
namespace aquila::websocket {
class MessageSink {
  public:
    virtual ~MessageSink() = default;
    virtual DeliveryResult handle(const MessageView& view) noexcept = 0;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the dedicated smoke test**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_types_test -V`
Expected: `websocket_types_test` PASS.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt core/CMakeLists.txt core/websocket/CMakeLists.txt \
  test/CMakeLists.txt test/websocket/CMakeLists.txt \
  core/websocket/types.h core/websocket/message_view.h core/websocket/message_sink.h \
  test/websocket/types_test.cpp
git commit -m "core: scaffold websocket contracts for low-latency client"
```

## Task 2: Implement Deterministic Memory Primitives And Prepared Writes

**Files:**
- Create: `core/websocket/fixed_buffer_pool.h`
- Create: `core/websocket/prepared_write.h`
- Create: `core/websocket/pending_write.h`
- Create: `test/websocket/fixed_buffer_pool_test.cpp`
- Create: `test/websocket/pending_write_test.cpp`

- [ ] **Step 1: Write failing tests for pool exhaustion, reuse, and bounded pending writes**

```cpp
int main() {
    using namespace aquila::websocket;

    FixedBufferPool pool(128, 2);
    auto a = pool.try_acquire();
    auto b = pool.try_acquire();
    auto c = pool.try_acquire();
    if (!a || !b || c) return 1;
    pool.release(std::move(*a));
    auto d = pool.try_acquire();
    if (!d) return 1;

    PendingWriteChain chain(2);
    if (!chain.try_push(PreparedWrite{})) return 1;
    if (!chain.try_push(PreparedWrite{})) return 1;
    if (chain.try_push(PreparedWrite{})) return 1;
    chain.pop_front();
    chain.pop_front();
    return chain.empty() ? 0 : 1;
}
```

- [ ] **Step 2: Run the tests and verify they fail**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(fixed_buffer_pool|pending_write)_test" -V`
Expected: compile failure because pool and pending-write headers do not exist.

- [ ] **Step 3: Implement fixed buffer pool, prepared-write ownership, and bounded write chain**

```cpp
// core/websocket/prepared_write.h
namespace aquila::websocket {
struct PreparedWrite {
    FixedBufferHandle buffer{};
    PayloadKind kind{PayloadKind::Binary};
    uint32_t encoded_size{0};
    uint32_t write_offset{0};
    bool empty() const noexcept { return encoded_size == 0; }
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/pending_write.h
namespace aquila::websocket {
class PendingWriteChain {
  public:
    explicit PendingWriteChain(size_t capacity);
    bool try_push(PreparedWrite write) noexcept;
    PreparedWrite* front() noexcept;
    void pop_front() noexcept;
    bool empty() const noexcept;
    size_t size() const noexcept;
    size_t high_watermark() const noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the memory-primitive tests**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(fixed_buffer_pool|pending_write)_test" -V`
Expected: PASS with fail-fast exhaustion and no implicit resize.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/fixed_buffer_pool.h core/websocket/prepared_write.h \
  core/websocket/pending_write.h test/websocket/fixed_buffer_pool_test.cpp \
  test/websocket/pending_write_test.cpp
git commit -m "core: add websocket fixed buffers and pending writes"
```

## Task 3: Implement Owner Command Ring And Fine-Grained State Machine

**Files:**
- Create: `core/websocket/owner_command.h`
- Create: `core/websocket/owner_command_ring.h`
- Create: `core/websocket/state_machine.h`
- Create: `test/websocket/owner_command_ring_test.cpp`
- Create: `test/websocket/state_machine_test.cpp`

- [ ] **Step 1: Write failing tests for command coalescing and invalid phase transitions**

```cpp
int main() {
    using namespace aquila::websocket;

    OwnerCommandRing ring(4);
    if (!ring.try_push({OwnerCommandKind::SendPing})) return 1;
    ring.coalesce({OwnerCommandKind::SendPing});
    auto first = ring.try_pop();
    if (!first || first->kind != OwnerCommandKind::SendPing) return 1;
    if (ring.try_pop()) return 1;

    StateMachine sm;
    if (!sm.transition_to(ConnectionPhase::Resolving)) return 1;
    if (!sm.transition_to(ConnectionPhase::TCPConnecting)) return 1;
    if (sm.transition_to(ConnectionPhase::Authenticating)) return 1;
    return sm.phase() == ConnectionPhase::TCPConnecting ? 0 : 1;
}
```

- [ ] **Step 2: Run the command/state tests and verify they fail**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(owner_command_ring|state_machine)_test" -V`
Expected: compile failure because command ring and state machine are missing.

- [ ] **Step 3: Implement minimal owner command channel and phase rules**

```cpp
namespace aquila::websocket {
enum class OwnerCommandKind : uint8_t {
    FlushWrites,
    SendPing,
    StartClose,
    Abort,
    Reconnect
};

struct OwnerCommand {
    OwnerCommandKind kind{OwnerCommandKind::FlushWrites};
    ConnectionError error{ConnectionError::None};
    uint64_t value{0};
};

class OwnerCommandRing {
  public:
    explicit OwnerCommandRing(size_t capacity);
    bool try_push(OwnerCommand command) noexcept;
    void coalesce(OwnerCommand command) noexcept;
    std::optional<OwnerCommand> try_pop() noexcept;
};

class StateMachine {
  public:
    ConnectionPhase phase() const noexcept;
    bool transition_to(ConnectionPhase next) noexcept;
    std::optional<ConnectionError> last_error() const noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the command/state tests**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(owner_command_ring|state_machine)_test" -V`
Expected: PASS with coalesced ping/reconnect commands and explicit invalid-transition rejection.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/owner_command.h core/websocket/owner_command_ring.h \
  core/websocket/state_machine.h test/websocket/owner_command_ring_test.cpp \
  test/websocket/state_machine_test.cpp
git commit -m "core: add websocket owner commands and state machine"
```

## Task 4: Implement Handshake Builder And Allocation-Free Frame Codec

**Files:**
- Create: `core/websocket/handshake.h`
- Create: `core/websocket/frame_codec.h`
- Create: `test/websocket/handshake_test.cpp`
- Create: `test/websocket/frame_codec_test.cpp`

- [ ] **Step 1: Write failing tests for handshake bytes, masked client frames, and partial decode**

```cpp
int main() {
    using namespace aquila::websocket;

    std::array<char, 512> request{};
    auto built = build_client_handshake("example.com", "/feed", "key==", request);
    if (!built.ok || built.bytes.find("Upgrade: websocket") == std::string_view::npos) return 1;

    FrameCodec codec(128);
    std::array<std::byte, 128> frame_storage{};
    auto encoded = codec.encode_text(std::as_bytes(std::span{"ok", 2}), frame_storage);
    if (!encoded.ok) return 1;

    auto partial = codec.feed(std::span(encoded.bytes).first(1));
    if (partial.status != DecodeStatus::NeedMore) return 1;
    auto full = codec.feed(encoded.bytes);
    return full.status == DecodeStatus::MessageReady ? 0 : 1;
}
```

- [ ] **Step 2: Run the handshake/frame tests and verify they fail**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(handshake|frame_codec)_test" -V`
Expected: compile failure because handshake builder and frame codec are missing.

- [ ] **Step 3: Implement caller-buffer handshake and fixed-capacity frame codec**

```cpp
namespace aquila::websocket {
struct HandshakeBuildResult {
    bool ok{false};
    std::string_view bytes{};
};

HandshakeBuildResult build_client_handshake(std::string_view host,
                                            std::string_view target,
                                            std::string_view client_key,
                                            std::span<char> output) noexcept;
bool validate_server_handshake(std::string_view response,
                               std::string_view client_key) noexcept;

enum class DecodeStatus : uint8_t { NeedMore, MessageReady, ProtocolError };

struct DecodeResult {
    DecodeStatus status{DecodeStatus::NeedMore};
    MessageView view{};
};

class FrameCodec {
  public:
    explicit FrameCodec(size_t max_payload_bytes);
    EncodeResult encode_text(std::span<const std::byte> payload,
                             std::span<std::byte> output) noexcept;
    EncodeResult encode_binary(std::span<const std::byte> payload,
                               std::span<std::byte> output) noexcept;
    EncodeResult encode_control(PayloadKind kind,
                                std::span<const std::byte> payload,
                                std::span<std::byte> output) noexcept;
    DecodeResult feed(std::span<const std::byte> bytes) noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the handshake/frame tests**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(handshake|frame_codec)_test" -V`
Expected: PASS for text/binary/ping/pong/close frames, masking, and partial-frame reassembly.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/handshake.h core/websocket/frame_codec.h \
  test/websocket/handshake_test.cpp test/websocket/frame_codec_test.cpp
git commit -m "core: add websocket handshake and fixed-capacity frame codec"
```

## Task 5: Implement The Owner Reactor Loop

**Files:**
- Create: `core/websocket/io_loop.h`
- Create: `test/websocket/io_loop_test.cpp`

- [ ] **Step 1: Write failing tests for eventfd wakeup, timerfd expiry, and epoll dispatch**

```cpp
int main() {
    using namespace aquila::websocket;

    IoLoop loop;
    if (!loop.init()) return 1;
    if (!loop.signal()) return 1;

    std::array<IoEvent, 4> events{};
    auto count = loop.poll_once(std::chrono::milliseconds(0), events);
    if (count == 0) return 1;
    return events[0].source != IoEventSource::Wakeup ? 1 : 0;
}
```

- [ ] **Step 2: Run the io-loop test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_io_loop_test -V`
Expected: compile failure because `IoLoop` does not exist.

- [ ] **Step 3: Implement epoll owner loop without generic hot-path callbacks**

```cpp
namespace aquila::websocket {
enum class IoEventSource : uint8_t { Wakeup, Timer, Socket };

struct IoEvent {
    IoEventSource source{IoEventSource::Socket};
    uint32_t events{0};
    void* token{nullptr};
};

class IoLoop {
  public:
    bool init() noexcept;
    bool register_fd(int fd, uint32_t events, void* token) noexcept;
    bool modify_fd(int fd, uint32_t events, void* token) noexcept;
    void unregister_fd(int fd) noexcept;
    bool signal() noexcept;
    int poll_once(std::chrono::milliseconds timeout,
                  std::span<IoEvent> out) noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the io-loop test**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_io_loop_test -V`
Expected: PASS with deterministic wakeup and timer dispatch through epoll.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/io_loop.h test/websocket/io_loop_test.cpp
git commit -m "core: add websocket owner io loop"
```

## Task 6: Implement Session Core With Strict Owner-Only Sends

**Files:**
- Create: `core/websocket/session.h`
- Create: `test/websocket/session_test.cpp`

- [ ] **Step 1: Write failing tests for `try_send_copy`, partial writes, sink delivery, and backpressure**

```cpp
int main() {
    using namespace aquila::websocket;

    SessionHarness harness;
    if (harness.try_send_copy_owner("one") != SendStatus::Ok) return 1;
    if (harness.try_send_copy_owner("two") != SendStatus::Ok) return 1;
    if (harness.try_send_copy_owner("three") != SendStatus::PendingFull) return 1;

    harness.drive_writable_once();
    if (harness.completed_writes() != 1) return 1;
    harness.drive_writable_once();
    if (harness.completed_writes() != 2) return 1;

    harness.feed_peer_text("ok");
    return harness.delivered_messages() == 1 ? 0 : 1;
}
```

- [ ] **Step 2: Run the session test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_session_test -V`
Expected: compile failure because session core is missing.

- [ ] **Step 3: Implement owner-only session APIs and serialized write path**

```cpp
namespace aquila::websocket {
class Session {
  public:
    Session(ConnectionConfig config,
            Transport& transport,
            StateMachine& state_machine,
            FrameCodec& codec,
            PendingWriteChain& pending_writes,
            OwnerCommandRing& commands,
            Metrics& metrics) noexcept;

    void set_message_sink(MessageSink* sink) noexcept;
    SendStatus try_send_copy(std::span<const std::byte> payload,
                             PayloadKind kind) noexcept;  // owner-thread only
    SendStatus try_send(PreparedWrite write) noexcept;    // owner-thread only
    void on_readable() noexcept;
    void on_writable() noexcept;
    void on_owner_command(OwnerCommand command) noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the session test**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_session_test -V`
Expected: PASS with one active write chain, partial-write continuation, fail-fast overflow, and owner-thread sink delivery.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/session.h test/websocket/session_test.cpp
git commit -m "core: add websocket session core with serialized sends"
```

## Task 7: Implement Plain TCP Transport And Loopback Integration

**Files:**
- Create: `core/websocket/transport.h`
- Create: `core/websocket/tcp_transport.h`
- Create: `test/websocket/tcp_transport_test.cpp`
- Create: `test/websocket/loopback_integration_test.cpp`

- [ ] **Step 1: Write failing tests for non-blocking connect and plain-ws loopback exchange**

```cpp
int main() {
    using namespace aquila::websocket;

    LoopbackServer server;
    auto endpoint = server.start_plain_ws();

    TcpTransport transport;
    if (!transport.start_connect(endpoint)) return 1;

    PlainWsHarness harness(server, transport);
    if (!harness.handshake()) return 1;
    if (!harness.send_text("ping")) return 1;
    return harness.recv_text() == "pong" ? 0 : 1;
}
```

- [ ] **Step 2: Run the transport/integration tests and verify they fail**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(tcp_transport|loopback_integration)_test" -V`
Expected: compile failure because TCP transport and loopback harness are missing.

- [ ] **Step 3: Implement transport abstraction and non-blocking TCP adapter**

```cpp
namespace aquila::websocket {
class Transport {
  public:
    virtual ~Transport() = default;
    virtual bool start_connect(const ConnectionConfig& config) noexcept = 0;
    virtual ssize_t read_some(std::span<std::byte> buffer) noexcept = 0;
    virtual ssize_t write_some(std::span<const std::byte> buffer) noexcept = 0;
    virtual int native_fd() const noexcept = 0;
    virtual void close() noexcept = 0;
};

class TcpTransport final : public Transport {
  public:
    bool start_connect(const ConnectionConfig& config) noexcept override;
    ssize_t read_some(std::span<std::byte> buffer) noexcept override;
    ssize_t write_some(std::span<const std::byte> buffer) noexcept override;
    int native_fd() const noexcept override;
    void close() noexcept override;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the transport/integration tests**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(tcp_transport|loopback_integration)_test" -V`
Expected: PASS with plain TCP connect, successful websocket handshake, and loopback text exchange.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/transport.h core/websocket/tcp_transport.h \
  test/websocket/tcp_transport_test.cpp test/websocket/loopback_integration_test.cpp
git commit -m "core: add websocket tcp transport and plain loopback integration"
```

## Task 8: Implement Control Plane, Metrics, And Public Client Orchestration

**Files:**
- Create: `core/websocket/metrics.h`
- Create: `core/websocket/control_plane.h`
- Create: `core/websocket/client.h`
- Create: `test/websocket/control_plane_test.cpp`

- [ ] **Step 1: Write failing tests for degraded transition, heartbeat timeout, and reconnect backoff**

```cpp
int main() {
    using namespace aquila::websocket;

    FakeClock clock;
    Metrics metrics;
    ControlPlane control(clock, metrics);

    control.on_phase(ConnectionPhase::Active);
    clock.advance(std::chrono::milliseconds(250));
    control.on_heartbeat_timeout();
    if (!control.should_enter_degraded()) return 1;

    control.on_disconnect(ConnectionError::HeartbeatTimeout);
    return control.next_retry_delay() > std::chrono::milliseconds(0) ? 0 : 1;
}
```

- [ ] **Step 2: Run the control-plane test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_control_plane_test -V`
Expected: compile failure because control plane and metrics are missing.

- [ ] **Step 3: Implement control decisions, metrics budget, and coarse client API**

```cpp
namespace aquila::websocket {
struct Metrics {
    uint64_t rx_bytes{0};
    uint64_t tx_bytes{0};
    uint64_t rx_messages{0};
    uint64_t tx_messages{0};
    uint64_t reconnects{0};
    uint64_t pending_write_high_watermark{0};
};

class ControlPlane {
  public:
    ControlPlane(Clock& clock, Metrics& metrics) noexcept;
    void on_phase(ConnectionPhase phase) noexcept;
    void on_delivery_result(DeliveryResult result) noexcept;
    void on_heartbeat_timeout() noexcept;
    void on_disconnect(ConnectionError error) noexcept;
    bool should_enter_degraded() const noexcept;
    std::chrono::milliseconds next_retry_delay() const noexcept;
};

class Client {
  public:
    bool start(const ConnectionConfig& config, MessageSink* sink) noexcept;
    void request_ping() noexcept;
    void request_close() noexcept;
    ConnectionState state() const noexcept;
    Metrics snapshot_metrics() const noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run the control-plane test**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_control_plane_test -V`
Expected: PASS with distinct degraded handling, heartbeat timeout accounting, and bounded reconnect backoff.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/metrics.h core/websocket/control_plane.h \
  core/websocket/client.h test/websocket/control_plane_test.cpp
git commit -m "core: add websocket control plane and client orchestration"
```

## Task 9: Implement TLS Transport And Probe Tool

**Files:**
- Create: `core/websocket/tls_transport.h`
- Create: `test/websocket/tls_transport_test.cpp`
- Create: `tools/websocket_probe.cpp`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: Write failing tests for OpenSSL adapter initialization and WANT_READ/WANT_WRITE flow**

```cpp
int main() {
    using namespace aquila::websocket;

    TlsTransport transport;
    if (!transport.init()) return 1;
    return transport.native_fd() == -1 ? 0 : 1;
}
```

- [ ] **Step 2: Run the TLS test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_tls_transport_test -V`
Expected: compile failure because TLS transport is missing.

- [ ] **Step 3: Implement OpenSSL transport and a smoke probe binary**

```cpp
namespace aquila::websocket {
class TlsTransport final : public Transport {
  public:
    bool init() noexcept;
    bool start_connect(const ConnectionConfig& config) noexcept override;
    ssize_t read_some(std::span<std::byte> buffer) noexcept override;
    ssize_t write_some(std::span<const std::byte> buffer) noexcept override;
    int native_fd() const noexcept override;
    void close() noexcept override;
};
}  // namespace aquila::websocket
```

```bash
build/debug/tools/websocket_probe --host 127.0.0.1 --port 8443 --target /echo --tls
```

- [ ] **Step 4: Run TLS smoke validation**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_tls_transport_test -V`
Expected: PASS for local TLS harness or BIO-pair handshake, and `websocket_probe` binary builds successfully.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/tls_transport.h test/websocket/tls_transport_test.cpp \
  tools/websocket_probe.cpp tools/CMakeLists.txt
git commit -m "core: add websocket tls transport and probe tool"
```

## Task 10: Add Benchmarks, Fault-Injection Verification, And Final Acceptance

**Files:**
- Modify: `benchmark/CMakeLists.txt`
- Create: `benchmark/websocket/CMakeLists.txt`
- Create: `benchmark/websocket/pending_write_benchmark.cpp`
- Create: `benchmark/websocket/frame_codec_benchmark.cpp`
- Create: `benchmark/websocket/session_owner_benchmark.cpp`

- [ ] **Step 1: Write benchmark skeletons and verification commands**

```cpp
int main() {
    // print fixed-capacity latency stats: count, p50, p99, p99.9, max
}
```

- [ ] **Step 2: Build once to verify benchmark targets are wired**

Run: `./build.sh release`
Expected: initial benchmark target build fails because benchmark sources are not implemented.

- [ ] **Step 3: Implement microbenchmarks and final verification recipe**

```text
pending_write_benchmark.cpp:
  - measure push/front/pop latency at fixed capacities
frame_codec_benchmark.cpp:
  - measure encode/decode latency for text/binary/control frames
session_owner_benchmark.cpp:
  - measure owner-thread wakeup, write-drain, and sink-delivery latency using fake transport
```

```bash
./build.sh debug
ctest --test-dir build/debug --output-on-failure -R "websocket_"
./build.sh release
build/release/benchmark/websocket/pending_write_benchmark
build/release/benchmark/websocket/frame_codec_benchmark
build/release/benchmark/websocket/session_owner_benchmark
```

- [ ] **Step 4: Run final acceptance**

Expected:

```text
All websocket tests pass in Debug
Release build succeeds
Benchmark binaries print p50/p99/p99.9/max
No test or benchmark reports hidden dynamic growth in pending writes or owner commands
```

- [ ] **Step 5: Commit**

```bash
git add benchmark/CMakeLists.txt benchmark/websocket/CMakeLists.txt \
  benchmark/websocket/pending_write_benchmark.cpp \
  benchmark/websocket/frame_codec_benchmark.cpp \
  benchmark/websocket/session_owner_benchmark.cpp
git commit -m "benchmark: add websocket latency verification"
```

## Spec Coverage Check

- 单连接唯一 owner：Task 5, Task 6, Task 7
- 热路径零临时分配：Task 2, Task 4, Task 6
- 发送严格串行且 fail-fast：Task 2, Task 6
- `WebSocket core` 不内建业务队列：Task 1, Task 6, Task 8
- 控制面与 I/O owner 隔离：Task 3, Task 8
- `ws/wss` transport-agnostic：Task 7, Task 9
- 细粒度状态机和失败分类：Task 3, Task 8
- 消息交付 contract 与 backpressure 可观测：Task 1, Task 6, Task 8
- benchmark 与证据化收尾：Task 10

## Placeholder Scan

- 本计划不使用 `TODO/TBD/implement later`。
- 每个任务都包含具体文件、最小 API 轮廓、构建命令和期望结果。
- 若执行过程中必须调整命名或签名，应先同步修改本计划中的后续任务，再进入实现。

## Execution Handoff

Plan complete and saved to `doc/superpowers/plans/2026-04-22-websocket-client-implementation-hft.md`.

Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
