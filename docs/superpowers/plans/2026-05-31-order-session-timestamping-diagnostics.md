# Order Session Timestamping Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 Gate order session RTT probe 增加 socket TX/RX timestamp 诊断，把 Ack outlier 拆成本机写路径、kernel/NIC 出站、TCP 往返、业务 Ack 入站和本机读取解析阶段。

**Architecture:** 先做旁路 pcap runbook，再在 `core/websocket` 增加可关闭的 Linux `SO_TIMESTAMPING` software timestamping；Gate order session 只在 probe/diagnostic 配置开启时采样并写入 diagnostic log 和 RTT probe CSV。Hardware timestamping 作为第二阶段，只在 software timestamping 指向本机 TX/RX 路径后启用。

**Tech Stack:** C++20、Linux socket timestamping、`sendmsg`/`recvmsg(MSG_ERRQUEUE)`、`SO_TIMESTAMPING`、`TCP_INFO`、CMake、gtest、现有 Gate RTT probe。

---

## Scope And Constraints

- 默认关闭 timestamping；生产默认行为和热路径不变。
- 第一版只要求支持 RTT probe 的 private non-TLS order session；TLS socket 可保留同一 fd 级 timestamping 接口，但不作为首轮验收目标。
- 只用本机同一时钟域字段做阶段归因：`request_send_local_ns`、`write_complete_ns`、`tx_*_ns`、`rx_*_ns`、`ack_receive_local_ns`。Gate `exchange_ns` 只能作为远端上下文，不作为确认本机/NIC 的硬边界。
- `SO_TIMESTAMPING` error queue 必须非阻塞 drain；不能在 Ack 热路径中做无限循环。
- 新增诊断字段必须同步更新 `docs/diagnostic_fields.md`。

## Stage Classification

首轮要产出这些同一时钟域字段：

```text
request_send_local_ns
write_complete_ns
tx_sched_ns
tx_software_ns
tx_ack_ns
rx_software_ns
ack_receive_local_ns
```

归因规则：

```text
write_complete_ns -> tx_sched_ns / tx_software_ns 大
  本机 kernel qdisc / driver / NIC 出站前排队嫌疑上升。

tx_software_ns -> tx_ack_ns 大
  request bytes 离开本机 kernel 后，到远端 TCP ACK 返回慢；偏 private link / 网络 / 远端 TCP 栈。

tx_ack_ns -> rx_software_ns 大
  远端 TCP 已确认收到 request bytes，但业务 Ack 包到达本机 kernel 慢；偏 Gate 应用处理、远端发送路径或回程网络。

rx_software_ns -> ack_receive_local_ns 大
  Ack 包已到本机 kernel，但用户态读取、frame parse 或调度慢。
```

## Files

- Create: `core/websocket/socket_timestamping.h`
- Modify: `core/websocket/types.h`
- Modify: `core/websocket/plain_socket.h`
- Modify: `core/websocket/tls_socket.h`
- Modify: `core/websocket/critical_session.h`
- Modify: `core/websocket/CMakeLists.txt`
- Modify: `exchange/gate/trading/order_latency_diagnostics.h`
- Modify: `exchange/gate/trading/order_session.h`
- Modify: `exchange/gate/trading/order_session_config.h`
- Modify: `exchange/gate/trading/order_session_config.cpp`
- Modify: `tools/gate/order_session_rtt_probe/config.h`
- Modify: `tools/gate/order_session_rtt_probe/config.cpp`
- Modify: `tools/gate/order_session_rtt_probe/session_config_builder.h`
- Modify: `tools/gate/order_session_rtt_probe/sample_flow.h`
- Modify: `tools/gate/order_session_rtt_probe/sample_csv_writer.h`
- Modify: `tools/gate/order_session_rtt_probe/sample_csv_writer.cpp`
- Modify: `config/order_session_rtt_probe/gate_order_session_rtt_probe.toml`
- Modify: `docs/gate_order_session_rtt_probe_design.md`
- Modify: `docs/diagnostic_fields.md`
- Test: `test/websocket/socket_timestamping_test.cpp`
- Test: `test/exchange/gate/trading/order_session_test.cpp`
- Test: `test/tools/gate/order_session_rtt_probe/order_session_rtt_probe_test.cpp`
- Modify: `test/websocket/CMakeLists.txt`

