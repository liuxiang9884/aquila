# Common USDT Perp Kline Volatility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Python script that fetches Gate and Binance 1m klines for common USDT perpetual symbols, saves raw per-exchange CSVs under `/home/liuxiang/tmp`, and computes 30m / 60m close-to-close realized volatility.

**Architecture:** Add one script under `scripts/market_data/` with pure parsing/math helpers plus a CLI runner. Add one `unittest` file under `scripts/test/market_data/` covering catalog loading, exchange kline parsing, closed-candle filtering, volatility math, and CSV writers.

**Tech Stack:** Python 3 via `/home/liuxiang/dev/pyenv/lx/bin/python`, standard library `argparse/csv/datetime/json/math/pathlib/time/urllib`, `unittest`.

---

## File Structure

- Create `scripts/market_data/query_common_usdt_perp_klines.py`: REST fetch, kline normalization, volatility computation, CSV writing, CLI.
- Create `scripts/test/market_data/query_common_usdt_perp_klines_test.py`: unit tests with fixture payloads and temporary output files.
- Do not commit `/home/liuxiang/tmp/common_usdt_perp_klines_*` run output.

## Task 1: Write Tests First

**Files:**
- Create: `scripts/test/market_data/query_common_usdt_perp_klines_test.py`
- Create later: `scripts/market_data/query_common_usdt_perp_klines.py`

- [ ] **Step 1: Write failing tests**

Create tests for:

```python
#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import math
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "market_data"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import query_common_usdt_perp_klines as klines


class QueryCommonUsdtPerpKlinesTest(unittest.TestCase):
    def test_load_common_symbols_pairs_gate_and_binance_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            catalog_path = Path(tmp) / "catalog.csv"
            catalog_path.write_text(
                "symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,settle_asset,product_type,status,contract_type,price_tick,price_decimal_places,quantity_step,quantity_decimal_places,min_quantity,max_quantity,max_market_quantity,min_notional,notional_multiplier,price_limit_up,price_limit_down,market_price_bound\n"
                "0,BTC_USDT,gate,BTC_USDT,BTC,USDT,USDT,linear_perpetual,TRADING,direct,0.1,1,1,0,1,1000,,,0.0001,0.5,0.5,\n"
                "0,BTC_USDT,binance,BTCUSDT,BTC,USDT,USDT,linear_perpetual,TRADING,PERPETUAL,0.1,1,0.001,3,0.001,1000,120,100,1,0.05,0.05,0.05\n"
                "1,ONLY_GATE,gate,ONLY_GATE,ONLY,USDT,USDT,linear_perpetual,TRADING,direct,0.1,1,1,0,1,1000,,,0.0001,0.5,0.5,\n",
                encoding="utf-8",
            )

            symbols = klines.load_common_symbols(catalog_path)

        self.assertEqual(len(symbols), 1)
        self.assertEqual(symbols[0].symbol, "BTC_USDT")
        self.assertEqual(symbols[0].gate_symbol, "BTC_USDT")
        self.assertEqual(symbols[0].binance_symbol, "BTCUSDT")

    def test_parse_binance_kline_array(self):
        row = klines.parse_binance_kline(
            "BTC_USDT",
            "BTCUSDT",
            [1000, "10", "12", "9", "11", "100", 60999, "1100", 10, "1", "2", "0"],
            now_ms=70000,
        )

        self.assertEqual(row.exchange, "binance")
        self.assertEqual(row.symbol, "BTC_USDT")
        self.assertEqual(row.open_time_ms, 1000)
        self.assertEqual(row.close_time_ms, 60999)
        self.assertEqual(row.close, 11.0)
        self.assertEqual(row.quote_volume, 1100.0)
        self.assertTrue(row.closed)

    def test_parse_gate_kline_dict_and_array(self):
        dict_row = klines.parse_gate_kline(
            "BTC_USDT",
            "BTC_USDT",
            {"t": 60, "o": "10", "h": "12", "l": "9", "c": "11", "v": "100", "sum": "1100"},
            now_ms=121000,
        )
        array_row = klines.parse_gate_kline(
            "ETH_USDT",
            "ETH_USDT",
            [60, "100", "11", "12", "9", "10", "1100"],
            now_ms=121000,
        )

        self.assertEqual(dict_row.open_time_ms, 60000)
        self.assertEqual(dict_row.close_time_ms, 119999)
        self.assertEqual(dict_row.close, 11.0)
        self.assertTrue(dict_row.closed)
        self.assertEqual(array_row.close, 11.0)

    def test_compute_realized_volatility_uses_last_n_plus_one_closed_closes(self):
        closes = [100.0, 101.0, 99.0, 102.0]
        expected = math.sqrt(
            math.log(101.0 / 100.0) ** 2
            + math.log(99.0 / 101.0) ** 2
            + math.log(102.0 / 99.0) ** 2
        ) * 10000

        actual = klines.realized_vol_bps(closes, 3)

        self.assertAlmostEqual(actual, expected)
        self.assertIsNone(klines.realized_vol_bps(closes, 4))

    def test_write_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp)
            kline_rows = [
                klines.KlineRow(
                    exchange="gate",
                    symbol="BTC_USDT",
                    exchange_symbol="BTC_USDT",
                    open_time_ms=0,
                    close_time_ms=59999,
                    open=10.0,
                    high=11.0,
                    low=9.0,
                    close=10.5,
                    volume=100.0,
                    quote_volume=1000.0,
                    closed=True,
                )
            ]
            summary_rows = [
                {
                    "symbol": "BTC_USDT",
                    "gate_vol_30m_bps": "12.3",
                    "gate_vol_60m_bps": "",
                    "gate_valid_30m": "true",
                    "gate_valid_60m": "false",
                    "gate_close_count": 31,
                    "binance_vol_30m_bps": "10.0",
                    "binance_vol_60m_bps": "",
                    "binance_valid_30m": "true",
                    "binance_valid_60m": "false",
                    "binance_close_count": 31,
                    "max_vol_60m_bps": "",
                    "min_vol_60m_bps": "",
                }
            ]

            klines.write_kline_csv(output_dir / "gate.csv", kline_rows)
            klines.write_summary_csv(output_dir / "summary.csv", summary_rows)

            with (output_dir / "gate.csv").open(newline="", encoding="utf-8") as handle:
                loaded_kline = list(csv.DictReader(handle))
            with (output_dir / "summary.csv").open(newline="", encoding="utf-8") as handle:
                loaded_summary = list(csv.DictReader(handle))

        self.assertEqual(loaded_kline[0]["symbol"], "BTC_USDT")
        self.assertEqual(loaded_summary[0]["gate_valid_30m"], "true")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run tests to verify failure**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/query_common_usdt_perp_klines_test.py
```

