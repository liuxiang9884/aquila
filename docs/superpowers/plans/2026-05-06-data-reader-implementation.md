# Data Reader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 strategy 侧 `DataReader`，从 Gate 和 Binance 的 book ticker SHM channel 非阻塞读取行情。

**Architecture:** `DataReader` 不拥有线程，strategy loop 主动调用 `Poll(handler)`。配置在启动冷路径解析，runtime 持有多个 `BookTickerShmReader` source；`read_mode=latest` 每个 source 每次 poll 最多产出最新一条，`read_mode=drain` 每个 source 每次 poll 最多产出 `max_events_per_source` 条。诊断统计通过编译期 policy 开关，生产默认 no-op。

**Tech Stack:** C++20, CMake, toml++, Nova SHM / broadcast queue, GoogleTest, CLI11, fmtlib, magic_enum.

---

## 当前配置

`config/data_readers/strategy_data_reader.toml` 已采用第一版目标结构：

```toml
[data_reader]
name = "strategy_data_reader"
max_events_per_source = 64

[data_reader.execution_policy]
bind_cpu_id = 4
idle_policy = "spin"

[[data_reader.sources]]
name = "gate_book_ticker"
type = "shm"
exchange = "gate"
feed = "book_ticker"
shm_name = "aquila_gate_market_data"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "latest"
required = true

[[data_reader.sources]]
name = "binance_book_ticker"
type = "shm"
exchange = "binance"
feed = "book_ticker"
shm_name = "aquila_binance_market_data"
channel_name = "book_ticker_channel"
start_position = "latest"
read_mode = "latest"
required = true
```

## 文件结构

- Create `core/config/data_reader_config.h`: data reader 配置结构、枚举和 loader API。
- Create `core/config/data_reader_config.cpp`: TOML 解析、字段校验、相对 instrument catalog 路径解析。
- Modify `core/config/CMakeLists.txt`: 把 data reader config parser 加入 `aquila_config`。
- Create `test/config/data_reader_config_test.cpp`: 配置解析和非法配置测试。
- Modify `test/config/CMakeLists.txt`: 添加 `data_reader_config_test`。
- Modify `core/market_data/data_shm.h`: 为 `BookTickerShmReader` 增加 `TryReadLatest()`。
- Modify `test/core/market_data/data_shm_test.cpp`: 覆盖 latest 读取和 skipped 计数。
- Create `core/market_data/data_reader.h`: `DataReader` runtime、source 管理、read mode 分发和 diagnostics policy。
- Create `test/core/market_data/data_reader_test.cpp`: 多 source poll、latest/drain 语义、公平性和 no-op diagnostics 测试。
- Modify `test/core/market_data/CMakeLists.txt`: 添加 `core_market_data_reader_test`。
- Create `tools/market_data/data_reader_probe.cpp`: 用真实 config attach SHM，低频打印 stats。
- Modify `tools/CMakeLists.txt`: 添加 `data_reader_probe`。
- Create `doc/data_reader_config.md`: 记录 TOML 字段和 runtime 语义。
- Modify `doc/project_onboarding_guide.md`: 增加 data reader 代码入口和验证命令。

---

### Task 1: Data Reader Config Parser

**Files:**
- Create: `core/config/data_reader_config.h`
- Create: `core/config/data_reader_config.cpp`
- Modify: `core/config/CMakeLists.txt`
- Create: `test/config/data_reader_config_test.cpp`
- Modify: `test/config/CMakeLists.txt`

- [ ] **Step 1: 写配置结构和 API 壳**

`core/config/data_reader_config.h`：

```cpp
#ifndef AQUILA_CORE_CONFIG_DATA_READER_CONFIG_H_
#define AQUILA_CORE_CONFIG_DATA_READER_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "core/common/result.h"
#include "core/common/types.h"
#include "core/config/instrument_catalog.h"

namespace aquila::config {

enum class DataReaderSourceType : std::uint8_t {
  kShm,
};

enum class DataReaderFeed : std::uint8_t {
  kBookTicker,
};

enum class DataReaderStartPosition : std::uint8_t {
  kLatest,
  kEarliestVisible,
};

enum class DataReaderReadMode : std::uint8_t {
  kLatest,
  kDrain,
};

struct DataReaderSourceConfig {
  std::string name;
  DataReaderSourceType type{DataReaderSourceType::kShm};
  Exchange exchange{Exchange::kGate};
  DataReaderFeed feed{DataReaderFeed::kBookTicker};
  std::string shm_name;
  std::string channel_name;
  DataReaderStartPosition start_position{DataReaderStartPosition::kLatest};
  DataReaderReadMode read_mode{DataReaderReadMode::kLatest};
  bool required{true};
};

struct DataReaderExecutionPolicyConfig {
  std::int32_t bind_cpu_id{-1};
  std::string idle_policy{"spin"};
};

struct DataReaderConfig {
  std::string name;
  std::uint32_t max_events_per_source{64};
  DataReaderExecutionPolicyConfig execution_policy;
  InstrumentCatalog instrument_catalog;
  std::vector<DataReaderSourceConfig> sources;
};

using DataReaderConfigResult = Result<DataReaderConfig>;

[[nodiscard]] DataReaderConfigResult ParseDataReaderConfig(
    const toml::table& node);

[[nodiscard]] DataReaderConfigResult ParseDataReaderConfig(
    const toml::table& node, const std::filesystem::path& config_file_path);

[[nodiscard]] DataReaderConfigResult LoadDataReaderConfigFile(
    const std::filesystem::path& path);

}  // namespace aquila::config

#endif  // AQUILA_CORE_CONFIG_DATA_READER_CONFIG_H_
```

