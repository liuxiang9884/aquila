#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import math
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "market_data"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import query_common_usdt_perp_klines as klines


class QueryCommonUsdtPerpKlinesTest(unittest.TestCase):
    def test_default_catalog_uses_current_universe(self):
        self.assertEqual(
            klines.DEFAULT_CATALOG,
            Path("config/instruments/usdt_future_universe.csv"),
        )

    def test_load_common_symbols_pairs_gate_and_binance_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            catalog_path = Path(tmp) / "catalog.csv"
            catalog_path.write_text(
                "symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,settle_asset,product_type,status,contract_type,price_tick,price_decimal_places,quantity_step,quantity_decimal_places,min_quantity,max_quantity,max_market_quantity,min_notional,notional_multiplier,price_limit_up,price_limit_down,market_price_bound\n"
                "0,BTC_USDT,gate,BTC_USDT,BTC,USDT,USDT,linear_perpetual,TRADING,direct,1e-05,5,1,0,1,1000,,,0.0001,0.5,0.5,\n"
                "0,BTC_USDT,binance,BTCUSDT,BTC,USDT,USDT,linear_perpetual,TRADING,PERPETUAL,0.001,3,0.001,3,0.001,1000,120,100,1,0.05,0.05,0.05\n"
                "1,ONLY_GATE,gate,ONLY_GATE,ONLY,USDT,USDT,linear_perpetual,TRADING,direct,0.1,1,1,0,1,1000,,,0.0001,0.5,0.5,\n",
                encoding="utf-8",
            )

            symbols = klines.load_common_symbols(catalog_path)

        self.assertEqual(len(symbols), 1)
        self.assertEqual(symbols[0].symbol, "BTC_USDT")
        self.assertEqual(symbols[0].gate_symbol, "BTC_USDT")
        self.assertEqual(symbols[0].binance_symbol, "BTCUSDT")
        self.assertEqual(symbols[0].gate_tick_size, "0.00001")
        self.assertEqual(symbols[0].binance_tick_size, "0.001")

    def test_parse_binance_kline_array(self):
        row = klines.parse_binance_kline(
            "BTC_USDT",
            "BTCUSDT",
            [1000, "10", "12", "9", "11", "100", 60999, "1100", 10, "1", "2", "0"],
            now_ms=70000,
        )

        self.assertEqual(row.exchange, "binance")
        self.assertEqual(row.symbol, "BTC_USDT")
        self.assertEqual(row.open_time_ms, 1000)
        self.assertEqual(row.close_time_ms, 60999)
        self.assertEqual(row.close, 11.0)
        self.assertEqual(row.quote_volume, 1100.0)
        self.assertTrue(row.closed)

    def test_parse_gate_kline_dict_and_array(self):
        dict_row = klines.parse_gate_kline(
            "BTC_USDT",
            "BTC_USDT",
            {
                "t": 60,
                "o": "10",
                "h": "12",
                "l": "9",
                "c": "11",
                "v": "100",
                "sum": "1100",
            },
            now_ms=121000,
        )
        array_row = klines.parse_gate_kline(
            "ETH_USDT",
            "ETH_USDT",
            [60, "100", "11", "12", "9", "10", "1100"],
            now_ms=121000,
        )

        self.assertEqual(dict_row.open_time_ms, 60000)
        self.assertEqual(dict_row.close_time_ms, 119999)
        self.assertEqual(dict_row.close, 11.0)
        self.assertTrue(dict_row.closed)
        self.assertEqual(array_row.close, 11.0)

    def test_compute_realized_volatility_uses_last_n_plus_one_closed_closes(
        self,
    ):
        closes = [100.0, 101.0, 99.0, 102.0]
        expected = math.sqrt(
            math.log(101.0 / 100.0) ** 2
            + math.log(99.0 / 101.0) ** 2
            + math.log(102.0 / 99.0) ** 2
        ) * 10000

        actual = klines.realized_vol_bps(closes, 3)

        self.assertAlmostEqual(actual, expected)
        self.assertIsNone(klines.realized_vol_bps(closes, 4))

    def test_write_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            output_dir = Path(tmp)
            kline_rows = [
                klines.KlineRow(
                    exchange="gate",
                    symbol="BTC_USDT",
                    exchange_symbol="BTC_USDT",
                    open_time_ms=0,
                    close_time_ms=59999,
                    open=10.0,
                    high=11.0,
                    low=9.0,
                    close=10.5,
                    volume=100.0,
                    quote_volume=1000.0,
                    closed=True,
                )
            ]
            summary_rows = [
                {
                    "symbol": "BTC_USDT",
                    "gate_vol_30m_bps": "12.3",
                    "gate_vol_60m_bps": "",
                    "gate_valid_30m": "true",
                    "gate_valid_60m": "false",
                    "gate_close_count": 31,
                    "binance_vol_30m_bps": "10.0",
                    "binance_vol_60m_bps": "",
                    "binance_valid_30m": "true",
                    "binance_valid_60m": "false",
                    "binance_close_count": 31,
                    "max_vol_60m_bps": "",
                    "min_vol_60m_bps": "",
                }
            ]

            klines.write_kline_csv(output_dir / "gate.csv", kline_rows)
            klines.write_summary_csv(output_dir / "summary.csv", summary_rows)

            with (output_dir / "gate.csv").open(newline="", encoding="utf-8") as handle:
                loaded_kline = list(csv.DictReader(handle))
            with (output_dir / "summary.csv").open(newline="", encoding="utf-8") as handle:
                loaded_summary = list(csv.DictReader(handle))

        self.assertEqual(loaded_kline[0]["symbol"], "BTC_USDT")
        self.assertEqual(loaded_summary[0]["gate_valid_30m"], "true")

    def test_build_exchange_result_rows_sums_latest_closed_quote_volume(self):
        symbols = [
            klines.CommonSymbol(
                symbol="BTC_USDT",
                gate_symbol="BTC_USDT",
                binance_symbol="BTCUSDT",
                gate_tick_size="0.00001",
                binance_tick_size="0.001",
            )
        ]
        rows = [
            klines.KlineRow(
                exchange="gate",
                symbol="BTC_USDT",
                exchange_symbol="BTC_USDT",
                open_time_ms=index * klines.INTERVAL_MS,
                close_time_ms=(index + 1) * klines.INTERVAL_MS - 1,
                open=100.0 + index,
                high=101.0 + index,
                low=99.0 + index,
                close=100.0 + index,
                volume=float(index + 1),
                quote_volume=float((index + 1) * 10),
                closed=True,
            )
            for index in range(31)
        ]

        result_rows = klines.build_exchange_result_rows(
            "gate",
            symbols,
            {"BTC_USDT": rows},
            [30, 60],
        )

        self.assertEqual(len(result_rows), 1)
        row = result_rows[0]
        self.assertEqual(row["exchange"], "gate")
        self.assertEqual(row["symbol"], "BTC_USDT")
        self.assertEqual(row["exchange_symbol"], "BTC_USDT")
        self.assertEqual(row["tick_size"], "0.00001")
        self.assertNotEqual(row["vol_30m_bps"], "")
        self.assertEqual(row["quote_volume_30m"], "4950.000000")
        self.assertEqual(row["volume_30m"], "495.000000")
        self.assertEqual(row["valid_30m"], "true")
        self.assertEqual(row["valid_60m"], "false")
        self.assertEqual(row["close_count"], 31)
        self.assertEqual(row["latest_closed_open_time_ms"], 30 * klines.INTERVAL_MS)
        self.assertEqual(row["reference_price"], "130.000000")
        self.assertNotIn("latest_close", row)

    def test_write_exchange_result_csv(self):
        with tempfile.TemporaryDirectory() as tmp:
            output_path = Path(tmp) / "gate_volatility_liquidity.csv"
            rows = [
                {
                    "exchange": "gate",
                    "symbol": "BTC_USDT",
                    "exchange_symbol": "BTC_USDT",
                    "tick_size": "0.00001",
                    "vol_30m_bps": "123.000000",
                    "vol_60m_bps": "",
                    "quote_volume_30m": "4800.000000",
                    "quote_volume_60m": "",
                    "volume_30m": "480.000000",
                    "volume_60m": "",
                    "valid_30m": "true",
                    "valid_60m": "false",
                    "close_count": 31,
                    "latest_closed_open_time_ms": 1800000,
                    "reference_price": "130.000000",
                }
            ]

            klines.write_exchange_result_csv(output_path, rows)

            with output_path.open(newline="", encoding="utf-8") as handle:
                loaded_rows = list(csv.DictReader(handle))

        self.assertEqual(loaded_rows[0]["quote_volume_30m"], "4800.000000")
        self.assertEqual(loaded_rows[0]["tick_size"], "0.00001")
        self.assertEqual(loaded_rows[0]["reference_price"], "130.000000")
        self.assertNotIn("latest_close", loaded_rows[0])
        self.assertEqual(loaded_rows[0]["valid_60m"], "false")


if __name__ == "__main__":
    unittest.main()
