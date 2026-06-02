# Common USDT Perp Catalog Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a Python script that fetches all tradable Gate and Binance USDT perpetual futures, takes their symbol intersection, and writes a new `aquila.instrument.v1` instrument catalog.

**Architecture:** Add one focused script under `scripts/instruments/` that imports existing Gate and Binance metadata mappers, adds all-contract fetch/filter helpers, and writes the current catalog CSV schema. Add unit tests under `scripts/test/instruments/` covering pure filtering, normalization, intersection, catalog rows, and overwrite protection.

**Tech Stack:** Python 3 via `/home/liuxiang/dev/pyenv/lx/bin/python`, standard library `argparse/csv/json/pathlib/urllib`, existing `pandas`-based Gate and Binance query modules, `unittest`.

---

## File Structure

- Create `scripts/instruments/generate_common_usdt_perp_catalog.py`: CLI, REST fetch helpers, symbol normalization, intersection, catalog row generation, and CSV writer.
- Create `scripts/test/instruments/generate_common_usdt_perp_catalog_test.py`: unit tests for pure helpers and overwrite behavior.
- Modify no existing default catalog; generated live output goes to `config/instruments/usdt_futures_common_gate_binance_<YYYYMMDD>.csv`.
- Optionally create `scripts/instruments/__pycache__/` only through Python execution; do not commit generated cache files.

## Task 1: Tests For Filtering And Catalog Rows

**Files:**
- Create: `scripts/test/instruments/generate_common_usdt_perp_catalog_test.py`
- Create later: `scripts/instruments/generate_common_usdt_perp_catalog.py`

- [ ] **Step 1: Write failing tests**

Create `scripts/test/instruments/generate_common_usdt_perp_catalog_test.py` with these tests:

```python
#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "instruments"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import generate_common_usdt_perp_catalog as catalog


class GenerateCommonUsdtPerpCatalogTest(unittest.TestCase):
    def test_binance_filter_keeps_only_trading_usdt_perpetuals(self):
        rows = catalog.select_binance_usdt_perpetuals(
            {
                "symbols": [
                    {
                        "symbol": "BTCUSDT",
                        "baseAsset": "BTC",
                        "quoteAsset": "USDT",
                        "marginAsset": "USDT",
                        "contractType": "PERPETUAL",
                        "status": "TRADING",
                        "filters": [],
                    },
                    {
                        "symbol": "ETHUSDC",
                        "baseAsset": "ETH",
                        "quoteAsset": "USDC",
                        "marginAsset": "USDC",
                        "contractType": "PERPETUAL",
                        "status": "TRADING",
                        "filters": [],
                    },
                    {
                        "symbol": "SOLUSDT",
                        "baseAsset": "SOL",
                        "quoteAsset": "USDT",
                        "marginAsset": "USDT",
                        "contractType": "CURRENT_QUARTER",
                        "status": "TRADING",
                        "filters": [],
                    },
                    {
                        "symbol": "OLDUSDT",
                        "baseAsset": "OLD",
                        "quoteAsset": "USDT",
                        "marginAsset": "USDT",
                        "contractType": "PERPETUAL",
                        "status": "BREAK",
                        "filters": [],
                    },
                ]
            }
        )

        self.assertEqual([catalog.binance_internal_symbol(row) for row in rows], ["BTC_USDT"])

    def test_gate_filter_keeps_only_trading_usdt_contracts(self):
        rows = catalog.select_gate_usdt_perpetuals(
            [
                {"name": "BTC_USDT", "status": "trading", "type": "direct"},
                {"name": "ETH_USD", "status": "trading", "type": "direct"},
                {"name": "SOL_USDT", "status": "delisted", "type": "direct"},
            ]
        )

        self.assertEqual([catalog.gate_internal_symbol(row) for row in rows], ["BTC_USDT"])

    def test_build_catalog_rows_assigns_shared_symbol_id(self):
        gate_payload = {
            "name": "BTC_USDT",
            "type": "direct",
            "status": "trading",
            "quanto_multiplier": "0.0001",
            "order_price_round": "0.1",
            "order_size_min": "1",
            "order_size_max": "1000",
            "market_order_size_max": "100",
            "order_price_deviate": "0.5",
            "market_order_slip_ratio": "0.025",
        }
        binance_payload = {
            "symbol": "BTCUSDT",
            "baseAsset": "BTC",
            "quoteAsset": "USDT",
            "marginAsset": "USDT",
            "contractType": "PERPETUAL",
            "status": "TRADING",
            "marketTakeBound": "0.05",
            "filters": [
                {"filterType": "PRICE_FILTER", "tickSize": "0.10"},
                {"filterType": "LOT_SIZE", "minQty": "0.001", "maxQty": "1000", "stepSize": "0.001"},
                {"filterType": "MARKET_LOT_SIZE", "maxQty": "120"},
                {"filterType": "MIN_NOTIONAL", "notional": "100"},
                {"filterType": "PERCENT_PRICE", "multiplierUp": "1.05", "multiplierDown": "0.95"},
            ],
        }

        rows = catalog.build_catalog_rows([gate_payload], [binance_payload])

        self.assertEqual(catalog.CATALOG_COLUMNS, list(rows[0].keys()))
        self.assertEqual(len(rows), 2)
        self.assertEqual([row["symbol_id"] for row in rows], [0, 0])
        self.assertEqual([row["symbol"] for row in rows], ["BTC_USDT", "BTC_USDT"])
        self.assertEqual([row["exchange"] for row in rows], ["gate", "binance"])
        self.assertEqual([row["product_type"] for row in rows], ["linear_perpetual", "linear_perpetual"])
        self.assertEqual(rows[1]["exchange_symbol"], "BTCUSDT")

    def test_write_catalog_refuses_to_overwrite_by_default(self):
        rows = [
            {
                column: ""
                for column in catalog.CATALOG_COLUMNS
            }
        ]
        rows[0].update(
            {
                "symbol_id": 0,
                "symbol": "BTC_USDT",
                "exchange": "gate",
                "exchange_symbol": "BTC_USDT",
                "base_asset": "BTC",
                "quote_asset": "USDT",
                "settle_asset": "USDT",
                "product_type": "linear_perpetual",
                "status": "TRADING",
                "contract_type": "direct",
                "price_tick": 0.1,
                "price_decimal_places": 1,
                "min_quantity": 1,
                "max_quantity": 100,
                "notional_multiplier": 0.0001,
            }
        )

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "catalog.csv"
            catalog.write_catalog_csv(output, rows, overwrite=False)

            with self.assertRaises(FileExistsError):
                catalog.write_catalog_csv(output, rows, overwrite=False)

            catalog.write_catalog_csv(output, rows, overwrite=True)
            with output.open(newline="", encoding="utf-8") as handle:
                loaded = list(csv.DictReader(handle))
            self.assertEqual(loaded[0]["symbol"], "BTC_USDT")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run tests to verify failure**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/instruments/generate_common_usdt_perp_catalog_test.py
```

