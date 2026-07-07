# Fusion Config Tooling Unification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Aggressively deduplicate BookTicker / Trade fusion cold-path config parsing and tool support, and migrate metadata build-mode names to feed-neutral fusion names.

**Architecture:** Keep BookTicker / Trade public config, runner, thread, and CLI binary types where they describe real feed-specific records. Move duplicate parser and tool support logic behind small compile-time feed traits. Rename metadata mode from BookTicker-specific to feed-neutral at CMake, macro, header, constant, tests, and active docs.

**Tech Stack:** C++20, CMake, toml++, GTest, fmt, Nova logging, existing `aquila_core` / `aquila_config` / market-data fusion targets.

---

## File Map

- Create `core/common/fusion_metadata_mode.h`: feed-neutral metadata mode macro guard and `kFusionMetadataEnabled`.
- Delete `core/common/book_ticker_fusion_metadata_mode.h`: old compatibility header is intentionally removed.
- Modify `CMakeLists.txt` and `core/CMakeLists.txt`: rename cache option and compile definition to `AQUILA_FUSION_METADATA_MODE` / `AQUILA_FUSION_METADATA_ENABLED`.
- Rename test source `test/core/common/book_ticker_fusion_metadata_mode_test.cpp` to `fusion_metadata_mode_test.cpp`; update `test/core/common/CMakeLists.txt`.
- Modify active code includes and macros in `core/config/*fusion_config.cpp`, `core/market_data/*fusion_metadata_policy.h`, runner/thread tests, config tests, and CLI files.
- Create `core/config/fusion_config_parser.h`: generic TOML parser shared by BookTicker and Trade wrappers.
- Modify `core/config/book_ticker_fusion_config.cpp` and `core/config/trade_fusion_config.cpp`: replace duplicate parser classes with traits and calls into `ParseFusionConfig<Traits>()`.
- Modify `core/config/CMakeLists.txt`: add the parser header to the config library source list.
- Modify `tools/market_data/data_fusion_tool_support.h`: replace feed-specific helpers with `BookTickerDataFusionFeedTraits`, `TradeDataFusionFeedTraits`, and generic helpers.
- Modify `tools/gate/gate_data_fusion.cpp` and `tools/binance/binance_data_fusion.cpp`: call generic helpers and keep behavior in feed branches.
- Modify `test/tools/market_data/data_fusion_tool_support_test.cpp`: test generic helper names and traits.
- Modify active docs: `docs/diagnostic_fields.md`, `docs/project_onboarding_guide.md`, `docs/lead_lag_live_operations_pipeline.md`, `docs/gate_fastest_route_fusion_design.md`, `docs/trade_fastest_route_fusion_design.md`.

## Task 1: Rename Fusion Metadata Mode

**Files:**
- Create: `core/common/fusion_metadata_mode.h`
- Delete: `core/common/book_ticker_fusion_metadata_mode.h`
- Rename: `test/core/common/book_ticker_fusion_metadata_mode_test.cpp` -> `test/core/common/fusion_metadata_mode_test.cpp`
- Modify: `CMakeLists.txt`, `core/CMakeLists.txt`, `test/core/common/CMakeLists.txt`
- Modify: active source/test includes and macro references found by `rg 'book_ticker_fusion_metadata_mode|AQUILA_BOOK_TICKER_FUSION_METADATA|kBookTickerFusionMetadataEnabled' core tools test benchmark`

- [ ] **Step 1: Write the failing metadata mode test**

Replace the metadata mode test with:

```cpp
#include "core/common/fusion_metadata_mode.h"

#include <gtest/gtest.h>

TEST(FusionMetadataModeTest, ExposesCompileTimeMode) {
  EXPECT_TRUE(aquila::kFusionMetadataEnabled ||
              !aquila::kFusionMetadataEnabled);
  EXPECT_GE(AQUILA_FUSION_METADATA_ENABLED, 0);
  EXPECT_LE(AQUILA_FUSION_METADATA_ENABLED, 1);
}
```

