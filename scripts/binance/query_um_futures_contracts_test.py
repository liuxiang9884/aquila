#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest

import query_um_futures_contracts as contracts


EXPECTED_COLUMNS = [
    "exchange",
    "symbol_id",
    "exchange_symbol",
    "base_asset",
    "quote_asset",
    "settle_asset",
    "status",
    "contract_type",
    "price_tick",
    "price_decimal_places",
    "quantity_step",
    "quantity_decimal_places",
    "quantity_min",
    "quantity_max",
    "market_quantity_max",
    "min_notional",
    "contract_multiplier",
    "price_limit_up",
    "price_limit_down",
    "market_price_bound",
]


class QueryUmFuturesContractsTest(unittest.TestCase):
    def test_parse_symbol_inputs_deduplicates_and_normalizes_usdt_symbols(self):
        parsed = contracts.parse_symbol_inputs(
            ["btc_usdt", "ETHUSDT", "BTCUSDT"],
            file_lines=["# comment", "sol/usdt", "", "ETH_USDT"],
        )

        self.assertEqual(parsed, ["BTCUSDT", "ETHUSDT", "SOLUSDT"])

    def test_exchange_info_symbol_maps_to_dataframe_row(self):
        payload = {
            "symbol": "BTCUSDT",
            "pair": "BTCUSDT",
            "contractType": "PERPETUAL",
            "deliveryDate": 4133404800000,
            "onboardDate": 1569398400000,
            "status": "TRADING",
            "baseAsset": "BTC",
            "quoteAsset": "USDT",
            "marginAsset": "USDT",
            "pricePrecision": 2,
            "quantityPrecision": 3,
            "baseAssetPrecision": 8,
            "quotePrecision": 8,
            "underlyingType": "COIN",
            "underlyingSubType": ["PoW"],
            "triggerProtect": "0.0500",
            "liquidationFee": "0.012500",
            "marketTakeBound": "0.30",
            "filters": [
                {
                    "filterType": "PRICE_FILTER",
                    "minPrice": "0.10",
                    "maxPrice": "1000000",
                    "tickSize": "0.10",
                },
                {
                    "filterType": "LOT_SIZE",
                    "minQty": "0.001",
                    "maxQty": "1000",
                    "stepSize": "0.001",
                },
                {
                    "filterType": "MARKET_LOT_SIZE",
                    "minQty": "0.001",
                    "maxQty": "120",
                    "stepSize": "0.001",
                },
                {"filterType": "MAX_NUM_ORDERS", "limit": 200},
                {"filterType": "MAX_NUM_ALGO_ORDERS", "limit": 10},
                {"filterType": "MIN_NOTIONAL", "notional": "100"},
                {
                    "filterType": "PERCENT_PRICE",
                    "multiplierUp": "1.1500",
                    "multiplierDown": "0.8500",
                    "multiplierDecimal": "4",
                },
            ],
            "orderTypes": ["LIMIT", "MARKET"],
            "timeInForce": ["GTC", "IOC"],
        }

        frame = contracts.symbols_to_dataframe([payload])

        self.assertEqual(list(frame.columns), EXPECTED_COLUMNS)
        self.assertEqual(list(frame["exchange"]), ["binance"])
        self.assertEqual(list(frame["symbol_id"]), [0])
        self.assertEqual(list(frame["exchange_symbol"]), ["BTCUSDT"])
        self.assertEqual(list(frame["base_asset"]), ["BTC"])
        self.assertEqual(list(frame["quote_asset"]), ["USDT"])
        self.assertEqual(list(frame["settle_asset"]), ["USDT"])
        self.assertEqual(list(frame["price_tick"]), [0.1])
        self.assertEqual(list(frame["price_decimal_places"]), [1])
        self.assertEqual(list(frame["quantity_step"]), [0.001])
        self.assertEqual(list(frame["quantity_decimal_places"]), [3])
        self.assertEqual(list(frame["quantity_min"]), [0.001])
        self.assertEqual(list(frame["quantity_max"]), [1000.0])
        self.assertEqual(list(frame["market_quantity_max"]), [120.0])
        self.assertEqual(list(frame["min_notional"]), [100.0])
        self.assertEqual(list(frame["contract_multiplier"]), [1.0])
        self.assertAlmostEqual(list(frame["price_limit_up"])[0], 0.15)
        self.assertAlmostEqual(list(frame["price_limit_down"])[0], 0.15)
        self.assertEqual(list(frame["market_price_bound"]), [0.3])


if __name__ == "__main__":
    unittest.main()
