#!/usr/bin/env python3

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "market_data" / "analyze_book_ticker_latency.py"
MARKET_DATA_SCRIPT_DIR = REPO_ROOT / "scripts" / "market_data"
if str(MARKET_DATA_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(MARKET_DATA_SCRIPT_DIR))

import typed_binary  # noqa: E402


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_book_ticker_latency", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class AnalyzeBookTickerLatencyTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()

    def test_dtype_matches_book_ticker_abi(self):
        dtype = self.module.book_ticker_dtype()

        self.assertEqual(dtype.itemsize, 72)
        self.assertEqual(dtype.names[0:10], (
            "id",
            "symbol_id",
            "exchange",
            "exchange_ns",
            "event_ns",
            "local_ns",
            "bid_price",
            "bid_volume",
            "ask_price",
            "ask_volume",
        ))
        self.assertEqual(dtype.fields["exchange_ns"][1], 16)
        self.assertEqual(dtype.fields["event_ns"][1], 24)
        self.assertEqual(dtype.fields["local_ns"][1], 32)
        self.assertEqual(dtype.fields["ask_volume"][1], 64)

    def test_load_book_tickers_reads_typed_binary(self):
        dtype = self.module.book_ticker_dtype()
        records = np.zeros(2, dtype=dtype)
        records["id"] = [10, 11]
        records["exchange"] = [2, 2]
        records["exchange_ns"] = [1_000_000, 2_000_000]
        records["local_ns"] = [1_001_500, 2_002_500]

        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(path, "book_ticker", records)

            loaded = self.module.load_book_tickers(path)

        np.testing.assert_array_equal(loaded["id"], records["id"])
        np.testing.assert_array_equal(loaded["local_ns"], records["local_ns"])

    def test_summarize_latency_reports_percentiles_and_top_outliers(self):
        dtype = self.module.book_ticker_dtype()
        records = np.zeros(5, dtype=dtype)
        records["id"] = [10, 11, 12, 13, 14]
        records["symbol_id"] = [92, 92, 92, 92, 92]
        records["exchange"] = [2, 2, 2, 2, 2]
        records["exchange_ns"] = [1_000, 2_000, 3_000, 4_000, 5_000]
        records["local_ns"] = [1_100, 2_200, 2_950, 4_400, 5_800]
        records["bid_price"] = [1, 2, 3, 4, 5]
        records["ask_price"] = [2, 3, 4, 5, 6]

        summary = self.module.summarize_latency(records, top_n=2)

        self.assertEqual(summary["count"], 5)
        self.assertEqual(summary["negative_count"], 1)
        self.assertEqual(summary["latency_ns"]["min"], -50)
        self.assertEqual(summary["latency_ns"]["max"], 800)
        self.assertEqual(summary["latency_ns"]["p50"], 200)
        self.assertEqual(summary["by_exchange"]["kGate"]["count"], 5)
        self.assertEqual([row["id"] for row in summary["top_outliers"]], [14, 13])

    def test_summarize_latency_rejects_empty_input(self):
        dtype = self.module.book_ticker_dtype()
        records = np.zeros(0, dtype=dtype)

        with self.assertRaisesRegex(ValueError, "no BookTicker records"):
            self.module.summarize_latency(records)


if __name__ == "__main__":
    unittest.main()
