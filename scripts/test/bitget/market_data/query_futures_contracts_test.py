#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[3] / "bitget" / "market_data"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import query_futures_contracts as bitget_contracts


class QueryBitgetFuturesContractsTest(unittest.TestCase):
    def test_filter_keeps_only_online_usdt_perpetuals(self):
        rows = bitget_contracts.select_usdt_perpetuals(
            [
                {
                    "symbol": "BTCUSDT",
                    "category": "USDT-FUTURES",
                    "status": "online",
                    "type": "perpetual",
                },
                {
                    "symbol": "ETHUSDT",
                    "category": "SPOT",
                    "status": "online",
                    "type": "perpetual",
                },
                {
                    "symbol": "SOLUSDT",
                    "category": "USDT-FUTURES",
                    "status": "limit_open",
                    "type": "perpetual",
                },
                {
                    "symbol": "BTCUSDT_260327",
                    "category": "USDT-FUTURES",
                    "status": "online",
                    "type": "delivery",
                },
            ]
        )

        self.assertEqual([bitget_contracts.internal_symbol(row) for row in rows],
                         ["BTC_USDT"])

    def test_instrument_to_row_maps_futures_fields(self):
        payload = {
            "symbol": "BTCUSDT",
            "category": "USDT-FUTURES",
            "baseCoin": "BTC",
            "quoteCoin": "USDT",
            "status": "online",
            "type": "perpetual",
            "priceMultiplier": "0.1",
            "pricePrecision": "1",
            "quantityMultiplier": "0.0001",
            "quantityPrecision": "4",
            "minOrderQty": "0.0001",
            "maxOrderQty": "1200",
            "maxMarketOrderQty": "220",
            "minOrderAmount": "5",
            "buyLimitPriceRatio": "0.05",
            "sellLimitPriceRatio": "0.04",
        }

        row = bitget_contracts.instrument_to_row(7, payload)

        self.assertEqual(row["exchange"], "bitget")
        self.assertEqual(row["symbol_id"], 7)
        self.assertEqual(row["exchange_symbol"], "BTCUSDT")
        self.assertEqual(row["base_asset"], "BTC")
        self.assertEqual(row["quote_asset"], "USDT")
        self.assertEqual(row["settle_asset"], "USDT")
        self.assertEqual(row["status"], "online")
        self.assertEqual(row["contract_type"], "perpetual")
        self.assertEqual(row["price_tick"], 0.1)
        self.assertEqual(row["price_decimal_places"], 1)
        self.assertEqual(row["quantity_step"], 0.0001)
        self.assertEqual(row["quantity_decimal_places"], 4)
        self.assertEqual(row["min_quantity"], 0.0001)
        self.assertEqual(row["max_quantity"], 1200.0)
        self.assertEqual(row["max_market_quantity"], 220.0)
        self.assertEqual(row["min_notional"], 5.0)
        self.assertEqual(row["notional_multiplier"], 1.0)
        self.assertEqual(row["price_limit_up"], 0.05)
        self.assertEqual(row["price_limit_down"], 0.04)
        self.assertIsNone(row["market_price_bound"])

    def test_select_symbols_can_skip_missing_symbols(self):
        instruments = [
            {
                "symbol": "BTCUSDT",
                "category": "USDT-FUTURES",
                "status": "online",
                "type": "perpetual",
            }
        ]

        rows = bitget_contracts.select_symbols(
            instruments, ["BTC_USDT", "MISSING_USDT"], allow_missing=True
        )

        self.assertEqual([row["symbol"] for row in rows], ["BTCUSDT"])


if __name__ == "__main__":
    unittest.main()