- [ ] **Step 2: 写失败测试：真实配置可解析**

`test/config/data_reader_config_test.cpp` 先写：

```cpp
#include "core/config/data_reader_config.h"

#include <filesystem>
#include <string_view>

#include <gtest/gtest.h>

namespace {

std::filesystem::path SourcePath(std::string_view path) {
  return std::filesystem::path{AQUILA_SOURCE_DIR} / path;
}

TEST(DataReaderConfigTest, LoadsReadyStrategyDataReaderConfig) {
  const auto result = aquila::config::LoadDataReaderConfigFile(
      SourcePath("config/data_readers/strategy_data_reader.toml"));

  ASSERT_TRUE(result.ok) << result.error;
  const aquila::config::DataReaderConfig& config = result.value;

  EXPECT_EQ(config.name, "strategy_data_reader");
  EXPECT_EQ(config.max_events_per_source, 64U);
  EXPECT_EQ(config.execution_policy.bind_cpu_id, 4);
  EXPECT_EQ(config.execution_policy.idle_policy, "spin");

  ASSERT_EQ(config.sources.size(), 2U);
  EXPECT_EQ(config.sources[0].name, "gate_book_ticker");
  EXPECT_EQ(config.sources[0].type,
            aquila::config::DataReaderSourceType::kShm);
  EXPECT_EQ(config.sources[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(config.sources[0].feed, aquila::config::DataReaderFeed::kBookTicker);
  EXPECT_EQ(config.sources[0].shm_name, "aquila_gate_market_data");
  EXPECT_EQ(config.sources[0].channel_name, "book_ticker_channel");
  EXPECT_EQ(config.sources[0].start_position,
            aquila::config::DataReaderStartPosition::kLatest);
  EXPECT_EQ(config.sources[0].read_mode,
            aquila::config::DataReaderReadMode::kLatest);
  EXPECT_TRUE(config.sources[0].required);

  EXPECT_EQ(config.sources[1].name, "binance_book_ticker");
  EXPECT_EQ(config.sources[1].exchange, aquila::Exchange::kBinance);
  EXPECT_EQ(config.sources[1].shm_name, "aquila_binance_market_data");
}

}  // namespace
```

- [ ] **Step 3: 把测试 target 加入 CMake 并确认失败**

Modify `test/config/CMakeLists.txt`：

```cmake
add_executable(data_reader_config_test
    data_reader_config_test.cpp
)

target_link_libraries(data_reader_config_test
    PRIVATE
        aquila_config
        GTest::gtest_main
        nova
)

target_compile_definitions(data_reader_config_test
    PRIVATE AQUILA_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

add_test(NAME data_reader_config_test
         COMMAND data_reader_config_test)
```

Run:

```bash
cmake --build build/debug --target data_reader_config_test
```

Expected: build fails because `core/config/data_reader_config.cpp` is not implemented or not linked.

- [ ] **Step 4: 实现 parser 最小通过路径**

Add `data_reader_config.cpp` to `core/config/CMakeLists.txt`:

```cmake
add_library(aquila_config STATIC
    data_reader_config.cpp
    data_reader_config.h
    instrument_catalog.cpp
    instrument_catalog.h
    websocket_config.cpp
    websocket_config.h
)
```

Implement `core/config/data_reader_config.cpp` with these parsing rules:

```cpp
Parse root["instrument_catalog"].file/schema.
Resolve relative catalog path against config_file_path.parent_path().
LoadInstrumentCatalogFromCsv(catalog_path).
Parse [data_reader].name, max_events_per_source.
Parse [data_reader.execution_policy].bind_cpu_id, idle_policy.
Parse [[data_reader.sources]] array.
For each source:
  type must be "shm".
  exchange must be "gate" or "binance".
  feed must be "book_ticker".
  start_position must be "latest" or "earliest_visible".
  read_mode must be "latest" or "drain".
  shm_name and channel_name are required for type="shm".
  required defaults to true.
Reject empty sources.
Reject duplicate source names.
Reject max_events_per_source == 0.
```

Use `Result<DataReaderConfig>` consistently with existing config parsers:

