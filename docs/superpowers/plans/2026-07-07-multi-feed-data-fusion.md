# Multi-Feed Data Fusion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `gate_data_fusion` / `binance_data_fusion` can run one source-worker bundle with configurable `book_ticker` and `trade` fusion threads.

**Architecture:** Parse enabled feeds from launch config, prepare each source once with a `DataShmPublisher` that creates only enabled feed channels, and start one fusion thread per enabled feed. Keep fusion configs single-feed and use shared tool support helpers for alignment, source overrides, CPU conflict validation, dry-run logging and run summaries.

**Tech Stack:** C++20, CMake, GTest, TOML++, existing `DataSession`, `DataShmPublisher`, and `BasicFastestRouteFusionThread`.

---

### Task 1: Tests First

**Files:**
- Modify: `test/tools/gate/gate_data_fusion_config_test.cpp`
- Modify: `test/tools/binance/binance_data_fusion_config_test.cpp`
- Modify: `test/tools/market_data/data_fusion_tool_support_test.cpp`
- Modify: `test/core/market_data/data_shm_test.cpp`

- [ ] Add config parser tests for `feeds = ["book_ticker", "trade"]`, `[launch.fusion_configs]`, `data_shm_name`, per-feed channel names, duplicate feeds, and missing fusion config.
- [ ] Add tool support tests for multi-feed source override and CPU binding conflict detection.
- [ ] Add SHM tests proving `DataShmPublisher(DataShmConfig)` can create only book_ticker, only trade, or both channels.
- [ ] Run focused tests and verify they fail for missing production behavior.

### Task 2: Core SHM Channel Enablement

**Files:**
- Modify: `core/market_data/data_shm_config.h`
- Modify: `core/market_data/data_shm.h`

- [ ] Add `book_ticker_enabled` and `trade_enabled` to `DataShmConfig`.
- [ ] Make `BookTickerConfig()` and `TradeConfig()` propagate per-channel enabled state.
- [ ] Make `DataShmManager` construct/find only enabled channels and fail if no channels are enabled.
- [ ] Keep `DataShmPublisher` hot methods branch-free except for existing unlikely null-channel guard.
- [ ] Run `core_market_data_data_shm_test`.

### Task 3: Launch Config Multi-Feed Schema

**Files:**
- Modify: `tools/gate/gate_data_fusion_config.h`
- Modify: `tools/gate/gate_data_fusion_config.cpp`
- Modify: `tools/binance/binance_data_fusion_config.h`
- Modify: `tools/binance/binance_data_fusion_config.cpp`

- [ ] Add launch-level `feeds`, `book_ticker_fusion_config`, `trade_fusion_config`.
- [ ] Add source-level `data_shm_name`.
- [ ] Parse the unified `launch.feeds` / `[launch.fusion_configs]` / `data_shm_name` schema for both single-feed and multi-feed launch configs; reject the old `feed` / `fusion_config` / feed-specific source SHM launch schema.
- [ ] Validate duplicate feed, missing enabled-feed fusion config, and missing source SHM name.
- [ ] Run Gate and Binance config tests.

### Task 4: Shared Tool Support

**Files:**
- Modify: `tools/market_data/data_fusion_tool_support.h`

- [ ] Add helpers to query enabled feeds and fusion config path by feed.
- [ ] Add `ApplyFusionSourceOverrides()` to set `DataSessionConfig.feeds`, `data_shm`, per-feed channel names, source id diagnostics, and bind CPU once per source.
- [ ] Add `ValidateDataFusionCpuBindings()` to reject duplicate non-negative CPU IDs across source workers and enabled fusion configs.
- [ ] Add multi-feed dry-run and run summary helpers while preserving per-feed logs.
- [ ] Run `data_fusion_tool_support_test`.

### Task 5: Gate/Binance Bundle Runtime

**Files:**
- Modify: `tools/gate/gate_data_fusion.cpp`
- Modify: `tools/binance/binance_data_fusion.cpp`

- [ ] Replace separate book_ticker/trade source worker classes with one source worker using `DataShmPublisher(config.data_shm)`.
- [ ] Load all enabled fusion configs, validate alignment, and validate CPU bindings before connecting.
- [ ] Start source workers once and one fusion thread per enabled feed.
- [ ] Stop all threads on signal, max runtime, or startup failure; return non-zero if any fusion stats are not ok.
- [ ] Run data fusion focused tests and dry-run binaries.

### Task 6: Config/Docs Sync

**Files:**
- Modify: `config/market_data_fusion/*data_fusion*.toml` as needed for checked-in multi-feed examples
- Modify: `docs/gate_fastest_route_fusion_threaded_bundle_guide.md`
- Modify: `docs/project_onboarding_guide.md` only if the new behavior changes onboarding facts

- [ ] Add or update one Gate and one Binance multi-feed example.
- [ ] Document `N source + M fusion + 1 log` thread accounting and CPU overlap rule.
- [ ] Run `git diff --check`.

### Task 7: Verification and Commit

- [ ] Run focused debug build/tests.
- [ ] Run release fusion benchmark and compare existing benchmark keys against `/home/liuxiang/tmp/fusion_refactor_perf/review_round2_duplicate_fanin.json`.
- [ ] Run evaluation boundary checks.
- [ ] Commit with English message.
