# Trade HistoricalDataReader Replay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `HistoricalDataReader` read trade-only binary files and let `data_reader_probe` validate recorder trade artifacts.

**Architecture:** Keep one `HistoricalDataReader` type and the existing exactly-one-`binary_file` source constraint. The reader stores the configured feed, validates files against the matching record size, and dispatches either `OnBookTicker()` or `OnTrade()` while preserving finite reader behavior. Manifest config generation gains a feed option so recorder trade manifests can produce trade binary reader TOML.

**Tech Stack:** C++20, CMake, GTest, toml++, Python unittest, Nova logging.

---

## File Map

- Modify `core/config/data_reader_config.cpp`: allow `binary_file + feed = "trade"`.
- Modify `test/config/data_reader_config_test.cpp`: update trade binary parser expectation.
- Modify `core/market_data/historical_data_reader.h`: dispatch book ticker or trade records from one binary source.
- Modify `test/core/market_data/historical_data_reader_test.cpp`: add trade binary tests and diagnostics checks.
- Modify `tools/market_data/data_reader_probe.cpp`: include `handler_trades` in historical summary.
- Modify `scripts/market_data/manifest_to_data_reader_config.py`: add `--feed book_ticker|trade`.
- Modify `scripts/test/market_data/manifest_to_data_reader_config_test.py`: cover default and trade feed config output.
- Modify `docs/data_reader_config.md` and `docs/lead_lag_live_replay_testing.md`: document supported trade historical reader boundary and LeadLag replay exclusion.

## Task 1: Config Allows Trade Binary Source

**Files:**
- Modify: `test/config/data_reader_config_test.cpp`
- Modify: `core/config/data_reader_config.cpp`

- [ ] **Step 1: Replace the rejecting config test with an accepting test**

Change `RejectsTradeBinaryFileSource` into `ParsesTradeBinaryFileSource`:

```cpp
TEST(DataReaderConfigTest, ParsesTradeBinaryFileSource) {
  const std::string toml_text = CatalogPrefix() + R"toml(
[data_reader]
name = "binary_trade_reader"

[[data_reader.sources]]
name = "binary_trade"
type = "binary_file"
feed = "trade"
files = ["/home/liuxiang/tmp/trade.bin"]
start_position = "earliest_visible"
read_mode = "drain"
)toml";

  const toml::parse_result parsed = toml::parse(toml_text);
  const auto result = aquila::config::ParseDataReaderConfig(parsed);
  ASSERT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.value.sources.size(), 1U);
  const auto& source = result.value.sources[0];
  EXPECT_EQ(source.name, "binary_trade");
  EXPECT_EQ(source.type, aquila::config::DataReaderSourceType::kBinaryFile);
  EXPECT_EQ(source.feed, aquila::config::DataReaderFeed::kTrade);
  ASSERT_EQ(source.files.size(), 1U);
  EXPECT_EQ(source.files[0], std::filesystem::path{"/home/liuxiang/tmp/trade.bin"});
}
```

- [ ] **Step 2: Verify red**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_reader_config_test -j8
ctest --test-dir build/debug -R '^data_reader_config_test$' --output-on-failure
```

Expected: test fails with a config error mentioning trade support for shm only.

- [ ] **Step 3: Remove the binary trade rejection**

In `core/config/data_reader_config.cpp`, delete only this branch from `ValidateSource()`:

```cpp
if (source.feed != DataReaderFeed::kBookTicker) {
  Fail("data_reader.sources.feed",
       " trade is only supported for shm sources");
  return;
}
```

- [ ] **Step 4: Verify green**

Run the same build and ctest commands. Expected: `data_reader_config_test` passes.

## Task 2: HistoricalDataReader Dispatches Trade

**Files:**
- Modify: `test/core/market_data/historical_data_reader_test.cpp`
- Modify: `core/market_data/historical_data_reader.h`

- [ ] **Step 1: Add trade handler state and helpers to the test**

Extend the test handler with `OnTrade()` and add helpers equivalent to existing BookTicker helpers:

```cpp
void OnTrade(const aquila::Trade& trade) noexcept {
  trades.push_back(trade);
}

std::vector<aquila::Trade> trades;
```

Add `MakeTrade()`, `WriteTradeFile()`, `WriteTradeTrailingByteFile()`,
`MakeTradeBinaryReaderConfig()`, and `ExpectTradeEquals()` in the anonymous namespace.

- [ ] **Step 2: Add failing trade reader tests**

Add tests:

```cpp
TEST(HistoricalDataReaderTest, PollReadsTradeRecordsAcrossFiles);
TEST(HistoricalDataReaderTest, DrainReadsTradeRecordsAtMostMaxEvents);
TEST(HistoricalDataReaderTest, RejectsTradeFileWithTrailingBytes);
```

The first two should assert `handler.trades` order and diagnostics `total_count`, `trade_count`, `book_ticker_count`, and `files_completed`.

- [ ] **Step 3: Verify red**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target core_market_data_historical_data_reader_test -j8
ctest --test-dir build/debug -R '^core_market_data_historical_data_reader_test$' --output-on-failure
```

