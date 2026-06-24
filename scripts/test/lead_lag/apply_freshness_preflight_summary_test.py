#!/usr/bin/env python3

import csv
import importlib.util
import json
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = (
    REPO_ROOT / "scripts" / "lead_lag" / "apply_freshness_preflight_summary.py"
)


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_text(path: Path, text: str) -> None:
    path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")


class ApplyFreshnessPreflightSummaryTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module(SCRIPT_PATH, "apply_freshness_preflight_summary")

    def write_summary(self, path: Path) -> None:
        path.write_text(
            json.dumps(
                {
                    "method": "lead_decision_ns_minus_exchange_ns_mean_plus_3std",
                    "groups": [
                        {
                            "symbol_id": 4,
                            "exchange": "binance",
                            "sample_count": 100,
                            "mean_ms": 1.2,
                            "p50_ms": 1.1,
                            "p95_ms": 1.8,
                            "threshold_ms": 3,
                        },
                        {
                            "symbol_id": 4,
                            "exchange": "gate",
                            "sample_count": 100,
                            "mean_ms": 40.0,
                            "p50_ms": 32.0,
                            "p95_ms": 180.0,
                            "threshold_ms": 500,
                        },
                        {
                            "symbol_id": 7,
                            "exchange": "binance",
                            "sample_count": 20,
                            "mean_ms": 1.3,
                            "p50_ms": 1.2,
                            "p95_ms": 1.9,
                            "threshold_ms": 3,
                        },
                        {
                            "symbol_id": 7,
                            "exchange": "gate",
                            "sample_count": 20,
                            "mean_ms": 400.0,
                            "p50_ms": 120.0,
                            "p95_ms": 900.0,
                            "threshold_ms": 3000,
                        },
                    ],
                }
            ),
            encoding="utf-8",
        )

    def write_config(self, path: Path) -> None:
        write_text(
            path,
            """
            [lead_lag]
            name = "lead_lag"
            version = "1.0"

            [[lead_lag.pairs]]
            symbol = "PROVE_USDT"
            symbol_id = 4
            lead_exchange = "binance"
            lag_exchange = "gate"
            max_lead_freshness_ms = 5
            max_lag_freshness_ms = 20

            [[lead_lag.pairs]]
            symbol = "TEST_USDT"
            symbol_id = 7
            lead_exchange = "binance"
            lag_exchange = "gate"
            max_lead_freshness_ms = 5
            max_lag_freshness_ms = 20
            """,
        )

    def test_updates_lag_freshness_from_lag_p50_bucket(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            base = Path(temp_dir)
            summary_path = base / "summary.json"
            config_in = base / "strategy.toml"
            config_out = base / "strategy.generated.toml"
            csv_out = base / "freshness.csv"
            self.write_summary(summary_path)
            self.write_config(config_in)

            result = self.module.apply_summary_to_config(
                summary_json=summary_path,
                config_in=config_in,
                config_out=config_out,
                csv_out=csv_out,
            )

            output = config_out.read_text(encoding="utf-8")
            with csv_out.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))

        self.assertEqual(result.updated_pair_count, 2)
        self.assertIn("max_lead_freshness_ms = 5", output)
        self.assertIn("symbol = \"PROVE_USDT\"", output)
        self.assertIn("max_lag_freshness_ms = 50", output)
        self.assertIn("symbol = \"TEST_USDT\"", output)
        self.assertIn("max_lag_freshness_ms = 200", output)
        self.assertEqual(rows[0]["symbol"], "PROVE_USDT")
        self.assertEqual(rows[0]["lag_p50_ms"], "32.000000")
        self.assertEqual(rows[0]["generated_lag_freshness_ms"], "50")
        self.assertEqual(rows[1]["symbol"], "TEST_USDT")
        self.assertEqual(rows[1]["generated_lag_freshness_ms"], "200")

    def test_p50_bucket_boundaries(self):
        self.assertEqual(self.module.lag_freshness_from_p50_ms(0.0), 20)
        self.assertEqual(self.module.lag_freshness_from_p50_ms(20.0), 20)
        self.assertEqual(self.module.lag_freshness_from_p50_ms(20.1), 50)
        self.assertEqual(self.module.lag_freshness_from_p50_ms(50.0), 50)
        self.assertEqual(self.module.lag_freshness_from_p50_ms(50.1), 100)
        self.assertEqual(self.module.lag_freshness_from_p50_ms(100.0), 100)
        self.assertEqual(self.module.lag_freshness_from_p50_ms(100.1), 200)
        self.assertEqual(self.module.lag_freshness_from_p50_ms(1000.0), 200)


if __name__ == "__main__":
    unittest.main()
