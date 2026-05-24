# DataReader Live Record Smoke Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Run a reproducible Gate / Binance `BookTicker` SHM live record smoke, while improving DataReader tool logs so startup mode/source/output and exit diagnostics are explicit.

**Architecture:** Keep production reader hot paths unchanged. Add small tool-only helpers for formatting source/startup logs and probe mode detection, wire them into `data_reader_recorder` and `data_reader_probe`, then use `data_reader_probe` with `HistoricalDataReader` mode to validate the recorded binary. Live evidence and any blocker go into DataReader docs/onboarding after the run.

**Tech Stack:** C++20, CMake, GTest, CLI11, toml++, fmt, magic_enum, existing `RealtimeDataReader` / `HistoricalDataReader` / `BookTicker` SHM.

---

## File Structure

- Create `tools/market_data/data_reader_tool_logging.h`: tool-only inline helpers for `read_mode`, `start_position`, source config log lines, and startup lines.
- Create `tools/market_data/data_reader_probe_mode.h`: tool-only inline helper that chooses realtime SHM probe vs historical binary probe from `DataReaderConfig`.
- Modify `tools/market_data/data_reader_recorder.cpp`: reuse logging helpers and print a single final summary with stop reason and diagnostics-backed source stats on every handled exit path after reader construction.
- Modify `tools/market_data/data_reader_probe.cpp`: log startup mode/source config, support binary_file configs through `HistoricalDataReader<HistoricalDataReaderDiagnostics>`, and print diagnostics on exit.
- Create `test/tools/market_data/data_reader_tool_logging_test.cpp`: unit tests for stable log formatting.
- Create `test/tools/market_data/data_reader_probe_mode_test.cpp`: unit tests for probe mode selection and mixed-source rejection.
- Modify `test/tools/market_data/CMakeLists.txt`: add the two new tests.
- Modify `doc/data_reader_config.md`: record live record smoke command/result or the concrete live blocker.
- Modify `doc/project_onboarding_guide.md`: update DataReader next-step summary if live record smoke completes or is blocked by environment.

## Task 1: Logging Helper

**Files:**
- Create: `tools/market_data/data_reader_tool_logging.h`
- Create: `test/tools/market_data/data_reader_tool_logging_test.cpp`
- Modify: `test/tools/market_data/CMakeLists.txt`

- [ ] **Step 1: Write the failing logging helper test**

Add `test/tools/market_data/data_reader_tool_logging_test.cpp`:

```cpp
#include "tools/market_data/data_reader_tool_logging.h"

#include <filesystem>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "core/config/data_reader_config.h"

namespace {

namespace cfg = aquila::config;
namespace md_tools = aquila::tools::market_data;

cfg::DataReaderSourceConfig MakeShmSource() {
  return cfg::DataReaderSourceConfig{
      .name = "gate_book_ticker",
      .type = cfg::DataReaderSourceType::kShm,
      .exchange = aquila::Exchange::kGate,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = "aquila_gate_market_data",
      .channel_name = "book_ticker_channel",
      .files = {},
      .start_position = cfg::DataReaderStartPosition::kLatest,
      .read_mode = cfg::DataReaderReadMode::kDrain,
      .required = true,
  };
}

cfg::DataReaderSourceConfig MakeBinarySource() {
  return cfg::DataReaderSourceConfig{
      .name = "recorded_book_ticker",
      .type = cfg::DataReaderSourceType::kBinaryFile,
      .exchange = aquila::Exchange::kGate,
      .feed = cfg::DataReaderFeed::kBookTicker,
      .shm_name = {},
      .channel_name = {},
      .files = {std::filesystem::path{"/home/liuxiang/tmp/live.bin"}},
      .start_position = cfg::DataReaderStartPosition::kEarliestVisible,
      .read_mode = cfg::DataReaderReadMode::kDrain,
      .required = true,
  };
}

TEST(DataReaderToolLoggingTest, FormatsShmSourceConfig) {
  const std::string line = md_tools::FormatSourceConfigLog(2, MakeShmSource());
  EXPECT_NE(line.find("source_config index=2"), std::string::npos);
  EXPECT_NE(line.find("name=gate_book_ticker"), std::string::npos);
  EXPECT_NE(line.find("exchange=kGate"), std::string::npos);
  EXPECT_NE(line.find("type=kShm"), std::string::npos);
  EXPECT_NE(line.find("start_position=latest"), std::string::npos);
  EXPECT_NE(line.find("read_mode=drain"), std::string::npos);
  EXPECT_NE(line.find("shm_name=aquila_gate_market_data"), std::string::npos);
  EXPECT_NE(line.find("channel_name=book_ticker_channel"),
            std::string::npos);
}

TEST(DataReaderToolLoggingTest, FormatsBinarySourceConfig) {
  const std::string line =
      md_tools::FormatSourceConfigLog(0, MakeBinarySource());
  EXPECT_NE(line.find("source_config index=0"), std::string::npos);
  EXPECT_NE(line.find("type=kBinaryFile"), std::string::npos);
  EXPECT_NE(line.find("start_position=earliest_visible"), std::string::npos);
  EXPECT_NE(line.find("files=[/home/liuxiang/tmp/live.bin]"),
            std::string::npos);
}

TEST(DataReaderToolLoggingTest, FormatsStartupWithAndWithoutOutputPath) {
  const std::string recorder = md_tools::FormatToolStartupLog(
      "data_reader_recorder", "realtime", "config.toml",
      std::filesystem::path{"/home/liuxiang/tmp/out.bin"}, 100, 64,
      sizeof(aquila::BookTicker));
  EXPECT_NE(recorder.find("tool=data_reader_recorder"), std::string::npos);
  EXPECT_NE(recorder.find("mode=realtime"), std::string::npos);
  EXPECT_NE(recorder.find("output=/home/liuxiang/tmp/out.bin"),
            std::string::npos);
  EXPECT_NE(recorder.find("book_ticker_abi_size="), std::string::npos);

  const std::string probe = md_tools::FormatToolStartupLog(
      "data_reader_probe", "historical", "binary.toml", std::nullopt, 0, 4096,
      sizeof(aquila::BookTicker));
  EXPECT_NE(probe.find("tool=data_reader_probe"), std::string::npos);
  EXPECT_NE(probe.find("mode=historical"), std::string::npos);
  EXPECT_NE(probe.find("output=none"), std::string::npos);
}

}  // namespace
```

- [ ] **Step 2: Add the test target and verify it fails**

Modify `test/tools/market_data/CMakeLists.txt` by adding:

```cmake
add_executable(data_reader_tool_logging_test
    data_reader_tool_logging_test.cpp
)

target_link_libraries(data_reader_tool_logging_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME data_reader_tool_logging_test
         COMMAND data_reader_tool_logging_test)
```

Run:

```bash
cmake --build build/debug --target data_reader_tool_logging_test -j 8
```

Expected: compile fails because `tools/market_data/data_reader_tool_logging.h` does not exist.

- [ ] **Step 3: Implement the logging helper**

Create `tools/market_data/data_reader_tool_logging.h`:

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/core.h>
#include <magic_enum/magic_enum.hpp>

#include "core/config/data_reader_config.h"

namespace aquila::tools::market_data {

[[nodiscard]] inline std::string_view ReadModeName(
    config::DataReaderReadMode read_mode) noexcept {
  switch (read_mode) {
    case config::DataReaderReadMode::kLatest:
      return "latest";
    case config::DataReaderReadMode::kDrain:
      return "drain";
  }
  return "unknown";
}

[[nodiscard]] inline std::string_view StartPositionName(
    config::DataReaderStartPosition start_position) noexcept {
  switch (start_position) {
    case config::DataReaderStartPosition::kLatest:
      return "latest";
    case config::DataReaderStartPosition::kEarliestVisible:
      return "earliest_visible";
  }
  return "unknown";
}

[[nodiscard]] inline std::string JoinFiles(
    const std::vector<std::filesystem::path>& files) {
  std::string joined;
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (i != 0) {
      joined.append(",");
    }
    joined.append(files[i].string());
  }
  return joined;
}