## Task 1: Add Timestamping Data Types

**Files:**
- Create: `core/websocket/socket_timestamping.h`
- Modify: `core/websocket/types.h`
- Modify: `core/websocket/CMakeLists.txt`
- Test: `test/websocket/socket_timestamping_test.cpp`
- Modify: `test/websocket/CMakeLists.txt`

- [ ] **Step 1: Add failing tests for disabled defaults and stage math**

Create `test/websocket/socket_timestamping_test.cpp`:

```cpp
#include "core/websocket/socket_timestamping.h"

#include <gtest/gtest.h>

namespace aquila::websocket {
namespace {

TEST(SocketTimestampingTest, DefaultConfigIsDisabled) {
  SocketTimestampingConfig config;

  EXPECT_FALSE(config.enabled);
  EXPECT_FALSE(config.tx_software);
  EXPECT_FALSE(config.tx_sched);
  EXPECT_FALSE(config.tx_ack);
  EXPECT_FALSE(config.rx_software);
}

TEST(SocketTimestampingTest, ComputesLocalStageDurations) {
  SocketTimestampingSnapshot snapshot;
  snapshot.write_complete_ns = 100;
  snapshot.tx_software_ns = 140;
  snapshot.tx_ack_ns = 250;
  snapshot.rx_software_ns = 400;
  snapshot.ack_receive_local_ns = 430;

  const SocketTimestampingStages stages =
      ComputeSocketTimestampingStages(snapshot);

  EXPECT_EQ(stages.write_complete_to_tx_software_ns, 40);
  EXPECT_EQ(stages.tx_software_to_tx_ack_ns, 110);
  EXPECT_EQ(stages.tx_ack_to_rx_software_ns, 150);
  EXPECT_EQ(stages.rx_software_to_ack_receive_ns, 30);
}

}  // namespace
}  // namespace aquila::websocket
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```bash
cmake --build build/debug --target websocket_socket_timestamping_test -j8
```

Expected: target or symbols do not exist.

- [ ] **Step 3: Add timestamping structs and pure stage computation**

Create `core/websocket/socket_timestamping.h` with:

```cpp
#ifndef AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_
#define AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_

#include <cstdint>

namespace aquila::websocket {

struct SocketTimestampingConfig {
  bool enabled{false};
  bool tx_sched{false};
  bool tx_software{false};
  bool tx_ack{false};
  bool rx_software{false};
  bool hardware{false};
  std::uint32_t max_errqueue_events_per_drain{16};
};

struct SocketTimestampingSnapshot {
  bool available{false};
  std::int64_t write_complete_ns{0};
  std::int64_t tx_sched_ns{0};
  std::int64_t tx_software_ns{0};
  std::int64_t tx_ack_ns{0};
  std::int64_t rx_software_ns{0};
  std::int64_t ack_receive_local_ns{0};
};

struct SocketTimestampingStages {
  std::int64_t write_complete_to_tx_software_ns{-1};
  std::int64_t tx_software_to_tx_ack_ns{-1};
  std::int64_t tx_ack_to_rx_software_ns{-1};
  std::int64_t rx_software_to_ack_receive_ns{-1};
};

[[nodiscard]] inline std::int64_t DiffOrMinusOne(std::int64_t end_ns,
                                                 std::int64_t begin_ns) {
  return end_ns > 0 && begin_ns > 0 && end_ns >= begin_ns ? end_ns - begin_ns
                                                          : -1;
}

[[nodiscard]] inline SocketTimestampingStages ComputeSocketTimestampingStages(
    const SocketTimestampingSnapshot& snapshot) {
  return SocketTimestampingStages{
      .write_complete_to_tx_software_ns =
          DiffOrMinusOne(snapshot.tx_software_ns, snapshot.write_complete_ns),
      .tx_software_to_tx_ack_ns =
          DiffOrMinusOne(snapshot.tx_ack_ns, snapshot.tx_software_ns),
      .tx_ack_to_rx_software_ns =
          DiffOrMinusOne(snapshot.rx_software_ns, snapshot.tx_ack_ns),
      .rx_software_to_ack_receive_ns =
          DiffOrMinusOne(snapshot.ack_receive_local_ns,
                         snapshot.rx_software_ns),
  };
}

}  // namespace aquila::websocket

