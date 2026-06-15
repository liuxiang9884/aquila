# BookTicker Fusion Metadata Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a compile-time policy that can remove fusion sidecar metadata recording while keeping canonical SHM publish and basic fusion run stats.

**Architecture:** Add a CMake cache mode and a small compile-time mode header. Parse `metadata_bin` only when metadata is enabled. Keep `BookTickerFusionRunner` as the public runner type, but make metadata handling policy-based so the `off` build does not construct `FusionMetadataRecord` or open a writer.

**Tech Stack:** CMake, C++20, GoogleTest, existing `BookTickerFusionRunner`, `FusionMetadataWriter`, TOML config parser.

---

### Task 1: Compile-Time Mode

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `core/CMakeLists.txt`
- Create: `core/common/book_ticker_fusion_metadata_mode.h`
- Test: `test/core/common/book_ticker_fusion_metadata_mode_test.cpp`
- Modify: `test/core/common/CMakeLists.txt`

- [ ] **Step 1: Write the failing mode test**

Add `test/core/common/book_ticker_fusion_metadata_mode_test.cpp`:

```cpp
#include "core/common/book_ticker_fusion_metadata_mode.h"

#include <gtest/gtest.h>

TEST(BookTickerFusionMetadataModeTest, ExposesCompileTimeMode) {
  EXPECT_TRUE(aquila::kBookTickerFusionMetadataEnabled ||
              !aquila::kBookTickerFusionMetadataEnabled);
  EXPECT_GE(AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED, 0);
  EXPECT_LE(AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED, 1);
}
```

- [ ] **Step 2: Register the test target**

Add the target to `test/core/common/CMakeLists.txt` using the existing `aquila_core` link pattern:

```cmake
add_executable(core_book_ticker_fusion_metadata_mode_test
    book_ticker_fusion_metadata_mode_test.cpp
)
target_link_libraries(core_book_ticker_fusion_metadata_mode_test
    PRIVATE
        aquila_core
        GTest::gtest_main
)
add_test(NAME core_book_ticker_fusion_metadata_mode_test
         COMMAND core_book_ticker_fusion_metadata_mode_test)
```

- [ ] **Step 3: Run the failing test**

Run:

```bash
cmake --build build/debug --target core_book_ticker_fusion_metadata_mode_test -j8
```

Expected: compile fails because `core/common/book_ticker_fusion_metadata_mode.h` does not exist.

- [ ] **Step 4: Implement the CMake mode**

Add to root `CMakeLists.txt` near the existing diagnostic cache variables:

```cmake
set(AQUILA_BOOK_TICKER_FUSION_METADATA_MODE "file"
    CACHE STRING
    "Build BookTicker fusion sidecar metadata mode: file or off")
set_property(CACHE AQUILA_BOOK_TICKER_FUSION_METADATA_MODE PROPERTY STRINGS
             file off)
if(NOT AQUILA_BOOK_TICKER_FUSION_METADATA_MODE MATCHES "^(file|off)$")
    message(FATAL_ERROR
            "AQUILA_BOOK_TICKER_FUSION_METADATA_MODE must be file or off")
endif()
if(AQUILA_BOOK_TICKER_FUSION_METADATA_MODE STREQUAL "file")
    set(AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED 1)
else()
    set(AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED 0)
endif()
```

Add to `core/CMakeLists.txt` public compile definitions:

```cmake
AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED=${AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED}
```

- [ ] **Step 5: Implement the mode header**

Create `core/common/book_ticker_fusion_metadata_mode.h`:

```cpp
#ifndef AQUILA_CORE_COMMON_BOOK_TICKER_FUSION_METADATA_MODE_H_
#define AQUILA_CORE_COMMON_BOOK_TICKER_FUSION_METADATA_MODE_H_

#ifndef AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED
#define AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED 1
#endif

#if AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED < 0 || \
    AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED > 1
#error "AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED must be 0 or 1"
#endif

namespace aquila {

inline constexpr bool kBookTickerFusionMetadataEnabled =
    AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED != 0;

}  // namespace aquila

#endif  // AQUILA_CORE_COMMON_BOOK_TICKER_FUSION_METADATA_MODE_H_
```

- [ ] **Step 6: Run the mode test**

Run:

```bash
cmake --build build/debug --target core_book_ticker_fusion_metadata_mode_test -j8
./build/debug/test/core/common/core_book_ticker_fusion_metadata_mode_test
```

Expected: test passes.

### Task 2: Config Parser Behavior

**Files:**
- Modify: `core/config/book_ticker_fusion_config.cpp`
- Modify: `test/config/book_ticker_fusion_config_test.cpp`
- Modify: `test/config/CMakeLists.txt`

- [ ] **Step 1: Write the failing off-mode config test**

Add a new test source or compile variant that defines `AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED=0` and verifies this TOML parses:

```cpp
TEST(BookTickerFusionConfigTest, AllowsMissingMetadataBinWhenMetadataDisabled) {
  const toml::parse_result parsed = ParseToml(R"toml(
[fusion]
name = "gate_bbo_fusion"

[fusion.output]
shm_name = "aquila_gate_book_ticker_fusion"
channel_name = "book_ticker_channel"

[[fusion.sources]]
source_id = 0
name = "gate_src_0"
shm_name = "aquila_gate_book_ticker_src_0"
channel_name = "book_ticker_channel"
)toml");

  const auto result = aquila::config::ParseBookTickerFusionConfig(parsed);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.output.metadata_bin.empty());
}
```