[[nodiscard]] inline std::string FormatSourceConfigLog(
    std::size_t index, const config::DataReaderSourceConfig& source) {
  return fmt::format(
      "source_config index={} name={} exchange={} type={} feed={} "
      "start_position={} read_mode={} shm_name={} channel_name={} files=[{}]",
      index, source.name, magic_enum::enum_name(source.exchange),
      magic_enum::enum_name(source.type), magic_enum::enum_name(source.feed),
      StartPositionName(source.start_position), ReadModeName(source.read_mode),
      source.shm_name.empty() ? "none" : source.shm_name,
      source.channel_name.empty() ? "none" : source.channel_name,
      JoinFiles(source.files));
}

[[nodiscard]] inline std::string FormatToolStartupLog(
    std::string_view tool, std::string_view mode,
    const std::filesystem::path& config_path,
    std::optional<std::filesystem::path> output_path, std::uint64_t max_polls,
    std::uint64_t drain_budget, std::size_t book_ticker_abi_size) {
  return fmt::format(
      "tool={} mode={} config={} output={} max_polls={} "
      "max_events_per_drain={} book_ticker_abi_size={}",
      tool, mode, config_path.string(),
      output_path.has_value() ? output_path->string() : std::string{"none"},
      max_polls, drain_budget, book_ticker_abi_size);
}

}  // namespace aquila::tools::market_data
```

- [ ] **Step 4: Run the logging helper test**

Run:

```bash
cmake --build build/debug --target data_reader_tool_logging_test -j 8
./build/debug/test/tools/market_data/data_reader_tool_logging_test
```

Expected: test binary exits 0.

- [ ] **Step 5: Commit Task 1**

```bash
git add tools/market_data/data_reader_tool_logging.h \
  test/tools/market_data/data_reader_tool_logging_test.cpp \
  test/tools/market_data/CMakeLists.txt
git commit -m "Add data reader tool log formatting helpers"
```

## Task 2: Probe Mode Helper

**Files:**
- Create: `tools/market_data/data_reader_probe_mode.h`
- Create: `test/tools/market_data/data_reader_probe_mode_test.cpp`
- Modify: `test/tools/market_data/CMakeLists.txt`

- [ ] **Step 1: Write the failing mode helper test**

Add `test/tools/market_data/data_reader_probe_mode_test.cpp`:

```cpp
#include "tools/market_data/data_reader_probe_mode.h"

#include <filesystem>
#include <string>
#include <stdexcept>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/config/data_reader_config.h"