```cpp
[[nodiscard]] DataReaderConfigResult Failure(std::string error) {
  DataReaderConfigResult result;
  result.error = std::move(error);
  return result;
}

[[nodiscard]] DataReaderConfigResult Success(DataReaderConfig config) {
  DataReaderConfigResult result;
  result.value = std::move(config);
  result.ok = true;
  return result;
}
```

- [ ] **Step 5: 验证 parser 通过**

Run:

```bash
cmake --build build/debug --target data_reader_config_test
./build/debug/test/config/data_reader_config_test
```

Expected: `LoadsReadyStrategyDataReaderConfig` passes.

- [ ] **Step 6: 增加非法配置测试**

Add these tests in `test/config/data_reader_config_test.cpp`:

- `RejectsDuplicateSourceNames`: build TOML with two `[[data_reader.sources]]` entries both using `name = "dup"`; assert `result.ok == false` and `result.error` contains `data_reader.sources.name`.
- `RejectsUnsupportedReadMode`: build TOML with `read_mode = "all"`; assert `result.ok == false` and `result.error` contains `read_mode`.
- `RejectsMissingShmNameForShmSource`: build TOML with `type = "shm"` and no `shm_name`; assert `result.ok == false` and `result.error` contains `shm_name`.
- `RejectsZeroMaxEventsPerSource`: build TOML with `max_events_per_source = 0`; assert `result.ok == false` and `result.error` contains `data_reader.max_events_per_source`.
- `RejectsEmptySources`: build TOML with `[data_reader]` but no `[[data_reader.sources]]`; assert `result.ok == false` and `result.error` contains `data_reader.sources`.

Each test should assert the relevant field name appears in `result.error`:

```cpp
EXPECT_NE(result.error.find("data_reader.max_events_per_source"),
          std::string::npos);
```

- [ ] **Step 7: 运行并提交**

Run:

```bash
cmake --build build/debug --target data_reader_config_test
./build/debug/test/config/data_reader_config_test
git diff --check
```

Commit:

```bash
git add core/config/data_reader_config.h core/config/data_reader_config.cpp core/config/CMakeLists.txt test/config/data_reader_config_test.cpp test/config/CMakeLists.txt
git commit -m "feat: parse data reader config"
```

---

### Task 2: BookTickerShmReader Latest Read

**Files:**
- Modify: `core/market_data/data_shm.h`
- Modify: `test/core/market_data/data_shm_test.cpp`

- [ ] **Step 1: 写失败测试：latest 只返回最后一条**

Add to `test/core/market_data/data_shm_test.cpp`:

```cpp
TEST(DataShmTest, ReaderTryReadLatestReturnsLastVisibleBookTicker) {
  const md::BookTickerShmConfig config = MakeCreateConfig("read_latest");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  publisher.OnBookTicker(MakeBookTicker(10));
  publisher.OnBookTicker(MakeBookTicker(11));
  publisher.OnBookTicker(MakeBookTicker(12));

  aquila::BookTicker actual{};
  std::uint64_t skipped{0};
  ASSERT_TRUE(reader.TryReadLatest(&actual, &skipped));
  EXPECT_EQ(actual.id, 12);
  EXPECT_EQ(skipped, 2U);
  EXPECT_FALSE(reader.TryReadLatest(&actual, &skipped));
}
```

- [ ] **Step 2: 运行失败测试**

Run:

```bash
cmake --build build/debug --target core_market_data_shm_test
```

Expected: build fails because `BookTickerShmReader::TryReadLatest()` does not exist.

- [ ] **Step 3: 实现 `TryReadLatest()`**

Add to `BookTickerShmReader` in `core/market_data/data_shm.h`:

```cpp
[[nodiscard]] bool TryReadLatest(aquila::BookTicker* out,
                                 std::uint64_t* skipped_count) noexcept {
  const auto current = manager_.channel().queue.Current();
  if (read_pos_ == current) {
    if (skipped_count != nullptr) {
      *skipped_count = 0;
    }
    return false;
  }

  const auto capacity = manager_.channel().queue.capacity();
  const auto unread_count = current - read_pos_;
  if (unread_count > capacity) {
    read_pos_ = current - capacity;
    ++overrun_count_;
  }

  const auto visible_unread_count = current - read_pos_;
  const auto latest_pos = current - 1;
  *out = manager_.channel().queue.Value(latest_pos);
  read_pos_ = current;

  if (skipped_count != nullptr) {
    *skipped_count = visible_unread_count > 0 ? visible_unread_count - 1 : 0;
  }
  return true;
}
```

- [ ] **Step 4: 增加 overrun + latest 测试**

Add:

