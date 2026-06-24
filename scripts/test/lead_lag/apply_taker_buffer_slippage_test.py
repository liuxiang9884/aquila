#!/usr/bin/env python3

import csv
import importlib.util
import json
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "lead_lag" / "apply_taker_buffer_slippage.py"


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_text(path: Path, text: str) -> None:
    path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")


def write_params(path: Path, symbol_id: int, open_slippage: int, close_slippage: int):
    path.write_text(
        json.dumps(
            {
                "symbol_id": symbol_id,
                "lead_exchange": "binance",
                "lag_exchange": "gate",
                "taker_buffer": {
                    "entry_fixed_pct": 0.0002,
                    "normal_close_fixed_pct": 0.0003,
                },
                "slippage": {
                    "price_tick": 0.05,
                    "reference_price_method": "lag_bbo_max",
                    "reference_price": 100.4,
                    "max_bid_price": 100.0,
                    "max_ask_price": 100.4,
                    "open_long_slippage": open_slippage - 1,
                    "open_short_slippage": open_slippage,
                    "close_long_slippage": close_slippage - 1,
                    "close_short_slippage": close_slippage,
                    "open_slippage": open_slippage,
                    "close_slippage": close_slippage,
                },
            }
        ),
        encoding="utf-8",
    )


class ApplyTakerBufferSlippageTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module(SCRIPT_PATH, "apply_taker_buffer_slippage")

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

            [lead_lag.pairs.execute]
            open_notional = 100.0
            open_slippage = 3
            close_slippage = 3
            parallel = 1

            [[lead_lag.pairs]]
            symbol = "TEST_USDT"
            symbol_id = 7
            lead_exchange = "binance"
            lag_exchange = "gate"

            [lead_lag.pairs.execute]
            open_notional = 100.0
            open_slippage = 2
            close_slippage = 2
            parallel = 1
            """,
        )

    def test_updates_slippage_ticks_and_writes_audit_csv(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            base = Path(temp_dir)
            params_4 = base / "prove_params.json"
            params_7 = base / "test_params.json"
            config_in = base / "strategy.toml"
            config_out = base / "strategy.generated.toml"
            csv_out = base / "slippage.csv"
            write_params(params_4, symbol_id=4, open_slippage=5, close_slippage=6)
            write_params(params_7, symbol_id=7, open_slippage=7, close_slippage=8)
            self.write_config(config_in)

            result = self.module.apply_slippage_to_config(
                params_json=[params_4, params_7],
                config_in=config_in,
                config_out=config_out,
                csv_out=csv_out,
            )

            output = config_out.read_text(encoding="utf-8")
            with csv_out.open(newline="", encoding="utf-8") as handle:
                rows = list(csv.DictReader(handle))

        self.assertEqual(result.updated_pair_count, 2)
        self.assertIn("symbol = \"PROVE_USDT\"", output)
        self.assertIn("open_slippage = 5", output)
        self.assertIn("close_slippage = 6", output)
        self.assertIn("symbol = \"TEST_USDT\"", output)
        self.assertIn("open_slippage = 7", output)
        self.assertIn("close_slippage = 8", output)
        self.assertEqual(rows[0]["symbol"], "PROVE_USDT")
        self.assertEqual(rows[0]["generated_open_slippage"], "5")
        self.assertEqual(rows[0]["generated_close_slippage"], "6")
        self.assertEqual(rows[1]["symbol"], "TEST_USDT")
        self.assertEqual(rows[1]["generated_open_slippage"], "7")
        self.assertEqual(rows[1]["generated_close_slippage"], "8")


if __name__ == "__main__":
    unittest.main()
