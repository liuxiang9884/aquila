#!/usr/bin/env python3

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "market_data" / "analyze_book_ticker_fusion_latency.py"
MARKET_DATA_SCRIPT_DIR = REPO_ROOT / "scripts" / "market_data"
if str(MARKET_DATA_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(MARKET_DATA_SCRIPT_DIR))

import typed_binary  # noqa: E402


def load_module():
    spec = importlib.util.spec_from_file_location("analyze_book_ticker_fusion_latency", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class AnalyzeBookTickerFusionLatencyTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()
        self.dtype = self.module.book_ticker_dtype()

    def make_records(self, rows):
        records = np.zeros(len(rows), dtype=self.dtype)
        for index, (record_id, latency_ns) in enumerate(rows):
            records[index]["id"] = record_id
            records[index]["symbol_id"] = 92
            records[index]["exchange"] = 2
            records[index]["exchange_ns"] = record_id * 1_000_000
            records[index]["local_ns"] = records[index]["exchange_ns"] + latency_ns
            records[index]["bid_price"] = 100.0 + index
            records[index]["ask_price"] = 101.0 + index
        return records

    def make_source_records(self, rows, source_offset=0):
        records = np.zeros(len(rows), dtype=self.dtype)
        for index, (record_id, latency_ns) in enumerate(rows):
            records[index]["id"] = record_id
            records[index]["symbol_id"] = 92
            records[index]["exchange"] = 2
            records[index]["exchange_ns"] = record_id * 1_000
            records[index]["local_ns"] = records[index]["exchange_ns"] + latency_ns
            records[index]["bid_price"] = 100.0 + source_offset + index
            records[index]["bid_volume"] = 1.0 + index
            records[index]["ask_price"] = 101.0 + source_offset + index
            records[index]["ask_volume"] = 2.0 + index
        return records

    def test_fusion_metadata_dtype_matches_sidecar_record_abi(self):
        dtype = self.module.fusion_metadata_dtype()

        self.assertEqual(dtype.itemsize, 48)
        self.assertEqual(
            dtype.names,
            (
                "source_id",
                "symbol_id",
                "record_id",
                "exchange_ns",
                "event_ns",
                "source_local_ns",
                "fusion_publish_ns",
            ),
        )
        self.assertEqual(dtype.fields["record_id"][1], 8)
        self.assertEqual(dtype.fields["event_ns"][1], 24)
        self.assertEqual(dtype.fields["fusion_publish_ns"][1], 40)

    def test_analyze_published_fusion_latency_uses_fusion_and_metadata_bins(self):
        source_records_by_id = {
            0: self.make_source_records([(100, 120), (101, 150)], source_offset=0),
            1: self.make_source_records([(100, 80), (102, 200)], source_offset=10),
        }
        fusion_records = self.make_source_records([(100, 90), (101, 155)], source_offset=100)

        metadata_dtype = self.module.fusion_metadata_dtype()
        metadata_records = np.zeros(2, dtype=metadata_dtype)
        metadata_records[0]["source_id"] = 1
        metadata_records[0]["symbol_id"] = 92
        metadata_records[0]["record_id"] = 100
        metadata_records[0]["exchange_ns"] = 100_000
        metadata_records[0]["event_ns"] = 100_000
        metadata_records[0]["source_local_ns"] = 100_080
        metadata_records[0]["fusion_publish_ns"] = 100_090
        metadata_records[1]["source_id"] = 0
        metadata_records[1]["symbol_id"] = 92
        metadata_records[1]["record_id"] = 101
        metadata_records[1]["exchange_ns"] = 101_000
        metadata_records[1]["event_ns"] = 101_000
        metadata_records[1]["source_local_ns"] = 101_150
        metadata_records[1]["fusion_publish_ns"] = 101_155

        summary = self.module.analyze_published_fusion_latency(
            source_records_by_id,
            fusion_records,
            metadata_records,
            top_n=1,
        )

        self.assertEqual(summary["source_ids"], [0, 1])
        self.assertEqual(summary["sources"]["0"]["count"], 2)
        self.assertEqual(summary["sources"]["0"]["latency_ns"]["max"], 150)
        self.assertEqual(summary["sources"]["1"]["latency_ns"]["min"], 80)

        self.assertEqual(summary["fusion"]["count"], 2)
        self.assertEqual(summary["fusion"]["latency_ns"]["min"], 90)
        self.assertEqual(summary["fusion"]["latency_ns"]["max"], 155)

        metadata = summary["metadata"]
        self.assertEqual(metadata["count"], 2)
        self.assertEqual(metadata["winner_counts"], {"0": 1, "1": 1})
        self.assertEqual(metadata["winner_ratio"], {"0": 0.5, "1": 0.5})
        self.assertEqual(metadata["source_latency_ns"]["min"], 80)
        self.assertEqual(metadata["source_latency_ns"]["max"], 150)
        self.assertEqual(metadata["fusion_hop_ns"]["min"], 5)
        self.assertEqual(metadata["fusion_hop_ns"]["max"], 10)

        self.assertEqual(
            summary["fusion_metadata_alignment"],
            {
                "fusion_count": 2,
                "metadata_count": 2,
                "compared_count": 2,
                "mismatch_count": 0,
                "fusion_without_metadata_count": 0,
                "metadata_without_fusion_count": 0,
            },
        )
        self.assertEqual(summary["source_metadata_alignment"]["matched_count"], 2)
        self.assertEqual(summary["source_metadata_alignment"]["missing_count"], 0)
        self.assertEqual(summary["top_fusion_hop_outliers"][0]["id"], 100)

    def test_load_fusion_metadata_reads_fixed_size_records(self):
        dtype = self.module.fusion_metadata_dtype()
        records = np.zeros(2, dtype=dtype)
        records["source_id"] = [0, 1]
        records["symbol_id"] = [92, 93]
        records["record_id"] = [100, 200]
        records["exchange_ns"] = [1_000, 2_000]
        records["event_ns"] = [1_000, 2_000]
        records["source_local_ns"] = [1_100, 2_100]
        records["fusion_publish_ns"] = [1_120, 2_130]

        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "fusion_metadata.bin"
            records.tofile(path)

            loaded = self.module.load_fusion_metadata(path)

        np.testing.assert_array_equal(loaded["source_id"], records["source_id"])
        np.testing.assert_array_equal(
            loaded["fusion_publish_ns"], records["fusion_publish_ns"]
        )

    def test_load_book_tickers_reads_typed_book_ticker_binary(self):
        records = self.make_records([(10, 100), (11, 200)])

        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "source.bin"
            typed_binary.write_records(path, "book_ticker", records)

            loaded = self.module.load_book_tickers(path)

        np.testing.assert_array_equal(loaded["id"], records["id"])
        np.testing.assert_array_equal(loaded["local_ns"], records["local_ns"])

    def test_analyze_uses_four_feed_id_interval_and_union_within_combo(self):
        records_by_label = {
            "A": self.make_records(
                [
                    (100, 1_000),
                    (101, 100),
                    (102, 500),
                    (103, 400),
                    (104, 900),
                    (104, 300),
                    (105, 800),
                    (106, 1_100),
                ]
            ),
            "B": self.make_records(
                [
                    (98, 1_000),
                    (101, 90),
                    (102, 700),
                    (104, 100),
                    (105, 600),
                ]
            ),
            "C": self.make_records(
                [
                    (101, 200),
                    (102, 200),
                    (103, 200),
                    (104, 200),
                    (105, 200),
                    (108, 1_000),
                ]
            ),
            "D": self.make_records(
                [
                    (99, 1_000),
                    (102, 50),
                    (103, 450),
                    (105, 700),
                    (106, 1_000),
                ]
            ),
        }

        summary = self.module.analyze_fusion_latency(records_by_label, top_n=3)

        self.assertEqual(
            summary["global_id_interval"],
            {"start_id": 101, "end_id": 105, "id_count": 5},
        )
        self.assertEqual(summary["feeds"]["A"]["duplicate_id_count"], 1)
        self.assertEqual(summary["feeds"]["A"]["unique_id_count"], 5)

        self.assertEqual(summary["combinations"]["D"]["observed_id_count"], 3)
        self.assertEqual(summary["combinations"]["D"]["missing_id_count"], 2)
        self.assertEqual(summary["combinations"]["AD"]["observed_id_count"], 5)
        self.assertEqual(summary["combinations"]["AD"]["missing_id_count"], 0)

        ab = summary["combinations"]["AB"]
        self.assertEqual(ab["observed_id_count"], 5)
        self.assertEqual(ab["latency_ns"]["min"], 90)
        self.assertEqual(ab["latency_ns"]["p50"], 400)
        self.assertEqual(ab["latency_ns"]["max"], 600)
        self.assertEqual(ab["best_source_counts"], {"A": 2, "B": 3})
        self.assertEqual([row["id"] for row in ab["top_outliers"]], [105, 102, 103])

        abcd = summary["combinations"]["ABCD"]
        self.assertEqual(abcd["observed_id_count"], 5)
        self.assertEqual(abcd["latency_ns"]["p50"], 100)
        self.assertEqual(abcd["latency_ns"]["max"], 200)
        self.assertEqual(abcd["best_source_counts"], {"B": 2, "C": 2, "D": 1})

    def test_generates_all_one_two_three_four_feed_combinations(self):
        records_by_label = {
            label: self.make_records([(10, 100), (11, 200)])
            for label in ["A", "B", "C", "D"]
        }

        summary = self.module.analyze_fusion_latency(records_by_label, top_n=0)

        combo_names = set(summary["combinations"])
        self.assertEqual(len(combo_names), 15)
        self.assertEqual(
            combo_names,
            {
                "A",
                "B",
                "C",
                "D",
                "AB",
                "AC",
                "AD",
                "BC",
                "BD",
                "CD",
                "ABC",
                "ABD",
                "ACD",
                "BCD",
                "ABCD",
            },
        )

    def test_rejects_empty_or_non_overlapping_feeds(self):
        with self.assertRaisesRegex(ValueError, "exactly 4 feeds"):
            self.module.analyze_fusion_latency({"A": np.zeros(0, dtype=self.dtype)})

        records_by_label = {
            "A": self.make_records([(1, 100), (2, 100)]),
            "B": self.make_records([(3, 100), (4, 100)]),
            "C": self.make_records([(5, 100), (6, 100)]),
            "D": self.make_records([(7, 100), (8, 100)]),
        }
        with self.assertRaisesRegex(ValueError, "no common id interval"):
            self.module.analyze_fusion_latency(records_by_label)


if __name__ == "__main__":
    unittest.main()