#endif  // AQUILA_CORE_WEBSOCKET_SOCKET_TIMESTAMPING_H_
```

Modify `core/websocket/types.h`:

```cpp
#include "core/websocket/socket_timestamping.h"
```

and add to `ConnectionConfig`:

```cpp
SocketTimestampingConfig socket_timestamping{};
```

Modify `core/websocket/CMakeLists.txt` and `test/websocket/CMakeLists.txt` so the new header and test target are built.

- [ ] **Step 4: Run test**

Run:

```bash
cmake --build build/debug --target websocket_socket_timestamping_test -j8
ctest --test-dir build/debug -R websocket_socket_timestamping_test --output-on-failure
```

Expected: test passes.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/socket_timestamping.h core/websocket/types.h core/websocket/CMakeLists.txt test/websocket/socket_timestamping_test.cpp test/websocket/CMakeLists.txt
git commit -m "Add websocket socket timestamping types"
```

## Task 2: Apply Linux SO_TIMESTAMPING To PlainSocket

**Files:**
- Modify: `core/websocket/socket_timestamping.h`
- Modify: `core/websocket/plain_socket.h`
- Test: `test/websocket/socket_timestamping_test.cpp`

- [ ] **Step 1: Add tests for unsupported-safe apply behavior**

Extend `test/websocket/socket_timestamping_test.cpp`:

```cpp
#include <sys/socket.h>
#include <unistd.h>

TEST(SocketTimestampingTest, ApplyDisabledConfigSucceedsOnInvalidFd) {
  SocketTimestampingConfig config;

  SocketTimestampingApplyResult result =
      ApplySocketTimestampingConfig(-1, config);

  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.enabled);
}

TEST(SocketTimestampingTest, ApplyEnabledConfigReportsFailureOnInvalidFd) {
  SocketTimestampingConfig config;
  config.enabled = true;
  config.tx_software = true;
  config.rx_software = true;

  SocketTimestampingApplyResult result =
      ApplySocketTimestampingConfig(-1, config);

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error_errno, 0);
}
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```bash
cmake --build build/debug --target websocket_socket_timestamping_test -j8
```

Expected: `SocketTimestampingApplyResult` is not defined.

- [ ] **Step 3: Implement Linux setsockopt helper**

Add to `core/websocket/socket_timestamping.h`:

```cpp
#include <cerrno>
#include <sys/socket.h>

#if defined(__linux__)
#include <linux/net_tstamp.h>
#endif

struct SocketTimestampingApplyResult {
  bool ok{false};
  bool enabled{false};
  int error_errno{0};
};

[[nodiscard]] inline SocketTimestampingApplyResult
ApplySocketTimestampingConfig(int fd,
                              const SocketTimestampingConfig& config) noexcept {
  if (!config.enabled) {
    return SocketTimestampingApplyResult{.ok = true, .enabled = false};
  }
#if defined(__linux__)
  if (fd < 0) {
    return SocketTimestampingApplyResult{.ok = false,
                                         .enabled = false,
                                         .error_errno = EBADF};
  }
  int flags = SOF_TIMESTAMPING_OPT_TSONLY | SOF_TIMESTAMPING_OPT_ID |
              SOF_TIMESTAMPING_OPT_ID_TCP;
  if (config.tx_sched) {
    flags |= SOF_TIMESTAMPING_TX_SCHED;
  }
  if (config.tx_software) {
    flags |= SOF_TIMESTAMPING_TX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  }
  if (config.tx_ack) {
    flags |= SOF_TIMESTAMPING_TX_ACK | SOF_TIMESTAMPING_SOFTWARE;
  }
  if (config.rx_software) {
    flags |= SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
  }
  if (config.hardware) {
    flags |= SOF_TIMESTAMPING_TX_HARDWARE |
             SOF_TIMESTAMPING_RX_HARDWARE |
             SOF_TIMESTAMPING_RAW_HARDWARE;
  }
  if (::setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) !=
      0) {
    return SocketTimestampingApplyResult{.ok = false,
                                         .enabled = false,
                                         .error_errno = errno};
  }
  return SocketTimestampingApplyResult{.ok = true, .enabled = true};
#else
  (void)fd;
  return SocketTimestampingApplyResult{.ok = false,
                                       .enabled = false,
                                       .error_errno = ENOTSUP};
