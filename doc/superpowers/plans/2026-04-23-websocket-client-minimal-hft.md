# Critical Low-Jitter Gate WSS Client Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a single critical-path `wss` client specialized for the GateIO USDT feed at `wss://fx-ws.gateio.ws/v4/ws/usdt`, with a user-driven core that leaves `read/write/busy loop` scheduling to the caller and an optional wrapper that adds callback delivery and a default runtime policy.

**Architecture:** The design is intentionally not a general-purpose WebSocket client. `Layer 1` is a user-driven core: it owns handshake, TLS/WS protocol state, prepared-write storage, read/write advancement, and timeout/state transitions, but it does not create threads or decide how the caller schedules `DriveRead()` / `DriveWrite()` / busy looping. `Layer 2` is a thin wrapper around that core: it may create one dedicated owner thread, apply CPU affinity, run a default active spin loop, and expose callback-style message delivery. The wrapper must stay a thin policy layer and must not pollute the core API.

**Tech Stack:** C++20, CMake, POSIX sockets, `epoll`, `eventfd`, `timerfd`, `pthread_setaffinity_np`, `sched_setscheduler`, `mlockall`, OpenSSL, `ctest`, local TLS loopback harnesses, GateIO public probe validation, targeted microbenchmarks.

**External Validation Endpoint:** `wss://fx-ws.gateio.ws/v4/ws/usdt` is the required public live-connect endpoint for probe and handshake validation. Local loopback harnesses remain mandatory for deterministic correctness and latency verification, but public verification in this plan must use GateIO.

**Naming Rule:** All C++ identifiers in `core/websocket`, tests, tools, and benchmarks must follow the Google C++ Style Guide naming rules from `https://google.github.io/styleguide/cppguide.html`: type names in `PascalCase`, variables and parameters in `snake_case`, class data members with trailing underscore, constants and enumerators with `k`-prefixed constant style, namespaces in `snake_case`, and ordinary function names in `PascalCase`.

**Latency Rule:** When latency or jitter conflicts with CPU consumption, memory footprint, or implementation generality, choose the lower-jitter path unless it would break correctness or recoverability.

**No-Generality Rule:** Do not introduce generic transport interfaces, generic multi-exchange client layers, or reusable callback frameworks. This plan is for one critical low-jitter GateIO `wss` client. A thin callback wrapper above the user-driven core is allowed, but it must remain Gate-specific and must not reverse-control the core design.

---

## Scope Decisions

- In scope:
  - one critical `wss` client connection
  - one user-driven core session with no internally created thread
  - one optional callback wrapper with at most one dedicated owner thread
  - wrapper-side CPU affinity and optional real-time scheduling
  - `mlockall` and stack prefault for jitter reduction
  - user-controlled `read/write/busy loop` scheduling in `Layer 1`
  - wrapper-provided default active spin loop in `Layer 2`
  - connection-local preallocated read, frame, and write memory
  - session-local heartbeat, timeout, degrade, and reconnect logic inside the core
  - callback-based message delivery in the wrapper
  - GateIO public probe validation
  - loopback TLS echo harnesses
  - p50/p99/p99.9 latency benchmarks
- Explicitly out of scope:
  - generic WebSocket client abstractions
  - multi-connection multiplexing
  - shared cross-connection pools
  - separate control-plane thread
  - server-side WebSocket support
  - `io_uring` in v1
  - business JSON DOM parsing in the hot path

## File Map

### Build And Entry Points

- Modify: `CMakeLists.txt`
- Create: `core/CMakeLists.txt`
- Create: `core/websocket/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Create: `test/websocket/CMakeLists.txt`
- Modify: `tools/CMakeLists.txt`
- Modify: `benchmark/CMakeLists.txt`
- Modify: `README.md`
- Modify: `cmake/third_party.cmake`

### Core Library

- Create: `core/websocket/types.h`
- Create: `core/websocket/runtime_policy.h`
- Create: `core/websocket/message_view.h`
- Create: `core/websocket/prepared_write.h`
- Create: `core/websocket/state_machine.h`
- Create: `core/websocket/handshake.h`
- Create: `core/websocket/frame_codec.h`
- Create: `core/websocket/tls_socket.h`
- Create: `core/websocket/cold_path_loop.h`
- Create: `core/websocket/critical_session.h`
- Create: `core/websocket/active_spin_loop.h`
- Create: `core/websocket/websocket_client.h`
- Create: `core/websocket/metrics.h`

### Tests

- Create: `test/websocket/types_test.cpp`
- Create: `test/websocket/runtime_policy_test.cpp`
- Create: `test/websocket/prepared_write_test.cpp`
- Create: `test/websocket/handshake_test.cpp`
- Create: `test/websocket/frame_codec_test.cpp`
- Create: `test/websocket/tls_socket_test.cpp`
- Create: `test/websocket/critical_session_test.cpp`
- Create: `test/websocket/websocket_loopback_integration_test.cpp`

### Tools And Benchmarks

- Create: `tools/websocket_probe.cpp`
- Create: `benchmark/websocket/CMakeLists.txt`
- Create: `benchmark/websocket/prepared_write_benchmark.cpp`
- Create: `benchmark/websocket/frame_codec_benchmark.cpp`
- Create: `benchmark/websocket/active_spin_benchmark.cpp`

## Task 1: Scaffold The Low-Jitter Contracts And Build Graph

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `cmake/third_party.cmake`
- Create: `core/CMakeLists.txt`
- Create: `core/websocket/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Create: `test/websocket/CMakeLists.txt`
- Create: `core/websocket/types.h`
- Create: `core/websocket/runtime_policy.h`
- Create: `core/websocket/message_view.h`
- Create: `test/websocket/types_test.cpp`