Expected: FAIL with `ModuleNotFoundError: No module named 'generate_common_usdt_perp_catalog'`.

## Task 2: Script Implementation

**Files:**
- Create: `scripts/instruments/generate_common_usdt_perp_catalog.py`
- Test: `scripts/test/instruments/generate_common_usdt_perp_catalog_test.py`

- [ ] **Step 1: Implement the script**

Create `scripts/instruments/generate_common_usdt_perp_catalog.py` with:

- `CATALOG_COLUMNS` matching `config/instruments/usdt_futures.csv`.
- Import path setup for existing `scripts/gate/query_futures_contracts.py` and `scripts/binance/query_um_futures_contracts.py`.
- `fetch_gate_contracts()` calling `GET /futures/usdt/contracts` with `X-Gate-Size-Decimal: 1`.
- `fetch_binance_exchange_info()` delegating to existing Binance helper.
- `select_binance_usdt_perpetuals(payload)`.
- `select_gate_usdt_perpetuals(payloads)`.
- `binance_internal_symbol(payload)` and `gate_internal_symbol(payload)`.
- `build_catalog_rows(gate_payloads, binance_payloads)` that sorts intersection symbols alphabetically and emits Gate then Binance rows for each symbol.
- `write_catalog_csv(output, rows, overwrite)`.
- CLI options `--output`, `--output-dir`, `--date`, `--overwrite`, `--gate-base-url`, `--binance-base-url`, `--timeout`, `--format`.

- [ ] **Step 2: Run unit tests**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/instruments/generate_common_usdt_perp_catalog_test.py
```

Expected: `Ran 4 tests` and `OK`.

## Task 3: Generate Live Catalog

**Files:**
- Generated: `config/instruments/usdt_futures_common_gate_binance_<YYYYMMDD>.csv`

- [ ] **Step 1: Run script against live REST endpoints**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/instruments/generate_common_usdt_perp_catalog.py \
  --output-dir config/instruments
```

Expected: exit `0`, stdout reports Gate count, Binance count, intersection count, and output path.

- [ ] **Step 2: Inspect generated catalog**

Run:

```bash
head -5 config/instruments/usdt_futures_common_gate_binance_20260602.csv
python3 - <<'PY'
import csv
from pathlib import Path
path = Path("config/instruments/usdt_futures_common_gate_binance_20260602.csv")
rows = list(csv.DictReader(path.open(newline="", encoding="utf-8")))
symbols = sorted({row["symbol"] for row in rows})
print("rows", len(rows))
print("symbols", len(symbols))
print("first", symbols[:5])
print("last", symbols[-5:])
print("all_have_two_rows", all(sum(1 for row in rows if row["symbol"] == symbol) == 2 for symbol in symbols))
PY
```

Expected: row count is exactly `symbols * 2`, and `all_have_two_rows True`.

## Task 4: Verification And Commit

**Files:**
- `scripts/instruments/generate_common_usdt_perp_catalog.py`
- `scripts/test/instruments/generate_common_usdt_perp_catalog_test.py`
- `config/instruments/usdt_futures_common_gate_binance_20260602.csv`

- [ ] **Step 1: Run final verification**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/instruments/generate_common_usdt_perp_catalog_test.py
git diff --check
```

Expected: unit tests pass; `git diff --check` prints no errors.

- [ ] **Step 2: Check git status**

Run:

```bash
git status --short --branch
```

Expected: only the new script, new test, new generated catalog, and the already committed plan/design history are relevant.

- [ ] **Step 3: Commit implementation**

Run:

```bash
git add scripts/instruments/generate_common_usdt_perp_catalog.py \
  scripts/test/instruments/generate_common_usdt_perp_catalog_test.py \
  config/instruments/usdt_futures_common_gate_binance_20260602.csv
git commit -m "Add common USDT perpetual instrument catalog"
```

Expected: commit succeeds with only implementation artifacts.
