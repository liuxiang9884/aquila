# WebSocket Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `core/websocket` 下实现一个面向 Linux x86_64 的低延迟 C++20 WebSocket client 内核，满足单连接唯一 owner、严格串行发送、控制面隔离、热路径零临时分配和细粒度状态机的设计约束。

**Architecture:** 实现拆成三层：`I/O owner + transport + websocket session core` 作为热路径；`codec/message view + bounded queues + memory pools` 作为数据面基础件；`state machine + control plane + observability` 作为冷路径控制层。交付顺序按“先可单测基础件，再 plain TCP/ws，再 session/control，再 TLS，再 benchmark/tooling”推进，每一阶段都要形成可运行、可回归的最小闭环。

**Tech Stack:** C++20, Linux `epoll/eventfd/timerfd`, non-blocking sockets, OpenSSL, CMake, ctest, yyjson（仅用于后续 JSON 最小解析适配，不进入热路径内核依赖）。

---

## File Map

### Build And Entry Points

- Modify: `CMakeLists.txt`
  - 新增 `add_subdirectory(core)`，把 `benchmark` 按顶层项目启用。
- Create: `core/CMakeLists.txt`
  - 注册 `core/websocket` 子模块。
- Create: `core/websocket/CMakeLists.txt`
  - 定义 `aquila_websocket` 静态库。
- Modify: `test/CMakeLists.txt`
  - 新增 `test/websocket` 子目录和 `ctest` 注册。
- Create: `test/websocket/CMakeLists.txt`
  - 管理 websocket 相关单元测试可执行文件。
- Modify: `tools/CMakeLists.txt`
  - 新增最小 probe 工具。
- Create: `benchmark/websocket/CMakeLists.txt`
  - 管理 websocket micro/chain benchmark。
- Modify: `benchmark/CMakeLists.txt`
  - 接入 `benchmark/websocket`。

### Core Library

- Create: `core/websocket/types.h`
  - 基础类型：配置、错误码、消息类型、粗粒度和细粒度状态。
- Create: `core/websocket/message_view.h`
  - 零拷贝消息视图和 frame/meta 视图。
- Create: `core/websocket/bounded_spsc_queue.h`
  - 有界 `SPSC` 队列，默认 `fail-fast`。
- Create: `core/websocket/fixed_buffer_pool.h`
  - 固定大小 buffer 池。
- Create: `core/websocket/state_machine.h`
  - 细粒度状态和转移规则。
- Create: `core/websocket/io_loop.h`
  - `epoll` owner loop, `eventfd`, `timerfd` 集成。
- Create: `core/websocket/transport.h`
  - 统一 `read/write/connect/close` 抽象。
- Create: `core/websocket/tcp_transport.h`
  - 非阻塞 TCP transport。
- Create: `core/websocket/tls_transport.h`
  - OpenSSL transport 适配层。
- Create: `core/websocket/handshake.h`
  - WebSocket client handshake request/response 校验。
- Create: `core/websocket/frame_codec.h`
  - frame encode/decode, ping/pong/close 支持。
- Create: `core/websocket/session.h`
  - 连接 owner、发送串行化、读写推进、message view 投递。
- Create: `core/websocket/control_plane.h`
  - 心跳、阶段超时、退避重连、退化状态判断。
- Create: `core/websocket/client.h`
  - 对外粗粒度 client 接口和生命周期入口。
- Create: `core/websocket/metrics.h`
  - `L0/L1` 指标结构和预算化事件记录。

### Tests

- Create: `test/websocket/types_test.cpp`
- Create: `test/websocket/queue_test.cpp`
- Create: `test/websocket/state_machine_test.cpp`
- Create: `test/websocket/io_loop_test.cpp`
- Create: `test/websocket/handshake_test.cpp`
- Create: `test/websocket/frame_codec_test.cpp`
- Create: `test/websocket/session_test.cpp`
- Create: `test/websocket/control_plane_test.cpp`
- Create: `test/websocket/tls_transport_test.cpp`

### Tools And Benchmarks

