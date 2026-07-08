#!/home/liuxiang/dev/pyenv/lx/bin/python

import struct
import sys
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[3] / "bitget" / "market_data"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import probe_bbo_sbe as probe


def build_bbo_frame(
    include_stream_fields: bool = True, block_length: int | None = None
) -> bytes:
    body = struct.pack(
        "<QqqqqbbQ",
        1_700_000_000_000_001,
        6_569_038,
        15_000,
        6_569_042,
        20_000,
        -2,
        -4,
        42,
    )
    if include_stream_fields:
        body += struct.pack("<QB", 1_700_000_000_001_001, 1)
    actual_block_length = max(56, len(body)) if block_length is None else block_length
    header = struct.pack("<HHHH", actual_block_length, 1002, 1, 2)
    padding = b"\x00" * (actual_block_length - len(body))
    symbol = b"\x07BTCUSDT"
    return header + body + padding + symbol


class ProbeBboSbeTest(unittest.TestCase):
    def test_decodes_books1_bbo_frame(self):
        decoded = probe.decode_bbo_frame(build_bbo_frame())

        self.assertEqual(decoded.header.block_length, 59)
        self.assertEqual(decoded.header.template_id, 1002)
        self.assertEqual(decoded.header.schema_id, 1)
        self.assertEqual(decoded.header.version, 2)
        self.assertEqual(decoded.ts, 1_700_000_000_000_001)
        self.assertEqual(decoded.bid1_price, "65690.38")
        self.assertEqual(decoded.bid1_size, "1.5000")
        self.assertEqual(decoded.ask1_price, "65690.42")
        self.assertEqual(decoded.ask1_size, "2.0000")
        self.assertEqual(decoded.price_exponent, -2)
        self.assertEqual(decoded.size_exponent, -4)
        self.assertEqual(decoded.seq, 42)
        self.assertEqual(decoded.sts, 1_700_000_000_001_001)
        self.assertEqual(decoded.category, 1)
        self.assertEqual(decoded.symbol, "BTCUSDT")

    def test_decodes_live_observed_books1_bbo_block_length(self):
        decoded = probe.decode_bbo_frame(build_bbo_frame(block_length=64))

        self.assertEqual(decoded.header.block_length, 64)
        self.assertEqual(decoded.bid1_price, "65690.38")
        self.assertEqual(decoded.bid1_size, "1.5000")
        self.assertEqual(decoded.ask1_price, "65690.42")
        self.assertEqual(decoded.ask1_size, "2.0000")
        self.assertEqual(decoded.seq, 42)
        self.assertEqual(decoded.sts, 1_700_000_000_001_001)
        self.assertEqual(decoded.category, 1)
        self.assertEqual(decoded.symbol, "BTCUSDT")

    def test_decodes_books1_bbo_frame_without_stream_fields(self):
        decoded = probe.decode_bbo_frame(build_bbo_frame(include_stream_fields=False))

        self.assertEqual(decoded.header.block_length, 56)
        self.assertEqual(decoded.bid1_price, "65690.38")
        self.assertEqual(decoded.bid1_size, "1.5000")
        self.assertEqual(decoded.ask1_price, "65690.42")
        self.assertEqual(decoded.ask1_size, "2.0000")
        self.assertEqual(decoded.seq, 42)
        self.assertIsNone(decoded.sts)
        self.assertIsNone(decoded.category)
        self.assertEqual(decoded.symbol, "BTCUSDT")

    def test_rejects_non_bbo_template(self):
        frame = bytearray(build_bbo_frame())
        struct.pack_into("<H", frame, 2, 1001)

        with self.assertRaisesRegex(ValueError, "unexpected templateId"):
            probe.decode_bbo_frame(bytes(frame))


if __name__ == "__main__":
    unittest.main()