#endif
}
```

Modify `PlainSocket::OpenAndConnect` so after `fd_ = fd` and before returning connected it calls:

```cpp
timestamping_apply_result_ =
    ApplySocketTimestampingConfig(fd_, config.socket_timestamping);
if (config.socket_timestamping.enabled && !timestamping_apply_result_.ok) {
  ::close(fd_);
  fd_ = -1;
  return false;
}
```

Add accessors:

```cpp
[[nodiscard]] SocketTimestampingApplyResult timestamping_apply_result()
    const noexcept {
  return timestamping_apply_result_;
}
```

- [ ] **Step 4: Run tests**

Run:

```bash
cmake --build build/debug --target websocket_socket_timestamping_test websocket_plain_socket_test -j8
ctest --test-dir build/debug -R 'websocket_(socket_timestamping|plain_socket)' --output-on-failure
```

Expected: tests pass.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/socket_timestamping.h core/websocket/plain_socket.h test/websocket/socket_timestamping_test.cpp
git commit -m "Enable optional timestamping on plain sockets"
```

## Task 3: Drain TX Error Queue And RX Software Timestamps

**Files:**
- Modify: `core/websocket/socket_timestamping.h`
- Modify: `core/websocket/plain_socket.h`
- Modify: `core/websocket/critical_session.h`
- Test: `test/websocket/socket_timestamping_test.cpp`

- [ ] **Step 1: Add a loopback integration test**

Add a test that opens a local TCP server/client, enables software timestamping on the client, sends a short payload, reads it on the server, drains `MSG_ERRQUEUE`, and accepts either a timestamp event or a clear unsupported result:

```cpp
TEST(SocketTimestampingTest, LoopbackTxSoftwareTimestampCanBeDrained) {
  SocketTimestampingConfig config;
  config.enabled = true;
  config.tx_software = true;
  config.tx_ack = true;
  config.max_errqueue_events_per_drain = 16;

  LoopbackTcpPair pair = CreateLoopbackTcpPairForTest();
  const SocketTimestampingApplyResult apply =
      ApplySocketTimestampingConfig(pair.client_fd, config);
  if (!apply.ok) {
    GTEST_SKIP() << "SO_TIMESTAMPING unavailable errno=" << apply.error_errno;
  }

  ASSERT_EQ(::send(pair.client_fd, "x", 1, MSG_NOSIGNAL), 1);
  char byte = 0;
  ASSERT_EQ(::recv(pair.server_fd, &byte, 1, 0), 1);

  SocketTimestampingEventDrain drain =
      DrainSocketTimestampingErrorQueue(pair.client_fd, 16);

  EXPECT_TRUE(drain.ok);
  EXPECT_GE(drain.events_seen, 0U);
}
```

- [ ] **Step 2: Run test and verify it fails**

Run:

```bash
cmake --build build/debug --target websocket_socket_timestamping_test -j8
ctest --test-dir build/debug -R websocket_socket_timestamping_test --output-on-failure
```

Expected: `DrainSocketTimestampingErrorQueue` and `LoopbackTcpPair` do not exist.

- [ ] **Step 3: Implement nonblocking errqueue drain**

Add to `core/websocket/socket_timestamping.h`:

```cpp
enum class SocketTimestampingEventKind : std::uint8_t {
  kUnknown,
  kTxSoftware,
  kTxSched,
  kTxAck,
  kRxSoftware,
};

struct SocketTimestampingEvent {
  SocketTimestampingEventKind kind{SocketTimestampingEventKind::kUnknown};
  std::int64_t timestamp_ns{0};
  std::uint32_t id{0};
};

struct SocketTimestampingEventDrain {
  bool ok{true};
  int error_errno{0};
  std::uint32_t events_seen{0};
  std::array<SocketTimestampingEvent, 32> events{};
};

[[nodiscard]] SocketTimestampingEventDrain DrainSocketTimestampingErrorQueue(
    int fd, std::uint32_t max_events) noexcept;
```

Implement `DrainSocketTimestampingErrorQueue` inline or split it into a `.cpp` if the function grows. It must:

