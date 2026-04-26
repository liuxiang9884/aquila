# WebSocket Client P2-B Hotpath Pacing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement P2-B hot-path pacing for WebSocket read pumping, control-frame write coordination, and clock-source measurement without regressing the default low-latency path.

**Architecture:** Keep `CriticalSession` as the hot-path owner. Add bounded read pump knobs to `ConnectionConfig`, add a dedicated in-session control write slot, and centralize runtime clock selection behind a tiny `ClockSource` helper. Every behavioral change is guarded by tests and benchmark evidence.

**Tech Stack:** C++20, header-only `core/websocket`, CMake, GTest, Google Benchmark, Linux/OpenSSL.

---

## 文件结构

- 修改：`core/websocket/types.h`
  - 新增 read pump 配置。
- 修改：`core/websocket/runtime_policy.h`
  - 新增 `ClockSource` 和 `ClockSource clock_source`。
- 新建：`core/websocket/runtime_clock.h`
  - 新增 `NowNs(ClockSource)` helper，避免在轻量 policy/type 头里引入 `<chrono>`。
- 修改：`core/websocket/tls_socket.h`
  - 新增 `PendingReadableBytes() const noexcept`。
- 修改：`core/websocket/critical_session.h`
  - G2：实现 bounded read pump。
  - G4：实现 dedicated control slot 和 heartbeat ping send delay metrics。
- 修改：`core/websocket/websocket_client.h`
  - G8：degraded evaluation 复用 loop clock。
- 修改：`core/websocket/active_spin_loop.h`
  - G8：按 `runtime_policy.clock_source` 取时。
- 修改：`core/websocket/metrics.h`
  - 新增 heartbeat ping send delay 指标。
- 修改：`test/websocket/critical_session_test.cpp`
  - 增加 read pump 和 control slot 单元测试。
- 修改：`test/websocket/types_test.cpp`
  - 增加新增配置和 enum 默认值测试。
- 修改：`benchmark/websocket/session_read_path_benchmark.cpp`
  - 增加 burst / pump variant。
- 修改：`benchmark/websocket/session_write_path_benchmark.cpp`
  - 增加 control slot variant。
- 新建：`benchmark/websocket/clock_source_benchmark.cpp`
  - 比较 clock source 成本。
- 修改：`benchmark/websocket/CMakeLists.txt`
  - 注册 `clock_source_benchmark`。
- 修改：`doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
  - P2-B 完成后回填 G2 / G4 / G8 处理方案和验证证据。

---

## Task 1：类型与默认配置测试

- [ ] **Step 1：写失败测试**

在 `test/websocket/types_test.cpp` 增加断言：

```cpp
static_assert(kEnumValueExists<ws::ClockSource::kSteady>);
static_assert(kEnumValueExists<ws::ClockSource::kMonotonic>);
static_assert(kEnumValueExists<ws::ClockSource::kMonotonicCoarse>);

EXPECT_EQ(config.max_reads_per_drive, 1U);
EXPECT_FALSE(config.read_until_would_block);
EXPECT_EQ(config.runtime_policy.clock_source, ws::ClockSource::kSteady);
```

- [ ] **Step 2：确认失败**

运行：

```bash
./build.sh debug
```

预期：编译失败，提示 `ClockSource` / `max_reads_per_drive` / `read_until_would_block` 不存在。

- [ ] **Step 3：最小实现**

在 `core/websocket/runtime_policy.h` 增加：

```cpp
enum class ClockSource : std::uint8_t {
  kSteady,
  kMonotonic,
  kMonotonicCoarse,
};
```

在 `core/websocket/types.h` 的 `ConnectionConfig` 增加：

```cpp

struct ConnectionConfig {
  ...
  std::uint32_t max_reads_per_drive = 1;
  bool read_until_would_block = false;
  ...
};
```

在 `core/websocket/runtime_policy.h` 的 `RuntimePolicy` 增加：

```cpp
ClockSource clock_source = ClockSource::kSteady;
```

- [ ] **Step 4：确认通过**

运行：

```bash
./build.sh debug
./build/debug/test/websocket/websocket_types_test
```

- [ ] **Step 5：提交**

```bash
git add core/websocket/types.h core/websocket/runtime_policy.h test/websocket/types_test.cpp
git commit -m "core: add websocket hotpath pacing knobs"
```

---

## Task 2：G2 Bounded Read Pump

- [ ] **Step 1：写失败测试**

在 `test/websocket/critical_session_test.cpp` 扩展 `FakeTlsSocket`：

```cpp
size_t read_calls_{0};
bool pending_readable_{false};

