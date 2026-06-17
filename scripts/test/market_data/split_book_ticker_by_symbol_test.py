#!/usr/bin/env python3

import importlib.util
import tempfile
import unittest
from pathlib import Path

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "market_data" / "split_book_ticker_by_symbol.py"


def load_module():
    spec = importlib.util.spec_from_file_location("split_book_ticker_by_symbol", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class SplitBookTickerBySymbolTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.dtype = self.module.book_ticker_dtype()

    def make_records(self, rows) -> np.ndarray:
        records = np.zeros(len(rows), dtype=self.dtype)
        for index, (record_id, symbol_id) in enumerate(rows):
            records[index]["id"] = record_id
            records[index]["symbol_id"] = symbol_id
            records[index]["exchange"] = 2
            records[index]["exchange_ns"] = record_id * 1_000
            records[index]["local_ns"] = record_id * 1_000 + 100
            records[index]["bid_price"] = 10.0 + record_id
            records[index]["ask_price"] = 11.0 + record_id
        return records

    def write_catalog(self, path: Path) -> None:
        path.write_text(
            "symbol_id,symbol,exchange,exchange_symbol\n"
            "71,BEAT_USDT,gate,BEAT_USDT\n"
            "71,BEAT_USDT,binance,BEATUSDT\n"
            "92,CLO_USDT,gate,CLO_USDT\n"
        )

    def test_split_appends_multiple_input_files_to_one_symbol_file(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            temp_path = Path(temp_dir)
            catalog = temp_path / "catalog.csv"
            self.write_catalog(catalog)
            first = temp_path / "first.bin"
            second = temp_path / "second.bin"
            output_root = temp_path / "out"

            self.make_records([(1, 71), (2, 92), (3, 71)]).tofile(first)
            self.make_records([(4, 92), (5, 71)]).tofile(second)

            summary = self.module.split_book_ticker_files_by_symbol(
                input_paths=[first, second],
                catalog_path=catalog,
                output_root=output_root,
                run_id="run_a",
                symbols={"BEAT_USDT"},
                chunk_records=2,
            )

            beat_path = output_root / "run_a" / "BEAT_USDT.bin"
            self.assertTrue(beat_path.exists())
            self.assertFalse((output_root / "run_a" / "CLO_USDT.bin").exists())
            loaded = np.fromfile(beat_path, dtype=self.dtype)
            np.testing.assert_array_equal(loaded["id"], np.array([1, 3, 5]))
            self.assertEqual(summary.total_records_read, 5)
            self.assertEqual(summary.records_written_by_symbol, {"BEAT_USDT": 3})

    def test_split_all_known_symbols_creates_one_file_per_seen_symbol(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            temp_path = Path(temp_dir)
            catalog = temp_path / "catalog.csv"
            self.write_catalog(catalog)
            source = temp_path / "source.bin"
            output_root = temp_path / "out"

            self.make_records([(10, 71), (11, 92), (12, 71)]).tofile(source)

            summary = self.module.split_book_ticker_files_by_symbol(
                input_paths=[source],
                catalog_path=catalog,
                output_root=output_root,
                run_id="run_b",
                symbols=None,
                chunk_records=3,
            )

            beat = np.fromfile(output_root / "run_b" / "BEAT_USDT.bin", dtype=self.dtype)
            clo = np.fromfile(output_root / "run_b" / "CLO_USDT.bin", dtype=self.dtype)
            np.testing.assert_array_equal(beat["id"], np.array([10, 12]))
            np.testing.assert_array_equal(clo["id"], np.array([11]))
            self.assertEqual(summary.files_processed, 1)
            self.assertEqual(summary.records_written_by_symbol["BEAT_USDT"], 2)
            self.assertEqual(summary.records_written_by_symbol["CLO_USDT"], 1)


if __name__ == "__main__":
    unittest.main()
