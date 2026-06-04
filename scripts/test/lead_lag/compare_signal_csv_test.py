#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import compare_signal_csv as compare


FIELDS = [
    "symbol_id",
    "exchange",
    "role",
    "exchange_ns",
    "event_ns",
    "action",
    "side",
    "raw_price",
    "reduce_only",
    "lead_drifted_event_ns",
    "lag_exchange_ns",
    "position_direction",
]


def row(**overrides):
    values = {
        "symbol_id": "42",
        "exchange": "kGate",
        "role": "kLag",
        "exchange_ns": "1000000000",
        "event_ns": "1000000000",
        "action": "kOpenLong",
        "side": "kBuy",
        "raw_price": "12.340000000",
        "reduce_only": "false",
        "lead_drifted_event_ns": "999999900",
        "lag_exchange_ns": "1000000000",
        "position_direction": "kLong",
    }
    values.update(overrides)
    return values


def write_csv(path: Path, rows):
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)


class CompareSignalCsvTest(unittest.TestCase):
    def test_identical_rows_match_without_differences(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            live = Path(temp_dir) / "live.csv"
            replay = Path(temp_dir) / "replay.csv"
            write_csv(live, [row()])
            write_csv(replay, [row()])

            summary = compare.compare_csv_files(live, replay)

        self.assertEqual(summary["counts"]["live_signals"], 1)
        self.assertEqual(summary["counts"]["replay_signals"], 1)
        self.assertEqual(summary["counts"]["matched"], 1)
        self.assertEqual(summary["counts"]["live_only"], 0)
        self.assertEqual(summary["counts"]["replay_only"], 0)
        self.assertEqual(summary["counts"]["mismatched"], 0)
        self.assertEqual(summary["mismatches"], [])

    def test_live_only_and_replay_only_rows_are_counted_separately(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            live = Path(temp_dir) / "live.csv"
            replay = Path(temp_dir) / "replay.csv"
            write_csv(live, [row(exchange_ns="1001", event_ns="1001")])
            write_csv(replay, [row(exchange_ns="1002", event_ns="1002")])

            summary = compare.compare_csv_files(live, replay)

        self.assertEqual(summary["counts"]["matched"], 0)
        self.assertEqual(summary["counts"]["live_only"], 1)
        self.assertEqual(summary["counts"]["replay_only"], 1)
        self.assertEqual(summary["live_only"][0]["key"]["exchange_ns"], "1001")
        self.assertEqual(summary["replay_only"][0]["key"]["exchange_ns"], "1002")

    def test_matched_key_with_different_trade_intent_reports_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            live = Path(temp_dir) / "live.csv"
            replay = Path(temp_dir) / "replay.csv"
            write_csv(live, [row(action="kOpenLong", side="kBuy", raw_price="12.34")])
            write_csv(
                replay,
                [row(action="kCloseLong", side="kSell", raw_price="12.35")],
            )

            summary = compare.compare_csv_files(live, replay)

        self.assertEqual(summary["counts"]["matched"], 1)
        self.assertEqual(summary["counts"]["mismatched"], 1)
        fields = {
            difference["field"]
            for difference in summary["mismatches"][0]["differences"]
        }
        self.assertEqual(fields, {"action", "side", "raw_price"})

    def test_price_difference_within_tolerance_is_not_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            live = Path(temp_dir) / "live.csv"
            replay = Path(temp_dir) / "replay.csv"
            write_csv(live, [row(raw_price="12.340000000")])
            write_csv(replay, [row(raw_price="12.340000004")])

            summary = compare.compare_csv_files(live, replay)

        self.assertEqual(summary["counts"]["matched"], 1)
        self.assertEqual(summary["counts"]["mismatched"], 0)


if __name__ == "__main__":
    unittest.main()
