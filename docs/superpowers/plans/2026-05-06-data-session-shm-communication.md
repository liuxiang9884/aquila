# Data Session SHM Communication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add first-version SHM book ticker publishing from Gate / Binance data session processes and SHM reading from strategy or probe processes.

**Architecture:** Keep the WebSocket and exchange decoding path unchanged. Rename the decoded data output interface to `DataSink`, implement `DataShmPublisher` as one `DataSink`, and publish fixed-size `aquila::BookTicker` values into a Nova static broadcast queue stored in shared memory. Reader processes attach the same named channel, keep local cursors, and handle only `unread_count > capacity` overrun in the first version.

**Tech Stack:** C++20, CMake, Nova `ShmAllocator<64>`, Nova `static_impl::SPBroadcastQueue`, CLI11, fmt, GTest, google benchmark.

---

## File Structure

- Create `core/market_data/data_shm_config.h`: config-only structs and compile-time capacity constants; no Nova dependency.
- Create `core/market_data/data_shm.h`: SHM header/channel, manager, `DataShmPublisher`, and `BookTickerShmReader`.
- Modify `exchange/gate/market_data/client.h`: rename `Consumer` template parameter to `DataSink`.
- Modify `exchange/binance/market_data/client.h`: rename `Consumer` template parameter to `DataSink`.
- Modify `exchange/gate/market_data/data_session.h`: rename `Consumer` template parameter to `DataSink`.
- Modify `exchange/binance/market_data/data_session.h`: rename `Consumer` template parameter to `DataSink`.
- Modify `exchange/gate/market_data/data_session_config.h/.cpp`: parse optional top-level `[book_ticker_shm]`.
- Modify `exchange/binance/market_data/data_session_config.h/.cpp`: parse optional top-level `[book_ticker_shm]`.
- Modify `tools/gate/data_session.cpp`: choose `CountingDataSink` or `DataShmPublisher` from config.
- Modify `tools/binance/data_session.cpp`: choose `CountingDataSink` or `DataShmPublisher` from config.
- Create `tools/market_data/book_ticker_shm_reader.cpp`: standalone reader/probe process.
- Modify `tools/CMakeLists.txt`: build `book_ticker_shm_reader`.
- Create `test/core/market_data/data_shm_test.cpp`: unit tests for publisher/reader behavior.
- Modify `test/core/market_data/CMakeLists.txt`: build `core_market_data_shm_test`.
- Modify `test/config/data_session_config_test.cpp`: config parsing tests for SHM blocks.
- Modify `test/exchange/gate/market_data/data_session_test.cpp` and `test/exchange/binance/market_data/data_session_test.cpp`: rename local test sinks.
- Create `benchmark/core/market_data/data_shm_benchmark.cpp`: publisher and reader microbenchmarks.
- Create `benchmark/core/market_data/CMakeLists.txt` and modify `benchmark/CMakeLists.txt`: build the benchmark.
- Modify `doc/data_session_shm_communication_design.md`: keep it synchronized with implementation naming and commands.

### Task 1: Rename Data Session Output Interface

**Files:**
- Modify: `exchange/gate/market_data/client.h`
- Modify: `exchange/binance/market_data/client.h`
- Modify: `exchange/gate/market_data/data_session.h`
- Modify: `exchange/binance/market_data/data_session.h`
- Modify: `tools/gate/data_session.cpp`
- Modify: `tools/binance/data_session.cpp`
- Modify: `test/exchange/gate/market_data/data_session_test.cpp`
- Modify: `test/exchange/binance/market_data/data_session_test.cpp`
- Modify: `test/config/data_session_config_test.cpp`

- [ ] **Step 1: Rename template parameters in headers**

Replace the first template parameter in both `DataSession` and `FuturesMarketDataClient` headers:

```cpp
template <typename DataSink,
          typename WebSocketPolicy = DefaultTlsWebSocketPolicy,
          typename DiagnosticsPolicy = NoopDataSessionDiagnosticsPolicy>
class DataSession;
```