```cpp
TEST(DataShmTest, ReaderTryReadLatestCountsOverrunAndSkipsToLast) {
  const md::BookTickerShmConfig config =
      MakeCreateConfig("read_latest_overrun");
  ShmCleanup cleanup(config.shm_name);

  md::DataShmPublisher publisher(config);
  md::BookTickerShmReader reader(MakeAttachConfig(config));
  reader.SeekLatest();

  for (std::uint64_t i = 0; i < md::kBookTickerShmCapacity + 3; ++i) {
    publisher.OnBookTicker(MakeBookTicker(static_cast<std::int64_t>(i)));
  }

  aquila::BookTicker actual{};
  std::uint64_t skipped{0};
  ASSERT_TRUE(reader.TryReadLatest(&actual, &skipped));
  EXPECT_EQ(actual.id, static_cast<std::int64_t>(md::kBookTickerShmCapacity + 2));
  EXPECT_EQ(reader.overrun_count(), 1U);
  EXPECT_EQ(skipped, md::kBookTickerShmCapacity - 1);
}
```

- [ ] **Step 5: 运行并提交**

Run:

```bash
cmake --build build/debug --target core_market_data_shm_test
./build/debug/test/core/market_data/core_market_data_shm_test
git diff --check
```

Commit:

```bash
git add core/market_data/data_shm.h test/core/market_data/data_shm_test.cpp
git commit -m "feat: read latest book ticker from shm"
```

---

### Task 3: DataReader Runtime

**Files:**
- Create: `core/market_data/data_reader.h`
- Create: `test/core/market_data/data_reader_test.cpp`
- Modify: `test/core/market_data/CMakeLists.txt`

- [ ] **Step 1: 写 runtime API 壳**

Create `core/market_data/data_reader.h`:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_DATA_READER_H_
#define AQUILA_CORE_MARKET_DATA_DATA_READER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_shm.h"

namespace aquila::market_data {

struct NoopDataReaderDiagnostics {};

template <typename Diagnostics = NoopDataReaderDiagnostics>
class DataReader {
 public:
  explicit DataReader(config::DataReaderConfig config);

  template <typename Handler>
  std::uint64_t Poll(Handler& handler) noexcept;

  [[nodiscard]] const Diagnostics& diagnostics() const noexcept {
    return diagnostics_;
  }

 private:
  struct Source {
    config::DataReaderSourceConfig config;
    BookTickerShmReader reader;
  };

  template <typename Handler>
  std::uint64_t PollLatestSource(std::size_t source_index, Source& source,
                                 Handler& handler) noexcept;

  template <typename Handler>
  std::uint64_t PollDrainSource(std::size_t source_index, Source& source,
                                Handler& handler) noexcept;

  std::uint32_t max_events_per_source_{64};
  std::size_t next_source_index_{0};
  std::vector<std::unique_ptr<Source>> sources_;
  [[no_unique_address]] Diagnostics diagnostics_{};
};

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_DATA_READER_H_
```

- [ ] **Step 2: 写失败测试：两个 source 都能 poll 到**

Create `test/core/market_data/data_reader_test.cpp` with helpers copied locally from `data_shm_test.cpp`: `ShmCleanup`, `UniqueShmName`, `MakeCreateConfig`, `MakeBookTicker`, `ExpectBookTickerEq`.

Add test:

```cpp
TEST(DataReaderTest, PollReadsLatestBookTickerFromTwoSources) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("gate");
  const md::BookTickerShmConfig binance_config = MakeCreateConfig("binance");
  ShmCleanup gate_cleanup(gate_config.shm_name);
  ShmCleanup binance_cleanup(binance_config.shm_name);

  md::DataShmPublisher gate_publisher(gate_config);
  md::DataShmPublisher binance_publisher(binance_config);

  aquila::config::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_source = 64;
  config.sources.push_back(MakeSourceConfig(
      "gate_book_ticker", aquila::Exchange::kGate, gate_config.shm_name,
      aquila::config::DataReaderReadMode::kLatest));
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", aquila::Exchange::kBinance,
      binance_config.shm_name, aquila::config::DataReaderReadMode::kLatest));

  md::DataReader reader(std::move(config));
  gate_publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  binance_publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kBinance));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 2U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[0].exchange, aquila::Exchange::kGate);
  EXPECT_EQ(handler.book_tickers[1].exchange, aquila::Exchange::kBinance);
}
```

- [ ] **Step 3: 添加 CMake target 并确认失败**

Modify `test/core/market_data/CMakeLists.txt`:

```cmake
add_executable(core_market_data_reader_test
    data_reader_test.cpp
)

target_link_libraries(core_market_data_reader_test
    PRIVATE
        aquila_config
        aquila_core
        fmt::fmt-header-only
        GTest::gtest_main
        nova
)

add_test(NAME core_market_data_reader_test
         COMMAND core_market_data_reader_test)