Rename the CMake target and test to `core_fusion_metadata_mode_test`.

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build build/debug --target core_fusion_metadata_mode_test -j
```

Expected: build fails because `core/common/fusion_metadata_mode.h` or `AQUILA_FUSION_METADATA_ENABLED` does not exist yet.

- [ ] **Step 3: Implement the rename**

Create `core/common/fusion_metadata_mode.h` with:

```cpp
#ifndef AQUILA_CORE_COMMON_FUSION_METADATA_MODE_H_
#define AQUILA_CORE_COMMON_FUSION_METADATA_MODE_H_

#ifndef AQUILA_FUSION_METADATA_ENABLED
#define AQUILA_FUSION_METADATA_ENABLED 1
#endif

#if AQUILA_FUSION_METADATA_ENABLED < 0 || AQUILA_FUSION_METADATA_ENABLED > 1
#error "AQUILA_FUSION_METADATA_ENABLED must be 0 or 1"
#endif

namespace aquila {

inline constexpr bool kFusionMetadataEnabled =
    AQUILA_FUSION_METADATA_ENABLED != 0;

}  // namespace aquila

#endif  // AQUILA_CORE_COMMON_FUSION_METADATA_MODE_H_
```

In root `CMakeLists.txt`, replace the cache option block with `AQUILA_FUSION_METADATA_MODE`, derive `AQUILA_FUSION_METADATA_ENABLED`, and update the fatal error string. In `core/CMakeLists.txt`, export `AQUILA_FUSION_METADATA_ENABLED=${AQUILA_FUSION_METADATA_ENABLED}`.

Update all active includes, macros, constants, and CMake test target names to the new names. Delete the old header.

- [ ] **Step 4: Run focused verification**

Run:

```bash
cmake --build build/debug --target core_fusion_metadata_mode_test -j
ctest --test-dir build/debug -R 'fusion_metadata_mode' --output-on-failure
rg 'book_ticker_fusion_metadata_mode|AQUILA_BOOK_TICKER_FUSION_METADATA|kBookTickerFusionMetadataEnabled' core tools test benchmark
```

Expected: build and ctest pass; `rg` has no active-code matches.

## Task 2: Extract the Generic Fusion Config Parser

**Files:**
- Create: `core/config/fusion_config_parser.h`
- Modify: `core/config/book_ticker_fusion_config.cpp`
- Modify: `core/config/trade_fusion_config.cpp`
- Modify: `core/config/CMakeLists.txt`
- Modify: `test/config/book_ticker_fusion_config_test.cpp`
- Modify: `test/config/trade_fusion_config_test.cpp`

- [ ] **Step 1: Write failing parser coverage for shared behavior**

Update both config tests to include `core/common/fusion_metadata_mode.h` and `AQUILA_FUSION_METADATA_ENABLED`. Add this assertion to the missing metadata test in each file:

```cpp
#if AQUILA_FUSION_METADATA_ENABLED
  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("fusion.output.metadata_bin"), std::string::npos);
#else
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.value.output.metadata_bin.empty());
#endif
```

Add a default-channel assertion for Trade missing `channel_name`:

```cpp
EXPECT_EQ(result.value.sources[0].channel_name, "trade_channel");
```

- [ ] **Step 2: Run config tests and verify RED**

Run:

```bash
cmake --build build/debug --target book_ticker_fusion_config_test trade_fusion_config_test -j
```

Expected: build fails because the tests now use `AQUILA_FUSION_METADATA_ENABLED` before Task 1 production updates are complete, or because includes still reference the old header if Task 1 has not been completed.

- [ ] **Step 3: Implement `fusion_config_parser.h`**

Add a template parser that exposes:

```cpp
template <typename Traits>
[[nodiscard]] typename Traits::Result ParseFusionConfig(const toml::table& node);

template <typename Traits>
[[nodiscard]] typename Traits::Result LoadFusionConfigFile(
    const std::filesystem::path& path);
