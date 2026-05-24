# LeadLag Live Replay Compare Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 跑一次 30 分钟 LeadLag signal-only 实盘、同步记录 DataReader binary、replay 生成信号，并输出 live/replay 信号差异报告。

**Architecture:** 复用现有 `lead_lag_strategy`、`data_reader_recorder`、`lead_lag_replay` CLI；新增一个轻量 Python CSV 对比工具，按 signal key 统计 matched、live-only、replay-only 和字段差异。运行时在 `/home/liuxiang/tmp/<run_id>` 生成临时 config，让所有 log、CSV、bin 和报告落到同一目录。

**Tech Stack:** Python `unittest`、标准库 `csv/json/argparse`；现有 C++ debug binaries。

---

### Task 1: Signal CSV Compare Tool

**Files:**
- Create: `scripts/lead_lag/compare_signal_csv.py`
- Create: `scripts/lead_lag/compare_signal_csv_test.py`

- [ ] **Step 1: Write failing unit tests**

Create tests covering:
- identical rows produce one matched row and zero differences;
- rows only present in live/replay are counted separately;
- matched keys with different action/side/price fields are reported as mismatches.

Run:

```bash
PYTHONPATH=scripts/lead_lag python3 scripts/lead_lag/compare_signal_csv_test.py
```

Expected: fails because `compare_signal_csv` does not exist yet.

- [ ] **Step 2: Implement minimal compare tool**

Implement:
- CSV loading with row numbers;
- configurable key fields defaulting to `ticker_id,symbol_id,exchange,role`;
- compared fields defaulting to signal intent, price, event time, drift and group diagnostics;
- numeric tolerance for `price`;
- JSON and markdown summary output.

- [ ] **Step 3: Verify tests**

Run:

```bash
PYTHONPATH=scripts/lead_lag python3 scripts/lead_lag/compare_signal_csv_test.py
```

Expected: all tests pass.

### Task 2: 30 Minute Live/Replay Run

**Files:**
- Runtime only: `/home/liuxiang/tmp/lead_lag_live_replay_compare_<timestamp>/`

- [ ] **Step 1: Generate run directory and temp configs**

Create copied configs under the run directory:
- Gate/Binance data session configs with log file paths under the run directory;
- live strategy config using `read_mode = "latest"`;
- recorder data reader config using `read_mode = "drain"`;
- replay binary data reader config pointing at the recorded `.bin`.

- [ ] **Step 2: Run live signal-only and recorder**

Start Gate/Binance data sessions, then run:

```bash
lead_lag_strategy --config <run_dir>/strategy_live.toml --connect-data --duration-sec 1800 --signals-output <run_dir>/live_signals.csv
data_reader_recorder --config <run_dir>/data_reader_drain.toml --output <run_dir>/recorded_book_ticker.bin --mode truncate
```

Stop recorder and data sessions after strategy completes.

- [ ] **Step 3: Replay and compare**

Run:

```bash
lead_lag_replay --config <run_dir>/strategy_replay.toml --data-reader-config <run_dir>/data_reader_binary.toml --signals-output <run_dir>/replay_signals.csv
python3 scripts/lead_lag/compare_signal_csv.py <run_dir>/live_signals.csv <run_dir>/replay_signals.csv --json-output <run_dir>/compare_summary.json --markdown-output <run_dir>/compare_summary.md
```

Expected: report includes counts and examples for matched, live-only, replay-only, and mismatched signals.

### Task 3: Verification And Commit

**Files:**
- Modify/create files from Task 1 and this plan.

- [ ] **Step 1: Run checks**

Run:

```bash
PYTHONPATH=scripts/lead_lag python3 scripts/lead_lag/compare_signal_csv_test.py
git diff --check
```

- [ ] **Step 2: Commit**

Commit only the new compare tool, tests, and plan:

```bash
git add scripts/lead_lag/compare_signal_csv.py scripts/lead_lag/compare_signal_csv_test.py doc/superpowers/plans/2026-05-24-lead-lag-live-replay-compare-plan.md
git commit -m "Add lead lag signal compare tool"
```