```

Run:

```bash
cmake --build build/debug --target core_market_data_reader_test
```

Expected: build fails because `DataReader` constructor and `Poll()` are not implemented.

- [ ] **Step 4: 实现 constructor**

In `core/market_data/data_reader.h`, constructor initializes diagnostics from the configured source count and creates attach config for each source. Use `std::unique_ptr<Source>` so the implementation does not require `BookTickerShmReader` or its SHM manager to be movable:

```cpp
explicit DataReader(config::DataReaderConfig config)
    : max_events_per_source_(config.max_events_per_source),
      diagnostics_(config.sources.size()) {
  sources_.reserve(config.sources.size());
  for (config::DataReaderSourceConfig& source_config : config.sources) {
    BookTickerShmConfig shm_config{
        .enabled = true,
        .shm_name = source_config.shm_name,
        .channel_name = source_config.channel_name,
        .create = false,
        .remove_existing = false,
    };

    auto source = std::make_unique<Source>(
        Source{.config = std::move(source_config),
               .reader = BookTickerShmReader(shm_config)});

    if (source->config.start_position ==
        config::DataReaderStartPosition::kLatest) {
      source->reader.SeekLatest();
    } else {
      source->reader.SeekEarliestVisible();
    }

    sources_.push_back(std::move(source));
  }
}
```

If aggregate initialization with `BookTickerShmReader` does not compile, give `Source` an explicit constructor:

```cpp
struct Source {
  Source(config::DataReaderSourceConfig source_config,
         const BookTickerShmConfig& shm_config)
      : config(std::move(source_config)), reader(shm_config) {}

  config::DataReaderSourceConfig config;
  BookTickerShmReader reader;
  std::uint64_t last_overrun_count{0};
};
```

Then construct with:

```cpp
auto source = std::make_unique<Source>(std::move(source_config), shm_config);
```

The attach config values are:

```cpp
BookTickerShmConfig shm_config{
    .enabled = true,
    .shm_name = source_config.shm_name,
    .channel_name = source_config.channel_name,
    .create = false,
    .remove_existing = false,
};
```

Store `max_events_per_source_ = config.max_events_per_source`.

- [ ] **Step 5: 实现 switch 分发的 `Poll()`**

Use this shape exactly; do not use an outer drain loop:

```cpp
template <typename Diagnostics>
template <typename Handler>
std::uint64_t DataReader<Diagnostics>::Poll(Handler& handler) noexcept {
  if (sources_.empty()) {
    return 0;
  }

  std::uint64_t handled = 0;
  const std::size_t source_count = sources_.size();
  for (std::size_t checked = 0; checked < source_count; ++checked) {
    const std::size_t index =
        (next_source_index_ + checked) % source_count;
    Source& source = *sources_[index];

    switch (source.config.read_mode) {
      case config::DataReaderReadMode::kLatest:
        handled += PollLatestSource(index, source, handler);
        break;
      case config::DataReaderReadMode::kDrain:
        handled += PollDrainSource(index, source, handler);
        break;
    }
  }

  next_source_index_ = (next_source_index_ + 1) % source_count;
  return handled;
}
```

Implement latest path without loop:

```cpp
template <typename Diagnostics>
template <typename Handler>
std::uint64_t DataReader<Diagnostics>::PollLatestSource(
    std::size_t source_index, Source& source, Handler& handler) noexcept {
  BookTicker book_ticker{};
  std::uint64_t skipped{0};
  if (!source.reader.TryReadLatest(&book_ticker, &skipped)) {
    return 0;
  }
  if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
    diagnostics_.RecordBookTicker(source_index, book_ticker);
    diagnostics_.RecordSkipped(source_index, skipped);
    const std::uint64_t overrun = source.reader.overrun_count();
    diagnostics_.RecordOverrun(source_index,
                               overrun - source.last_overrun_count);
    source.last_overrun_count = overrun;
  }
  handler.OnBookTicker(book_ticker);
  return 1;
}
```

Implement drain path with per-source limit:

```cpp
template <typename Diagnostics>
template <typename Handler>
std::uint64_t DataReader<Diagnostics>::PollDrainSource(
    std::size_t source_index, Source& source, Handler& handler) noexcept {
  std::uint64_t handled = 0;
  while (handled < max_events_per_source_) {
    BookTicker book_ticker{};
    if (!source.reader.TryReadOne(&book_ticker)) {
      break;
    }
    if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
      diagnostics_.RecordBookTicker(source_index, book_ticker);
      const std::uint64_t overrun = source.reader.overrun_count();
      diagnostics_.RecordOverrun(source_index,
                                 overrun - source.last_overrun_count);
      source.last_overrun_count = overrun;
    }
    handler.OnBookTicker(book_ticker);
    ++handled;
  }
  return handled;
}
```

- [ ] **Step 6: 增加 drain limit 测试**

Add:

```cpp
TEST(DataReaderTest, DrainReadsAtMostMaxEventsPerSource) {
  const md::BookTickerShmConfig gate_config = MakeCreateConfig("drain_limit");
  ShmCleanup cleanup(gate_config.shm_name);
  md::DataShmPublisher publisher(gate_config);

  aquila::config::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_source = 2;
  config.sources.push_back(MakeSourceConfig(
      "gate_book_ticker", aquila::Exchange::kGate, gate_config.shm_name,
      aquila::config::DataReaderReadMode::kDrain));

  md::DataReader reader(std::move(config));
  publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(3, aquila::Exchange::kGate));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 2U);
  ASSERT_EQ(handler.book_tickers.size(), 2U);
  EXPECT_EQ(handler.book_tickers[0].id, 1);
  EXPECT_EQ(handler.book_tickers[1].id, 2);

  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].id, 3);
}
```

- [ ] **Step 7: 增加 latest skips intermediate 测试**

Add:

```cpp
TEST(DataReaderTest, LatestReadsOnlyLastBookTickerPerSource) {
  const md::BookTickerShmConfig binance_config =
      MakeCreateConfig("latest_only");
  ShmCleanup cleanup(binance_config.shm_name);
  md::DataShmPublisher publisher(binance_config);

  aquila::config::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_source = 64;
  config.sources.push_back(MakeSourceConfig(
      "binance_book_ticker", aquila::Exchange::kBinance,
      binance_config.shm_name, aquila::config::DataReaderReadMode::kLatest));

  md::DataReader reader(std::move(config));
  publisher.OnBookTicker(MakeBookTicker(10, aquila::Exchange::kBinance));
  publisher.OnBookTicker(MakeBookTicker(11, aquila::Exchange::kBinance));
  publisher.OnBookTicker(MakeBookTicker(12, aquila::Exchange::kBinance));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  ASSERT_EQ(handler.book_tickers.size(), 1U);
  EXPECT_EQ(handler.book_tickers[0].id, 12);

  handler.book_tickers.clear();
  EXPECT_EQ(reader.Poll(handler), 0U);
  EXPECT_TRUE(handler.book_tickers.empty());
}
```

- [ ] **Step 8: 运行并提交**

Run:

```bash
cmake --build build/debug --target core_market_data_reader_test core_market_data_shm_test
./build/debug/test/core/market_data/core_market_data_reader_test
./build/debug/test/core/market_data/core_market_data_shm_test
git diff --check
```

Commit:

```bash
git add core/market_data/data_reader.h test/core/market_data/data_reader_test.cpp test/core/market_data/CMakeLists.txt
git commit -m "feat: add data reader shm polling"
```

---

### Task 4: Compile-Time Diagnostics Policy

**Files:**
- Modify: `core/market_data/data_reader.h`
- Modify: `test/core/market_data/data_reader_test.cpp`

- [ ] **Step 1: 增加 diagnostics 类型**

Add to `core/market_data/data_reader.h`:

```cpp
struct DataReaderSourceStats {
  std::uint64_t book_tickers{0};
  std::uint64_t skipped{0};
  std::uint64_t overruns{0};
  std::int64_t last_book_ticker_id{0};
};