Expected: FAIL with `ModuleNotFoundError: No module named 'query_common_usdt_perp_klines'`.

## Task 2: Implement Script

**Files:**
- Create: `scripts/market_data/query_common_usdt_perp_klines.py`
- Test: `scripts/test/market_data/query_common_usdt_perp_klines_test.py`

- [ ] **Step 1: Add implementation**

Implement:

- `CommonSymbol` and `KlineRow` dataclasses.
- `load_common_symbols(catalog_path, requested_symbols=None)`.
- `fetch_binance_klines(exchange_symbol, limit, base_url, timeout)`.
- `fetch_gate_klines(exchange_symbol, limit, base_url, timeout)`.
- `parse_binance_kline(...)`.
- `parse_gate_kline(...)` accepting dict and array payloads.
- `realized_vol_bps(closes, window_minutes)`.
- `build_summary_rows(common_symbols, gate_rows_by_symbol, binance_rows_by_symbol, windows)`.
- `write_kline_csv(...)`, `write_summary_csv(...)`, `write_metadata_json(...)`.
- CLI options: `--catalog`, `--symbols`, `--lookback-minutes`, `--vol-window`, `--output-root`, `--run-id`, `--request-sleep-sec`, `--timeout`, `--gate-base-url`, `--binance-base-url`, `--max-symbols`.

- [ ] **Step 2: Run unit tests**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/query_common_usdt_perp_klines_test.py
```

Expected: `Ran 5 tests` and `OK`.

## Task 3: Live Smoke

**Files:**
- Generated outside repo: `/home/liuxiang/tmp/common_usdt_perp_klines_<run_id>/`

- [ ] **Step 1: Run small live smoke**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/market_data/query_common_usdt_perp_klines.py \
  --symbols BTC_USDT,ETH_USDT \
  --lookback-minutes 70 \
  --request-sleep-sec 0.05
```

Expected: exit `0`, run directory under `/home/liuxiang/tmp`, two kline CSVs, summary CSV, metadata JSON.

- [ ] **Step 2: Inspect smoke output**

Run:

```bash
ls -l /home/liuxiang/tmp/<run_id>/
head -5 /home/liuxiang/tmp/<run_id>/volatility_summary.csv
```

Expected: `BTC_USDT` and `ETH_USDT` rows with valid 30m and 60m fields if enough closed candles are returned.

## Task 4: Final Verification And Commit

**Files:**
- `scripts/market_data/query_common_usdt_perp_klines.py`
- `scripts/test/market_data/query_common_usdt_perp_klines_test.py`

- [ ] **Step 1: Run final verification**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/query_common_usdt_perp_klines_test.py
git diff --check
```

Expected: tests pass; no whitespace errors.

- [ ] **Step 2: Commit**

Run:

```bash
git add scripts/market_data/query_common_usdt_perp_klines.py \
  scripts/test/market_data/query_common_usdt_perp_klines_test.py
git commit -m "Add common USDT perpetual kline volatility tool"
```

Expected: commit succeeds. Do not add `/home/liuxiang/tmp` outputs.