```

The parser must:

- parse `fusion.name`, `max_events_per_source`, `bind_cpu_id`, `max_symbol_id`;
- parse `fusion.output.shm_name`, `channel_name`, `remove_existing`, `metadata_bin`;
- require `metadata_bin` when `aquila::kFusionMetadataEnabled` is true;
- parse non-empty `fusion.sources` arrays;
- enforce non-negative unique `source_id`;
- use `Traits::kDefaultSourceChannel` for missing source `channel_name`;
- use `Traits::kLoadErrorPrefix` in load failures.

- [ ] **Step 4: Shrink feed-specific config wrappers**

In `book_ticker_fusion_config.cpp`, define `BookTickerFusionConfigParseTraits` and implement:

```cpp
BookTickerFusionConfigResult ParseBookTickerFusionConfig(
    const toml::table& node) {
  return ParseFusionConfig<BookTickerFusionConfigParseTraits>(node);
}
```

In `trade_fusion_config.cpp`, define `TradeFusionConfigParseTraits` the same way with `trade_channel` and the existing trade load error prefix.

- [ ] **Step 5: Run focused parser verification**

Run:

```bash
cmake --build build/debug --target book_ticker_fusion_config_test trade_fusion_config_test -j
ctest --test-dir build/debug -R '(book_ticker_fusion_config|trade_fusion_config)' --output-on-failure
```

Expected: both tests pass.

## Task 3: Replace Tool Support Helpers with Feed Traits

**Files:**
- Modify: `tools/market_data/data_fusion_tool_support.h`
- Modify: `test/tools/market_data/data_fusion_tool_support_test.cpp`

- [ ] **Step 1: Write failing generic helper tests**

Replace calls to old helper names in `data_fusion_tool_support_test.cpp` with:

```cpp
support::ValidateFusionAlignment<support::BookTickerDataFusionFeedTraits>(
    launch_config, fusion_config, &error);
support::ValidateFusionAlignment<support::TradeDataFusionFeedTraits>(
    launch_config, fusion_config, &error);
support::ApplyFusionSourceOverride<support::BookTickerDataFusionFeedTraits>(
    source, &config);
support::ApplyFusionSourceOverride<support::TradeDataFusionFeedTraits>(
    source, &config);