struct DataReaderStats {
  std::uint64_t poll_calls{0};
  std::uint64_t empty_polls{0};
  std::uint64_t book_tickers{0};
  std::vector<DataReaderSourceStats> sources;
};

struct NoopDataReaderDiagnostics {
  explicit NoopDataReaderDiagnostics(std::size_t source_count) noexcept {
    (void)source_count;
  }
  void RecordPoll() noexcept {}
  void RecordEmptyPoll() noexcept {}
  void RecordBookTicker(std::size_t, const BookTicker&) noexcept {}
  void RecordSkipped(std::size_t, std::uint64_t) noexcept {}
  void RecordOverrun(std::size_t, std::uint64_t) noexcept {}
};

class DataReaderDiagnostics {
 public:
  explicit DataReaderDiagnostics(std::size_t source_count)
      : stats_{.sources = std::vector<DataReaderSourceStats>(source_count)} {}

  void RecordPoll() noexcept { ++stats_.poll_calls; }
  void RecordEmptyPoll() noexcept { ++stats_.empty_polls; }
  void RecordBookTicker(std::size_t source_index,
                        const BookTicker& book_ticker) noexcept {
    ++stats_.book_tickers;
    ++stats_.sources[source_index].book_tickers;
    stats_.sources[source_index].last_book_ticker_id = book_ticker.id;
  }
  void RecordSkipped(std::size_t source_index, std::uint64_t skipped) noexcept {
    stats_.sources[source_index].skipped += skipped;
  }
  void RecordOverrun(std::size_t source_index,
                     std::uint64_t overrun_delta) noexcept {
    stats_.sources[source_index].overruns += overrun_delta;
  }
  [[nodiscard]] const DataReaderStats& stats() const noexcept {
    return stats_;
  }