- Create: `tools/websocket_probe.cpp`
  - 本地或远端 WebSocket 连接 probe，用于非性能 smoke test。
- Create: `benchmark/websocket/queue_benchmark.cpp`
- Create: `benchmark/websocket/frame_codec_benchmark.cpp`
- Create: `benchmark/websocket/session_loop_benchmark.cpp`

## Task 1: Scaffold The Core Module And Public Types

**Files:**
- Modify: `CMakeLists.txt`
- Create: `core/CMakeLists.txt`
- Create: `core/websocket/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Create: `test/websocket/CMakeLists.txt`
- Create: `core/websocket/types.h`
- Create: `core/websocket/message_view.h`
- Create: `test/websocket/types_test.cpp`

- [ ] **Step 1: Write the failing type smoke test**

```cpp
#include "core/websocket/types.h"
#include "core/websocket/message_view.h"

int main() {
    using namespace aquila::websocket;
    static_assert(static_cast<int>(ConnectionPhase::Disconnected) == 0);
    MessageView view{};
    return view.payload.data() == nullptr ? 0 : 1;
}
```

- [ ] **Step 2: Run build to verify it fails because the module is missing**

Run: `./build.sh debug`
Expected: CMake or compile failure because `core/websocket` targets and headers do not exist yet.

- [ ] **Step 3: Add the module scaffold and the minimal public types**

```cmake
# CMakeLists.txt
if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory(core)
    add_subdirectory(tools)
    add_subdirectory(test)
    add_subdirectory(benchmark)
