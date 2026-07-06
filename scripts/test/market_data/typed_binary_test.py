#!/usr/bin/env python3

import io
import importlib.util
import struct
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

    def write_header_file(
        self,
        path: Path,
        *,
        version: int | None = None,
        header_size: int | None = None,
        flags: int | None = None,
        record_size: int | None = None,
    ) -> None:
        path.write_bytes(
            struct.pack(
                "<IHHHHI",
                int.from_bytes(self.module.MAGIC_BYTES, "little"),
                self.module.VERSION if version is None else version,
                self.module.HEADER_SIZE if header_size is None else header_size,
                self.module.BOOK_TICKER_FEED_TYPE,
                self.module.book_ticker_dtype().itemsize
                if record_size is None
                else record_size,
                self.module.FLAGS if flags is None else flags,
            )
        )

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

    def test_unsupported_header_version_is_rejected(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "unsupported_version.bin"
            self.write_header_file(path, version=self.module.VERSION + 1)

            with self.assertRaisesRegex(
                ValueError, "unsupported market data version"
            ):
                self.module.load_records(path, "book_ticker")

    def test_unsupported_header_size_is_rejected(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "unsupported_header_size.bin"
            self.write_header_file(path, header_size=self.module.HEADER_SIZE + 8)

            with self.assertRaisesRegex(
                ValueError, "unsupported market data header size"
            ):
                self.module.load_records(path, "book_ticker")

    def test_unsupported_header_flags_are_rejected(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "unsupported_flags.bin"
            self.write_header_file(path, flags=1)

            with self.assertRaisesRegex(ValueError, "unsupported market data flags"):
                self.module.load_records(path, "book_ticker")

    def test_record_size_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "bad_record_size.bin"
            self.write_header_file(
                path, record_size=self.module.book_ticker_dtype().itemsize - 1
            )

            with self.assertRaisesRegex(ValueError, "record size mismatch"):
                self.module.load_records(path, "book_ticker")

    def test_short_file_is_rejected(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "short.bin"
            path.write_bytes(b"AQMD")

            with self.assertRaisesRegex(ValueError, "missing market data header"):
                self.module.load_records(path, "book_ticker")

    def test_header_only_file_is_valid_zero_records(self):
        with tempfile.TemporaryDirectory(dir="/home/liuxiang/tmp") as temp_dir:
            path = Path(temp_dir) / "header_only.bin"
            self.write_header_file(path)

            loaded = self.module.load_records(path, "book_ticker")
            count = self.module.checked_record_count(path, "book_ticker")

            try:
                mapped = self.module.memmap_records(path, "book_ticker")
            except ValueError:
                mapped = None

        self.assertEqual(len(loaded), 0)
        self.assertEqual(count, 0)
        if mapped is not None:
            self.assertEqual(len(mapped), 0)


if __name__ == "__main__":
    unittest.main()
