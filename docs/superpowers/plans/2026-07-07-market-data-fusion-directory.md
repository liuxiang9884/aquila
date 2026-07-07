# Market Data Fusion Directory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move market-data fusion headers, tests, and benchmark into dedicated `fusion/` directories, merge alias-only headers, and rename fusion test / benchmark targets to match the new structure.

**Architecture:** Production fusion code lives under `core/market_data/fusion/`. Pure aliases are grouped by type in `config.h`, `metadata.h`, and `thread.h`; feed-specific core / runner / metadata-policy facades live in `book_ticker.h` and `trade.h`; shared implementation is aggressively grouped into `fastest_route.h`, `metadata.h`, and `thread.h`. No old include forwarding headers are kept.

**Implementation note:** final implementation went further than the initial file map below: `fastest_route_runner.h`, `fastest_route_thread.h`, `metadata_policy.h`, and `metadata_writer.h` were folded into the six production headers `book_ticker.h`, `trade.h`, `fastest_route.h`, `thread.h`, `config.h`, and `metadata.h`.

**Tech Stack:** C++20, CMake, GTest, Google Benchmark, existing `aquila_core`, `aquila_config`, `DataShmPublisher`, BookTicker / Trade SHM readers.

---

## File Map

- Create directory `core/market_data/fusion/`.
- Create `core/market_data/fusion/config.h`: common config structs plus BookTicker / Trade config aliases.
- Create `core/market_data/fusion/metadata.h`: common metadata record / writer plus BookTicker / Trade metadata aliases.
- Create `core/market_data/fusion/thread.h`: BookTicker / Trade fusion thread aliases.
- Create `core/market_data/fusion/book_ticker.h`: BookTicker traits, core facade, runner traits / aliases, metadata policy aliases.
- Create `core/market_data/fusion/trade.h`: Trade traits, core facade, runner traits / aliases, metadata policy aliases.
- Move shared headers:
  - `core/market_data/fastest_route_fusion.h` -> `core/market_data/fusion/fastest_route.h`
  - `core/market_data/fastest_route_fusion_runner.h` -> `core/market_data/fusion/fastest_route_runner.h`
  - `core/market_data/fastest_route_fusion_thread.h` -> `core/market_data/fusion/fastest_route_thread.h`
  - `core/market_data/fusion_metadata_policy.h` -> `core/market_data/fusion/metadata_policy.h`
  - `core/market_data/fusion_metadata_writer.h` -> `core/market_data/fusion/metadata_writer.h`
- Delete old alias / facade headers under `core/market_data/*fusion*.h`.
- Modify `core/market_data/CMakeLists.txt`: list new headers only.
- Modify includes in `core/config`, `tools`, `test`, `benchmark`, and active docs.
- Create directory `test/core/market_data/fusion/` and move fusion tests into it with shorter file names.
- Modify `test/core/market_data/CMakeLists.txt`: update source paths and rename target / ctest names.
- Create directory `benchmark/core/market_data/fusion/` and move benchmark to `fusion/fastest_route_benchmark.cpp`.
- Modify `benchmark/core/market_data/CMakeLists.txt`: rename target to `market_data_fusion_fastest_route_benchmark`.
- Modify docs with active path / command references: at minimum `docs/project_onboarding_guide.md`, `docs/superpowers/specs/2026-07-07-market-data-fusion-directory-design.md`, and recent fusion refactor docs if they describe current entry points.

## Task 1: Compile-Facing RED for New Include Paths and Target Names

**Files:**
- Modify: `test/core/market_data/CMakeLists.txt`
- Move test source paths in CMake only after files are moved in Task 3.
- Modify test include lines in existing fusion test files.

- [ ] **Step 1: Change one representative test include to the new path**

In `test/core/market_data/fastest_route_fusion_test.cpp`, replace:

```cpp
#include "core/market_data/fastest_route_fusion.h"
```

with:

```cpp
#include "core/market_data/fusion/fastest_route.h"
```

