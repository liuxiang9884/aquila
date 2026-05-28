#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import struct
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

SCRIPT_DIR = Path(__file__).resolve().parents[1]
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import h5py
import numpy as np
import pandas as pd

import hdf_book_ticker_to_binary as converter


CONFIG_DTYPE = np.dtype(
    [
        ("symbol_id", "<u4"),
        ("date", "<i8"),
        ("price_multiplier", "<i8"),
        ("qty_multiplier", "<i8"),
        ("contract_value", "<f8"),
        ("worker_index", "u1"),
        ("hdf_version", "<u8"),
        ("bbo", "u1"),
        ("depth", "u1"),
        ("trade", "u1"),
        ("agg_trade", "u1"),
        ("force_order", "u1"),
        ("bbo_ns", "u1"),
        ("depth_ns", "u1"),
        ("trade_ns", "u1"),
        ("agg_trade_ns", "u1"),
        ("rpi_bbo", "u1"),
        ("rpi_depth", "u1"),
        ("non_rpi_trade", "u1"),
    ]
)

BBO_DTYPE = np.dtype(
    [
        ("id", "<u4"),
        ("update_id", "<u8"),
        ("ask_price", "<u4"),
        ("ask_qty", "<u4"),
        ("bid_price", "<u4"),
        ("bid_qty", "<u4"),
        ("localtime", "<i8"),
        ("timestamp", "<i8"),
        ("tx_time", "<i8"),
    ]
)

BOOK_TICKER = struct.Struct("<qibxxxqqdddd")


def write_hdf(path, *, date_hour, price_multiplier, qty_multiplier, rows):
    config = np.zeros(1, dtype=CONFIG_DTYPE)
    config["symbol_id"] = 999
    config["date"] = date_hour
    config["price_multiplier"] = price_multiplier
    config["qty_multiplier"] = qty_multiplier
    config["contract_value"] = 1.0
    config["hdf_version"] = 3
    config["bbo"] = 1

    bbo = np.zeros(len(rows), dtype=BBO_DTYPE)
    for index, row in enumerate(rows):
        bbo[index] = row

    with h5py.File(path, "w") as handle:
        handle.create_dataset("config", data=config)
        handle.create_dataset("bbo", data=bbo)


class HdfBookTickerToBinaryTest(unittest.TestCase):
    def test_convert_date_restores_prices_and_writes_sorted_book_ticker_binary(self):
        with TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            input_dir = root / "hdf"
            input_dir.mkdir()
            output_path = root / "out" / "20260415.bin"

            write_hdf(
                input_dir / "binance.ordi_usdt_swap_2026041500.h5",
                date_hour=2026041500,
                price_multiplier=1000,
                qty_multiplier=10,
                rows=[
                    (1, 11, 10500, 25, 10400, 35, 1002, 1002, 1002),
                    (2, 12, 10700, 45, 10600, 55, 1004, 1004, 1004),
                ],
            )
            write_hdf(
                input_dir / "gateio.ordi_usdt_swap_2026041500.h5",
                date_hour=2026041500,
                price_multiplier=100,
                qty_multiplier=1,
                rows=[
                    (1, 21, 1030, 7, 1020, 9, 1001, 1001, 1001),
                    (2, 22, 1090, 11, 1080, 13, 1003, 1003, 1003),
                ],
            )

            sources = [
                converter.HdfSource(
                    exchange_name="binance",
                    exchange_id=converter.EXCHANGE_BINANCE,
                    file_prefix="binance.ordi_usdt_swap",
                    symbol_id=3,
                ),
                converter.HdfSource(
                    exchange_name="gate",
                    exchange_id=converter.EXCHANGE_GATE,
                    file_prefix="gateio.ordi_usdt_swap",
                    symbol_id=3,
                ),
            ]

            frame = converter.read_hdf_bbo_frame(
                input_dir / "binance.ordi_usdt_swap_2026041500.h5",
                sources[0],
                source_order=0,
                expected_date_hour="2026041500",
            )
            self.assertIsInstance(frame, pd.DataFrame)
            self.assertEqual(
                list(frame.columns),
                [
                    "exchange_ns",
                    "exchange_id",
                    "source_order",
                    "input_sequence",
                    "symbol_id",
                    "local_ns",
                    "bid_price",
                    "bid_volume",
                    "ask_price",
                    "ask_volume",
                ],
            )
            self.assertEqual(frame["exchange_ns"].tolist(), [1002000000, 1004000000])
            self.assertEqual(frame["bid_price"].tolist(), [10.4, 10.6])

            stats = converter.convert_date(
                input_dir=input_dir,
                output_path=output_path,
                sources=sources,
                date="20260415",
                hours=[0],
            )

            self.assertEqual(stats.records_written, 4)
            self.assertEqual(stats.records_by_exchange, {"binance": 2, "gate": 2})
            raw = output_path.read_bytes()
            self.assertEqual(len(raw), 4 * BOOK_TICKER.size)
            records = [
                BOOK_TICKER.unpack_from(raw, offset)
                for offset in range(0, len(raw), BOOK_TICKER.size)
            ]

            self.assertEqual([record[0] for record in records], [0, 1, 2, 3])
            self.assertEqual([record[2] for record in records], [2, 0, 2, 0])
            self.assertEqual(
                [record[3] for record in records],
                [1001000000, 1002000000, 1003000000, 1004000000],
            )
            self.assertEqual(records[0][5:], (10.2, 9.0, 10.3, 7.0))
            self.assertEqual(records[1][5:], (10.4, 3.5, 10.5, 2.5))

    def test_rejects_config_date_mismatch(self):
        with TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            input_dir = root / "hdf"
            input_dir.mkdir()
            output_path = root / "out" / "20260415.bin"
            write_hdf(
                input_dir / "binance.ordi_usdt_swap_2026041500.h5",
                date_hour=2026041501,
                price_multiplier=1000,
                qty_multiplier=10,
                rows=[(1, 11, 10500, 25, 10400, 35, 1002, 1002, 1002)],
            )

            with self.assertRaisesRegex(ValueError, "config.date"):
                converter.convert_date(
                    input_dir=input_dir,
                    output_path=output_path,
                    sources=[
                        converter.HdfSource(
                            exchange_name="binance",
                            exchange_id=converter.EXCHANGE_BINANCE,
                            file_prefix="binance.ordi_usdt_swap",
                            symbol_id=3,
                        )
                    ],
                    date="20260415",
                    hours=[0],
                )


if __name__ == "__main__":
    unittest.main()