size_t PendingReadableBytes() const noexcept {
  return pending_readable_ && !read_chunks_.empty() ? read_chunks_.front().size()
                                                    : 0;
}
```

新增测试：

```cpp
TEST(WebsocketCriticalSessionTest, BoundedReadPumpDrainsPendingChunks) {
  auto config = BuildSmallConfig(4);
  config.max_reads_per_drive = 3;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.pending_readable_ = true;
  socket.read_chunks_.push_back(BuildServerTextFrame("a"));
  socket.read_chunks_.push_back(BuildServerTextFrame("b"));
  socket.read_chunks_.push_back(BuildServerTextFrame("c"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);
  session.DriveRead();

  EXPECT_EQ(bytes, 3U);
  EXPECT_EQ(metrics.rx_messages, 3U);
  EXPECT_EQ(socket.read_calls_, 3U);
}

TEST(WebsocketCriticalSessionTest, BoundedReadPumpDoesNotProbeWithoutPending) {
  auto config = BuildSmallConfig(4);
  config.max_reads_per_drive = 3;
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.pending_readable_ = false;
  socket.read_chunks_.push_back(BuildServerTextFrame("a"));
  socket.read_chunks_.push_back(BuildServerTextFrame("b"));

  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);
  session.DriveRead();

  EXPECT_EQ(bytes, 1U);
  EXPECT_EQ(metrics.rx_messages, 1U);
  EXPECT_EQ(socket.read_calls_, 1U);
}
```

- [ ] **Step 2：确认失败**

运行：

```bash
./build/debug/test/websocket/websocket_critical_session_test \
  --gtest_filter='WebsocketCriticalSessionTest.BoundedReadPump*'
```

预期：编译或断言失败。

- [ ] **Step 3：实现 `PendingReadableBytes`**

在 `core/websocket/tls_socket.h` 增加：

```cpp
size_t PendingReadableBytes() const noexcept {
  if (ssl_ == nullptr) {
    return 0;
  }
  const int pending = SSL_pending(ssl_);
  return pending > 0 ? static_cast<size_t>(pending) : 0;
}
```

在 benchmark `LocalFdSocket` 中增加：

```cpp
size_t PendingReadableBytes() const noexcept { return 0; }
```

- [ ] **Step 4：实现 `DriveRead` pump**

在 `core/websocket/critical_session.h` 拆出 helper：

```cpp
std::uint32_t MaxReadsPerDrive() const noexcept {
  return config_.max_reads_per_drive == 0 ? 1 : config_.max_reads_per_drive;
}

bool ShouldContinueReadPump(std::uint32_t reads_done) const noexcept {
  if (reads_done >= MaxReadsPerDrive()) {
    return false;
  }
  if (config_.read_until_would_block) {
    return true;
  }
  return tls_socket_.PendingReadableBytes() != 0;
}
```

`DriveRead()` 改为循环调用 `ReadSome()`，每次成功后仍然 drain `codec_.Poll()` 到 `NeedMore`。

- [ ] **Step 5：测试**

运行：

```bash
./build.sh debug
./build/debug/test/websocket/websocket_critical_session_test \
  --gtest_filter='WebsocketCriticalSessionTest.BoundedReadPump*'
```

- [ ] **Step 6：Benchmark 扩展**

在 `benchmark/websocket/session_read_path_benchmark.cpp` 增加两个 benchmark：

- `session_read_path_burst_single_read`
- `session_read_path_burst_bounded_pump`

两者都预先写入 4 个 server text frame；前者 `max_reads_per_drive = 1`，后者 `max_reads_per_drive = 4` 并使用支持 pending 的 fake/local socket adapter。

- [ ] **Step 7：提交**

```bash
git add core/websocket/tls_socket.h core/websocket/critical_session.h \
        benchmark/websocket/io_benchmark_support.h \
        test/websocket/critical_session_test.cpp \
        benchmark/websocket/session_read_path_benchmark.cpp
