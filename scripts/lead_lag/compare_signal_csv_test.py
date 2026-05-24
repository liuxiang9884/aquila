#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import tempfile
import unittest
from pathlib import Path

import compare_signal_csv as compare


FIELDS = [
    "ticker_id",
    "symbol_id",
    "exchange",
    "role",
    "event_ns",
    "action",
    "side",
    "price",
    "reduce_only",
    "lead_drifted_event_ns",
    "lag_event_ns",
    "position_direction",
]


def row(**overrides):
    values = {
        "ticker_id": "1001",
        "symbol_id": "42",
        "exchange": "kGate",
        "role": "kLag",
        "event_ns": "1000000000",
        "action": "kOpenLong",
        "side": "kBuy",
        "price": "12.340000000",
        "reduce_only": "false",
        "lead_drifted_event_ns": "999999900",
        "lag_event_ns": "1000000000",
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
            write_csv(live, [row(ticker_id="1001")])
            write_csv(replay, [row(ticker_id="1002")])

            summary = compare.compare_csv_files(live, replay)

        self.assertEqual(summary["counts"]["matched"], 0)
        self.assertEqual(summary["counts"]["live_only"], 1)
        self.assertEqual(summary["counts"]["replay_only"], 1)
        self.assertEqual(summary["live_only"][0]["key"]["ticker_id"], "1001")
        self.assertEqual(summary["replay_only"][0]["key"]["ticker_id"], "1002")

    def test_matched_key_with_different_trade_intent_reports_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            live = Path(temp_dir) / "live.csv"
            replay = Path(temp_dir) / "replay.csv"
            write_csv(live, [row(action="kOpenLong", side="kBuy", price="12.34")])
            write_csv(
                replay,
                [row(action="kCloseLong", side="kSell", price="12.35")],
            )

            summary = compare.compare_csv_files(live, replay)

        self.assertEqual(summary["counts"]["matched"], 1)
        self.assertEqual(summary["counts"]["mismatched"], 1)
        fields = {
            difference["field"]
            for difference in summary["mismatches"][0]["differences"]
        }
        self.assertEqual(fields, {"action", "side", "price"})

    def test_price_difference_within_tolerance_is_not_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            live = Path(temp_dir) / "live.csv"
            replay = Path(temp_dir) / "replay.csv"
            write_csv(live, [row(price="12.340000000")])
            write_csv(replay, [row(price="12.340000004")])

            summary = compare.compare_csv_files(live, replay)

        self.assertEqual(summary["counts"]["matched"], 1)
        self.assertEqual(summary["counts"]["mismatched"], 0)


if __name__ == "__main__":
    unittest.main()