```text
call recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT)
stop on EAGAIN/EWOULDBLOCK
parse SCM_TIMESTAMPING / SCM_TIMESTAMPING_NEW
parse sock_extended_err from IP_RECVERR/IPV6_RECVERR
map SO_EE_ORIGIN_TIMESTAMPING + ee_info to TX_SCHED / TX_SOFTWARE / TX_ACK
copy at most 32 events into the returned array
```

Modify `PlainSocket` with:

```cpp
[[nodiscard]] SocketTimestampingEventDrain DrainTimestampingErrorQueue(
    std::uint32_t max_events) noexcept {
  return DrainSocketTimestampingErrorQueue(fd_, max_events);
}
```

Modify `CriticalSession` to call this drain after successful `WriteSome` and before/after `DriveRead`, bounded by `max_errqueue_events_per_drain`.

- [ ] **Step 4: Run tests**

Run:

```bash
cmake --build build/debug --target websocket_socket_timestamping_test websocket_critical_session_test -j8
ctest --test-dir build/debug -R 'websocket_(socket_timestamping|critical_session)' --output-on-failure
```

Expected: tests pass or timestamping loopback test skips only when kernel support is missing.

- [ ] **Step 5: Commit**

```bash
git add core/websocket/socket_timestamping.h core/websocket/plain_socket.h core/websocket/critical_session.h test/websocket/socket_timestamping_test.cpp
git commit -m "Drain websocket socket timestamp events"
```

## Task 4: Match Timestamp Events To Gate Order Requests

**Files:**
- Modify: `exchange/gate/trading/order_latency_diagnostics.h`
- Modify: `exchange/gate/trading/order_session.h`
- Test: `test/exchange/gate/trading/order_session_test.cpp`

- [ ] **Step 1: Add tests for stage classification with injected timestamps**

In `test/exchange/gate/trading/order_session_test.cpp`, add a pure test around the diagnostic structs:

```cpp
TEST(GateOrderSessionTest, SocketTimestampingStagesClassifyLocalSegments) {
  websocket::SocketTimestampingSnapshot snapshot;
  snapshot.write_complete_ns = 1'000;
  snapshot.tx_software_ns = 1'010;
  snapshot.tx_ack_ns = 8'000;
  snapshot.rx_software_ns = 9'000;
  snapshot.ack_receive_local_ns = 9'050;

  const websocket::SocketTimestampingStages stages =
      websocket::ComputeSocketTimestampingStages(snapshot);

  EXPECT_EQ(stages.write_complete_to_tx_software_ns, 10);
  EXPECT_EQ(stages.tx_software_to_tx_ack_ns, 6'990);
  EXPECT_EQ(stages.tx_ack_to_rx_software_ns, 1'000);
  EXPECT_EQ(stages.rx_software_to_ack_receive_ns, 50);
}
```

- [ ] **Step 2: Run test and verify it fails if structs are not wired**

Run:

```bash
cmake --build build/debug --target gate_order_session_test -j8
ctest --test-dir build/debug -R gate_order_session_test --output-on-failure
```

Expected before implementation: missing fields if the diagnostic record has not been extended.

- [ ] **Step 3: Extend diagnostic window and log record**

Modify `OrderLatencyDiagnosticWindow` and `OrderLatencyDiagnosticLogRecord` in `exchange/gate/trading/order_latency_diagnostics.h`:

```cpp
websocket::SocketTimestampingSnapshot socket_timestamps{};
websocket::SocketTimestampingStages socket_timestamp_stages{};
```

Modify `GateOrderSession` so when a request is sent it records:

```cpp
window.socket_timestamps.write_complete_ns = write_path.write_complete_ns;
```

When an Ack is handled, match the timestamp events captured by `CriticalSession` to the request sequence and fill:

```cpp
window.socket_timestamps.tx_sched_ns = matched.tx_sched_ns;
window.socket_timestamps.tx_software_ns = matched.tx_software_ns;
window.socket_timestamps.tx_ack_ns = matched.tx_ack_ns;
window.socket_timestamps.rx_software_ns = matched.rx_software_ns;
window.socket_timestamps.ack_receive_local_ns = ack_local_receive_ns;
window.socket_timestamp_stages =
    websocket::ComputeSocketTimestampingStages(window.socket_timestamps);
```

The first implementation may use per-session sequential matching because RTT probe keeps one active request per order session. If later strategy paths need multiple concurrent requests, extend the matcher to use `SOF_TIMESTAMPING_OPT_ID_TCP` byte IDs.