git commit -m "core: add bounded websocket read pump"
```

---

## Task 3：G4 Dedicated Control Slot

- [ ] **Step 1：写失败测试**

在 `test/websocket/critical_session_test.cpp` 新增 3 个测试：

```cpp
TEST(WebsocketCriticalSessionTest, HeartbeatPingUsesControlSlotWhenQueueFull) {
  auto config = BuildSmallConfig(1);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);

  auto* hold = session.TryAcquirePreparedWrite();
  ASSERT_NE(hold, nullptr);
  hold->encoded_size = 4;
  hold->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(hold), SendStatus::kOk);

  session.AdvanceHeartbeat(6'000'000'000ULL);
  EXPECT_EQ(metrics.control_frame_enqueue_failures, 0U);

  session.DriveWrite();
  EXPECT_FALSE(socket.written_.empty());
}

TEST(WebsocketCriticalSessionTest, AutoPongUsesControlSlotWhenQueueFull) {
  auto config = BuildSmallConfig(1);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);

  auto* hold = session.TryAcquirePreparedWrite();
  ASSERT_NE(hold, nullptr);
  hold->encoded_size = 4;
  hold->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(hold), SendStatus::kOk);
  socket.read_chunks_.push_back(BuildServerPingFrame("z"));

  session.DriveRead();
  EXPECT_EQ(metrics.control_frame_enqueue_failures, 0U);
  session.DriveWrite();
  EXPECT_FALSE(socket.written_.empty());
}

