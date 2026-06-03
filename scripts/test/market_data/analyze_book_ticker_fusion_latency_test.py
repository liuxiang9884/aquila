#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "market_data" / "analyze_book_ticker_fusion_latency.py"


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
