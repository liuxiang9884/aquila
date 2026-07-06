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


def manifest_entry(path: Path, *, feed: str = "book_ticker", records: int = 2) -> dict:
    return {
        "sequence": 1,
        "file": str(path),
        "records": records,
        "bytes": 16 + records * 64,
        "format": "aquila.market_data.binary",
        "version": 1,
        "feed": feed,
        "header_bytes": 16,
        "record_size": 64,
    }


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
                        json.dumps(manifest_entry(first)),
                        json.dumps(manifest_entry(second)),
                        json.dumps(manifest_entry(tmp)),
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
                json.dumps(manifest_entry(trade_file, feed="trade")) + "\n",
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
                json.dumps(manifest_entry(Path("book_ticker.bin.tmp"))) + "\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, "no replayable"):
                manifest_config.render_data_reader_config(
                    manifest_path=manifest,
                    name="empty_replay",
                    catalog_path=Path("config/instruments/usdt_futures.csv"),
                )

    def test_rejects_feed_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            manifest = root / "manifest.jsonl"
            segment = root / "segments" / "trade_000001.bin"
            manifest.write_text(
                json.dumps(manifest_entry(segment, feed="trade")) + "\n",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ValueError, r"manifest\.jsonl:1: feed"):
                manifest_config.render_data_reader_config(
                    manifest_path=manifest,
                    name="book_replay",
                    catalog_path=Path("config/instruments/usdt_futures.csv"),
                    feed="book_ticker",
                )

    def test_rejects_bytes_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            manifest = root / "manifest.jsonl"
            segment = root / "segments" / "book_ticker_000001.bin"
            entry = manifest_entry(segment, records=3)
            entry["bytes"] += 1
            manifest.write_text(json.dumps(entry) + "\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, r"manifest\.jsonl:1: bytes"):
                manifest_config.render_data_reader_config(
                    manifest_path=manifest,
                    name="book_replay",
                    catalog_path=Path("config/instruments/usdt_futures.csv"),
                )

    def test_rejects_missing_format(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            manifest = root / "manifest.jsonl"
            segment = root / "segments" / "book_ticker_000001.bin"
            entry = manifest_entry(segment)
            del entry["format"]
            manifest.write_text(json.dumps(entry) + "\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, r"manifest\.jsonl:1: format"):
                manifest_config.render_data_reader_config(
                    manifest_path=manifest,
                    name="book_replay",
                    catalog_path=Path("config/instruments/usdt_futures.csv"),
                )

    def test_rejects_record_size_mismatch(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            manifest = root / "manifest.jsonl"
            segment = root / "segments" / "book_ticker_000001.bin"
            entry = manifest_entry(segment)
            entry["record_size"] = 63
            manifest.write_text(json.dumps(entry) + "\n", encoding="utf-8")

            with self.assertRaisesRegex(
                ValueError, r"manifest\.jsonl:1: record_size"
            ):
                manifest_config.render_data_reader_config(
                    manifest_path=manifest,
                    name="book_replay",
                    catalog_path=Path("config/instruments/usdt_futures.csv"),
                )


if __name__ == "__main__":
    unittest.main()
