#!/usr/bin/env python3

import io
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_PATH = REPO_ROOT / "scripts" / "market_data" / "typed_binary.py"


def load_module():
    spec = importlib.util.spec_from_file_location("typed_binary", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class TypedBinaryTest(unittest.TestCase):
    def setUp(self):
        self.module = load_module()

    def make_book_records(self, count: int = 3) -> np.ndarray:
        records = np.zeros(count, dtype=self.module.book_ticker_dtype())
        records["id"] = np.arange(10, 10 + count)
        records["symbol_id"] = np.arange(90, 90 + count)
        records["exchange"] = 2
        records["exchange_ns"] = np.arange(count) * 1_000
        records["local_ns"] = records["exchange_ns"] + 100
        records["bid_price"] = 100.0 + np.arange(count)
        records["bid_volume"] = 1.0 + np.arange(count)
        records["ask_price"] = 101.0 + np.arange(count)
        records["ask_volume"] = 2.0 + np.arange(count)
        return records

    def test_book_ticker_dtype_matches_abi(self):
        dtype = self.module.book_ticker_dtype()

        self.assertEqual(dtype.itemsize, 64)
        self.assertEqual(
            [dtype.fields[name][1] for name in dtype.names],
            [0, 8, 12, 16, 24, 32, 40, 48, 56],
        )
        self.assertEqual(
            dtype.names,
            (
                "id",
                "symbol_id",
                "exchange",
                "exchange_ns",
                "local_ns",
                "bid_price",
                "bid_volume",
                "ask_price",
                "ask_volume",
            ),
        )

    def test_trade_dtype_matches_abi(self):
        dtype = self.module.trade_dtype()

        self.assertEqual(dtype.itemsize, 64)
        self.assertEqual(
            [dtype.fields[name][1] for name in dtype.names],
            [0, 8, 12, 13, 14, 16, 24, 32, 40, 48, 56, 60],
        )
        self.assertEqual(
            dtype.names,
            (
                "id",
                "symbol_id",
                "exchange",
                "side",
                "reserved",
                "exchange_ns",
                "trade_ns",
                "local_ns",
                "price",
                "volume",
                "batch_index",
                "batch_count",
            ),
        )

    def test_write_and_read_book_ticker_records(self):
        records = self.make_book_records()
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"

            self.module.write_records(path, "book_ticker", records)
            loaded = self.module.load_records(path, "book_ticker")
            header = self.module.read_header(path)

        self.assertEqual(header.feed, "book_ticker")
        np.testing.assert_array_equal(loaded, records)

    def test_memmap_records_skips_header(self):
        records = self.make_book_records(2)
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"
            self.module.write_records(path, "book_ticker", records)

            mapped = self.module.memmap_records(path, "book_ticker")
            ids = mapped["id"].copy()

        np.testing.assert_array_equal(ids, records["id"])

    def test_iter_record_chunks_rejects_trailing_payload_bytes(self):
        records = self.make_book_records(1)
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"
            with path.open("wb") as handle:
                self.module.write_header(handle, "book_ticker")
                records.tofile(handle)
                handle.write(b"x")

            with self.assertRaisesRegex(ValueError, "payload size .* not a multiple"):
                list(
                    self.module.iter_record_chunks(
                        path, "book_ticker", chunk_records=10
                    )
                )

    def test_stream_chunks_parse_header_first(self):
        records = self.make_book_records(3)
        payload = io.BytesIO()
        self.module.write_header(payload, "book_ticker")
        payload.write(records.tobytes())
        payload.seek(0)

        chunks = list(
            self.module.iter_record_chunks_from_stream(
                payload,
                "book_ticker",
                chunk_records=2,
                source_name="stream.bin.zst",
            )
        )

        self.assertEqual([len(chunk) for chunk in chunks], [2, 1])
        np.testing.assert_array_equal(np.concatenate(chunks), records)

    def test_stream_chunks_handle_non_record_aligned_short_reads(self):
        class ShortReadBytesIO(io.BytesIO):
            def read(self, size: int = -1) -> bytes:
                if size < 0:
                    return super().read(size)
                return super().read(min(size, 7))

        records = self.make_book_records(4)
        payload = io.BytesIO()
        self.module.write_header(payload, "book_ticker")
        payload.write(records.tobytes())

        stream = ShortReadBytesIO(payload.getvalue())
        chunks = list(
            self.module.iter_record_chunks_from_stream(
                stream,
                "book_ticker",
                chunk_records=2,
                source_name="short-read.bin.zst",
            )
        )

        self.assertEqual(sum(len(chunk) for chunk in chunks), 4)
        np.testing.assert_array_equal(np.concatenate(chunks), records)

    def test_stream_chunks_reject_trailing_payload_bytes(self):
        records = self.make_book_records(1)
        payload = io.BytesIO()
        self.module.write_header(payload, "book_ticker")
        payload.write(records.tobytes())
        payload.write(b"x")
        payload.seek(0)

        with self.assertRaisesRegex(ValueError, "trailing bytes"):
            list(
                self.module.iter_record_chunks_from_stream(
                    payload,
                    "book_ticker",
                    chunk_records=2,
                    source_name="trailing.bin.zst",
                )
            )

    def test_wrong_feed_is_rejected(self):
        records = self.make_book_records(1)
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "book_ticker.bin"
            self.module.write_records(path, "book_ticker", records)

            with self.assertRaisesRegex(ValueError, "feed mismatch"):
                self.module.load_records(path, "trade")

    def test_raw_headerless_file_is_rejected(self):
        records = self.make_book_records(1)
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "raw_book_ticker.bin"
            records.tofile(path)

            with self.assertRaisesRegex(ValueError, "invalid market data magic"):
                self.module.load_records(path, "book_ticker")


if __name__ == "__main__":
    unittest.main()
