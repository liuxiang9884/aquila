#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "instruments"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import generate_common_usdt_perp_catalog as catalog


class GenerateCommonUsdtPerpCatalogTest(unittest.TestCase):
    def test_binance_filter_keeps_only_trading_usdt_perpetuals(self):
        rows = catalog.select_binance_usdt_perpetuals(
            {
                "symbols": [
                    {
                        "symbol": "BTCUSDT",
                        "baseAsset": "BTC",
                        "quoteAsset": "USDT",
                        "marginAsset": "USDT",
                        "contractType": "PERPETUAL",
                        "status": "TRADING",
                        "filters": [],
                    },
                    {
                        "symbol": "ETHUSDC",
                        "baseAsset": "ETH",
                        "quoteAsset": "USDC",
                        "marginAsset": "USDC",
                        "contractType": "PERPETUAL",
                        "status": "TRADING",
                        "filters": [],
                    },
                    {
                        "symbol": "SOLUSDT",
                        "baseAsset": "SOL",
                        "quoteAsset": "USDT",
                        "marginAsset": "USDT",
                        "contractType": "CURRENT_QUARTER",
                        "status": "TRADING",
                        "filters": [],
                    },
                    {
                        "symbol": "OLDUSDT",
                        "baseAsset": "OLD",
                        "quoteAsset": "USDT",
                        "marginAsset": "USDT",
                        "contractType": "PERPETUAL",
                        "status": "BREAK",
                        "filters": [],
                    },
                ]
            }
        )

        self.assertEqual([catalog.binance_internal_symbol(row) for row in rows],
                         ["BTC_USDT"])

    def test_gate_filter_keeps_only_trading_usdt_contracts(self):
        rows = catalog.select_gate_usdt_perpetuals(
            [
                {"name": "BTC_USDT", "status": "trading", "type": "direct"},
                {"name": "ETH_USD", "status": "trading", "type": "direct"},
                {"name": "SOL_USDT", "status": "delisted", "type": "direct"},
            ]
        )

        self.assertEqual([catalog.gate_internal_symbol(row) for row in rows],
                         ["BTC_USDT"])

    def test_build_catalog_rows_assigns_shared_symbol_id(self):
        gate_payload = {
            "name": "BTC_USDT",
            "type": "direct",
            "status": "trading",
            "quanto_multiplier": "0.0001",
            "order_price_round": "0.1",
            "order_size_min": "1",
            "order_size_max": "1000",
            "market_order_size_max": "100",
            "order_price_deviate": "0.5",
            "market_order_slip_ratio": "0.025",
        }
        binance_payload = {
            "symbol": "BTCUSDT",
            "baseAsset": "BTC",
            "quoteAsset": "USDT",
            "marginAsset": "USDT",
            "contractType": "PERPETUAL",
            "status": "TRADING",
            "marketTakeBound": "0.05",
            "filters": [
                {"filterType": "PRICE_FILTER", "tickSize": "0.10"},
                {
                    "filterType": "LOT_SIZE",
                    "minQty": "0.001",
                    "maxQty": "1000",
                    "stepSize": "0.001",
                },
                {"filterType": "MARKET_LOT_SIZE", "maxQty": "120"},
                {"filterType": "MIN_NOTIONAL", "notional": "100"},
                {
                    "filterType": "PERCENT_PRICE",
                    "multiplierUp": "1.05",
                    "multiplierDown": "0.95",
                },
            ],
        }

        rows = catalog.build_catalog_rows([gate_payload], [binance_payload])

        self.assertEqual(catalog.CATALOG_COLUMNS, list(rows[0].keys()))
        self.assertEqual(len(rows), 2)
        self.assertEqual([row["symbol_id"] for row in rows], [0, 0])
        self.assertEqual([row["symbol"] for row in rows],
                         ["BTC_USDT", "BTC_USDT"])
        self.assertEqual([row["exchange"] for row in rows],
                         ["gate", "binance"])
        self.assertEqual([row["product_type"] for row in rows],
                         ["linear_perpetual", "linear_perpetual"])
        self.assertEqual(rows[1]["exchange_symbol"], "BTCUSDT")
        self.assertEqual(rows[0]["contract_multiplier"], 0.0001)
        self.assertEqual(rows[1]["contract_multiplier"], 1.0)
        self.assertEqual(rows[0]["contract_multiplier"], rows[0]["notional_multiplier"])
        self.assertEqual(rows[1]["contract_multiplier"], rows[1]["notional_multiplier"])

    def test_write_catalog_refuses_to_overwrite_by_default(self):
        rows = [{column: "" for column in catalog.CATALOG_COLUMNS}]
        rows[0].update(
            {
                "symbol_id": 0,
                "symbol": "BTC_USDT",
                "exchange": "gate",
                "exchange_symbol": "BTC_USDT",
                "base_asset": "BTC",
                "quote_asset": "USDT",
                "settle_asset": "USDT",
                "product_type": "linear_perpetual",
                "status": "TRADING",
                "contract_type": "direct",
                "price_tick": 0.1,
                "price_decimal_places": 1,
                "min_quantity": 1,
                "max_quantity": 100,
                "notional_multiplier": 0.0001,
            }
        )

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "catalog.csv"
            catalog.write_catalog_csv(output, rows, overwrite=False)

            with self.assertRaises(FileExistsError):
                catalog.write_catalog_csv(output, rows, overwrite=False)

            catalog.write_catalog_csv(output, rows, overwrite=True)
            with output.open(newline="", encoding="utf-8") as handle:
                loaded = list(csv.DictReader(handle))
            self.assertEqual(loaded[0]["symbol"], "BTC_USDT")


if __name__ == "__main__":
    unittest.main()
