#!/usr/bin/env python3

import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "lead_lag" / "generate_preflight_config_params.py"
MARKET_DATA_SCRIPT = (
    REPO_ROOT / "scripts" / "market_data" / "analyze_book_ticker_latency.py"
)
MARKET_DATA_SCRIPT_DIR = REPO_ROOT / "scripts" / "market_data"
if str(MARKET_DATA_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(MARKET_DATA_SCRIPT_DIR))

import typed_binary  # noqa: E402


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class GeneratePreflightConfigParamsTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module(SCRIPT_PATH, "generate_preflight_config_params")
        self.market_data = load_module(MARKET_DATA_SCRIPT, "analyze_book_ticker_latency")
        self.dtype = self.market_data.book_ticker_dtype()

    def make_records(self) -> np.ndarray:
        records = np.zeros(7, dtype=self.dtype)
        records["id"] = np.arange(1, 8)
        records["symbol_id"] = 4
        records["exchange"] = [0, 0, 0, 2, 2, 2, 2]
        records["exchange_ns"] = 1_000_000_000
        records["local_ns"] = [
            1_001_000_000,
            1_002_000_000,
            1_003_000_000,
            1_004_000_000,
            1_005_000_000,
            1_006_000_000,
            999_000_000,
        ]
        records["bid_price"] = [10.0, 10.0, 10.0, 100.0, 100.0, 100.0, 100.0]
        records["ask_price"] = [10.1, 10.1, 10.1, 100.1, 100.2, 100.4, 100.5]
        return records

    def test_generates_fixed_params_from_book_ticker_binary(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            binary_path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(binary_path, "book_ticker", self.make_records())

            result = self.module.generate_params(
                input_paths=[binary_path],
                symbol_id=4,
                lead_exchange="binance",
                lag_exchange="gate",
                buffer_percentile=100.0,
            )

        self.assertEqual(result["symbol_id"], 4)
        self.assertEqual(result["lead_exchange"], "binance")
        self.assertEqual(result["lag_exchange"], "gate")
        self.assertEqual(result["freshness"]["lead_threshold_ms"], 5)
        self.assertEqual(result["freshness"]["lag_threshold_ms"], 8)
        self.assertEqual(result["freshness"]["lead_sample_count"], 3)
        self.assertEqual(result["freshness"]["lag_sample_count"], 3)
        self.assertEqual(result["freshness"]["lag_negative_latency_count"], 1)

        expected_spread = (100.4 - 100.0) / ((100.4 + 100.0) / 2.0)
        self.assertAlmostEqual(
            result["taker_buffer"]["entry_fixed_pct"], expected_spread
        )
        self.assertAlmostEqual(
            result["taker_buffer"]["normal_close_fixed_pct"], expected_spread
        )
        self.assertEqual(result["taker_buffer"]["source"], "generated")
        self.assertEqual(result["taker_buffer"]["sample_count"], 3)
        self.assertEqual(result["taker_buffer"]["selected_percentile"], 100.0)
        self.assertIn("spread_percentiles", result["taker_buffer"])
        self.assertEqual(
            sorted(result["taker_buffer"]["spread_percentiles"].keys()),
            ["p100", "p50", "p95", "p99"],
        )
        self.assertAlmostEqual(
            result["taker_buffer"]["spread_percentiles"]["p100"], expected_spread
        )
        self.assertNotIn("slippage", result)

    def test_generates_slippage_ticks_from_buffer_pct_and_price_tick(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            binary_path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(binary_path, "book_ticker", self.make_records())

            result = self.module.generate_params(
                input_paths=[binary_path],
                symbol_id=4,
                lead_exchange="binance",
                lag_exchange="gate",
                buffer_percentile=100.0,
                lag_price_tick=0.05,
            )

        expected_spread = (100.4 - 100.0) / ((100.4 + 100.0) / 2.0)
        self.assertAlmostEqual(result["slippage"]["entry_buffer_pct"], expected_spread)
        self.assertAlmostEqual(
            result["slippage"]["normal_close_buffer_pct"], expected_spread
        )
        self.assertEqual(result["slippage"]["price_tick"], 0.05)
        self.assertEqual(result["slippage"]["reference_price_method"], "lag_bbo_max")
        self.assertEqual(result["slippage"]["reference_price"], 100.4)
        self.assertEqual(result["slippage"]["open_slippage_ticks"], 9)
        self.assertEqual(result["slippage"]["close_slippage_ticks"], 9)

    def test_cli_requires_explicit_buffer_percentile(self):
        argv = [
            str(SCRIPT_PATH),
            "--input",
            "/home/liuxiang/tmp/book_ticker.bin",
            "--symbol-id",
            "4",
            "--lead-exchange",
            "binance",
            "--lag-exchange",
            "gate",
        ]

        with mock.patch.object(sys, "argv", argv), mock.patch.object(
            sys, "stderr", io.StringIO()
        ):
            with self.assertRaises(SystemExit) as context:
                self.module.parse_args()

        self.assertNotEqual(context.exception.code, 0)

    def test_normalizes_exchange_names_and_keeps_zero_freshness_threshold(self):
        records = self.make_records()
        records["local_ns"] = records["exchange_ns"]
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            binary_path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(binary_path, "book_ticker", records)

            result = self.module.generate_params(
                input_paths=[binary_path],
                symbol_id=4,
                lead_exchange="kBinance",
                lag_exchange="kGate",
                buffer_percentile=100.0,
            )

        self.assertEqual(result["lead_exchange"], "binance")
        self.assertEqual(result["lag_exchange"], "gate")
        self.assertEqual(result["freshness"]["lead_threshold_ms"], 0)
        self.assertEqual(result["freshness"]["lag_threshold_ms"], 0)

    def test_keeps_large_taker_buffer_proxy(self):
        records = self.make_records()
        records["bid_price"][3:] = 1.0
        records["ask_price"][3:] = 100.0
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            binary_path = Path(temp_dir) / "book_ticker.bin"
            typed_binary.write_records(binary_path, "book_ticker", records)

            result = self.module.generate_params(
                input_paths=[binary_path],
                symbol_id=4,
                lead_exchange="binance",
                lag_exchange="gate",
                buffer_percentile=100.0,
            )

        expected_spread = (100.0 - 1.0) / ((100.0 + 1.0) / 2.0)
        self.assertAlmostEqual(
            result["taker_buffer"]["entry_fixed_pct"], expected_spread
        )
        self.assertAlmostEqual(
            result["taker_buffer"]["normal_close_fixed_pct"], expected_spread
        )

    def test_reads_typed_binary_payload_from_mocked_zstd(self):
        records = self.make_records()
        payload = io.BytesIO()
        typed_binary.write_header(payload, "book_ticker")
        payload.write(records.tobytes())
        payload.seek(0)

        class FakeProcess:
            def __init__(self, stderr_file):
                self.stdout = io.BytesIO(payload.getvalue())
                self.stderr_file = stderr_file
                self.return_code = 0
                self.terminated = False
                self.killed = False

            def poll(self):
                return self.return_code

            def terminate(self):
                self.terminated = True
                self.return_code = -15

            def kill(self):
                self.killed = True
                self.return_code = -9

            def wait(self, timeout=None):
                return self.return_code

        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            compressed_path = Path(temp_dir) / "book_ticker.bin.zst"
            compressed_path.write_bytes(b"mock-zstd")

            def popen_side_effect(*args, **kwargs):
                self.assertIs(kwargs["stdout"], self.module.subprocess.PIPE)
                self.assertIsNot(kwargs["stderr"], self.module.subprocess.PIPE)
                return FakeProcess(kwargs["stderr"])

            with mock.patch.object(
                self.module.subprocess,
                "Popen",
                side_effect=popen_side_effect,
            ):
                result = self.module.generate_params(
                    input_paths=[compressed_path],
                    symbol_id=4,
                    lead_exchange="binance",
                    lag_exchange="gate",
                    buffer_percentile=100.0,
                    chunk_records=2,
                )

        self.assertEqual(result["freshness"]["lead_sample_count"], 3)
        self.assertEqual(result["freshness"]["lag_sample_count"], 3)

    def test_zstd_nonzero_return_reports_stderr_text(self):
        class FailedProcess:
            def __init__(self, stderr_file):
                self.stdout = io.BytesIO(b"")
                self.return_code = 1
                self.terminated = False
                self.killed = False
                stderr_file.write(b"fixture zstd failure\n")
                stderr_file.flush()

            def poll(self):
                return self.return_code

            def terminate(self):
                self.terminated = True
                self.return_code = -15

            def kill(self):
                self.killed = True
                self.return_code = -9

            def wait(self, timeout=None):
                return self.return_code

        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            compressed_path = Path(temp_dir) / "book_ticker.bin.zst"
            compressed_path.write_bytes(b"mock-zstd")

            def popen_side_effect(*args, **kwargs):
                self.assertIs(kwargs["stdout"], self.module.subprocess.PIPE)
                self.assertIsNot(kwargs["stderr"], self.module.subprocess.PIPE)
                return FailedProcess(kwargs["stderr"])

            with mock.patch.object(
                self.module.subprocess,
                "Popen",
                side_effect=popen_side_effect,
            ):
                with self.assertRaisesRegex(
                    ValueError, "zstd failed .* fixture zstd failure"
                ):
                    list(
                        self.module.iter_book_ticker_chunks(
                            compressed_path,
                            dtype=self.dtype,
                            chunk_records=2,
                        )
                    )

    def test_renders_generated_toml_patch(self):
        params = {
            "slippage": {
                "open_slippage_ticks": 5,
                "close_slippage_ticks": 6,
            },
            "taker_buffer": {
                "entry_fixed_pct": 0.0002,
                "normal_close_fixed_pct": 0.0003,
            },
            "freshness": {
                "lead_threshold_ms": 5,
                "lag_threshold_ms": 20,
            },
        }

        text = self.module.render_toml_patch(params)

        self.assertIn("[lead_lag.pairs.execute]", text)
        self.assertIn("open_slippage_ticks = 5", text)
        self.assertIn("close_slippage_ticks = 6", text)
        self.assertIn("[lead_lag.pairs.execute.taker_buffer]", text)
        self.assertIn('mode = "shadow"', text)
        self.assertIn("entry_fixed_pct = 0.0002", text)
        self.assertIn("normal_close_fixed_pct = 0.0003", text)
        self.assertIn('source = "generated"', text)
        self.assertNotIn("freshness_shadow", text)
        self.assertNotIn("lead_threshold_ms", text)
        self.assertNotIn("lag_threshold_ms", text)


if __name__ == "__main__":
    unittest.main()
