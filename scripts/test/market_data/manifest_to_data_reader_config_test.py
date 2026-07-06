#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
MODULE_DIR = Path(__file__).resolve().parents[2] / "market_data"
if str(MODULE_DIR) not in sys.path:
    sys.path.insert(0, str(MODULE_DIR))

import manifest_to_data_reader_config as manifest_config  # noqa: E402


class ManifestToDataReaderConfigTest(unittest.TestCase):
    def test_generates_binary_file_reader_config_from_manifest(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / "segments" / "book_ticker_000001.bin"
            second = root / "segments" / "book_ticker_000002.bin"
            tmp = root / "segments" / "book_ticker_000003.bin.tmp"
            manifest = root / "manifest.jsonl"
            manifest.parent.mkdir(parents=True, exist_ok=True)
            manifest.write_text(
                "\n".join(
                    [
                        json.dumps({"sequence": 1, "file": str(first)}),
                        json.dumps({"sequence": 2, "file": str(second)}),
                        json.dumps({"sequence": 3, "file": str(tmp)}),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            config_text = manifest_config.render_data_reader_config(
                manifest_path=manifest,
                name="persistent_md_replay",
                catalog_path=Path("config/instruments/usdt_futures.csv"),
            )

        self.assertIn('name = "persistent_md_replay"', config_text)
        self.assertIn('name = "persistent_md_replay_book_ticker"', config_text)
        self.assertIn('type = "binary_file"', config_text)
        self.assertIn('feed = "book_ticker"', config_text)
        self.assertIn('start_position = "earliest_visible"', config_text)
        self.assertIn('read_mode = "drain"', config_text)
        self.assertLess(config_text.find(str(first)), config_text.find(str(second)))
        self.assertNotIn(str(tmp), config_text)

    def test_generates_trade_reader_config_from_manifest(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            trade_file = root / "segments_trade" / "trade_000001.bin"
            manifest = root / "trade_manifest.jsonl"
            manifest.parent.mkdir(parents=True, exist_ok=True)
            manifest.write_text(
                json.dumps({"sequence": 1, "file": str(trade_file)}) + "\n",
                encoding="utf-8",
            )

            config_text = manifest_config.render_data_reader_config(
                manifest_path=manifest,
                name="trade_replay",
                catalog_path=Path("config/instruments/usdt_futures.csv"),
                feed="trade",
            )

        self.assertIn('name = "trade_replay"', config_text)
        self.assertIn('name = "trade_replay_trade"', config_text)
        self.assertIn('type = "binary_file"', config_text)
        self.assertIn('feed = "trade"', config_text)
        self.assertIn(str(trade_file), config_text)

    def test_rejects_manifest_without_replayable_segments(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest = Path(temp_dir) / "manifest.jsonl"
            manifest.write_text(
                json.dumps({"sequence": 1, "file": "book_ticker.bin.tmp"}) + "\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "no replayable"):
                manifest_config.render_data_reader_config(
                    manifest_path=manifest,
                    name="empty_replay",
                    catalog_path=Path("config/instruments/usdt_futures.csv"),
                )


if __name__ == "__main__":
    unittest.main()
