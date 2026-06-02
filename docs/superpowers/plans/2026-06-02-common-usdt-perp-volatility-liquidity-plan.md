# Common USDT Perp Volatility / Liquidity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add per-exchange volatility and liquidity CSV outputs for Gate / Binance common USDT perpetual 1m kline runs.

**Architecture:** Extend the existing kline volatility script with pure helper functions that build per-exchange result rows from normalized `KlineRow` data. Keep raw kline CSV and existing combined summary unchanged, and write two new CSV files from the same fetched latest data.

**Tech Stack:** Python 3.12, `unittest`, CSV writer, existing Gate / Binance REST kline normalizer.

---

## Task 1: Add Failing Tests

**Files:**

- Modify: `scripts/test/market_data/query_common_usdt_perp_klines_test.py`

Steps:

- [x] Add a test that builds several closed `KlineRow` objects, calls `build_exchange_result_rows(...)`, and expects:
  - `vol_30m_bps` to be populated when 31 closed closes are available.
  - `quote_volume_30m` to equal the sum of the latest 30 closed rows.
  - `volume_30m` to equal the sum of the latest 30 closed rows.
  - `valid_60m=false` when 61 closed closes are unavailable.
- [x] Add a writer test that calls `write_exchange_result_csv(...)` and verifies `quote_volume_30m` appears in the CSV.
- [x] Run `/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/query_common_usdt_perp_klines_test.py` and confirm the tests fail because the new helpers do not exist.

## Task 2: Implement Per-Exchange Result Helpers

**Files:**

- Modify: `scripts/market_data/query_common_usdt_perp_klines.py`

Steps:

- [x] Add `EXCHANGE_RESULT_COLUMNS`.
- [x] Add helpers that sort closed rows by `open_time_ms`, compute latest-N volume / quote volume sums, latest close metadata, and per-window volatility.
- [x] Add `build_exchange_result_rows(exchange, symbols, rows_by_symbol, windows)`.
- [x] Add `write_exchange_result_csv(output_path, rows)`.
- [x] Run the kline unit test and confirm it passes.

## Task 3: Wire CLI Output

**Files:**

- Modify: `scripts/market_data/query_common_usdt_perp_klines.py`

Steps:

- [x] In `run(...)`, build Gate and Binance per-exchange result rows after `summary_rows`.
- [x] Write `gate_volatility_liquidity.csv` and `binance_volatility_liquidity.csv` into the run directory.
- [x] Add output file paths to `run_metadata.json`.
- [x] Print the two new paths in `main(...)`.

## Task 4: Verify And Run Latest Data

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/query_common_usdt_perp_klines_test.py
git diff --check
/home/liuxiang/dev/pyenv/lx/bin/python scripts/market_data/query_common_usdt_perp_klines.py \
  --catalog config/instruments/usdt_futures_common_gate_binance_20260602.csv \
  --lookback-minutes 120 \
  --vol-window 30 \
  --vol-window 60
```

Expected:

- Unit tests pass.
- No whitespace errors.
- Latest run writes raw Gate / Binance kline CSVs plus `gate_volatility_liquidity.csv` and `binance_volatility_liquidity.csv` under `/home/liuxiang/tmp/<run_id>/`.

## Task 5: Commit

Run:

```bash
git status --short --branch
git add docs/superpowers/specs/2026-06-02-common-usdt-perp-volatility-liquidity-design.md \
  docs/superpowers/plans/2026-06-02-common-usdt-perp-volatility-liquidity-plan.md \
  scripts/market_data/query_common_usdt_perp_klines.py \
  scripts/test/market_data/query_common_usdt_perp_klines_test.py
git commit -m "Add per-exchange volatility liquidity outputs"
```