namespace {

namespace cfg = aquila::config;
namespace md_tools = aquila::tools::market_data;

cfg::DataReaderSourceConfig MakeSource(std::string name,
                                       cfg::DataReaderSourceType type) {
  cfg::DataReaderSourceConfig source;
  source.name = std::move(name);
  source.type = type;
  source.exchange = aquila::Exchange::kGate;
  source.feed = cfg::DataReaderFeed::kBookTicker;
  source.shm_name = type == cfg::DataReaderSourceType::kShm
                        ? "aquila_gate_market_data"
                        : "";
  source.channel_name =
      type == cfg::DataReaderSourceType::kShm ? "book_ticker_channel" : "";
  source.files = type == cfg::DataReaderSourceType::kBinaryFile
                     ? std::vector<std::filesystem::path>{"/tmp/recorded.bin"}
                     : std::vector<std::filesystem::path>{};
  source.start_position =
      type == cfg::DataReaderSourceType::kBinaryFile
          ? cfg::DataReaderStartPosition::kEarliestVisible
          : cfg::DataReaderStartPosition::kLatest;
  source.read_mode = cfg::DataReaderReadMode::kDrain;
  source.required = true;
  return source;
}

TEST(DataReaderProbeModeTest, DetectsRealtimeForShmSources) {
  cfg::DataReaderConfig config;
  config.sources.push_back(MakeSource("gate", cfg::DataReaderSourceType::kShm));
  config.sources.push_back(
      MakeSource("binance", cfg::DataReaderSourceType::kShm));

  EXPECT_EQ(md_tools::DetectProbeMode(config),
            md_tools::DataReaderProbeMode::kRealtime);
}

TEST(DataReaderProbeModeTest, DetectsHistoricalForSingleBinarySource) {
  cfg::DataReaderConfig config;
  config.sources.push_back(
      MakeSource("recorded", cfg::DataReaderSourceType::kBinaryFile));

  EXPECT_EQ(md_tools::DetectProbeMode(config),
            md_tools::DataReaderProbeMode::kHistorical);
}

TEST(DataReaderProbeModeTest, RejectsMixedSources) {
  cfg::DataReaderConfig config;
  config.sources.push_back(MakeSource("gate", cfg::DataReaderSourceType::kShm));
  config.sources.push_back(
      MakeSource("recorded", cfg::DataReaderSourceType::kBinaryFile));

  EXPECT_THROW((void)md_tools::DetectProbeMode(config), std::invalid_argument);
}

TEST(DataReaderProbeModeTest, RejectsMultipleBinarySources) {
  cfg::DataReaderConfig config;
  config.sources.push_back(
      MakeSource("first", cfg::DataReaderSourceType::kBinaryFile));
  config.sources.push_back(
      MakeSource("second", cfg::DataReaderSourceType::kBinaryFile));

  EXPECT_THROW((void)md_tools::DetectProbeMode(config), std::invalid_argument);
}

}  // namespace
```

- [ ] **Step 2: Add the test target and verify it fails**

Append to `test/tools/market_data/CMakeLists.txt`:

```cmake
add_executable(data_reader_probe_mode_test
    data_reader_probe_mode_test.cpp
)

target_link_libraries(data_reader_probe_mode_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)

add_test(NAME data_reader_probe_mode_test
         COMMAND data_reader_probe_mode_test)
```

Run:

```bash
cmake --build build/debug --target data_reader_probe_mode_test -j 8
```

Expected: compile fails because `tools/market_data/data_reader_probe_mode.h` does not exist.

- [ ] **Step 3: Implement the mode helper**

Create `tools/market_data/data_reader_probe_mode.h`:

```cpp
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "core/config/data_reader_config.h"

namespace aquila::tools::market_data {

enum class DataReaderProbeMode : std::uint8_t {
  kRealtime,
  kHistorical,
};

[[nodiscard]] inline DataReaderProbeMode DetectProbeMode(
    const config::DataReaderConfig& config) {
  if (config.sources.empty()) {
    throw std::invalid_argument("data reader probe requires at least one source");
  }

  std::size_t shm_sources = 0;
  std::size_t binary_sources = 0;
  for (const config::DataReaderSourceConfig& source : config.sources) {
    switch (source.type) {
      case config::DataReaderSourceType::kShm:
        ++shm_sources;
        break;
      case config::DataReaderSourceType::kBinaryFile:
        ++binary_sources;
        break;
    }
  }

  if (shm_sources == config.sources.size()) {
    return DataReaderProbeMode::kRealtime;
  }
  if (binary_sources == 1 && config.sources.size() == 1) {
    return DataReaderProbeMode::kHistorical;
  }
  throw std::invalid_argument(
      "data reader probe requires all shm sources or exactly one binary_file "
      "source");
}

[[nodiscard]] inline std::string_view ProbeModeName(
    DataReaderProbeMode mode) noexcept {
  switch (mode) {
    case DataReaderProbeMode::kRealtime:
      return "realtime";
    case DataReaderProbeMode::kHistorical:
      return "historical";
  }
  return "unknown";
}

}  // namespace aquila::tools::market_data
```

- [ ] **Step 4: Run the mode helper test**

Run:

```bash
cmake --build build/debug --target data_reader_probe_mode_test -j 8
./build/debug/test/tools/market_data/data_reader_probe_mode_test
```

Expected: test binary exits 0.

- [ ] **Step 5: Commit Task 2**

```bash
git add tools/market_data/data_reader_probe_mode.h \
  test/tools/market_data/data_reader_probe_mode_test.cpp \
  test/tools/market_data/CMakeLists.txt
