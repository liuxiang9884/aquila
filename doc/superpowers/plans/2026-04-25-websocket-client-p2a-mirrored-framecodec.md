# WebSocket Client P2-A Mirrored FrameCodec Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the WebSocket receive codec hot path with a mirrored ring buffer, direct socket reads, fixed ready metadata, explicit capacity metrics, and benchmark evidence against the pre-P2-A baseline.

**Architecture:** `FrameCodec` owns a Linux mirrored receive ring and a fixed ready-frame metadata ring. `CriticalSession::DriveRead` reads directly into codec-owned writable spans, then polls decoded `MessageView` instances whose payload spans point into the mirrored ring.

**Tech Stack:** C++20, Linux `mmap`/`memfd_create`, OpenSSL, CMake, GoogleTest, Google Benchmark.

---

### Task 1: FrameCodec RED Tests

**Files:**
- Modify: `test/websocket/frame_codec_test.cpp`

- [x] **Step 1: Write failing tests**

Add tests for:

```cpp
TEST(WebsocketFrameCodecTest, DecodesPayloadAcrossMirroredBoundary) {
  FrameCodec codec(4096, 4096, 8);
  auto filler = BuildServerTextFrame(std::string(4088, 'x'));
  ASSERT_EQ(codec.Feed(filler).status, DecodeStatus::kMessageReady);
  ASSERT_EQ(codec.Poll().status, DecodeStatus::kNeedMore);

  auto wrapped = BuildServerTextFrame("abcdef");
  auto writable = codec.WritableSpan();
  ASSERT_GE(writable.size(), wrapped.size());
  std::copy(wrapped.begin(), wrapped.end(), writable.begin());
  codec.CommitWritten(wrapped.size());

  auto decoded = codec.Poll();
  ASSERT_EQ(decoded.status, DecodeStatus::kMessageReady);
  EXPECT_TRUE(PayloadEquals(decoded.view.payload, "abcdef"));
}
```

```cpp
TEST(WebsocketFrameCodecTest, ReportsCapacityExceededWhenReceiveRingIsFull) {
  FrameCodec codec(8192, 4096, 8);
  std::vector<std::byte> partial(4096, std::byte{0});
  partial[0] = std::byte{0x82};
  partial[1] = std::byte{126};
  partial[2] = std::byte{0x10};
  partial[3] = std::byte{0x01};
  ASSERT_EQ(codec.Feed(partial).status, DecodeStatus::kNeedMore);
  EXPECT_TRUE(codec.WritableSpan().empty());
  EXPECT_EQ(codec.Feed(std::span<const std::byte>(&partial[0], 1)).status,
            DecodeStatus::kCapacityExceeded);
}
```

- [x] **Step 2: Run RED**

Run: `./build.sh debug && ./build/debug/test/websocket/websocket_frame_codec_test`

Expected: compile or test failure because `FrameCodec(size_t,size_t,size_t)`, `WritableSpan`, `CommitWritten`, and `DecodeStatus::kCapacityExceeded` do not exist.

### Task 2: Mirrored Buffer and FrameCodec GREEN

**Files:**
- Create: `core/websocket/mirrored_buffer.h`
- Modify: `core/websocket/frame_codec.h`

- [x] **Step 1: Implement minimal mirrored buffer**

Create `MirroredBuffer` with `Init`, move support, destructor, `data`, and `capacity`. Use `syscall(SYS_memfd_create)`, `ftruncate`, one reserved `PROT_NONE` region, and two `MAP_SHARED | MAP_FIXED` mappings.

- [x] **Step 2: Replace decode storage**

Replace `std::vector` / `std::deque` decode storage with:

```cpp
MirroredBuffer receive_ring_{};
std::unique_ptr<ReadyFrame[]> ready_frames_{};
std::uint64_t consume_abs_{0};
std::uint64_t parse_abs_{0};
std::uint64_t write_abs_{0};
```

- [x] **Step 3: Run GREEN**

Run: `./build.sh debug && ./build/debug/test/websocket/websocket_frame_codec_test`

Expected: all frame codec tests pass.

### Task 3: CriticalSession and Degraded RED/GREEN

**Files:**
- Modify: `core/websocket/types.h`
- Modify: `core/websocket/metrics.h`
- Modify: `core/websocket/degraded_evaluator.h`
- Modify: `core/websocket/critical_session.h`
- Modify: `core/websocket/websocket_client.h`
- Modify: `test/websocket/critical_session_test.cpp`
- Modify: `test/websocket/degraded_evaluator_test.cpp`
- Modify: `test/websocket/types_test.cpp`

- [x] **Step 1: Write failing tests**

Add tests proving codec capacity increments `frame_codec_capacity_exhaustions` without reconnect, and `DegradedEvaluator` enters degraded when `frame_codec_capacity_exhaustions` increases inside the 1s window.

- [x] **Step 2: Run RED**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_(critical_session|degraded_evaluator|types)_test --output-on-failure`

Expected: failure because metrics and degraded sample fields do not exist.

- [x] **Step 3: Implement GREEN**

Wire new metrics/config fields through `ConnectionConfig`, `Metrics`, `DegradedSample`, `DegradedEvaluator`, `CriticalSession::DriveRead`, and `WebSocketClient::RuntimeSession`.

- [x] **Step 4: Run GREEN**

Run: `./build.sh debug && ctest --test-dir build/debug -R websocket_(critical_session|degraded_evaluator|types)_test --output-on-failure`

Expected: all selected tests pass.

### Task 4: Benchmarks and Validation

**Files:**
- Modify: `benchmark/websocket/frame_codec_benchmark.cpp`
- Modify: `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`

- [x] **Step 1: Update benchmark cases**

Split `frame_codec_benchmark` into encode, decode contiguous, and decode mirrored-boundary benchmark cases.

- [x] **Step 2: Run full verification**

Run:

```bash
./build.sh debug
ctest --test-dir build/debug -R websocket_ --output-on-failure
./build.sh release
ctest --test-dir build/release -R websocket_ --output-on-failure
./build/release/benchmark/websocket/frame_codec_benchmark
./build/release/benchmark/websocket/session_read_path_benchmark
./build/release/benchmark/websocket/session_write_path_benchmark
./build/release/benchmark/websocket/active_spin_benchmark
./build/release/benchmark/websocket/prepared_write_benchmark
```

Expected: tests pass; benchmark p50 / p99 / p99.9 are recorded.

- [x] **Step 3: Compare with pre-P2-A baseline**

Use a clean `dev` baseline before the implementation commit, run the same release benchmarks, and record both sets of numbers in the review document.

- [x] **Step 4: Commit**

Commit code and benchmark documentation separately:

```bash
git add core test benchmark
git commit -m "core: add mirrored websocket frame codec"
git add doc/reviews/2026-04-24-websocket-client-gap-analysis.md
git commit -m "docs: record mirrored frame codec validation"
```