TEST(WebsocketCriticalSessionTest,
     ControlWriteDoesNotInterruptPartialBusinessFrame) {
  auto config = BuildSmallConfig(2);
  PreparedWriteArena arena(config.prepared_write_slots,
                           config.prepared_write_bytes);
  Metrics metrics{};
  size_t bytes = 0;
  MessageConsumer consumer{&bytes, &RecordMessage};
  FakeTlsSocket socket;
  socket.max_write_bytes_per_call_ = 2;
  CriticalSession<FakeTlsSocket> session(config, socket, arena, metrics);
  session.SetConsumer(consumer);

  auto* business = session.TryAcquirePreparedWrite();
  ASSERT_NE(business, nullptr);
  std::copy_n(std::as_bytes(std::span{"abcd", 4}).begin(), 4,
              business->storage.begin());
  business->encoded_size = 4;
  business->kind = PayloadKind::kText;
  ASSERT_EQ(session.CommitPreparedWrite(business), SendStatus::kOk);

  session.DriveWrite();
  ASSERT_EQ(socket.written_.size(), 2U);
  session.AdvanceHeartbeat(6'000'000'000ULL);
  session.DriveWrite();

  ASSERT_GE(socket.written_.size(), 4U);
  EXPECT_EQ(static_cast<char>(socket.written_[0]), 'a');
  EXPECT_EQ(static_cast<char>(socket.written_[1]), 'b');
  EXPECT_EQ(static_cast<char>(socket.written_[2]), 'c');
  EXPECT_EQ(static_cast<char>(socket.written_[3]), 'd');
}
```

断言：

- 业务 queue 满时 `AdvanceHeartbeat()` 不递增 `control_frame_enqueue_failures`。
- 业务 queue 满时收到 ping 后，`DriveWrite()` 仍能写出 pong。
- 业务 frame 已部分写出后，control frame 必须等该业务 frame 完成。

- [ ] **Step 2：确认失败**

运行：

```bash
./build/debug/test/websocket/websocket_critical_session_test \
  --gtest_filter='WebsocketCriticalSessionTest.*ControlSlot*:WebsocketCriticalSessionTest.*AutoPong*:WebsocketCriticalSessionTest.*HeartbeatPing*'
```

- [ ] **Step 3：新增指标**

在 `core/websocket/metrics.h` 增加：

```cpp
std::uint64_t heartbeat_ping_send_delay_ns{0};
std::uint64_t heartbeat_ping_send_delay_max_ns{0};
```

- [ ] **Step 4：实现 control slot**

在 `core/websocket/critical_session.h` 增加固定 control storage：

```cpp
static constexpr size_t kControlWriteStorageBytes = 131;
std::array<std::byte, kControlWriteStorageBytes> control_write_storage_{};
PreparedWrite control_write_{};
bool control_write_pending_{false};
std::uint64_t control_queued_ns_{0};
```

`EnqueueControlFrame()` 改为写入 `control_write_`，不再 `TryAcquirePreparedWrite()`。

`DriveWrite()` 顺序：

```cpp
if (HasPartialBusinessWrite()) {
  DriveBusinessWrites();
  return;
}
if (control_write_pending_) {
  DriveControlWrite();
  if (control_write_pending_) return;
}
DriveBusinessWrites();
```

- [ ] **Step 5：测试**

运行：

```bash
./build.sh debug
./build/debug/test/websocket/websocket_critical_session_test
```

- [ ] **Step 6：Benchmark 扩展**

在 `benchmark/websocket/session_write_path_benchmark.cpp` 增加：

- `session_write_path_business_only`
- `session_write_path_control_slot_full_business_queue`

- [ ] **Step 7：提交**

```bash
git add core/websocket/critical_session.h core/websocket/metrics.h \
        test/websocket/critical_session_test.cpp \
        benchmark/websocket/session_write_path_benchmark.cpp
git commit -m "core: add websocket control write slot"
```

---

## Task 4：G8 Clock Source 与取时复用

- [ ] **Step 1：写失败 benchmark**

新建 `benchmark/websocket/clock_source_benchmark.cpp`：

```cpp
#include "benchmark/websocket/benchmark_support.h"
#include "benchmark/websocket/io_benchmark_support.h"
#include "core/websocket/runtime_policy.h"

#include <benchmark/benchmark.h>

using namespace aquila::websocket;

namespace {

void BenchmarkClockSource(benchmark::State& state, ClockSource source) {
  std::vector<std::uint64_t> samples_ns;
  samples_ns.reserve(static_cast<size_t>(state.iterations()));
  for (auto _ : state) {
    const auto start = benchmarking::NowNs();
    benchmark::DoNotOptimize(NowNs(source));
    samples_ns.push_back(benchmarking::NowNs() - start);
  }
  benchmarking::SetLatencyCounters(state, std::move(samples_ns), "calls",
                                   state.iterations());
}

BENCHMARK_CAPTURE(BenchmarkClockSource, steady, ClockSource::kSteady);
BENCHMARK_CAPTURE(BenchmarkClockSource, monotonic, ClockSource::kMonotonic);
BENCHMARK_CAPTURE(BenchmarkClockSource, monotonic_coarse,
                  ClockSource::kMonotonicCoarse);

}  // namespace
```

在 `benchmark/websocket/CMakeLists.txt` 注册 `clock_source_benchmark`。

- [ ] **Step 2：确认失败**

运行：

```bash
./build.sh release
```

预期：`NowNs(ClockSource)` 不存在。

- [ ] **Step 3：实现 clock helper**

新建 `core/websocket/runtime_clock.h`，增加 inline helper：

```cpp
inline std::uint64_t NowNs(ClockSource source) noexcept {
  if (source == ClockSource::kSteady) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  }
#if defined(__linux__)
  clockid_t id = source == ClockSource::kMonotonicCoarse
                     ? CLOCK_MONOTONIC_COARSE
                     : CLOCK_MONOTONIC;
  timespec ts{};
  if (::clock_gettime(id, &ts) == 0) {
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
  }
#endif
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}
```

- [ ] **Step 4：ActiveSpinLoop 使用 clock source 并支持复用 now**

在 `core/websocket/active_spin_loop.h` 增加两个小 helper：

```cpp
template <typename SessionT>
std::uint32_t ClockCheckInterval(const SessionT& session,
                                 std::uint32_t default_interval) const noexcept {
  if constexpr (requires { session.ClockCheckInterval(default_interval); }) {
    return session.ClockCheckInterval(default_interval);
  }
  return default_interval;
}