- [ ] **Step 2: Rename the first test target in CMake**

In `test/core/market_data/CMakeLists.txt`, rename:

```cmake
add_executable(core_market_data_fastest_route_fusion_test
    fastest_route_fusion_test.cpp
)
```

to:

```cmake
add_executable(core_market_data_fusion_fastest_route_test
    fastest_route_fusion_test.cpp
)
```

Update the corresponding `target_link_libraries()` and `add_test()` names to `core_market_data_fusion_fastest_route_test`.

- [ ] **Step 3: Reconfigure and verify RED**

Run:

```bash
cmake -S . -B build/debug
cmake --build build/debug --target core_market_data_fusion_fastest_route_test -j
```

Expected: build fails because `core/market_data/fusion/fastest_route.h` does not exist yet.

## Task 2: Move and Merge Production Fusion Headers

**Files:**
- Create: `core/market_data/fusion/config.h`
- Create: `core/market_data/fusion/metadata.h`
- Create: `core/market_data/fusion/thread.h`
- Create: `core/market_data/fusion/book_ticker.h`
- Create: `core/market_data/fusion/trade.h`
- Move: shared headers listed in File Map.
- Delete: old `core/market_data/*fusion*.h` headers that are replaced.
- Modify: `core/market_data/CMakeLists.txt`

- [ ] **Step 1: Move shared implementation headers**

Run non-destructive `git mv` commands:

```bash
mkdir -p core/market_data/fusion
git mv core/market_data/fastest_route_fusion.h core/market_data/fusion/fastest_route.h
git mv core/market_data/fastest_route_fusion_runner.h core/market_data/fusion/fastest_route_runner.h
git mv core/market_data/fastest_route_fusion_thread.h core/market_data/fusion/fastest_route_thread.h
git mv core/market_data/fusion_metadata_policy.h core/market_data/fusion/metadata_policy.h
git mv core/market_data/fusion_metadata_writer.h core/market_data/fusion/metadata_writer.h
```

- [ ] **Step 2: Create merged config header**