```cpp
template <typename DataSink,
          typename DiagnosticsT = NoopFuturesMarketDataDiagnostics,
          typename OptionsT = websocket::DefaultWebSocketOptions>
class FuturesMarketDataClient;
```

Use constructor parameters named `data_sink` in `DataSession`, and store the reference in `FuturesMarketDataClient` as:

```cpp
DataSink& data_sink_;
```

The call site becomes:

```cpp
data_sink_.OnBookTicker(book_ticker);
```

- [ ] **Step 2: Rename local test/tool structs**

Rename `RecordingConsumer` to `RecordingDataSink` and `CountingConsumer` to `CountingDataSink`. Keep the method signature unchanged:

```cpp
void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
```

- [ ] **Step 3: Run focused compile and tests**

Run:

```bash
cmake --build build/debug --target gate_data_session_test binance_data_session_test data_session_config_test
./build/debug/test/exchange/gate/market_data/gate_data_session_test
./build/debug/test/exchange/binance/market_data/binance_data_session_test
./build/debug/test/config/data_session_config_test
```

Expected: all three commands exit 0.

- [ ] **Step 4: Commit rename**

Run:

```bash
git add exchange/gate/market_data/client.h exchange/binance/market_data/client.h exchange/gate/market_data/data_session.h exchange/binance/market_data/data_session.h tools/gate/data_session.cpp tools/binance/data_session.cpp test/exchange/gate/market_data/data_session_test.cpp test/exchange/binance/market_data/data_session_test.cpp test/config/data_session_config_test.cpp
git commit -m "refactor: rename data session sink interface"
```

### Task 2: Add Core SHM Channel, Publisher, and Reader

**Files:**
- Create: `core/market_data/data_shm_config.h`
- Create: `core/market_data/data_shm.h`
- Create: `test/core/market_data/data_shm_test.cpp`
- Modify: `test/core/market_data/CMakeLists.txt`

- [ ] **Step 1: Add config-only constants and struct**

Create `core/market_data/data_shm_config.h` with:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_

#include <cstdint>
#include <string>

namespace aquila::market_data {

inline constexpr std::uint64_t kBookTickerShmCapacity = 65536;

struct BookTickerShmConfig {
  bool enabled{false};
  std::string shm_name;
  std::string channel_name;
  bool create{true};
  bool remove_existing{false};
  std::uint64_t expected_capacity{kBookTickerShmCapacity};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_SHM_CONFIG_H_
```

- [ ] **Step 2: Add SHM channel types**

Create `core/market_data/data_shm.h` with these public types:

```cpp
namespace aquila::market_data {

inline constexpr std::uint32_t kBookTickerShmMagic = 0x41514C42U;
inline constexpr std::uint32_t kBookTickerShmVersion = 1;
inline constexpr std::size_t kBookTickerShmAllocatorInstances = 64;

using BookTickerQueue =
    nova::static_impl::SPBroadcastQueue<aquila::BookTicker,
                                        kBookTickerShmCapacity>;

struct BookTickerShmHeader {
  std::uint32_t magic{kBookTickerShmMagic};
  std::uint32_t version{kBookTickerShmVersion};
  std::uint32_t abi_size{sizeof(aquila::BookTicker)};
  std::uint32_t capacity{kBookTickerShmCapacity};
  std::uint64_t producer_pid{0};
  std::uint64_t created_ns{0};
  std::atomic<std::uint64_t> published_count{0};
  std::atomic<std::uint64_t> heartbeat_ns{0};
};

struct BookTickerShmChannel {
  BookTickerShmHeader header{};
  BookTickerQueue queue{};
};

static_assert(kBookTickerShmCapacity == 65536);
static_assert((kBookTickerShmCapacity & (kBookTickerShmCapacity - 1)) == 0);
static_assert(std::is_standard_layout_v<BookTickerShmChannel>);
static_assert(std::is_trivially_copyable_v<BookTickerShmChannel>);

}  // namespace aquila::market_data
```

- [ ] **Step 3: Add manager, publisher, and reader APIs**

Add these classes in the same header:

```cpp
class BookTickerShmManager {
 public:
  explicit BookTickerShmManager(const BookTickerShmConfig& config);