Expected: compile or runtime failure because `HistoricalDataReader` still rejects `feed = "trade"` and diagnostics lack trade counters.

- [ ] **Step 4: Implement typed historical dispatch**

In `core/market_data/historical_data_reader.h`:

- Add `static_assert` for `Trade` layout.
- Add `book_ticker_count` and `trade_count` to `HistoricalDataReaderStats`.
- Add `RecordTrade()` to diagnostics.
- Store `feed_` from the single source.
- Change file size validation to use `RecordSizeForFeed(feed_)`.
- Change `Poll()` / `Drain()` to call `DispatchCurrentRecord(handler)` that reads either `BookTicker` or `Trade`.

- [ ] **Step 5: Verify green**

Run the same build and ctest commands. Expected: historical reader tests pass.

## Task 3: Probe Historical Summary Includes Trades

**Files:**
- Modify: `tools/market_data/data_reader_probe.cpp`

- [ ] **Step 1: Ensure probe summary can be validated manually**

No new C++ unit exists for the Nova summary line. The verification is the final probe dry run in Task 6.

- [ ] **Step 2: Update historical summary log**

Change the `RunHistoricalProbe()` summary format to include:

```text
handler_book_tickers={} handler_trades={} diagnostics_total_count={}
```

Pass `handler.trades()` between book ticker count and total count.

- [ ] **Step 3: Build probe**

Run:

```bash
TMPDIR=/home/liuxiang/tmp cmake --build build/debug --target data_reader_probe -j8
```

Expected: build succeeds.

## Task 4: Manifest Script Supports Trade Feed

**Files:**
- Modify: `scripts/market_data/manifest_to_data_reader_config.py`
- Modify: `scripts/test/market_data/manifest_to_data_reader_config_test.py`

- [ ] **Step 1: Add failing Python tests**

Extend the existing test to assert the default output includes `feed = "book_ticker"`. Add a new test calling:

```python
manifest_config.render_data_reader_config(
    manifest_path=manifest,
    name="trade_replay",
    catalog_path=Path("config/instruments/usdt_futures.csv"),
    feed="trade",
)
```

Assert it contains `name = "trade_replay_trade"` and `feed = "trade"`.

- [ ] **Step 2: Verify red**

Run:

```bash
python3 scripts/test/market_data/manifest_to_data_reader_config_test.py
```

Expected: `TypeError` for unexpected `feed`.

- [ ] **Step 3: Implement `--feed`**

Add `feed: str = "book_ticker"` to `render_data_reader_config()`, validate it is `book_ticker` or `trade`, derive `source_name = f"{name}_{feed}"`, and render `feed = "<feed>"`.

Add CLI arg:

```python
parser.add_argument("--feed", choices=["book_ticker", "trade"], default="book_ticker")
```

Pass `feed=args.feed` to `render_data_reader_config()`.

- [ ] **Step 4: Verify green**

Run the same Python unittest. Expected: passes.

## Task 5: Docs

**Files:**
- Modify: `docs/data_reader_config.md`
- Modify: `docs/lead_lag_live_replay_testing.md`

- [ ] **Step 1: Update binary source support wording**

Change statements saying binary_file only supports BookTicker to say it supports a single `book_ticker` or `trade` source, with no mixed ordering.

- [ ] **Step 2: Update recorder / manifest wording**

Document that Trade binary can now be read by `HistoricalDataReader` and generated from manifests with `--feed trade`, but still does not feed `lead_lag_replay`.

- [ ] **Step 3: Check docs diff**

Run:

```bash
git diff --check -- docs/data_reader_config.md docs/lead_lag_live_replay_testing.md
```

Expected: no output.

## Task 6: Full Verification and Commit

**Files:**
- All modified files.

- [ ] **Step 1: Run focused tests**

Run:

```bash
ctest --test-dir build/debug -R '(data_reader_config|core_market_data_historical_data_reader|data_reader_probe_mode)' --output-on-failure
python3 scripts/test/market_data/manifest_to_data_reader_config_test.py
```

Expected: all tests pass.

- [ ] **Step 2: Run real trade historical probe**

Create `/home/liuxiang/tmp/aquila_trade_historical_probe_<pid>/` artifacts containing a small `Trade` binary and generated `feed = "trade"` TOML. Run:

```bash
./build/debug/tools/data_reader_probe --config <generated.toml> --max-polls 8 --log-every 1
```

Expected: exit 0 and summary includes `handler_trades=<record_count>`.

- [ ] **Step 3: Check worktree**

Run:

```bash
git status --short --branch
git diff --check
```

Expected: only intended files changed, no whitespace errors.

- [ ] **Step 4: Commit**

Run:

```bash
git add core/config/data_reader_config.cpp test/config/data_reader_config_test.cpp core/market_data/historical_data_reader.h test/core/market_data/historical_data_reader_test.cpp tools/market_data/data_reader_probe.cpp scripts/market_data/manifest_to_data_reader_config.py scripts/test/market_data/manifest_to_data_reader_config_test.py docs/data_reader_config.md docs/lead_lag_live_replay_testing.md docs/superpowers/plans/2026-07-06-trade-historical-reader-replay.md
git commit -m "feat: add trade historical data reader"
```