```

Add explicit tests for `FormatFusionMetadataOutput<BookTickerDataFusionFeedTraits>()` and
`FormatFusionMetadataOutput<TradeDataFusionFeedTraits>()`.

- [ ] **Step 2: Run the test and verify RED**

Run:

```bash
cmake --build build/debug --target data_fusion_tool_support_test -j
```

Expected: build fails because `BookTickerDataFusionFeedTraits`, `TradeDataFusionFeedTraits`, and the generic helpers are not implemented.

- [ ] **Step 3: Implement feed traits and generic helpers**

In `data_fusion_tool_support.h`:

- add `BookTickerDataFusionFeedTraits` and `TradeDataFusionFeedTraits`;
- replace `FormatFusionMetadataOutput()` / `FormatTradeFusionMetadataOutput()` with one templated function;
- replace `FindFusionSource()` / `FindTradeFusionSource()` with one templated function;
- replace `ValidateBookTickerFusionAlignment()` / `ValidateTradeFusionAlignment()` with one `ValidateFusionAlignment<FeedTraits>()`;
- replace source override helpers with `ApplyFusionSourceOverride<FeedTraits>()`;
- replace dry-run helpers with `LogDataFusionDryRun<FeedTraits>()`;
- replace run-summary helpers with `LogDataFusionRunSummary<FeedTraits>()`;
- make all dry-run and summary logs include `feed={DataFusionFeedName(FeedTraits::kFeed)}`.

- [ ] **Step 4: Run focused tool-support verification**

Run:

```bash
cmake --build build/debug --target data_fusion_tool_support_test -j
ctest --test-dir build/debug -R 'data_fusion_tool_support' --output-on-failure
```

Expected: test passes and no old helper names are required.

## Task 4: Update Gate / Binance Data Fusion Tools

**Files:**
- Modify: `tools/gate/gate_data_fusion.cpp`
- Modify: `tools/binance/binance_data_fusion.cpp`
- Modify: related CLI tests if their expected output depends on feed or metadata mode names.

- [ ] **Step 1: Write compile-facing RED**

After Task 3 removes old helper names, build the data fusion tools:

```bash
cmake --build build/debug --target gate_data_fusion binance_data_fusion -j
```

Expected: build fails because tool call sites still reference deleted helper names.

- [ ] **Step 2: Update call sites**

In both Gate and Binance tools:

- call `ApplyFusionSourceOverride<BookTickerDataFusionFeedTraits>()` or `ApplyFusionSourceOverride<TradeDataFusionFeedTraits>()` in `LoadPreparedSources`;
- call `ValidateFusionAlignment<BookTickerDataFusionFeedTraits>()` or `ValidateFusionAlignment<TradeDataFusionFeedTraits>()`;
- call `LogDataFusionDryRun<BookTickerDataFusionFeedTraits>()` or `LogDataFusionDryRun<TradeDataFusionFeedTraits>()`;
- call `LogDataFusionRunSummary<BookTickerDataFusionFeedTraits>()` or `LogDataFusionRunSummary<TradeDataFusionFeedTraits>()`.

- [ ] **Step 3: Run tool build verification**

Run:

```bash
cmake --build build/debug --target gate_data_fusion binance_data_fusion -j
```

Expected: both tools build.

## Task 5: Active Docs, Full Verification, and Performance Check

**Files:**
- Modify: `docs/diagnostic_fields.md`
- Modify: `docs/project_onboarding_guide.md`
- Modify: `docs/lead_lag_live_operations_pipeline.md`
- Modify: `docs/gate_fastest_route_fusion_design.md`
- Modify: `docs/trade_fastest_route_fusion_design.md`

- [ ] **Step 1: Update active docs**

Replace active operational references to:

- `AQUILA_BOOK_TICKER_FUSION_METADATA_MODE` with `AQUILA_FUSION_METADATA_MODE`;
- `AQUILA_BOOK_TICKER_FUSION_METADATA_ENABLED` with `AQUILA_FUSION_METADATA_ENABLED`;
- `core/common/book_ticker_fusion_metadata_mode.h` with `core/common/fusion_metadata_mode.h`.

Do not rewrite archived `docs/superpowers/**` historical plan/spec files.

- [ ] **Step 2: Run full focused debug verification**

Run:

```bash
cmake --build build/debug --target core_fusion_metadata_mode_test book_ticker_fusion_config_test trade_fusion_config_test data_fusion_tool_support_test gate_data_fusion binance_data_fusion -j
ctest --test-dir build/debug -R '(fusion_config|data_fusion_tool_support|fusion_metadata_mode|book_ticker_fusion|trade_fusion)' --output-on-failure
git diff --check
rg 'book_ticker_fusion_metadata_mode|AQUILA_BOOK_TICKER_FUSION_METADATA|kBookTickerFusionMetadataEnabled|ValidateBookTickerFusionAlignment|ValidateTradeFusionAlignment|ApplyBookTickerSourceOverride|ApplyTradeSourceOverride|LogBookTickerDataFusion|LogTradeDataFusion' core tools test benchmark docs --glob '!docs/superpowers/**'
```

Expected: build and ctest pass; diff check passes; `rg` has no active-code or active-doc matches.

- [ ] **Step 3: Run release performance verification**

Run:

```bash
cmake --build build/release --target fastest_route_fusion_benchmark -j
build/release/benchmark/core/market_data/fastest_route_fusion_benchmark \
  --benchmark_repetitions=5 \
  --benchmark_format=json \
  --benchmark_out=/home/liuxiang/tmp/fusion_refactor_perf/config_tooling_unification.json
```

Compare against `/home/liuxiang/tmp/fusion_refactor_perf/refactor_final.json` from commit `5d4fa07`. Expected: no reproducible regression in `fusion/book_ticker_core_on_record`, `fusion/trade_core_on_record`, `fusion/book_ticker_runner_poll_once_noop_metadata`, or `fusion/trade_runner_poll_once_noop_metadata`.

- [ ] **Step 4: Commit**

Run:

```bash
git add CMakeLists.txt core/CMakeLists.txt core/common/fusion_metadata_mode.h core/config/fusion_config_parser.h core/config/book_ticker_fusion_config.cpp core/config/trade_fusion_config.cpp core/config/CMakeLists.txt tools/market_data/data_fusion_tool_support.h tools/gate/gate_data_fusion.cpp tools/binance/binance_data_fusion.cpp test docs
git add -u core/common test/core/common
git commit -m "refactor: unify market data fusion config tooling"
```