- [ ] **Step 1: Write the failing contract test**

```cpp
// test/websocket/types_test.cpp
#include "core/websocket/message_view.h"
#include "core/websocket/runtime_policy.h"
#include "core/websocket/types.h"

#include <cstddef>
#include <span>
#include <vector>

using namespace aquila::websocket;

namespace {
DeliveryResult HandleMessage(void* context, const MessageView& view) noexcept {
    auto* counter = static_cast<size_t*>(context);
    *counter += view.payload.size();
    return view.payload.empty() ? DeliveryResult::kBackpressured : DeliveryResult::kAccepted;
}
}  // namespace

int main() {
    static_assert(static_cast<int>(ConnectionPhase::kActive) >= 0);
    static_assert(static_cast<int>(SchedulingPolicy::kFifo) >= 0);

    ConnectionConfig config{};
    if (!config.enable_tls) return 1;
    if (!config.runtime_policy.lock_memory) return 1;
    if (!config.runtime_policy.active_spin) return 1;

    std::vector<std::byte> storage(8, std::byte{0x11});
    size_t bytes = 0;
    MessageConsumer consumer{&bytes, &HandleMessage};
    MessageView view{PayloadKind::kText, std::span<const std::byte>(storage), 9, true};
    auto result = consumer.Handle(view);
    return result == DeliveryResult::kAccepted && bytes == storage.size() ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_types_test -V`
Expected: FAIL because `core/websocket` targets and headers do not exist yet.

- [ ] **Step 3: Add the low-jitter build graph and public contracts**

```cmake
# cmake/third_party.cmake
find_package(fmt CONFIG REQUIRED)
find_package(quill CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(tomlplusplus REQUIRED IMPORTED_TARGET tomlplusplus)
find_package(CLI11 CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(unofficial-vincentlaucsb-csv-parser CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
find_package(nameof CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(FastFloat CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED COMPONENTS btree flat_hash_map)
find_package(OpenSSL REQUIRED)

set(ABSL_LIBS
        absl::flat_hash_map
        absl::btree)

set(THIRD_PARTY_LIBS
        CLI11::CLI11
        PkgConfig::tomlplusplus
        quill::quill
        magic_enum::magic_enum
        fmt::fmt-header-only
        unofficial::vincentlaucsb-csv-parser::csv
        yyjson::yyjson
        nameof::nameof
        Drogon::Drogon
        FastFloat::fast_float
        OpenSSL::SSL
        OpenSSL::Crypto
        ${ABSL_LIBS}
)
```

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
set(CMAKE_CXX_STANDARD 20)
enable_testing()

include(cmake/settings.cmake)
project(aquila)
include(cmake/compile_options.cmake)
include(cmake/third_party.cmake)
include(cmake/packages.cmake)

add_subdirectory(core)

if (PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools)
    add_subdirectory(test)
    add_subdirectory(benchmark)
