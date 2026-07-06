#!/usr/bin/env python3

import gzip
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "market_data" / "compare_fusion_tardis_book_ticker.py"
MARKET_DATA_SCRIPT_DIR = REPO_ROOT / "scripts" / "market_data"
if str(MARKET_DATA_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(MARKET_DATA_SCRIPT_DIR))

import typed_binary  # noqa: E402


def load_module():
    spec = importlib.util.spec_from_file_location(
        "compare_fusion_tardis_book_ticker", SCRIPT_PATH
    )
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class CompareFusionTardisBookTickerTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.dtype = self.module.book_ticker_dtype()

    def write_catalog(self, path: Path) -> None:
        path.write_text(
            "symbol_id,symbol,exchange,exchange_symbol,price_tick,quantity_step\n"
            "7,BTC_USDT,gate,BTC_USDT,0.1,0.001\n"
            "7,BTC_USDT,binance,BTCUSDT,0.1,0.001\n"
        )

    def make_fusion_file(self, path: Path) -> None:
        records = np.zeros(4, dtype=self.dtype)
        base_ns = 1_782_518_400_000_000_000
        records["id"] = [100, 101, 102, 103]
        records["symbol_id"] = [7, 7, 7, 8]
        records["exchange"] = [2, 2, 2, 2]
        records["exchange_ns"] = [
            base_ns,
            base_ns + 1_000_000,
            base_ns + 2_000_000,
            base_ns + 1_000_000,
        ]
        records["local_ns"] = records["exchange_ns"] + 100_000
        records["bid_price"] = [100.0, 100.5, 100.7, 200.0]
        records["bid_volume"] = [1.0, 3.0, 3.0, 1.0]
        records["ask_price"] = [101.0, 101.5, 101.7, 201.0]
        records["ask_volume"] = [2.0, 4.0, 4.0, 2.0]
        typed_binary.write_records(path, "book_ticker", records)

    def write_tardis_csv(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with gzip.open(path, "wt", newline="") as handle:
            handle.write(
                "exchange,symbol,timestamp,local_timestamp,ask_amount,ask_price,"
                "bid_price,bid_amount\n"
            )
            handle.write(
                "gate-io-futures,BTC_USDT,1782518400000000,1782518400000100,"
                "2.0,101.0,100.0,1.0\n"
            )
            handle.write(
                "gate-io-futures,BTC_USDT,1782518400001000,1782518400001100,"
                "4.0,101.5,100.5,9.0\n"
            )
            handle.write(
                "gate-io-futures,BTC_USDT,1782518400001000,1782518400001101,"
                "4.0,101.7,100.7,3.0\n"
            )

    def test_compare_detects_matching_and_missing_rows_in_derived_window(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            root = Path(temp_dir)
            catalog = root / "catalog.csv"
            fusion = root / "fusion.bin"
            tardis_root = root / "tardis"
            self.write_catalog(catalog)
            self.make_fusion_file(fusion)
            self.write_tardis_csv(
                tardis_root
                / "gate-io-futures/book_ticker/20260627/"
                / "BTC_USDT-book_ticker-20260627.csv.gz"
            )

            summary = self.module.compare_symbol(
                fusion_paths=[fusion],
                tardis_root=tardis_root,
                catalog_path=catalog,
                exchange="gate",
                symbol="BTC_USDT",
                date="20260627",
                near_ms=2,
                sample_limit=5,
            )

        self.assertEqual(summary["fusion_records"], 3)
        self.assertEqual(summary["tardis_records"], 3)
        self.assertEqual(summary["matched_records"], 1)
        self.assertEqual(summary["fusion_only_records"], 2)
        self.assertEqual(summary["tardis_only_records"], 2)
        self.assertEqual(summary["near_ms"], 2)
        self.assertEqual(summary["near_matched_records"], 1)
        self.assertEqual(summary["fusion_only_after_near_records"], 1)
        self.assertEqual(summary["tardis_only_after_near_records"], 1)
        self.assertEqual(summary["window_start_ms"], 1_782_518_400_000)
        self.assertEqual(summary["window_end_ms"], 1_782_518_400_002)
        self.assertEqual(summary["fusion_only_samples"][0]["timestamp_ms"], 1_782_518_400_001)
        self.assertEqual(summary["tardis_only_samples"][0]["bid_volume_units"], 9000)

    def test_resolves_tardis_path_with_catalog_exchange_symbol(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            root = Path(temp_dir)
            catalog = root / "catalog.csv"
            self.write_catalog(catalog)

            entry = self.module.load_catalog(catalog)[("binance", "BTC_USDT")]
            path = self.module.tardis_book_ticker_path(
                root / "tardis", entry, date="20260627"
            )

        self.assertEqual(
            path,
            root
            / "tardis/binance-futures/book_ticker/20260627/"
            / "BTCUSDT-book_ticker-20260627.csv.gz",
        )


if __name__ == "__main__":
    unittest.main()