git commit -m "Add data reader probe mode detection"
```

## Task 3: Tool Logging and Historical Probe

**Files:**
- Modify: `tools/market_data/data_reader_recorder.cpp`
- Modify: `tools/market_data/data_reader_probe.cpp`

- [ ] **Step 1: Update recorder startup and exit logging**

In `tools/market_data/data_reader_recorder.cpp`:

- include `tools/market_data/data_reader_tool_logging.h`;
- remove the local `ReadModeName()` and `StartPositionName()` helpers;
- change startup logging to:

```cpp
NOVA_INFO("{}",
          aquila::tools::market_data::FormatToolStartupLog(
              "data_reader_recorder", "realtime", config_path, output_path,
              max_polls, drain_budget, sizeof(aquila::BookTicker)));
```

- change `LogSourceConfig()` to log `FormatSourceConfigLog(i, source)`;
- keep the existing `latest_read_mode_source` warning;
- wrap final summary in a local lambda so `write_error`, `flush_error`, signal exit and `max_polls` exit all print recorder stats plus reader diagnostics after reader construction:

```cpp
auto log_summary = [&](std::string_view result, std::string_view stop_reason) {
  const auto& reader_stats = reader.diagnostics().stats();
  NOVA_INFO(
      "result={} stop_reason={} polls={} handler_book_tickers={} "
      "diagnostics_total_count={} output={}",
      result, stop_reason, polls, recorder.stats().total_records,
      reader_stats.total_count, output_path.string());
  LogRecorderStats(recorder.stats());
  LogSourceStats(source_labels, reader_stats.sources);
};
```

Use `stop_reason = "signal"` when `signal_stop_requested` is set, `"max_polls"` when the loop ends by budget, `"write_error"` and `"flush_error"` for those errors, and `"completed"` for a clean finite bounded run.

- [ ] **Step 2: Update probe startup logging and support historical binary configs**

In `tools/market_data/data_reader_probe.cpp`:

- include `core/market_data/historical_data_reader.h`;
- include `tools/market_data/data_reader_probe_mode.h`;
- include `tools/market_data/data_reader_tool_logging.h`;
- add `LogSourceConfig()` that logs `FormatSourceConfigLog(i, source)`;
- after parsing config, call `DetectProbeMode(config_result.value)`;
- log startup with `FormatToolStartupLog("data_reader_probe", ProbeModeName(mode), config_path, std::nullopt, max_polls, drain_budget, sizeof(aquila::BookTicker))`;
- branch into two local functions:

```cpp
int RunRealtimeProbe(config::DataReaderConfig config,
                     const std::vector<SourceLabel>& source_labels,
                     std::uint64_t max_polls, std::uint64_t log_every,
                     std::uint64_t drain_budget);

int RunHistoricalProbe(config::DataReaderConfig config,
                       std::uint64_t max_polls, std::uint64_t log_every,
                       std::uint64_t drain_budget);
```

The realtime path keeps the existing `RealtimeDataReader<RealtimeDataReaderDiagnostics>` loop and final source stats.

The historical path uses:

```cpp
using Reader = aquila::market_data::HistoricalDataReader<
    aquila::market_data::HistoricalDataReaderDiagnostics>;
Reader reader(std::move(config));
ProbeHandler handler(log_every);

std::uint64_t polls{0};
while (!signal_stop_requested.load(std::memory_order_relaxed) &&
       !reader.finished() && (max_polls == 0 || polls < max_polls)) {
  const std::uint64_t handled = reader.Drain(handler, drain_budget);
  ++polls;
  if (handled == 0) {
    std::this_thread::yield();
  }
}

const auto& stats = reader.diagnostics().stats();
NOVA_INFO(
    "result=ok mode=historical stop_reason={} polls={} "
    "handler_book_tickers={} diagnostics_total_count={} files_completed={}",
    reader.finished() ? "finished"
                      : signal_stop_requested.load(std::memory_order_relaxed)
                            ? "signal"
                            : "max_polls",
    polls, handler.book_tickers(), stats.total_count, stats.files_completed);