 private:
  DataReaderStats stats_{};
};
```

Construct diagnostics after sources are known:

```cpp
[[no_unique_address]] Diagnostics diagnostics_;
```

Initialize `diagnostics_(config.sources.size())` in the `DataReader` constructor member list before moving `config.sources` into runtime sources. Keep `NoopDataReaderDiagnostics` zero hot-path behavior.

- [ ] **Step 2: 记录 latest/drain stats**

In `Poll()`, wrap diagnostics calls with compile-time checks:

```cpp
if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
  diagnostics_.RecordPoll();
}
```

After each emitted `BookTicker`:

```cpp
if constexpr (!std::is_same_v<Diagnostics, NoopDataReaderDiagnostics>) {
  diagnostics_.RecordBookTicker(source_index, book_ticker);
  diagnostics_.RecordSkipped(source_index, skipped);
  const std::uint64_t overrun = source.reader.overrun_count();
  diagnostics_.RecordOverrun(source_index, overrun - source.last_overrun_count);
  source.last_overrun_count = overrun;
}
```

Add `std::uint64_t last_overrun_count{0};` to `Source`.

- [ ] **Step 3: 写 diagnostics 测试**

Add:

```cpp
TEST(DataReaderTest, DiagnosticsTrackBookTickersSkippedAndEmptyPolls) {
  using Reader = md::DataReader<md::DataReaderDiagnostics>;
  const md::BookTickerShmConfig config = MakeCreateConfig("diag_latest");
  ShmCleanup cleanup(config.shm_name);
  md::DataShmPublisher publisher(config);

  aquila::config::DataReaderConfig reader_config;
  reader_config.name = "test_data_reader";
  reader_config.max_events_per_source = 64;
  reader_config.sources.push_back(MakeSourceConfig(
      "gate_book_ticker", aquila::Exchange::kGate, config.shm_name,
      aquila::config::DataReaderReadMode::kLatest));

  Reader reader(std::move(reader_config));
  publisher.OnBookTicker(MakeBookTicker(1, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(2, aquila::Exchange::kGate));
  publisher.OnBookTicker(MakeBookTicker(3, aquila::Exchange::kGate));

  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 1U);
  EXPECT_EQ(reader.Poll(handler), 0U);

  const md::DataReaderStats& stats = reader.diagnostics().stats();
  EXPECT_EQ(stats.poll_calls, 2U);
  EXPECT_EQ(stats.empty_polls, 1U);
  EXPECT_EQ(stats.book_tickers, 1U);
  ASSERT_EQ(stats.sources.size(), 1U);
  EXPECT_EQ(stats.sources[0].book_tickers, 1U);
  EXPECT_EQ(stats.sources[0].skipped, 2U);
  EXPECT_EQ(stats.sources[0].last_book_ticker_id, 3);
}
```

Add a compile smoke test for no-op default:

```cpp
TEST(DataReaderTest, DefaultDiagnosticsCompileAndPoll) {
  aquila::config::DataReaderConfig config;
  config.name = "test_data_reader";
  config.max_events_per_source = 64;
  md::DataReader reader(std::move(config));
  RecordingHandler handler;
  EXPECT_EQ(reader.Poll(handler), 0U);
}
```

- [ ] **Step 4: 运行并提交**

Run:

```bash
cmake --build build/debug --target core_market_data_reader_test
./build/debug/test/core/market_data/core_market_data_reader_test
git diff --check
```

Commit:

```bash
git add core/market_data/data_reader.h test/core/market_data/data_reader_test.cpp
git commit -m "feat: add data reader diagnostics policy"
```

---

### Task 5: Data Reader Probe Tool

**Files:**
- Create: `tools/market_data/data_reader_probe.cpp`
- Modify: `tools/CMakeLists.txt`

- [ ] **Step 1: 创建 probe 工具**

Create `tools/market_data/data_reader_probe.cpp`:

```cpp
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <thread>

#include <CLI/CLI.hpp>
#include <fmt/format.h>
#include <toml++/toml.hpp>

#include "core/config/data_reader_config.h"
#include "core/market_data/data_reader.h"
#include "nova/utils/log.h"