Create `core/market_data/fusion/config.h` with the existing content of `fusion_config.h` plus BookTicker / Trade aliases:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace aquila::market_data {

struct FusionSourceConfig {
  std::int32_t source_id{-1};
  std::string name;
  std::string shm_name;
  std::string channel_name;
};

struct FusionOutputConfig {
  std::string shm_name;
  std::string channel_name;
  bool remove_existing{false};
  std::filesystem::path metadata_bin;
};

template <typename SourceConfig, typename OutputConfig>
struct BasicFusionConfig {
  std::string name;
  std::uint32_t max_events_per_source{1};
  std::int32_t bind_cpu_id{-1};
  std::uint32_t max_symbol_id{4096};
  OutputConfig output;
  std::vector<SourceConfig> sources;
};

using BookTickerFusionSourceConfig = FusionSourceConfig;
using BookTickerFusionOutputConfig = FusionOutputConfig;
using BookTickerFusionConfig = BasicFusionConfig<BookTickerFusionSourceConfig,
                                                 BookTickerFusionOutputConfig>;

using TradeFusionSourceConfig = FusionSourceConfig;
using TradeFusionOutputConfig = FusionOutputConfig;
using TradeFusionConfig =
    BasicFusionConfig<TradeFusionSourceConfig, TradeFusionOutputConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_CONFIG_H_
```

- [ ] **Step 3: Create merged metadata header**

Create `core/market_data/fusion/metadata.h` with the existing `FusionMetadataRecord`, writer include, and aliases:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_

#include <cstdint>

#include "core/market_data/fusion/metadata_writer.h"

namespace aquila::market_data {

struct FusionMetadataRecord {
  std::int32_t source_id{0};
  std::int32_t symbol_id{0};
  std::int64_t record_id{0};
  std::int64_t exchange_ns{0};
  std::int64_t event_ns{0};
  std::int64_t source_local_ns{0};
  std::int64_t fusion_publish_ns{0};
};

using BookTickerFusionMetadataRecord = FusionMetadataRecord;
using BookTickerFusionMetadataWriter = FusionMetadataWriter;
using TradeFusionMetadataRecord = FusionMetadataRecord;
using TradeFusionMetadataWriter = FusionMetadataWriter;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_METADATA_H_
```

- [ ] **Step 4: Create feed facade headers**

Create `core/market_data/fusion/book_ticker.h` by merging:

- `book_ticker_fusion.h`
- `book_ticker_fusion_runner.h`
- `book_ticker_fusion_metadata_policy.h`

It must include:

```cpp
#include "core/common/fusion_metadata_mode.h"
#include "core/market_data/data_shm.h"
#include "core/market_data/fusion/config.h"
#include "core/market_data/fusion/fastest_route.h"
#include "core/market_data/fusion/fastest_route_runner.h"
#include "core/market_data/fusion/metadata_policy.h"
#include "core/market_data/types.h"
```

Create `core/market_data/fusion/trade.h` by merging the corresponding Trade headers with the same shared includes.

- [ ] **Step 5: Create merged thread header**

Create `core/market_data/fusion/thread.h` with:

```cpp
#ifndef AQUILA_CORE_MARKET_DATA_FUSION_THREAD_H_
#define AQUILA_CORE_MARKET_DATA_FUSION_THREAD_H_

#include "core/market_data/fusion/book_ticker.h"
#include "core/market_data/fusion/fastest_route_thread.h"
#include "core/market_data/fusion/trade.h"

namespace aquila::market_data {

using BookTickerFusionThreadStats = FastestRouteFusionThreadStats;
using BookTickerFusionThread =
    BasicFastestRouteFusionThread<BookTickerFusionRunner,
                                  BookTickerFusionConfig>;

using TradeFusionThreadStats = FastestRouteFusionThreadStats;
using TradeFusionThread =
    BasicFastestRouteFusionThread<TradeFusionRunner, TradeFusionConfig>;

}  // namespace aquila::market_data

#endif  // AQUILA_CORE_MARKET_DATA_FUSION_THREAD_H_
```

- [ ] **Step 6: Update moved shared header includes and guards**

Update include guards and includes:

- `fastest_route.h`: guard `AQUILA_CORE_MARKET_DATA_FUSION_FASTEST_ROUTE_H_`.
- `fastest_route_runner.h`: include `core/market_data/fusion/fastest_route.h` if needed, guard with new path.
- `fastest_route_thread.h`: include `core/market_data/fusion/fastest_route_runner.h`.
- `metadata_policy.h`: include `core/market_data/fusion/fastest_route.h` and `core/market_data/fusion/metadata.h`.
- `metadata_writer.h`: guard `AQUILA_CORE_MARKET_DATA_FUSION_METADATA_WRITER_H_`.

- [ ] **Step 7: Delete replaced old headers and update CMake**

Delete these old files with `git rm`:

```text
core/market_data/book_ticker_fusion.h
core/market_data/book_ticker_fusion_config.h
core/market_data/book_ticker_fusion_metadata.h
core/market_data/book_ticker_fusion_metadata_policy.h
core/market_data/book_ticker_fusion_runner.h
core/market_data/book_ticker_fusion_thread.h
core/market_data/fusion_config.h
core/market_data/fusion_metadata.h
core/market_data/trade_fusion.h
core/market_data/trade_fusion_config.h
core/market_data/trade_fusion_metadata.h
core/market_data/trade_fusion_metadata_policy.h
core/market_data/trade_fusion_runner.h
core/market_data/trade_fusion_thread.h
```

Update `core/market_data/CMakeLists.txt` to list only:

```cmake
target_sources(aquila_core
    PRIVATE
        fusion/book_ticker.h
        fusion/config.h
        fusion/fastest_route.h
        fusion/fastest_route_runner.h
        fusion/fastest_route_thread.h
        fusion/metadata.h
        fusion/metadata_policy.h
        fusion/metadata_writer.h
        fusion/thread.h
        fusion/trade.h
)
```

Keep non-fusion sources in that file unchanged if they are present outside the `target_sources()` block.

## Task 3: Update Active Includes and Move Tests / Benchmark

**Files:**
- Modify includes under `core/config`, `tools`, `test`, `benchmark`.
- Move tests into `test/core/market_data/fusion/`.
- Move benchmark into `benchmark/core/market_data/fusion/`.
- Modify `test/core/market_data/CMakeLists.txt`.
- Modify `benchmark/core/market_data/CMakeLists.txt`.

- [ ] **Step 1: Update active include paths**

Apply these replacements in active code:

```text
core/market_data/book_ticker_fusion_config.h -> core/market_data/fusion/config.h
core/market_data/trade_fusion_config.h -> core/market_data/fusion/config.h
core/market_data/book_ticker_fusion_metadata.h -> core/market_data/fusion/metadata.h
core/market_data/trade_fusion_metadata.h -> core/market_data/fusion/metadata.h
core/market_data/book_ticker_fusion_thread.h -> core/market_data/fusion/thread.h
core/market_data/trade_fusion_thread.h -> core/market_data/fusion/thread.h
core/market_data/book_ticker_fusion_runner.h -> core/market_data/fusion/book_ticker.h
core/market_data/trade_fusion_runner.h -> core/market_data/fusion/trade.h
core/market_data/book_ticker_fusion.h -> core/market_data/fusion/book_ticker.h
core/market_data/trade_fusion.h -> core/market_data/fusion/trade.h
core/market_data/fastest_route_fusion.h -> core/market_data/fusion/fastest_route.h
core/market_data/fusion_config.h -> core/market_data/fusion/config.h
core/market_data/fusion_metadata.h -> core/market_data/fusion/metadata.h
core/market_data/fusion_metadata_policy.h -> core/market_data/fusion/metadata_policy.h
core/market_data/fusion_metadata_writer.h -> core/market_data/fusion/metadata_writer.h
```

- [ ] **Step 2: Move fusion tests**

Run:

```bash
mkdir -p test/core/market_data/fusion
git mv test/core/market_data/fastest_route_fusion_test.cpp test/core/market_data/fusion/fastest_route_test.cpp
git mv test/core/market_data/book_ticker_fusion_test.cpp test/core/market_data/fusion/book_ticker_test.cpp
git mv test/core/market_data/book_ticker_fusion_metadata_test.cpp test/core/market_data/fusion/book_ticker_metadata_test.cpp
git mv test/core/market_data/fusion_metadata_policy_test.cpp test/core/market_data/fusion/metadata_policy_test.cpp
git mv test/core/market_data/trade_fusion_metadata_test.cpp test/core/market_data/fusion/trade_metadata_test.cpp
git mv test/core/market_data/trade_fusion_test.cpp test/core/market_data/fusion/trade_test.cpp
git mv test/core/market_data/book_ticker_fusion_runner_test.cpp test/core/market_data/fusion/book_ticker_runner_test.cpp
git mv test/core/market_data/trade_fusion_runner_test.cpp test/core/market_data/fusion/trade_runner_test.cpp
git mv test/core/market_data/book_ticker_fusion_thread_test.cpp test/core/market_data/fusion/book_ticker_thread_test.cpp
git mv test/core/market_data/trade_fusion_thread_test.cpp test/core/market_data/fusion/trade_thread_test.cpp
```

- [ ] **Step 3: Rename test targets and source paths**

In `test/core/market_data/CMakeLists.txt`, use the exact new target names from the spec:

```cmake
add_executable(core_market_data_fusion_fastest_route_test
    fusion/fastest_route_test.cpp
)
```

Repeat for BookTicker / Trade metadata, core, runner, thread, and static review. Update the static review path to:

```cmake
${PROJECT_SOURCE_DIR}/core/market_data/fusion/book_ticker.h
```

- [ ] **Step 4: Move and rename benchmark target**

Run:

```bash
mkdir -p benchmark/core/market_data/fusion
git mv benchmark/core/market_data/fastest_route_fusion_benchmark.cpp benchmark/core/market_data/fusion/fastest_route_benchmark.cpp
```

In `benchmark/core/market_data/CMakeLists.txt`, replace:

```cmake
add_executable(fastest_route_fusion_benchmark
    fastest_route_fusion_benchmark.cpp
)
```

with:

```cmake
add_executable(market_data_fusion_fastest_route_benchmark
    fusion/fastest_route_benchmark.cpp
)
```

Update `target_link_libraries()` and GNU compile options to the new target name.

- [ ] **Step 5: Reconfigure and verify GREEN for focused targets**

Run:

```bash
cmake -S . -B build/debug
cmake --build build/debug --target \
  core_market_data_fusion_fastest_route_test \
  core_market_data_fusion_book_ticker_test \
  core_market_data_fusion_book_ticker_metadata_test \
  core_market_data_fusion_metadata_policy_test \
  core_market_data_fusion_trade_metadata_test \
  core_market_data_fusion_trade_test \
  core_market_data_fusion_book_ticker_runner_test \
  core_market_data_fusion_trade_runner_test \
  core_market_data_fusion_book_ticker_thread_test \
  core_market_data_fusion_trade_thread_test \
  gate_data_fusion binance_data_fusion \
  gate_book_ticker_fusion binance_book_ticker_fusion \
  gate_trade_fusion binance_trade_fusion -j
```

Expected: all targets build.

## Task 4: Active Docs, Static Cleanup, Verification, and Commit

**Files:**
- Modify active docs with current paths and benchmark target names.
- Update `docs/project_onboarding_guide.md`.
- Update current spec / plan if execution discovers a required path correction.

- [ ] **Step 1: Update active docs**

Replace active references:

```text
core/market_data/book_ticker_fusion* -> core/market_data/fusion/book_ticker.h
core/market_data/trade_fusion* -> core/market_data/fusion/trade.h
core/market_data/fastest_route_fusion* -> core/market_data/fusion/fastest_route*
fastest_route_fusion_benchmark -> market_data_fusion_fastest_route_benchmark
```

Do not rewrite archived `docs/superpowers/**` history except this plan if needed.

- [ ] **Step 2: Run full focused debug verification**

Run:

```bash
ctest --test-dir build/debug -R 'core_market_data_fusion|fusion_config|data_fusion_tool_support|fusion_cli_traits|book_ticker_fusion_cli|trade_fusion_cli' --output-on-failure
git diff --check
```

Expected: all focused tests pass; diff check has no output.

- [ ] **Step 3: Run old-path cleanup scans**

Run:

```bash
rg '#include "core/market_data/(book_ticker_fusion|trade_fusion|fastest_route_fusion|fusion_)' core tools test benchmark
find core/market_data -maxdepth 1 -type f -name '*fusion*'
rg 'fastest_route_fusion_benchmark|core_market_data_fastest_route_fusion_test|core_market_data_book_ticker_fusion|core_market_data_trade_fusion' core tools test benchmark docs --glob '!docs/superpowers/**'
```

Expected: no active old include paths, no fusion headers at `core/market_data` top level, and no active old target names outside archived docs.

- [ ] **Step 4: Run release benchmark**

Run:

```bash
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --target market_data_fusion_fastest_route_benchmark -j
build/release/benchmark/core/market_data/market_data_fusion_fastest_route_benchmark \
  --benchmark_repetitions=5 \
  --benchmark_format=json \
  --benchmark_out=/home/liuxiang/tmp/fusion_refactor_perf/directory_migration.json
```

Compare against `/home/liuxiang/tmp/fusion_refactor_perf/config_tooling_unification.json`. Expected: no reproducible regression in core or runner benchmark means.

- [ ] **Step 5: Commit**

Run:

```bash
git add -A
git commit -m "refactor: move market data fusion into subdirectory"
```