- [ ] **Step 4: Extend Ack diagnostic log**

Modify `LogOrderLatencyDiagnostic` in `exchange/gate/trading/order_session.h` to append:

```text
ts_write_complete_ns={}
ts_tx_sched_ns={}
ts_tx_software_ns={}
ts_tx_ack_ns={}
ts_rx_software_ns={}
ts_ack_receive_local_ns={}
ts_write_to_tx_software_ns={}
ts_tx_software_to_tx_ack_ns={}
ts_tx_ack_to_rx_software_ns={}
ts_rx_software_to_ack_receive_ns={}
```

- [ ] **Step 5: Run tests**

Run:

```bash
cmake --build build/debug --target gate_order_session_test -j8
ctest --test-dir build/debug -R gate_order_session_test --output-on-failure
```

Expected: test passes.

- [ ] **Step 6: Commit**

```bash
git add exchange/gate/trading/order_latency_diagnostics.h exchange/gate/trading/order_session.h test/exchange/gate/trading/order_session_test.cpp
git commit -m "Add order session timestamp stage diagnostics"
```

## Task 5: Add Config Parsing

**Files:**
- Modify: `exchange/gate/trading/order_session_config.h`
- Modify: `exchange/gate/trading/order_session_config.cpp`
- Modify: `tools/gate/order_session_rtt_probe/config.h`
- Modify: `tools/gate/order_session_rtt_probe/config.cpp`
- Modify: `tools/gate/order_session_rtt_probe/session_config_builder.h`
- Test: `test/config/order_session_config_test.cpp`
- Test: `test/tools/gate/order_session_rtt_probe/order_session_rtt_probe_test.cpp`

- [ ] **Step 1: Add failing config tests**

Add to order session config test:

```toml
[order_session.diagnostics.timestamping]
enabled = true
tx_software = true
tx_ack = true
rx_software = true
max_errqueue_events_per_drain = 16
```

Expect:

```cpp
EXPECT_TRUE(config.connection.socket_timestamping.enabled);
EXPECT_TRUE(config.connection.socket_timestamping.tx_software);
EXPECT_TRUE(config.connection.socket_timestamping.tx_ack);
EXPECT_TRUE(config.connection.socket_timestamping.rx_software);
EXPECT_EQ(config.connection.socket_timestamping.max_errqueue_events_per_drain,
          16U);
```

Add to RTT probe config test:

```toml
[probe.sessions.timestamping]
enabled = true
tx_software = true
tx_ack = true
rx_software = true
```

Expect the values are copied into the generated `OrderSessionConfig`.

- [ ] **Step 2: Run tests and verify failure**

Run:

```bash
cmake --build build/debug --target order_session_config_test gate_order_session_rtt_probe_test -j8
ctest --test-dir build/debug -R '(order_session_config|gate_order_session_rtt_probe)' --output-on-failure
```

Expected: parser fields missing.

- [ ] **Step 3: Implement parser**

Add `websocket::SocketTimestampingConfig timestamping` to `OrderSessionConfig` or store it directly in `connection.socket_timestamping`. Parse:

```toml
[order_session.diagnostics.timestamping]
enabled = false
tx_sched = false
tx_software = false
tx_ack = false
rx_software = false
hardware = false
max_errqueue_events_per_drain = 16
```

Add `ProbeSessionConfig::timestamping` and copy it in `BuildOrderSessionConfig`.

- [ ] **Step 4: Run tests**

Run:

```bash
cmake --build build/debug --target order_session_config_test gate_order_session_rtt_probe_test -j8
ctest --test-dir build/debug -R '(order_session_config|gate_order_session_rtt_probe)' --output-on-failure
```

Expected: tests pass.

- [ ] **Step 5: Commit**

```bash
git add exchange/gate/trading/order_session_config.h exchange/gate/trading/order_session_config.cpp tools/gate/order_session_rtt_probe/config.h tools/gate/order_session_rtt_probe/config.cpp tools/gate/order_session_rtt_probe/session_config_builder.h test/config/order_session_config_test.cpp test/tools/gate/order_session_rtt_probe/order_session_rtt_probe_test.cpp
git commit -m "Parse order session timestamping diagnostics config"
```

