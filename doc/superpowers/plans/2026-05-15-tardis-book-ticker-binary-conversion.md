# Tardis Book Ticker Binary Conversion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 Tardis `book_ticker` CSV gzip 数据转换为每日一个、可直接按 `aquila::BookTicker` 读取的裸 binary 文件。

**Architecture:** instrument catalog 提供 canonical symbol、exchange symbol 和 `symbol_id`。转换库负责解析 CSV 行、按交易所撮合时间排序、写出裸 `BookTicker` 数组；CLI 负责按日期构造 Binance/Gate 输入路径、读取 gzip、生成每日输出。

**Tech Stack:** C++20、CMake、GTest、CLI11、fmt、现有 `aquila_core` / `aquila_config`。

---

### Task 1: 补齐 Instrument Catalog

**Files:**
- Modify: `config/instruments/usdt_futures.csv`
- Test: `test/config/data_session_config_test.cpp`

- [x] **Step 1: Add ORDI_USDT rows**

添加 Gate `ORDI_USDT` 与 Binance `ORDIUSDT` 两行，二者共享 canonical symbol `ORDI_USDT` 和 `symbol_id=3`。

- [x] **Step 2: Run catalog config test**

Run: `cmake --build build/debug --target data_session_config_test -j$(nproc)`

Run: `./build/debug/test/config/data_session_config_test`

Expected: PASS。

### Task 2: 转换库 TDD

**Files:**
- Create: `tools/tardis/book_ticker_binary_converter.h`
- Test: `test/tools/tardis/book_ticker_binary_converter_test.cpp`
- Modify: `test/tools/CMakeLists.txt`
- Create: `test/tools/tardis/CMakeLists.txt`

- [x] **Step 1: Write failing tests**

覆盖从 Tardis CSV 行映射到 `BookTicker`、微秒到纳秒转换、按 `exchange_ns` 合并排序、写出裸 struct binary。

- [x] **Step 2: Verify RED**

Run: `cmake --build build/debug --target book_ticker_binary_converter_test -j$(nproc)`

Expected: build fails because converter header/target is not implemented yet.

- [x] **Step 3: Implement minimal converter library**

实现 `ReadBookTickerCsv`、`MergeBookTickerRecords`、`WriteBookTickerBinaryFile`、`ReadBookTickerBinaryFile`。

- [x] **Step 4: Verify GREEN**

Run: `cmake --build build/debug --target book_ticker_binary_converter_test -j$(nproc)`

Run: `./build/debug/test/tools/tardis/book_ticker_binary_converter_test`

Expected: PASS。

### Task 3: CLI 工具和实际数据生成

**Files:**
- Create: `tools/tardis/book_ticker_to_binary.cpp`
- Modify: `tools/CMakeLists.txt`

- [x] **Step 1: Add CLI target**

命令参数：`--data-root`、`--instrument-catalog`、`--symbol`、`--start-date`、`--end-date`、`--output-dir`。

- [x] **Step 2: Build target**

Run: `cmake --build build/debug --target tardis_book_ticker_to_binary -j$(nproc)`

Expected: build succeeds.

- [x] **Step 3: Generate ORDI_USDT binary files**

Run:

```bash
./build/debug/tools/tardis_book_ticker_to_binary \
  --data-root /home/liuxiang/tardis \
  --instrument-catalog config/instruments/usdt_futures.csv \
  --symbol ORDI_USDT \
  --start-date 20260415 \
  --end-date 20260417 \
  --output-dir /home/liuxiang/tardis/merged_book_ticker/ORDI_USDT
```

Expected: writes `20260415.bin`、`20260416.bin`、`20260417.bin`。

### Task 4: 收尾验证和提交

**Files:**
- All files above.

- [x] **Step 1: Run focused tests**

Run converter test and config catalog test.

- [x] **Step 2: Run output verification**

确认输出文件大小是 `sizeof(BookTicker)` 的整数倍，且工具输出的记录数与文件大小一致。

- [x] **Step 3: Run diff check**

Run: `git diff --check`

- [x] **Step 4: Commit**

Commit message: `Add Tardis book ticker binary converter`