endif ()
```

```cpp
// core/websocket/types.h
namespace aquila::websocket {
enum class ConnectionState { Disconnected, Connecting, Active, Closing };
enum class ConnectionPhase {
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
enum class PayloadKind { Text, Binary, Ping, Pong, Close };
struct ConnectionConfig {
    std::string host;
    std::string service;
    std::string target;
    bool enable_tls{false};
    size_t inbound_queue_capacity{1024};
    size_t outbound_queue_capacity{1024};
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
  core/websocket/types.h core/websocket/message_view.h \
  test/websocket/types_test.cpp benchmark/CMakeLists.txt
git commit -m "core: scaffold websocket module"
```

## Task 2: Implement The Bounded Queue And Fixed Buffer Pool

**Files:**
- Create: `core/websocket/bounded_spsc_queue.h`
- Create: `core/websocket/fixed_buffer_pool.h`
- Create: `test/websocket/queue_test.cpp`

- [ ] **Step 1: Write failing tests for queue full/empty and pool exhaustion**

```cpp
int main() {
    using namespace aquila::websocket;
    BoundedSpscQueue<int> queue(2);
    if (!queue.try_push(1)) return 1;
    if (!queue.try_push(2)) return 1;
    if (queue.try_push(3)) return 1;

    int value = 0;
    if (!queue.try_pop(value) || value != 1) return 1;
    if (!queue.try_pop(value) || value != 2) return 1;
    if (queue.try_pop(value)) return 1;

    FixedBufferPool pool(64, 2);
    auto a = pool.try_acquire();
    auto b = pool.try_acquire();
    auto c = pool.try_acquire();
    return (a && b && !c) ? 0 : 1;
}
```

- [ ] **Step 2: Run the targeted test and verify build/test fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_queue_test -V`
Expected: compile failure because queue/pool headers do not exist.

- [ ] **Step 3: Implement fixed-capacity queue and buffer pool with fail-fast semantics**

```cpp
template <typename T>
class BoundedSpscQueue {
  public:
    explicit BoundedSpscQueue(size_t capacity);
    bool try_push(const T& value) noexcept;
    bool try_pop(T& value) noexcept;
    size_t size() const noexcept;
    size_t capacity() const noexcept;
};
```

```cpp
class FixedBufferPool {
  public:
    struct Buffer {
        std::span<std::byte> bytes;
        size_t slot{0};
    };
    FixedBufferPool(size_t buffer_size, size_t buffer_count);
    std::optional<Buffer> try_acquire() noexcept;
    void release(Buffer buffer) noexcept;
};
```

- [ ] **Step 4: Run tests to verify queue and pool behavior**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_queue_test -V`
Expected: PASS with no blocking behavior and no implicit resize.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/bounded_spsc_queue.h core/websocket/fixed_buffer_pool.h \
  test/websocket/queue_test.cpp
git commit -m "core: add websocket queue and buffer pool primitives"
```

## Task 3: Implement The Fine-Grained State Machine

**Files:**
- Create: `core/websocket/state_machine.h`
- Create: `test/websocket/state_machine_test.cpp`

- [ ] **Step 1: Write failing tests for valid and invalid phase transitions**

```cpp
int main() {
    using namespace aquila::websocket;
    StateMachine sm;
    if (!sm.transition_to(ConnectionPhase::Resolving)) return 1;
    if (!sm.transition_to(ConnectionPhase::TCPConnecting)) return 1;
    if (sm.transition_to(ConnectionPhase::Authenticating)) return 1;
    return sm.phase() == ConnectionPhase::TCPConnecting ? 0 : 1;
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_state_machine_test -V`
Expected: compile failure because the state machine is missing.

- [ ] **Step 3: Implement phase transition rules and error reporting**

```cpp
class StateMachine {
  public:
    ConnectionPhase phase() const noexcept;
    bool transition_to(ConnectionPhase next) noexcept;
    std::optional<ConnectionError> last_error() const noexcept;
};
```

- [ ] **Step 4: Run the state-machine test**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_state_machine_test -V`
Expected: PASS for valid path and explicit rejection of invalid jumps.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/state_machine.h test/websocket/state_machine_test.cpp
git commit -m "core: add websocket state machine"
```

## Task 4: Implement The Owner I/O Loop

**Files:**
- Create: `core/websocket/io_loop.h`
- Create: `test/websocket/io_loop_test.cpp`

- [ ] **Step 1: Write a failing test for owner-thread task execution**

```cpp
int main() {
    using namespace aquila::websocket;
    IoLoop loop;
    std::atomic<int> value{0};
    loop.post([&value] { value.store(7); });
    loop.run_once(std::chrono::milliseconds(10));
    return value.load() == 7 ? 0 : 1;
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_io_loop_test -V`
Expected: compile failure because `IoLoop` does not exist.

- [ ] **Step 3: Implement `epoll` owner loop with `eventfd` wakeup and `timerfd` support**

```cpp
class IoLoop {
  public:
    using Task = std::function<void()>;
    IoLoop();
    ~IoLoop();
    void post(Task task);
    void run_once(std::chrono::milliseconds timeout);
    int fd() const noexcept;
};
```

- [ ] **Step 4: Run the test and verify owner-thread execution**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_io_loop_test -V`
Expected: PASS with tasks executed only when the owner loop drains its wakeup queue.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/io_loop.h test/websocket/io_loop_test.cpp
git commit -m "core: add websocket owner io loop"
```

## Task 5: Implement Plain TCP Transport And Serialized Writes

**Files:**
- Create: `core/websocket/transport.h`
- Create: `core/websocket/tcp_transport.h`
- Create: `core/websocket/session.h`
- Create: `test/websocket/session_test.cpp`

- [ ] **Step 1: Write a failing test for strict single-write-chain behavior**

```cpp
int main() {
    using namespace aquila::websocket;
    SessionHarness harness;
    harness.enqueue_outbound("one");
    harness.enqueue_outbound("two");
    harness.drive_writable_once();
    if (harness.completed_writes() != 1) return 1;
    harness.drive_writable_once();
    return harness.completed_writes() == 2 ? 0 : 1;
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_session_test -V`
Expected: compile failure because transport/session types do not exist.

- [ ] **Step 3: Implement non-blocking TCP transport and session write serialization**

```cpp
class Transport {
  public:
    virtual ~Transport() = default;
    virtual bool start_connect() = 0;
    virtual ssize_t read_some(std::span<std::byte>) = 0;
    virtual ssize_t write_some(std::span<const std::byte>) = 0;
    virtual void close() noexcept = 0;
};

class Session {
  public:
    bool try_send(MessageView message) noexcept;
    void on_readable();
    void on_writable();
};
```

- [ ] **Step 4: Run the session test**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_session_test -V`
Expected: PASS with one active write chain and queue-full fail-fast semantics.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/transport.h core/websocket/tcp_transport.h \
  core/websocket/session.h test/websocket/session_test.cpp
git commit -m "core: add websocket tcp transport and serialized session writes"
```

## Task 6: Implement Handshake And Frame Codec

**Files:**
- Create: `core/websocket/handshake.h`
- Create: `core/websocket/frame_codec.h`
- Create: `test/websocket/handshake_test.cpp`
- Create: `test/websocket/frame_codec_test.cpp`

- [ ] **Step 1: Write failing tests for RFC-compatible handshake and frame parsing**

```cpp
int main() {
    using namespace aquila::websocket;
    auto req = build_client_handshake("example.com", "/feed", "test-key");
    if (req.find("Upgrade: websocket") == std::string::npos) return 1;
    FrameCodec codec;
    auto encoded = codec.encode_text(std::as_bytes(std::span{"ok", 2}));
    auto decoded = codec.decode(encoded);
    return decoded && decoded->kind == PayloadKind::Text ? 0 : 1;
}
```

- [ ] **Step 2: Run the handshake/frame tests and verify they fail**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(handshake|frame_codec)_test" -V`
Expected: compile failure because handshake and codec are missing.

- [ ] **Step 3: Implement handshake builder/validator and frame encode/decode**

```cpp
std::string build_client_handshake(std::string_view host,
                                   std::string_view target,
                                   std::string_view client_key);
bool validate_server_handshake(std::string_view response,
                               std::string_view client_key);

class FrameCodec {
  public:
    std::vector<std::byte> encode_text(std::span<const std::byte> payload);
    std::vector<std::byte> encode_binary(std::span<const std::byte> payload);
    std::optional<MessageView> decode(std::span<const std::byte> bytes);
};
```

- [ ] **Step 4: Run handshake/frame tests**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(handshake|frame_codec)_test" -V`
Expected: PASS for text/binary/ping/pong/close control flow and handshake validation.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/handshake.h core/websocket/frame_codec.h \
  test/websocket/handshake_test.cpp test/websocket/frame_codec_test.cpp
git commit -m "core: add websocket handshake and frame codec"
```

## Task 7: Integrate Control Plane, Metrics, And The Public Client API

**Files:**
- Create: `core/websocket/control_plane.h`
- Create: `core/websocket/client.h`
- Create: `core/websocket/metrics.h`
- Create: `test/websocket/control_plane_test.cpp`

- [ ] **Step 1: Write failing tests for heartbeat timeout, degraded state, and reconnect backoff**

```cpp
int main() {
    using namespace aquila::websocket;
    FakeClock clock;
    ControlPlane control(clock);
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
Expected: compile failure because the control plane is missing.

- [ ] **Step 3: Implement control-plane decisions, `L0/L1` metrics, and the coarse client API**

```cpp
class ControlPlane {
  public:
    void on_phase(ConnectionPhase phase) noexcept;
    void on_heartbeat_timeout() noexcept;
    void on_disconnect(ConnectionError error) noexcept;
    bool should_enter_degraded() const noexcept;
    std::chrono::milliseconds next_retry_delay() const noexcept;
};

class Client {
  public:
    bool start(const ConnectionConfig& config);
    void close();
    ConnectionState state() const noexcept;
};
```

- [ ] **Step 4: Run the control-plane test**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_control_plane_test -V`
Expected: PASS with distinct handling for degraded state, heartbeat timeout, and reconnect backoff.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/control_plane.h core/websocket/client.h \
  core/websocket/metrics.h test/websocket/control_plane_test.cpp
git commit -m "core: add websocket control plane and client api"
```

## Task 8: Add TLS Transport And Probe Tool

**Files:**
- Create: `core/websocket/tls_transport.h`
- Create: `test/websocket/tls_transport_test.cpp`
- Create: `tools/websocket_probe.cpp`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: Write failing TLS smoke and probe tests**

```cpp
int main() {
    using namespace aquila::websocket;
    TlsTransport transport;
    return transport.ssl_handle() != nullptr ? 0 : 1;
}
```

- [ ] **Step 2: Run the TLS test and verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_tls_transport_test -V`
Expected: compile failure because the TLS transport is missing.

- [ ] **Step 3: Implement OpenSSL transport adapter and the minimal probe tool**

```cpp
class TlsTransport final : public Transport {
  public:
    TlsTransport();
    SSL* ssl_handle() noexcept;
    bool start_connect() override;
    ssize_t read_some(std::span<std::byte>) override;
    ssize_t write_some(std::span<const std::byte>) override;
    void close() noexcept override;
};
```

```bash
build/debug/tools/websocket_probe --host 127.0.0.1 --port 8443 --target /echo --tls
```

- [ ] **Step 4: Run TLS smoke and probe validation**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_tls_transport_test -V`
Expected: PASS for local TLS harness and successful probe binary startup.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/tls_transport.h test/websocket/tls_transport_test.cpp \
  tools/websocket_probe.cpp tools/CMakeLists.txt
git commit -m "core: add websocket tls transport and probe tool"
```

## Task 9: Add Benchmarks And Final Verification

**Files:**
- Modify: `benchmark/CMakeLists.txt`
- Create: `benchmark/websocket/CMakeLists.txt`
- Create: `benchmark/websocket/queue_benchmark.cpp`
- Create: `benchmark/websocket/frame_codec_benchmark.cpp`
- Create: `benchmark/websocket/session_loop_benchmark.cpp`

- [ ] **Step 1: Write benchmark harness skeletons**

```cpp
int main() {
    // measure queue push/pop latency and print p50/p99/p99.9
}
```

- [ ] **Step 2: Build once to verify benchmark targets are wired**

Run: `./build.sh release`
Expected: benchmark target build fails initially because source files are not implemented.

- [ ] **Step 3: Implement queue/frame/session-loop benchmarks and connect them to CMake**

```text
queue_benchmark.cpp: measure SPSC push/pop latency under fixed capacity
frame_codec_benchmark.cpp: measure encode/decode latency for text/binary frames
session_loop_benchmark.cpp: measure single-owner loop wakeup and outbound drain latency
```

- [ ] **Step 4: Run final verification**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug --output-on-failure
./build.sh release
```

Expected:

```text
All websocket tests pass in Debug
Release build succeeds
benchmark binaries are produced
```

- [ ] **Step 5: Commit**

```bash
git add benchmark/CMakeLists.txt benchmark/websocket/CMakeLists.txt \
  benchmark/websocket/queue_benchmark.cpp \
  benchmark/websocket/frame_codec_benchmark.cpp \
  benchmark/websocket/session_loop_benchmark.cpp
git commit -m "benchmark: add websocket latency benchmarks"
```

## Spec Coverage Check

- 单连接唯一 owner：Task 4, Task 5
- 收发零额外线程切换：Task 4, Task 5, Task 7
- 解析路径避免字符串分配：Task 1, Task 6
- 发送严格串行且可预测：Task 5
- 心跳/重连不污染主路径：Task 7
- `ws/wss` transport-agnostic：Task 5, Task 8
- 严格有界队列 + fail-fast：Task 2, Task 5
- 细粒度状态机与可观测性：Task 3, Task 7
- benchmark 与长稳验证入口：Task 8, Task 9

## Placeholder Scan

- 本计划不使用 `TODO/TBD/implement later`。
- 每个任务都给出精确文件路径、最小 API 轮廓、构建命令和期望结果。
- 若执行过程中需要调整 API 名称，应先统一计划中的文件和测试，再进入实现。

## Execution Handoff

Plan complete and saved to `doc/superpowers/plans/2026-04-22-websocket-client-implementation.md`.

Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

Which approach?