## Task 6: Extend RTT Probe CSV

**Files:**
- Modify: `tools/gate/order_session_rtt_probe/sample_flow.h`
- Modify: `tools/gate/order_session_rtt_probe/sample_csv_writer.h`
- Modify: `tools/gate/order_session_rtt_probe/sample_csv_writer.cpp`
- Test: `test/tools/gate/order_session_rtt_probe/order_session_rtt_probe_test.cpp`

- [ ] **Step 1: Add failing CSV header test**

Extend existing sample CSV writer test to assert the header contains:

```text
ts_tx_software_ns
ts_tx_ack_ns
ts_rx_software_ns
ts_write_to_tx_software_ns
ts_tx_software_to_tx_ack_ns
ts_tx_ack_to_rx_software_ns
ts_rx_software_to_ack_receive_ns
```

- [ ] **Step 2: Run test and verify failure**

Run:

```bash
cmake --build build/debug --target gate_order_session_rtt_probe_test -j8
ctest --test-dir build/debug -R gate_order_session_rtt_probe --output-on-failure
```

Expected: missing header fields.

- [ ] **Step 3: Extend CSV row**

Add fields to `ProbeSampleCsvRow`:

```cpp
std::int64_t ts_tx_sched_ns{0};
std::int64_t ts_tx_software_ns{0};
std::int64_t ts_tx_ack_ns{0};
std::int64_t ts_rx_software_ns{0};
std::int64_t ts_write_to_tx_software_ns{-1};
std::int64_t ts_tx_software_to_tx_ack_ns{-1};
std::int64_t ts_tx_ack_to_rx_software_ns{-1};
std::int64_t ts_rx_software_to_ack_receive_ns{-1};
```

Append them to `OrderSessionRttSampleCsvSchema::header` and `format`, and fill them from the order Ack diagnostic snapshot in `sample_flow.h`.

- [ ] **Step 4: Run test**

Run:

```bash
cmake --build build/debug --target gate_order_session_rtt_probe_test -j8
ctest --test-dir build/debug -R gate_order_session_rtt_probe --output-on-failure
```

Expected: test passes.

- [ ] **Step 5: Commit**

```bash
git add tools/gate/order_session_rtt_probe/sample_flow.h tools/gate/order_session_rtt_probe/sample_csv_writer.h tools/gate/order_session_rtt_probe/sample_csv_writer.cpp test/tools/gate/order_session_rtt_probe/order_session_rtt_probe_test.cpp
git commit -m "Write RTT probe socket timestamp fields"
```

## Task 7: Documentation And Probe Config

**Files:**
- Modify: `config/order_session_rtt_probe/gate_order_session_rtt_probe.toml`
- Modify: `docs/gate_order_session_rtt_probe_design.md`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: Add default-off config block**

Add to `config/order_session_rtt_probe/gate_order_session_rtt_probe.toml`:

```toml
[probe.sessions.timestamping]
enabled = false
tx_sched = true
tx_software = true
tx_ack = true
rx_software = true
hardware = false
max_errqueue_events_per_drain = 16
```

- [ ] **Step 2: Document diagnostic fields**

In `docs/diagnostic_fields.md`, add the field meanings:

```text
ts_tx_software_ns: local kernel software TX timestamp for request bytes.
ts_tx_ack_ns: local timestamp when TCP reports the request bytes were ACKed.
ts_rx_software_ns: local kernel software RX timestamp for the business Ack packet.
ts_tx_software_to_tx_ack_ns: local-clock TCP/network RTT segment.
ts_tx_ack_to_rx_software_ns: remote business processing plus response return segment.
```

- [ ] **Step 3: Document operation runbook**

In `docs/gate_order_session_rtt_probe_design.md`, add:

```bash
ip route get 10.0.1.154
sudo tcpdump -i <iface> -s 0 -nn -ttttt \
  'host 10.0.1.154 and tcp port 80' \
  -w /home/liuxiang/tmp/gate_order_session_rtt_probe/<run_id>/private_link.pcap
```

and the software timestamping run command:

```bash
./build/release/tools/gate_order_session_rtt_probe \
  --config /home/liuxiang/tmp/gate_order_session_rtt_probe/<run_id>/probe.toml \
  --execute --duration-sec 1800
```

- [ ] **Step 4: Verify docs**