- [ ] **Step 2: Run the failing off-mode config test**

Run the off-mode config target.

Expected: fails because parser still requires `fusion.output.metadata_bin`.

- [ ] **Step 3: Implement parser gating**

In `ParseOutput()`, include the mode header and branch with `if constexpr`:

```cpp
if constexpr (aquila::kBookTickerFusionMetadataEnabled) {
  const std::string metadata_bin =
      RequiredString(output["metadata_bin"], "fusion.output.metadata_bin");
  if (!ok_) {
    return;
  }
  config_.output.metadata_bin = metadata_bin;
} else {
  config_.output.metadata_bin =
      OptionalString(output["metadata_bin"], std::string{});
}
```

- [ ] **Step 4: Run config tests**

Run:

```bash
cmake --build build/debug --target book_ticker_fusion_config_test -j8
./build/debug/test/config/book_ticker_fusion_config_test
```

Expected: default metadata-enabled behavior still rejects missing `metadata_bin`.

Run the off-mode config target.

Expected: missing `metadata_bin` parses successfully.

### Task 3: Metadata Policy and Runner

**Files:**
- Create: `core/market_data/book_ticker_fusion_metadata_policy.h`
- Modify: `core/market_data/book_ticker_fusion_runner.h`
- Modify: `core/market_data/book_ticker_fusion_thread.h`
- Modify: `test/core/market_data/book_ticker_fusion_runner_test.cpp`
- Modify: `test/core/market_data/book_ticker_fusion_thread_test.cpp`
- Modify: `test/core/market_data/CMakeLists.txt`

- [ ] **Step 1: Write failing off-mode runner/thread tests**

Add off-mode compile variants of the runner and thread tests. The runner off test should use an empty `metadata_bin`, publish one source ticker, call `PollOnce()` and `Flush()`, verify canonical SHM has the ticker, and verify `stats.metadata_write_errors == 0` and `runner.total_metadata_write_errors() == 0`.

The thread off test should use an empty `metadata_bin`, start/stop the thread, verify canonical SHM publishes, and verify `stats.total_metadata_write_errors == 0`.

- [ ] **Step 2: Run failing off-mode tests**

Run:

```bash
cmake --build build/debug --target core_market_data_book_ticker_fusion_runner_metadata_off_test -j8
cmake --build build/debug --target core_market_data_book_ticker_fusion_thread_metadata_off_test -j8
```

Expected: compile or runtime failure because runner still constructs `FusionMetadataWriter` with an empty path.

- [ ] **Step 3: Add metadata policy**

Create `core/market_data/book_ticker_fusion_metadata_policy.h` with file and noop policies. `NoopBookTickerFusionMetadataPolicy::Write(...)` returns true without touching the record data; `Flush()` returns true; `enabled()` returns false.

- [ ] **Step 4: Update runner**

Make `BookTickerFusionRunner` use the default metadata policy selected from `aquila::kBookTickerFusionMetadataEnabled`. In the publish branch, call a helper that only constructs `FusionMetadataRecord` under the file policy. Keep read/publish stats unchanged.

- [ ] **Step 5: Update thread stats**

Keep `BookTickerFusionThreadStats::total_metadata_write_errors` for ABI/log compatibility, but ensure it is always zero in metadata-off builds and `ok` only depends on metadata errors when metadata is enabled.

- [ ] **Step 6: Run runner and thread tests**

Run:

```bash
cmake --build build/debug --target core_market_data_book_ticker_fusion_runner_test -j8
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_runner_test
cmake --build build/debug --target core_market_data_book_ticker_fusion_thread_test -j8
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_thread_test
cmake --build build/debug --target core_market_data_book_ticker_fusion_runner_metadata_off_test -j8
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_runner_metadata_off_test
cmake --build build/debug --target core_market_data_book_ticker_fusion_thread_metadata_off_test -j8
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_thread_metadata_off_test
```

Expected: all tests pass.

### Task 4: Tool Logs and Final Verification

**Files:**
- Modify: `tools/market_data/data_fusion_tool_support.h`
- Modify: `tools/market_data/book_ticker_fusion_cli.cpp`
- Test: existing fusion support and CLI build targets.

- [ ] **Step 1: Update logs**

Include `metadata_enabled=true|false` in dry-run and summary logs. In metadata-off builds, avoid printing a misleading `metadata_output` path when the path is empty.

- [ ] **Step 2: Run support tests**

Run:

```bash
cmake --build build/debug --target data_fusion_tool_support_test -j8
./build/debug/test/tools/market_data/data_fusion_tool_support_test
```

Expected: support tests pass after expected log string updates.

- [ ] **Step 3: Run final checks**

Run:

```bash
git diff --check
cmake --build build/debug --target book_ticker_fusion_config_test core_market_data_book_ticker_fusion_runner_test core_market_data_book_ticker_fusion_thread_test data_fusion_tool_support_test -j8
./build/debug/test/config/book_ticker_fusion_config_test
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_runner_test
./build/debug/test/core/market_data/core_market_data_book_ticker_fusion_thread_test
./build/debug/test/tools/market_data/data_fusion_tool_support_test
```

Expected: all checks pass.