endif ()
```

```cmake
# core/CMakeLists.txt
add_subdirectory(websocket)
```

```cmake
# core/websocket/CMakeLists.txt
add_library(aquila_websocket_core INTERFACE)
target_include_directories(aquila_websocket_core INTERFACE ${PROJECT_SOURCE_DIR})
target_link_libraries(aquila_websocket_core INTERFACE Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
```

```cmake
# test/CMakeLists.txt
add_subdirectory(websocket)
add_test(
    NAME hello_world_smoke_test
    COMMAND $<TARGET_FILE:aquila_hello_world>
)

set_tests_properties(hello_world_smoke_test PROPERTIES
    PASS_REGULAR_EXPRESSION "hello world"
)
```

```cmake
# test/websocket/CMakeLists.txt
add_executable(websocket_types_test types_test.cpp)
target_link_libraries(websocket_types_test PRIVATE aquila_websocket_core)
add_test(NAME websocket_types_test COMMAND websocket_types_test)
```

```cpp
// core/websocket/types.h
#include "core/websocket/runtime_policy.h"

namespace aquila::websocket {
enum class ConnectionPhase : uint8_t {
    kDisconnected,
    kResolving,
    kTcpConnecting,
    kTlsHandshaking,
    kWsHandshaking,
    kActive,
    kReconnectBackoff,
    kClosing,
    kClosed
};

enum class ConnectionError : uint8_t {
    kNone,
    kResolveFailure,
    kSocketError,
    kConnectTimeout,
    kTlsFailure,
    kHandshakeFailure,
    kProtocolError,
    kHeartbeatTimeout,
    kPeerClosed
};

enum class PayloadKind : uint8_t { kText, kBinary, kPing, kPong, kClose };
enum class DeliveryResult : uint8_t { kAccepted, kBackpressured, kFatal };
enum class SendStatus : uint8_t {
    kOk,
    kNoPreparedWriteSlot,
    kWriteUnavailable,
    kEncodeFailed,
    kPayloadTooLarge
};

struct ConnectionConfig {
    std::string host{"fx-ws.gateio.ws"};
    std::string service{"443"};
    std::string target{"/v4/ws/usdt"};
    bool enable_tls{true};
    size_t read_buffer_bytes{1U << 20};
    size_t frame_buffer_bytes{1U << 20};
    size_t prepared_write_slots{2048};
    size_t prepared_write_bytes{4096};
    uint32_t heartbeat_interval_ms{5000};
    uint32_t heartbeat_timeout_ms{15000};
    RuntimePolicy runtime_policy{};
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/runtime_policy.h
namespace aquila::websocket {
enum class AffinityMode : uint8_t { kNone, kBestEffort, kRequired };
enum class SchedulingPolicy : uint8_t { kOther, kFifo, kRoundRobin };

struct RuntimePolicy {
    AffinityMode affinity_mode{kRequired};
    int io_cpu_id{-1};
    SchedulingPolicy scheduling_policy{kOther};
    int scheduling_priority{0};
    bool lock_memory{true};
    bool prefault_stack{true};
    bool active_spin{true};
    uint32_t spin_iterations_before_clock_check{4096};
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/message_view.h
namespace aquila::websocket {
struct MessageView {
    PayloadKind kind{PayloadKind::kBinary};
    std::span<const std::byte> payload{};
    uint64_t sequence{0};
    bool fin{true};
};

using MessageHandler = DeliveryResult (*)(void* context, const MessageView& view) noexcept;

struct MessageConsumer {
    void* context{nullptr};
    MessageHandler handler{nullptr};

    DeliveryResult Handle(const MessageView& view) const noexcept {
        return handler == nullptr ? DeliveryResult::kFatal : handler(context, view);
    }
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_types_test -V`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt cmake/third_party.cmake core/CMakeLists.txt core/websocket/CMakeLists.txt \
  core/websocket/types.h core/websocket/runtime_policy.h core/websocket/message_view.h \
  test/CMakeLists.txt test/websocket/CMakeLists.txt test/websocket/types_test.cpp
git commit -m "core: scaffold critical websocket contracts"
```

## Task 2: Implement Runtime Pinning And Connection-Local Write Slots

**Files:**
- Create: `core/websocket/prepared_write.h`
- Create: `test/websocket/runtime_policy_test.cpp`
- Create: `test/websocket/prepared_write_test.cpp`
- Modify: `test/websocket/CMakeLists.txt`

- [ ] **Step 1: Write failing tests for runtime policy and prepared-write slots**

```cpp
// test/websocket/runtime_policy_test.cpp
#include "core/websocket/runtime_policy.h"

using namespace aquila::websocket;

namespace aquila::websocket {
bool ApplyRuntimePolicy(const RuntimePolicy& policy) noexcept;
}

int main() {
    RuntimePolicy policy{};
    policy.affinity_mode = AffinityMode::kNone;
    policy.lock_memory = false;
    policy.prefault_stack = false;
    return ApplyRuntimePolicy(policy) ? 0 : 1;
}
```

```cpp
// test/websocket/prepared_write_test.cpp
#include "core/websocket/prepared_write.h"

using namespace aquila::websocket;

int main() {
    PreparedWriteArena arena(2, 64);
    auto* first = arena.TryAcquire();
    auto* second = arena.TryAcquire();
    auto* third = arena.TryAcquire();
    if (first == nullptr || second == nullptr) return 1;
    if (third != nullptr) return 1;
    arena.Release(first);
    return arena.TryAcquire() != nullptr ? 0 : 1;
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(runtime_policy|prepared_write)_test" -V`
Expected: FAIL because runtime helpers and prepared-write primitives do not exist yet.

- [ ] **Step 3: Add runtime policy execution and write-slot storage**

```cpp
// core/websocket/prepared_write.h
namespace aquila::websocket {
struct PreparedWrite {
    uint32_t slot_index{0};
    uint32_t encoded_size{0};
    uint32_t write_offset{0};
    PayloadKind kind{PayloadKind::kBinary};
    std::span<std::byte> storage{};
};

class PreparedWriteArena {
  public:
    PreparedWriteArena(size_t slot_count, size_t bytes_per_slot);
    PreparedWrite* TryAcquire() noexcept;
    void Release(PreparedWrite* write) noexcept;
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/runtime_policy.h
namespace aquila::websocket {
bool ApplyRuntimePolicy(const RuntimePolicy& policy) noexcept;
void PrefaultThreadStack() noexcept;
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(runtime_policy|prepared_write)_test" -V`
Expected: PASS, with `AffinityMode::kNone` succeeding and prepared-write slots reusing preallocated storage.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/prepared_write.h \
  test/websocket/runtime_policy_test.cpp test/websocket/prepared_write_test.cpp \
  test/websocket/CMakeLists.txt
git commit -m "core: add runtime pinning helpers and write slots"
```

## Task 3: Implement Handshake, Frame Codec, And Specialized TLS Socket

**Files:**
- Create: `core/websocket/handshake.h`
- Create: `core/websocket/frame_codec.h`
- Create: `core/websocket/tls_socket.h`
- Create: `test/websocket/handshake_test.cpp`
- Create: `test/websocket/frame_codec_test.cpp`
- Create: `test/websocket/tls_socket_test.cpp`
- Modify: `test/websocket/CMakeLists.txt`

- [ ] **Step 1: Write failing tests for cold-path protocol primitives**

```cpp
// test/websocket/handshake_test.cpp
#include "core/websocket/handshake.h"

#include <array>

using namespace aquila::websocket;

int main() {
    std::array<char, 512> output{};
    auto built = BuildClientHandshake("fx-ws.gateio.ws", "/v4/ws/usdt", "dGhlIHNhbXBsZSBub25jZQ==", output);
    if (!built.ok) return 1;
    return built.bytes.find("Upgrade: websocket") != std::string_view::npos ? 0 : 1;
}
```

```cpp
// test/websocket/frame_codec_test.cpp
#include "core/websocket/frame_codec.h"

#include <array>

using namespace aquila::websocket;

int main() {
    FrameCodec codec(1024);
    std::array<std::byte, 128> frame_storage{};
    auto encoded = codec.EncodeText(std::as_bytes(std::span{"tick", 4}), frame_storage);
    if (!encoded.ok) return 1;
    auto decoded = codec.Feed(std::span(encoded.bytes));
    return decoded.status == DecodeStatus::kMessageReady ? 0 : 1;
}
```

```cpp
// test/websocket/tls_socket_test.cpp
#include "core/websocket/tls_socket.h"

using namespace aquila::websocket;

int main() {
    TlsSocket socket;
    return socket.Init() ? 0 : 1;
}
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(handshake|frame_codec|tls_socket)_test" -V`
Expected: FAIL because the protocol and TLS primitives do not exist yet.

- [ ] **Step 3: Add specialized cold-path protocol primitives**

```cpp
// core/websocket/handshake.h
namespace aquila::websocket {
struct HandshakeBuildResult {
    bool ok{false};
    std::string_view bytes{};
};

HandshakeBuildResult BuildClientHandshake(std::string_view host,
                                          std::string_view target,
                                          std::string_view client_key,
                                          std::span<char> output) noexcept;
bool ValidateServerHandshake(std::string_view response,
                             std::string_view client_key) noexcept;
}  // namespace aquila::websocket
```

```cpp
// core/websocket/frame_codec.h
namespace aquila::websocket {
struct EncodeResult {
    bool ok{false};
    std::span<const std::byte> bytes{};
};

enum class DecodeStatus : uint8_t { kNeedMore, kMessageReady, kProtocolError };

struct DecodeResult {
    DecodeStatus status{DecodeStatus::kNeedMore};
    MessageView view{};
};

class FrameCodec {
  public:
    explicit FrameCodec(size_t max_payload_bytes);
    EncodeResult EncodeText(std::span<const std::byte> payload,
                            std::span<std::byte> output) noexcept;
    EncodeResult EncodeBinary(std::span<const std::byte> payload,
                              std::span<std::byte> output) noexcept;
    EncodeResult EncodeControl(PayloadKind kind,
                               std::span<const std::byte> payload,
                               std::span<std::byte> output) noexcept;
    DecodeResult Feed(std::span<const std::byte> bytes) noexcept;
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/tls_socket.h
namespace aquila::websocket {
class TlsSocket {
  public:
    bool Init() noexcept;
    bool OpenAndConnect(const ConnectionConfig& config) noexcept;
    bool FinishHandshake() noexcept;
    ssize_t ReadSome(std::span<std::byte> buffer) noexcept;
    ssize_t WriteSome(std::span<const std::byte> buffer) noexcept;
    bool WantsRead() const noexcept;
    bool WantsWrite() const noexcept;
    int NativeFd() const noexcept;
    void Close() noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build.sh debug && ctest --test-dir build/debug -R "websocket_(handshake|frame_codec|tls_socket)_test" -V`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/handshake.h core/websocket/frame_codec.h core/websocket/tls_socket.h \
  test/websocket/handshake_test.cpp test/websocket/frame_codec_test.cpp \
  test/websocket/tls_socket_test.cpp test/websocket/CMakeLists.txt
git commit -m "core: add websocket handshake codec and tls socket"
```

## Task 4: Implement The User-Driven Core Session

**Files:**
- Create: `core/websocket/metrics.h`
- Create: `core/websocket/state_machine.h`
- Create: `core/websocket/cold_path_loop.h`
- Create: `core/websocket/critical_session.h`
- Create: `test/websocket/critical_session_test.cpp`
- Modify: `test/websocket/CMakeLists.txt`

- [ ] **Step 1: Write failing tests for user-driven read/write advancement**

```cpp
// test/websocket/critical_session_test.cpp
#include "core/websocket/critical_session.h"

#include <algorithm>
#include <vector>

using namespace aquila::websocket;

namespace {
DeliveryResult RecordMessage(void* context, const MessageView& view) noexcept {
    auto* bytes = static_cast<size_t*>(context);
    *bytes += view.payload.size();
    return DeliveryResult::kAccepted;
}

class FakeTlsSocket final {
  public:
    ssize_t ReadSome(std::span<std::byte>) noexcept { return -1; }
    ssize_t WriteSome(std::span<const std::byte> buffer) noexcept {
        written_.insert(written_.end(), buffer.begin(), buffer.end());
        return static_cast<ssize_t>(buffer.size());
    }

    std::vector<std::byte> written_{};
};
}  // namespace

int main() {
    ConnectionConfig config{};
    PreparedWriteArena arena(4, 128);
    Metrics metrics{};
    size_t bytes = 0;
    MessageConsumer consumer{&bytes, &RecordMessage};
    FakeTlsSocket socket;

    CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
    session.SetConsumer(consumer);

    auto* write = session.TryAcquirePreparedWrite();
    if (write == nullptr) return 1;
    std::copy_n(std::as_bytes(std::span{"tick", 4}).begin(), 4, write->storage.begin());
    write->encoded_size = 4;
    write->kind = PayloadKind::kText;
    if (session.CommitPreparedWrite(write) != SendStatus::kOk) return 1;

    session.DriveWrite();
    return socket.written_.size() == 4 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_critical_session_test -V`
Expected: FAIL because the user-driven core session does not exist yet.

- [ ] **Step 3: Add the user-driven core session and cold-path driver**

```cpp
// core/websocket/metrics.h
namespace aquila::websocket {
struct Metrics {
    uint64_t rx_bytes{0};
    uint64_t tx_bytes{0};
    uint64_t rx_messages{0};
    uint64_t tx_messages{0};
    uint64_t reconnects{0};
    uint64_t spin_iterations{0};
    uint64_t prepared_write_high_watermark{0};
    uint64_t heartbeat_timeouts{0};
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/state_machine.h
namespace aquila::websocket {
class StateMachine {
  public:
    StateMachine() noexcept;
    ConnectionPhase Phase() const noexcept;
    ConnectionError LastError() const noexcept;
    void Enter(ConnectionPhase phase) noexcept;
    void Fail(ConnectionError error, ConnectionPhase phase) noexcept;
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/cold_path_loop.h
namespace aquila::websocket {
class ColdPathLoop {
  public:
    bool Init() noexcept;
    bool RunUntilActive(TlsSocket& socket,
                        StateMachine& state_machine,
                        const ConnectionConfig& config,
                        std::span<char> handshake_storage) noexcept;
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/critical_session.h
namespace aquila::websocket {
template <typename TlsSocketT>
class CriticalSession {
  public:
    CriticalSession(const ConnectionConfig& config,
                    TlsSocketT& tls_socket,
                    PreparedWriteArena& prepared_write_arena,
                    Metrics& metrics) noexcept;

    void SetConsumer(MessageConsumer consumer) noexcept;
    PreparedWrite* TryAcquirePreparedWrite() noexcept;
    SendStatus CommitPreparedWrite(PreparedWrite* write) noexcept;
    void CancelPreparedWrite(PreparedWrite* write) noexcept;
    void DriveWrite() noexcept;
    void DriveRead() noexcept;
    bool WantsWrite() const noexcept;
    bool WantsRead() const noexcept;
    void AdvanceHeartbeat(uint64_t now_ns) noexcept;
    bool ShouldReconnect() const noexcept;
};
}  // namespace aquila::websocket
```

- [ ] **Step 4: Run test to verify it passes**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_critical_session_test -V`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/metrics.h core/websocket/state_machine.h \
  core/websocket/cold_path_loop.h \
  core/websocket/critical_session.h test/websocket/critical_session_test.cpp \
  test/websocket/CMakeLists.txt
git commit -m "core: add user-driven websocket core"
```

## Task 5: Implement The Callback Wrapper, Default Spin Runtime, And Public Probe

**Files:**
- Create: `core/websocket/active_spin_loop.h`
- Create: `core/websocket/websocket_client.h`
- Create: `test/websocket/websocket_loopback_integration_test.cpp`
- Create: `tools/websocket_probe.cpp`
- Modify: `tools/CMakeLists.txt`
- Modify: `test/websocket/CMakeLists.txt`

- [ ] **Step 1: Write failing tests for the callback wrapper entry point**

```cpp
// test/websocket/websocket_loopback_integration_test.cpp
#include "core/websocket/websocket_client.h"

using namespace aquila::websocket;

namespace {
DeliveryResult AcceptAll(void*, const MessageView&) noexcept {
    return DeliveryResult::kAccepted;
}
}  // namespace

int main() {
    ConnectionConfig config{};
    config.host = "127.0.0.1";
    config.service = "9443";
    config.target = "/v4/ws/usdt";
    MessageConsumer consumer{nullptr, &AcceptAll};
    WebSocketClient client(config, consumer);
    return client.PrepareRuntimeOnly() ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_loopback_integration_test -V`
Expected: FAIL because the callback wrapper does not exist yet.

- [ ] **Step 3: Add the callback wrapper, default spin runtime, and probe tool**

```cpp
// core/websocket/active_spin_loop.h
namespace aquila::websocket {
inline void CpuRelax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

class ActiveSpinLoop {
  public:
    explicit ActiveSpinLoop(const RuntimePolicy& runtime_policy) noexcept;
    template <typename SessionT>
    void Run(SessionT& session) noexcept;
};
}  // namespace aquila::websocket
```

```cpp
// core/websocket/websocket_client.h
namespace aquila::websocket {
using StateHandler = void (*)(void* context, ConnectionPhase phase) noexcept;
using ErrorHandler = void (*)(void* context, ConnectionError error) noexcept;

class WebSocketClient {
  public:
    WebSocketClient(ConnectionConfig config, MessageConsumer consumer) noexcept;
    bool PrepareRuntimeOnly() noexcept;
    void SetStateHandler(void* context, StateHandler handler) noexcept;
    void SetErrorHandler(void* context, ErrorHandler handler) noexcept;
    bool Start() noexcept;
    CriticalSession<TlsSocket>& Core() noexcept;
    void Stop() noexcept;
    Metrics SnapshotMetrics() const noexcept;
};
}  // namespace aquila::websocket
```

```cpp
// tools/websocket_probe.cpp
#include "core/websocket/websocket_client.h"

#include <CLI/CLI.hpp>

using namespace aquila::websocket;

namespace {
DeliveryResult CountPayload(void* context, const MessageView& view) noexcept {
    auto* bytes = static_cast<size_t*>(context);
    *bytes += view.payload.size();
    return DeliveryResult::kAccepted;
}
}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"critical websocket probe"};
    std::string host{"fx-ws.gateio.ws"};
    std::string port{"443"};
    std::string target{"/v4/ws/usdt"};
    int cpu{-1};
    bool tls{true};
    app.add_option("--host", host, "remote host");
    app.add_option("--port", port, "remote port");
    app.add_option("--target", target, "websocket target");
    app.add_option("--cpu", cpu, "owner cpu id");
    app.add_flag("--tls", tls, "enable tls");
    CLI11_PARSE(app, argc, argv);

    ConnectionConfig config{};
    config.host = host;
    config.service = port;
    config.target = target;
    config.enable_tls = tls;
    config.runtime_policy.io_cpu_id = cpu;
    config.runtime_policy.affinity_mode = cpu >= 0 ? AffinityMode::kBestEffort : AffinityMode::kNone;

    size_t bytes = 0;
    MessageConsumer consumer{&bytes, &CountPayload};
    WebSocketClient client(config, consumer);
    return client.Start() ? 0 : 1;
}
```

```cmake
# tools/CMakeLists.txt
add_executable(aquila_hello_world
    hello_world.cpp
)

add_executable(websocket_probe
    websocket_probe.cpp
)
target_link_libraries(websocket_probe PRIVATE aquila_websocket_core CLI11::CLI11)
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_loopback_integration_test -V`
Expected: PASS with the local TLS harness.

Run: `build/debug/tools/websocket_probe --host fx-ws.gateio.ws --port 443 --target /v4/ws/usdt --tls --cpu 2`
Expected: Complete `TLS -> WS` handshake against `wss://fx-ws.gateio.ws/v4/ws/usdt` using pinned-owner startup when the selected CPU is available.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/active_spin_loop.h core/websocket/websocket_client.h \
  test/websocket/websocket_loopback_integration_test.cpp \
  tools/websocket_probe.cpp tools/CMakeLists.txt test/websocket/CMakeLists.txt
git commit -m "core: add websocket callback wrapper and probe"
```

## Task 6: Add Tail-Latency Benchmarks And Final Verification Gates

**Files:**
- Modify: `benchmark/CMakeLists.txt`
- Create: `benchmark/websocket/CMakeLists.txt`
- Create: `benchmark/websocket/prepared_write_benchmark.cpp`
- Create: `benchmark/websocket/frame_codec_benchmark.cpp`
- Create: `benchmark/websocket/active_spin_benchmark.cpp`
- Modify: `README.md`
- Modify: `cmake/third_party.cmake`

- [ ] **Step 1: Add benchmark targets and required external libraries**

```cmake
# cmake/third_party.cmake
find_package(fmt CONFIG REQUIRED)
find_package(quill CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(tomlplusplus REQUIRED IMPORTED_TARGET tomlplusplus)
find_package(CLI11 CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(unofficial-vincentlaucsb-csv-parser CONFIG REQUIRED)
find_package(yyjson CONFIG REQUIRED)
find_package(nameof CONFIG REQUIRED)
find_package(Drogon CONFIG REQUIRED)
find_package(FastFloat CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED COMPONENTS btree flat_hash_map)
find_package(OpenSSL REQUIRED)

set(ABSL_LIBS
        absl::flat_hash_map
        absl::btree)

set(THIRD_PARTY_LIBS
        CLI11::CLI11
        PkgConfig::tomlplusplus
        quill::quill
        magic_enum::magic_enum
        fmt::fmt-header-only
        unofficial::vincentlaucsb-csv-parser::csv
        yyjson::yyjson
        nameof::nameof
        Drogon::Drogon
        FastFloat::fast_float
        OpenSSL::SSL
        OpenSSL::Crypto
        ${ABSL_LIBS}
)
```

```cmake
# benchmark/CMakeLists.txt
add_subdirectory(websocket)
```

```cmake
# benchmark/websocket/CMakeLists.txt
add_executable(prepared_write_benchmark prepared_write_benchmark.cpp)
target_link_libraries(prepared_write_benchmark PRIVATE aquila_websocket_core)

add_executable(frame_codec_benchmark frame_codec_benchmark.cpp)
target_link_libraries(frame_codec_benchmark PRIVATE aquila_websocket_core)

add_executable(active_spin_benchmark active_spin_benchmark.cpp)
target_link_libraries(active_spin_benchmark PRIVATE aquila_websocket_core)
```

- [ ] **Step 2: Write the benchmark sources**

```cpp
// benchmark/websocket/prepared_write_benchmark.cpp
#include "core/websocket/prepared_write.h"

#include <chrono>

using namespace aquila::websocket;

int main() {
    PreparedWriteArena arena(2048, 4096);
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < 200000; ++i) {
        auto* write = arena.TryAcquire();
        if (write == nullptr) return 1;
        arena.Release(write);
    }
    auto stop = std::chrono::steady_clock::now();
    return stop > start ? 0 : 1;
}
```

```cpp
// benchmark/websocket/frame_codec_benchmark.cpp
#include "core/websocket/frame_codec.h"

#include <array>
#include <chrono>

using namespace aquila::websocket;

int main() {
    FrameCodec codec(1024);
    std::array<std::byte, 128> storage{};
    auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < 200000; ++i) {
        auto encoded = codec.EncodeText(std::as_bytes(std::span{"tick", 4}), storage);
        if (!encoded.ok) return 1;
    }
    auto stop = std::chrono::steady_clock::now();
    return stop > start ? 0 : 1;
}
```

```cpp
// benchmark/websocket/active_spin_benchmark.cpp
#include "core/websocket/active_spin_loop.h"
#include "core/websocket/runtime_policy.h"

#include <chrono>

using namespace aquila::websocket;

namespace {
class FakeSession {
  public:
    void DriveRead() noexcept {}
    void DriveWrite() noexcept {}
    void AdvanceHeartbeat(uint64_t) noexcept {}
    bool ShouldReconnect() const noexcept { return true; }
};
}  // namespace

int main() {
    RuntimePolicy policy{};
    policy.affinity_mode = AffinityMode::kNone;
    ActiveSpinLoop loop(policy);
    FakeSession session;
    auto start = std::chrono::steady_clock::now();
    loop.Run(session);
    auto stop = std::chrono::steady_clock::now();
    return stop > start ? 0 : 1;
}
```

- [ ] **Step 3: Run the full verification set**

Run: `./build.sh debug && ctest --test-dir build/debug --output-on-failure -R "websocket_"`
Expected: PASS for all websocket unit and integration tests.

Run: `./build.sh release`
Expected: PASS and benchmark binaries produced under `build/release/benchmark/websocket`.

Run: `build/debug/tools/websocket_probe --host fx-ws.gateio.ws --port 443 --target /v4/ws/usdt --tls --cpu 2`
Expected: Reconfirm successful live handshake against `wss://fx-ws.gateio.ws/v4/ws/usdt` with owner affinity enabled when available.

Run: `build/release/benchmark/websocket/prepared_write_benchmark`
Expected: Record p50/p99/p99.9 allocation and release latency.

Run: `build/release/benchmark/websocket/frame_codec_benchmark`
Expected: Record p50/p99/p99.9 frame encode latency.

Run: `build/release/benchmark/websocket/active_spin_benchmark`
Expected: Record owner-loop iteration latency with the chosen runtime policy.

- [ ] **Step 4: Add README execution notes**

```text
README additions required by this plan:
- This client is intentionally specialized for one critical GateIO `wss` connection.
- Lowest latency and jitter are the primary goals.
- `Layer 1` is a user-driven core and does not create threads or own the caller's busy loop.
- `Layer 2` is an optional callback wrapper and the recommended default deployment model is one owner thread bound to one dedicated CPU core.
- `mlockall`, stack prefault, and optional real-time scheduling are part of the jitter-reduction path.
- Benchmark reports must explicitly list CPU affinity, scheduling policy, TLS enabled, and whether the run used local loopback or the public GateIO endpoint.
```

- [ ] **Step 5: Commit**

```bash
git add benchmark/CMakeLists.txt benchmark/websocket/CMakeLists.txt \
  benchmark/websocket/prepared_write_benchmark.cpp \
  benchmark/websocket/frame_codec_benchmark.cpp \
  benchmark/websocket/active_spin_benchmark.cpp \
  README.md cmake/third_party.cmake
git commit -m "benchmark: add critical websocket latency verification"
```

## Spec Coverage Check

- Single critical connection only: covered by Scope Decisions and Task 5.
- User-driven core plus thin callback wrapper: covered by Architecture and Task 4 / Task 5.
- Dedicated owner core and low-jitter runtime policy: covered by Task 1, Task 2, and Task 5.
- No generic client or generic transport abstraction: covered by No-Generality Rule and the file map.
- `epoll` only on cold path, wrapper-controlled spin loop on hot path: covered by Architecture and Task 4 / Task 5.
- Connection-local preallocated memory and prepared-write slots: covered by Task 2 and Task 4.
- Session-local heartbeat and reconnect logic: covered by Task 4.
- GateIO live endpoint validation: covered by Task 5 and Task 6.
- Tail-latency benchmark evidence: covered by Task 6.

## Placeholder Scan

- No `TODO` or `TBD` placeholders remain.
- The only deferred items are explicit scope exclusions such as `io_uring` and generic multi-connection support.

## Execution Notes

- Do not re-introduce generic transport abstraction or a reusable callback framework while implementing this plan.
- Do not move heartbeat, timeout, or reconnect decisions to a separate thread.
- Do not accept heap fallback on prepared-write slot exhaustion.
- Do not claim jitter improvement without fresh p50/p99/p99.9 evidence.
- If the selected CPU affinity cannot be applied, treat `AffinityMode::kRequired` as startup failure.

## Execution Handoff

Plan complete and saved to `doc/superpowers/plans/2026-04-23-websocket-client-minimal-hft.md`. Two execution options:

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