Run:

```bash
git diff --check
```

Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add config/order_session_rtt_probe/gate_order_session_rtt_probe.toml docs/gate_order_session_rtt_probe_design.md docs/diagnostic_fields.md
git commit -m "Document order session timestamp diagnostics"
```

## Task 8: Live Verification

**Files:**
- No source changes.

- [ ] **Step 1: Build release**

Run:

```bash
cmake --build build/release -j8
```

Expected: exit code 0.

- [ ] **Step 2: Run debug/release tests**

Run:

```bash
ctest --test-dir build/debug -R '(websocket_socket_timestamping|websocket_plain_socket|websocket_critical_session|order_session_config|gate_order_session|gate_order_session_rtt_probe)' --output-on-failure
```

Expected: all selected tests pass.

- [ ] **Step 3: Start a 30m private-focused probe**

Use a scratch config under `/home/liuxiang/tmp/gate_order_session_rtt_probe/<run_id>/probe.toml`:

```toml
[probe.sessions.timestamping]
enabled = true
tx_sched = true
tx_software = true
tx_ack = true
rx_software = true
hardware = false
max_errqueue_events_per_drain = 16
```

Run:

```bash
./build/release/tools/gate_order_session_rtt_probe \
  --config /home/liuxiang/tmp/gate_order_session_rtt_probe/<run_id>/probe.toml \
  --execute --duration-sec 1800
```

- [ ] **Step 4: Analyze outliers**

Run:

```bash
python3 - <<'PY'
import csv
path = "/home/liuxiang/tmp/gate_order_session_rtt_probe/<run_id>/order_session_rtt_samples.csv"
rows = list(csv.DictReader(open(path, newline="")))
def ns(row, key):
    value = row.get(key, "")
    return int(value) if value else -1
for row in sorted(rows, key=lambda r: ns(r, "ack_rtt_ns"), reverse=True)[:20]:
    print(row["session"], row["round"], row["sample"],
          ns(row, "ack_rtt_ns") / 1e6,
          ns(row, "ts_write_to_tx_software_ns") / 1e6,
          ns(row, "ts_tx_software_to_tx_ack_ns") / 1e6,
          ns(row, "ts_tx_ack_to_rx_software_ns") / 1e6,
          ns(row, "ts_rx_software_to_ack_receive_ns") / 1e6)
PY
```

Expected: each Ack outlier can be assigned to one dominant local-clock segment.

- [ ] **Step 5: Decide whether hardware timestamping is needed**

Use this decision table:

```text
write_to_tx_software_ns is the dominant outlier:
  inspect qdisc, NIC TX queue, CPU affinity, IRQ affinity, and consider hardware timestamping.

tx_software_to_tx_ack_ns is the dominant outlier:
  focus on private link / network / remote TCP ACK path.

tx_ack_to_rx_software_ns is the dominant outlier:
  focus on Gate application/session processing or response return path.

rx_software_to_ack_receive_ns is the dominant outlier:
  focus on local RX path, read loop, CPU scheduling, and parser hot path.
```

## Hardware Timestamping Follow-Up

Only start after software timestamping shows local TX/RX path as the likely bottleneck.

- [ ] Check capability:

```bash
ethtool -T <iface>
```

- [ ] Add config:

```toml
[probe.sessions.timestamping]
enabled = true
hardware = true
tx_software = true
tx_ack = true
rx_software = true
```

- [ ] Add fields:

```text
ts_tx_hardware_ns
ts_rx_hardware_ns
ts_tx_software_to_tx_hardware_ns
ts_rx_hardware_to_rx_software_ns
```

- [ ] Verify PHC/system clock alignment before comparing hardware timestamps with local realtime:

```bash
timedatectl
ethtool -T <iface>
```

## Final Verification Checklist

- [ ] `cmake --build build/debug --target websocket_socket_timestamping_test gate_order_session_rtt_probe_test -j8`
- [ ] `ctest --test-dir build/debug -R '(websocket_socket_timestamping|gate_order_session_rtt_probe)' --output-on-failure`
- [ ] `cmake --build build/release -j8`
- [ ] `git diff --check`
- [ ] `rg '#include "evaluation/' core exchange tools` has no output
- [ ] `rg 'aquila_evaluation' core exchange tools` has no output