  [[nodiscard]] BookTickerShmChannel& channel() noexcept;
  [[nodiscard]] const BookTickerShmChannel& channel() const noexcept;
  [[nodiscard]] static std::size_t StorageSize() noexcept;

 private:
  nova::ShmAllocator<kBookTickerShmAllocatorInstances> allocator_;
  BookTickerShmChannel* channel_{nullptr};
};

class DataShmPublisher {
 public:
  explicit DataShmPublisher(const BookTickerShmConfig& config);

  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept;
  void UpdateHeartbeatNs(std::uint64_t heartbeat_ns) noexcept;
  [[nodiscard]] std::uint64_t published_count() const noexcept;

 private:
  BookTickerShmManager manager_;
  std::uint64_t published_count_{0};
};

class BookTickerShmReader {
 public:
  explicit BookTickerShmReader(const BookTickerShmConfig& config);

  void SeekLatest() noexcept;
  void SeekEarliestVisible() noexcept;
  [[nodiscard]] bool TryReadOne(aquila::BookTicker* out) noexcept;
  [[nodiscard]] std::uint64_t read_pos() const noexcept;
  [[nodiscard]] std::uint64_t overrun_count() const noexcept;

 private:
  BookTickerShmManager manager_;
  std::uint64_t read_pos_{0};
  std::uint64_t overrun_count_{0};
};
```

Manager construction rules:

```cpp
if (config.expected_capacity != kBookTickerShmCapacity) {
  throw std::invalid_argument("book_ticker_shm.expected_capacity mismatch");
}
if (config.shm_name.empty()) {
  throw std::invalid_argument("book_ticker_shm.shm_name is required");
}
if (config.channel_name.empty()) {
  throw std::invalid_argument("book_ticker_shm.channel_name is required");
}
if (!config.create && config.remove_existing) {
  throw std::invalid_argument(
      "book_ticker_shm.remove_existing requires create=true");
}
```

Creation and attach behavior:

```text
create=true and remove_existing=true -> remove the old SHM segment first, then construct channel_name
create=true and remove_existing=false -> construct channel_name or attach the existing constructed object
create=false -> open existing SHM and Find<BookTickerShmChannel>(channel_name)
Find returns nullptr -> throw std::runtime_error("book_ticker_shm.channel_name not found")
after obtaining channel -> validate magic, version, abi_size, and capacity
```

- [ ] **Step 4: Implement reader overrun behavior**

`TryReadOne()` must use the exact first-version algorithm:

```cpp
bool BookTickerShmReader::TryReadOne(aquila::BookTicker* out) noexcept {
  const auto current = manager_.channel().queue.Current();
  if (read_pos_ == current) {
    return false;
  }

  const auto capacity = manager_.channel().queue.capacity();
  const auto unread_count = current - read_pos_;
  if (unread_count > capacity) {
    read_pos_ = current - capacity;
    ++overrun_count_;
  }

  *out = manager_.channel().queue.Value(read_pos_);
  ++read_pos_;
  return true;
}
```

`DataShmPublisher::OnBookTicker()` must update only the queue and `published_count`.
It must not read a clock or update `heartbeat_ns`. `UpdateHeartbeatNs()` is the
only method that stores `header.heartbeat_ns`, and callers must invoke it from a
cold path or a low-frequency timer.

- [ ] **Step 5: Add focused unit tests**

Create tests named:

```cpp
TEST(DataShmTest, PublisherWritesAndReaderReadsBookTicker)
TEST(DataShmTest, ReaderStartsAtLatestWhenRequested)
TEST(DataShmTest, ReaderCanSeekEarliestVisible)
TEST(DataShmTest, ReaderCountsOverrunWhenUnreadCountExceedsCapacity)
TEST(DataShmTest, RejectsExpectedCapacityMismatch)
```

Each test must use a unique SHM name:

```cpp
std::string UniqueShmName(std::string_view suffix) {
  return fmt::format("aquila_data_shm_test_{}_{}", ::getpid(), suffix);
}
```

- [ ] **Step 6: Build and run SHM unit test**

Run:

```bash
cmake --build build/debug --target core_market_data_shm_test
./build/debug/test/core/market_data/core_market_data_shm_test
```

Expected: test exits 0.

- [ ] **Step 7: Commit core SHM layer**

Run:

```bash
git add core/market_data/data_shm_config.h core/market_data/data_shm.h test/core/market_data/data_shm_test.cpp test/core/market_data/CMakeLists.txt
git commit -m "feat: add book ticker shm channel"
```

### Task 3: Parse SHM Config

**Files:**
- Modify: `exchange/gate/market_data/data_session_config.h`
- Modify: `exchange/gate/market_data/data_session_config.cpp`
- Modify: `exchange/binance/market_data/data_session_config.h`
- Modify: `exchange/binance/market_data/data_session_config.cpp`
- Modify: `test/config/data_session_config_test.cpp`

- [ ] **Step 1: Add config field**

Add to both exchange `DataSessionConfig` structs:

```cpp
::aquila::market_data::BookTickerShmConfig book_ticker_shm;
```

Include:

```cpp
#include "core/market_data/data_shm_config.h"
```

- [ ] **Step 2: Parse top-level `[book_ticker_shm]`**

Parse this block in both config parsers:

```toml
[book_ticker_shm]
enabled = true
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
create = true
remove_existing = false
expected_capacity = 65536
```

Rules:

```text
missing [book_ticker_shm] -> enabled=false
enabled=true requires non-empty shm_name and channel_name
capacity key is rejected with "book_ticker_shm.capacity is not supported; use expected_capacity"
expected_capacity must equal 65536
remove_existing=true requires create=true
```

- [ ] **Step 3: Add parser tests**

Add tests named:

```cpp
TEST(DataSessionConfigTest, ParsesGateBookTickerShmConfig)
TEST(DataSessionConfigTest, ParsesBinanceBookTickerShmConfig)
TEST(DataSessionConfigTest, RejectsRuntimeBookTickerShmCapacity)
TEST(DataSessionConfigTest, RejectsBookTickerShmExpectedCapacityMismatch)
```

- [ ] **Step 4: Run config tests**

Run:

```bash
cmake --build build/debug --target data_session_config_test
./build/debug/test/config/data_session_config_test
```

Expected: test exits 0.

- [ ] **Step 5: Commit config parsing**

Run:

```bash
git add exchange/gate/market_data/data_session_config.h exchange/gate/market_data/data_session_config.cpp exchange/binance/market_data/data_session_config.h exchange/binance/market_data/data_session_config.cpp test/config/data_session_config_test.cpp
git commit -m "feat: parse data session shm config"
```

### Task 4: Wire Publisher Into Data Session Tools

**Files:**
- Modify: `tools/gate/data_session.cpp`
- Modify: `tools/binance/data_session.cpp`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: Factor session runner over sink type**

In both tools, replace `RunDataSession<WebSocketPolicy>(config, connect)` with:

```cpp
template <typename WebSocketPolicy, typename DataSink>
int RunDataSessionWithSink(aq_gate::DataSessionConfig data_session_config,
                           DataSink& data_sink, bool connect);
```

Use the corresponding `aq_binance` namespace in the Binance tool.

- [ ] **Step 2: Choose sink from config**

Use:

```cpp
if (config_result.value.book_ticker_shm.enabled) {
  aquila::market_data::DataShmPublisher data_sink{
      config_result.value.book_ticker_shm};
  return RunDataSessionWithSink<WebSocketPolicy>(
      std::move(config_result.value), data_sink, connect);
}

CountingDataSink data_sink;
return RunDataSessionWithSink<WebSocketPolicy>(
    std::move(config_result.value), data_sink, connect);
```

Do not call `UpdateHeartbeatNs()` from `OnBookTicker()` or from any decoded
message callback. If the tool updates heartbeat in this task, update it once
before `session.Run()` or from an external low-frequency timer path only.

- [ ] **Step 3: Build data session tools**

Run:

```bash
cmake --build build/debug --target gate_data_session binance_data_session
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml
./build/debug/tools/binance_data_session --config config/data_sessions/binance_data_session.toml
```

Expected: both dry-run commands exit 0.

- [ ] **Step 4: Commit tool wiring**

Run:

```bash
git add tools/gate/data_session.cpp tools/binance/data_session.cpp tools/CMakeLists.txt
git commit -m "feat: publish data session book tickers to shm"
```

### Task 5: Add Standalone SHM Reader Tool

**Files:**
- Create: `tools/market_data/book_ticker_shm_reader.cpp`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: Add reader CLI**

Create a CLI with:

```text
--shm-name
--channel-name
--from-latest
--from-earliest
--max-messages
```

Default behavior: attach with `create=false`, seek latest, and run until interrupted.

- [ ] **Step 2: Print received book tickers with fmt**

Use `fmt::print` and `magic_enum::enum_name(book_ticker.exchange)` to print:

```text
id symbol_id exchange exchange_ns local_ns bid_price bid_volume ask_price ask_volume overrun_count
```

- [ ] **Step 3: Build reader tool**

Run:

```bash
cmake --build build/debug --target book_ticker_shm_reader
```

Expected: build exits 0.

- [ ] **Step 4: Commit reader tool**

Run:

```bash
git add tools/market_data/book_ticker_shm_reader.cpp tools/CMakeLists.txt
git commit -m "feat: add book ticker shm reader tool"
```

### Task 6: Add SHM Microbenchmark

**Files:**
- Create: `benchmark/core/market_data/data_shm_benchmark.cpp`
- Create: `benchmark/core/market_data/CMakeLists.txt`
- Modify: `benchmark/CMakeLists.txt`

- [ ] **Step 1: Add publisher benchmark**

Benchmark `DataShmPublisher::OnBookTicker()` with a fixed `BookTicker` and a unique SHM name per benchmark process.
This benchmark must measure the hot path without heartbeat timestamp updates.

- [ ] **Step 2: Add reader benchmark**

Pre-fill the queue, seek earliest visible, and benchmark `BookTickerShmReader::TryReadOne()` until the visible window is consumed.

- [ ] **Step 3: Build and run benchmark**

Run:

```bash
cmake --build build/release --target data_shm_benchmark
./build/release/benchmark/core/market_data/data_shm_benchmark
```

Expected: benchmark exits 0 and prints publisher/reader timings.

- [ ] **Step 4: Commit benchmark**

Run:

```bash
git add benchmark/core/market_data/data_shm_benchmark.cpp benchmark/core/market_data/CMakeLists.txt benchmark/CMakeLists.txt
git commit -m "benchmark: add data shm benchmark"
```

### Task 7: Final Verification and Documentation Sync

**Files:**
- Modify: `doc/data_session_shm_communication_design.md`

- [ ] **Step 1: Run focused tests**

Run:

```bash
cmake --build build/debug --target core_market_data_shm_test data_session_config_test gate_data_session_test binance_data_session_test gate_data_session binance_data_session book_ticker_shm_reader
./build/debug/test/core/market_data/core_market_data_shm_test
./build/debug/test/config/data_session_config_test
./build/debug/test/exchange/gate/market_data/gate_data_session_test
./build/debug/test/exchange/binance/market_data/binance_data_session_test
```

Expected: all tests exit 0.

- [ ] **Step 2: Run benchmark once in release**

Run:

```bash
cmake --build build/release --target data_shm_benchmark
./build/release/benchmark/core/market_data/data_shm_benchmark
```

Expected: benchmark exits 0. Record the exact output in the final implementation summary.

- [ ] **Step 3: Check docs and whitespace**

Run:

```bash
git diff --check
```

Expected: no output and exit 0.

- [ ] **Step 4: Commit doc sync if needed**

Run:

```bash
git add doc/data_session_shm_communication_design.md
git commit -m "docs: sync data shm implementation notes"
```

Skip this commit only when the design doc has no implementation-sync diff.