namespace {

volatile std::sig_atomic_t g_stop_requested = 0;

void HandleSignal(int signal) {
  (void)signal;
  g_stop_requested = 1;
}

struct CountingHandler {
  void OnBookTicker(const aquila::BookTicker& book_ticker) noexcept {
    (void)book_ticker;
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path config_path{
      "config/data_readers/strategy_data_reader.toml"};
  std::uint64_t max_polls{0};

  CLI::App app{"Data reader probe"};
  app.add_option("--config", config_path, "data reader TOML path");
  app.add_option("--max-polls", max_polls, "0 means unlimited");
  CLI11_PARSE(app, argc, argv);

  const toml::parse_result toml = toml::parse_file(config_path.string());
  nova::LogConfig log_config;
  log_config.FromToml(toml["log"]);
  nova::InitializeLogging(log_config);

  const auto config_result =
      aquila::config::ParseDataReaderConfig(toml, config_path);
  if (!config_result.ok) {
    NOVA_ERROR("config_error={}", config_result.error);
    nova::StopLogging();
    return 1;
  }

  using Reader = aquila::market_data::DataReader<
      aquila::market_data::DataReaderDiagnostics>;
  Reader reader(std::move(config_result.value));
  CountingHandler handler;

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  std::uint64_t polls{0};
  while (g_stop_requested == 0 &&
         (max_polls == 0 || polls < max_polls)) {
    const std::uint64_t handled = reader.Poll(handler);
    ++polls;
    if (handled == 0) {
      std::this_thread::yield();
    }
  }

  const auto& stats = reader.diagnostics().stats();
  NOVA_INFO("result=ok polls={} book_tickers={} empty_polls={}", polls,
            stats.book_tickers, stats.empty_polls);
  nova::StopLogging();
  return 0;
}
```

- [ ] **Step 2: 添加 CMake target**

Modify `tools/CMakeLists.txt`:

```cmake
add_executable(data_reader_probe
    market_data/data_reader_probe.cpp
)

target_link_libraries(data_reader_probe
    PRIVATE
        aquila_config
        aquila_core
        CLI11::CLI11
        fmt::fmt-header-only
        nova
)
```

- [ ] **Step 3: 构建并 dry-run attach 失败路径**

Run when SHM producers are not running:

```bash
cmake --build build/debug --target data_reader_probe
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 1
```

Expected: exits non-zero with an attach/open error if Gate/Binance SHM is not present. This confirms config parser and attach path are wired.

- [ ] **Step 4: 实盘 smoke 验证**

Start producers in separate terminals:

```bash
./build/debug/tools/gate_data_session --config config/data_sessions/gate_data_session.toml --connect
./build/debug/tools/binance_data_session --config config/data_sessions/binance_data_session.toml --connect
```

Then run:

```bash
./build/debug/tools/data_reader_probe --config config/data_readers/strategy_data_reader.toml --max-polls 100000
```

Expected: exits `0`, final log has `book_tickers > 0`, no process crash.

- [ ] **Step 5: 提交**

Run:

```bash
git diff --check
```

Commit:

```bash
git add tools/market_data/data_reader_probe.cpp tools/CMakeLists.txt
git commit -m "tools: add data reader probe"
```

---

### Task 6: Documentation and Final Verification

**Files:**
- Create: `doc/data_reader_config.md`
- Modify: `doc/project_onboarding_guide.md`

- [ ] **Step 1: 写 data reader 配置文档**

Create `doc/data_reader_config.md` with these sections:

```markdown
# Data Reader 配置说明

## 范围

`data_reader` 描述 strategy 侧行情输入。第一版只支持 `type = "shm"` 和 `feed = "book_ticker"`。

## 线程模型

`DataReader` 不创建线程，由 strategy loop 调用 `Poll(handler)`。

## 字段

- `[data_reader].name`
- `[data_reader].max_events_per_source`
- `[data_reader.execution_policy].bind_cpu_id`
- `[data_reader.execution_policy].idle_policy`
- `[[data_reader.sources]].name`
- `[[data_reader.sources]].type`
- `[[data_reader.sources]].exchange`
- `[[data_reader.sources]].feed`
- `[[data_reader.sources]].shm_name`
- `[[data_reader.sources]].channel_name`
- `[[data_reader.sources]].start_position`
- `[[data_reader.sources]].read_mode`
- `[[data_reader.sources]].required`

## read_mode

`latest` 每个 source 每次 poll 最多产出一条最新 book ticker，主动跳过中间数据。
`drain` 每个 source 每次 poll 最多产出 `max_events_per_source` 条，不主动跳到最后一条。
```

- [ ] **Step 2: 更新 onboarding**

Modify `doc/project_onboarding_guide.md` code-entry table to include:

```markdown
| `core/config/data_reader_config.h` | Strategy data reader TOML parser / loader，加载 instrument catalog，并生成 `DataReader` 可直接消费的 source config。 |
| `core/market_data/data_reader.h` | Strategy 侧 `DataReader`，从 Gate / Binance SHM book ticker source poll 行情。 |
| `tools/market_data/data_reader_probe.cpp` | 独立 data reader probe，按 `config/data_readers/strategy_data_reader.toml` attach 多个 SHM source 并输出低频统计。 |
```

- [ ] **Step 3: 最终验证**

Run:

```bash
cmake --build build/debug --target data_reader_config_test core_market_data_shm_test core_market_data_reader_test data_reader_probe
./build/debug/test/config/data_reader_config_test
./build/debug/test/core/market_data/core_market_data_shm_test
./build/debug/test/core/market_data/core_market_data_reader_test
git diff --check
```

Expected:

```text
data_reader_config_test: all tests pass
core_market_data_shm_test: all tests pass
core_market_data_reader_test: all tests pass
git diff --check: no output
```

- [ ] **Step 4: 提交文档**

Commit:

```bash
git add doc/data_reader_config.md doc/project_onboarding_guide.md
git commit -m "doc: document data reader config"
```

---

## Self-Review

- Spec coverage: plan covers TOML parser, `latest` SHM read, `drain` read, switch-case poll dispatch, compile-time diagnostics, probe tool, docs and final tests.
- Placeholder scan: no open placeholder markers remain.
- Type consistency: config enum names use `DataReaderSourceType`, `DataReaderFeed`, `DataReaderStartPosition`, and `DataReaderReadMode`; runtime consumes `config::DataReaderConfig` and `config::DataReaderSourceConfig`.