```

This path validates a recorded binary through `HistoricalDataReader` without adding a new production component.

- [ ] **Step 3: Build the modified tools**

Run:

```bash
cmake --build build/debug --target data_reader_recorder data_reader_probe -j 8
```

Expected: build exits 0.

- [ ] **Step 4: Run focused tests**

Run:

```bash
TMPDIR=/home/liuxiang/tmp ./build/debug/test/tools/market_data/data_reader_recorder_test
./build/debug/test/tools/market_data/data_reader_tool_logging_test
./build/debug/test/tools/market_data/data_reader_probe_mode_test
./build/debug/test/core/market_data/core_market_data_historical_data_reader_test
```

Expected: all four binaries exit 0.

- [ ] **Step 5: Commit Task 3**

```bash
git add tools/market_data/data_reader_recorder.cpp \
  tools/market_data/data_reader_probe.cpp
git commit -m "Improve data reader tool runtime logging"
```

## Task 4: Live Record Smoke

**Files:**
- Temporary only: `/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524/*`

- [ ] **Step 1: Build live smoke tools**

Run:

```bash
./build.sh debug
```

Expected: build exits 0.

- [ ] **Step 2: Prepare a temporary drain config**

Run:

```bash
SMOKE_DIR=/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524
mkdir -p "${SMOKE_DIR}"
cp config/data_readers/strategy_data_reader.toml \
  "${SMOKE_DIR}/strategy_data_reader_drain.toml"
perl -0pi -e 's/read_mode = "latest"/read_mode = "drain"/g' \
  "${SMOKE_DIR}/strategy_data_reader_drain.toml"
```

Expected: the temporary config exists and both Gate / Binance sources use `read_mode = "drain"`. Do not modify `config/data_readers/strategy_data_reader.toml`.

- [ ] **Step 3: Start Gate / Binance data sessions**

Run:

```bash
SMOKE_DIR=/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524
/usr/bin/timeout --kill-after=5s 75s ./build/debug/tools/gate_data_session \
  --connect >"${SMOKE_DIR}/gate_data_session.out" 2>&1 &
GATE_PID=$!
/usr/bin/timeout --kill-after=5s 75s ./build/debug/tools/binance_data_session \
  --connect >"${SMOKE_DIR}/binance_data_session.out" 2>&1 &
BINANCE_PID=$!
sleep 10
```

Expected: both logs show active session startup and SHM publishing. If either exchange cannot connect, continue only if at least one SHM producer is available and record the partial coverage in docs.

- [ ] **Step 4: Run recorder**

Run:

```bash
SMOKE_DIR=/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524
/usr/bin/timeout --kill-after=5s 45s ./build/debug/tools/data_reader_recorder \
  --config "${SMOKE_DIR}/strategy_data_reader_drain.toml" \
  --output "${SMOKE_DIR}/live_merged_book_ticker.bin" \
  --mode truncate >"${SMOKE_DIR}/data_reader_recorder.out" 2>&1
RECORDER_STATUS=$?
wait "${GATE_PID}" || true
wait "${BINANCE_PID}" || true
test "${RECORDER_STATUS}" -eq 124 -o "${RECORDER_STATUS}" -eq 0
```

Expected: recorder log includes startup mode/source/output lines and final stats. Exit code `124` is acceptable when GNU `timeout` stops an otherwise healthy unlimited recorder; exit code `0` is acceptable if it stops by `--max-polls` in a later adjusted run.

- [ ] **Step 5: Create a temporary binary replay config**

Run:

```bash
SMOKE_DIR=/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524
cat >"${SMOKE_DIR}/recorded_binary_reader.toml" <<EOF
[data_reader]
name = "recorded_binary_reader"
max_events_per_drain = 4096

[[data_reader.sources]]
name = "recorded_book_ticker"
type = "binary_file"
feed = "book_ticker"
files = ["${SMOKE_DIR}/live_merged_book_ticker.bin"]
start_position = "earliest_visible"
read_mode = "drain"
required = true
EOF
```

Expected: config points at the recorder output and uses `binary_file` + `drain`.

- [ ] **Step 6: Verify recorded binary through data_reader_probe historical mode**

Run:

```bash
SMOKE_DIR=/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524
./build/debug/tools/data_reader_probe \
  --config "${SMOKE_DIR}/recorded_binary_reader.toml" \
  --max-polls 0 \
  --log-every 100000 >"${SMOKE_DIR}/binary_probe.out" 2>&1
```

Expected: `binary_probe.out` contains `mode=historical`, `result=ok`, `stop_reason=finished`, `diagnostics_total_count` greater than 0, and `files_completed=1`.

- [ ] **Step 7: Inspect smoke logs**

Run:

```bash
SMOKE_DIR=/home/liuxiang/tmp/aquila_data_reader_live_smoke_20260524
rg 'tool=data_reader_recorder|source_config|result=|source_stats|recorder_stats|overruns|skipped' \
  "${SMOKE_DIR}/data_reader_recorder.out"
rg 'tool=data_reader_probe|mode=historical|result=ok|diagnostics_total_count|files_completed' \
  "${SMOKE_DIR}/binary_probe.out"
stat -c '%n %s bytes' "${SMOKE_DIR}/live_merged_book_ticker.bin"
```

Expected: startup log shows SHM names and output path; final stats show `skipped` and `overruns`; output file size is nonzero.

## Task 5: Documentation and Final Verification

**Files:**
- Modify: `doc/data_reader_config.md`
- Modify: `doc/project_onboarding_guide.md`

- [ ] **Step 1: Update DataReader docs with smoke evidence**

In `doc/data_reader_config.md`, add a short `Live Record Smoke Evidence` subsection near the existing live drain evidence. Include:

- date `2026-05-24`;
- exact smoke directory under `/home/liuxiang/tmp`;
- recorder command;
- binary probe command;
- total record count;
- Gate / Binance per-source counts;
- `skipped` / `overruns`;
- any partial-coverage warning if one exchange failed.

- [ ] **Step 2: Update onboarding next step**

In `doc/project_onboarding_guide.md`, adjust the DataReader completed/next-step text:

- if smoke passed, say live record smoke and replay readability verification are complete, and next DataReader work can move to longer guarded recording or feed expansion later;
- if live environment blocked the smoke, say the tool/logging work is complete but live record evidence is still pending with the concrete blocker.

- [ ] **Step 3: Run final verification**

Run:

```bash
cmake --build build/debug --target \
  data_reader_recorder \
  data_reader_probe \
  data_reader_tool_logging_test \
  data_reader_probe_mode_test \
  data_reader_recorder_test \
  core_market_data_historical_data_reader_test \
  -j 8
TMPDIR=/home/liuxiang/tmp ./build/debug/test/tools/market_data/data_reader_recorder_test
./build/debug/test/tools/market_data/data_reader_tool_logging_test
./build/debug/test/tools/market_data/data_reader_probe_mode_test
./build/debug/test/core/market_data/core_market_data_historical_data_reader_test
git diff --check
```

Expected: all commands exit 0.

- [ ] **Step 4: Commit docs**

```bash
git add doc/data_reader_config.md doc/project_onboarding_guide.md
git commit -m "Document data reader live record smoke"
```

- [ ] **Step 5: Report final state**

Final response must include:

- code commit hash for tool/logging changes;
- docs commit hash if docs changed;
- live smoke outcome with exact commands and result counts;
- verification commands and exit status;
- note that existing unrelated `signal_repport_20260522_021957/*` deletion and `run_logs/` remain untouched.

## Self-Review

- Spec coverage: startup mode/source/output logging is covered by Tasks 1 and 3; exit diagnostics are covered by Task 3; live record smoke and binary readability verification are covered by Task 4; docs/onboarding updates are covered by Task 5.
- Scope check: no trade / order book feed, no reader hot path changes, no runtime changes.
- Type consistency: helper names are `FormatSourceConfigLog`, `FormatToolStartupLog`, `DetectProbeMode`, `DataReaderProbeMode`, and `ProbeModeName`; later tasks use the same names.