template <typename SessionT>
void AdvanceClock(SessionT& session, std::uint64_t now_ns,
                  std::uint32_t elapsed_iterations) const noexcept {
  if constexpr (requires { session.AdvanceClock(now_ns, elapsed_iterations); }) {
    session.AdvanceClock(now_ns, elapsed_iterations);
  } else {
    (void)elapsed_iterations;
    session.AdvanceHeartbeat(now_ns);
  }
}
```

loop 中用 `NowNs(runtime_policy_.clock_source)` 取时，并调用 helper：

```cpp
const auto now_ns = NowNs(runtime_policy_.clock_source);
AdvanceClock(session, now_ns, iterations_since_clock);
```

- [ ] **Step 5：RuntimeSession 复用 now 且保留 iteration 语义**

在 `core/websocket/websocket_client.h` 的 `RuntimeSession` 增加：

```cpp
std::uint32_t ClockCheckInterval(std::uint32_t default_interval) const noexcept {
  if (evaluation_interval_iterations == 0) {
    return default_interval;
  }
  return std::min(default_interval, evaluation_interval_iterations);
}

void AdvanceClock(std::uint64_t now_ns,
                  std::uint32_t elapsed_iterations) noexcept {
  if (!stop_requested.load()) {
    core.AdvanceHeartbeat(now_ns);
    EvaluateDegradedIfDue(now_ns, elapsed_iterations);
  }
}
```

`EvaluateDegradedIfDue()` 改为：

```cpp
void EvaluateDegradedIfDue(std::uint64_t now_ns,
                           std::uint32_t elapsed_iterations) noexcept {
  iterations_since_evaluation += elapsed_iterations;
  if (iterations_since_evaluation < evaluation_interval_iterations) {
    return;
  }
  iterations_since_evaluation = 0;
  const auto evaluation = degraded_evaluator.Evaluate(DegradedSample{
      .now_ns = now_ns,
      ...
  });
  ...
}
```

并从 `DriveRead()` 中移除 `EvaluateDegradedIfDue()`，避免单独 clock call，同时保持 `evaluation_interval_iterations` 表示 spin iteration 数。

- [ ] **Step 6：测试和 benchmark**

运行：

```bash
./build.sh debug
ctest --test-dir build/debug -R websocket_ --output-on-failure
./build.sh release
./build/release/benchmark/websocket/clock_source_benchmark
./build/release/benchmark/websocket/active_spin_benchmark
```

- [ ] **Step 7：提交**

```bash
git add core/websocket/runtime_policy.h core/websocket/runtime_clock.h \
        core/websocket/active_spin_loop.h core/websocket/websocket_client.h \
        benchmark/websocket/CMakeLists.txt \
        benchmark/websocket/clock_source_benchmark.cpp
git commit -m "core: add websocket runtime clock source"
```

---

## Task 5：P2-B 收尾验证与文档回填

- [ ] **Step 1：完整 debug 验证**

```bash
./build.sh debug
ctest --test-dir build/debug -R websocket_ --output-on-failure
```

- [ ] **Step 2：release benchmark**

建议固定 CPU：

```bash
taskset -c 2 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/active_spin_benchmark
taskset -c 2 ./build/release/benchmark/websocket/clock_source_benchmark
```

记录 p50 / p99 / p99.9。

- [ ] **Step 3：回填 review**

更新：

```text
doc/reviews/2026-04-24-websocket-client-gap-analysis.md
```

在 G2 / G4 / G8 条目追加：

- 处理方案
- 关联提交
- 验证证据
- 确认日期

- [ ] **Step 4：提交文档**

```bash
git add doc/reviews/2026-04-24-websocket-client-gap-analysis.md
git commit -m "doc: record websocket p2b verification"
```

---

## 验收标准

- `websocket_critical_session_test` 覆盖 read pump 和 control slot。
- `websocket_types_test` 覆盖新增默认配置。
- `clock_source_benchmark` 存在并可运行。
- 默认配置下单帧 `session_read_path` 和 business-only `session_write_path` 无明确 p50 退化。
- G2 burst benchmark 展示 bounded pump 的 drain 轮次或尾延迟收益。
- G4 control slot benchmark 展示业务 queue 满时 ping/pong 不依赖业务 slot。
- G8 benchmark 给出 clock source p50 / p99 / p99.9，不凭主观选择默认。
