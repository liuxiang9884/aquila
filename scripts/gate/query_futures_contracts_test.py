#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest

import query_futures_contracts as contracts


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
    "min_quantity",
    "max_quantity",
    "max_market_quantity",
    "min_notional",
    "notional_multiplier",
    "price_limit_up",
    "price_limit_down",
    "market_price_bound",
]


class QueryFuturesContractsTest(unittest.TestCase):
    def test_parse_symbol_inputs_deduplicates_and_ignores_comments(self):
        parsed = contracts.parse_symbol_inputs(
            ["btc_usdt", "ETH_USDT", "BTC_USDT"],
            file_lines=["# comment", "sol_usdt", "", "ETH_USDT"],
        )

        self.assertEqual(parsed, ["BTC_USDT", "ETH_USDT", "SOL_USDT"])

    def test_contract_payload_maps_to_dataframe_row(self):
        payload = {
            "name": "BTC_USDT",
            "type": "direct",
            "quanto_multiplier": "0.0001",
            "order_price_round": "0.1",
            "mark_price_round": "0.01",
            "order_size_min": 1,
            "order_size_max": 1000000,
            "enable_decimal": False,
            "order_price_deviate": "0.5",
            "market_order_size_max": "200000",
            "market_order_slip_ratio": "0.025",
            "orders_limit": 100,
            "maker_fee_rate": "-0.00025",
            "taker_fee_rate": "0.00075",
            "leverage_min": "1",
            "leverage_max": "125",
            "risk_limit_base": "1000000",
            "risk_limit_step": "1000000",
            "risk_limit_max": "5000000",
            "maintenance_rate": "0.005",
            "in_delisting": False,
            "status": "trading",
            "config_change_time": 1710000000,
        }

        frame = contracts.contracts_to_dataframe([payload])

        self.assertEqual(list(frame.columns), EXPECTED_COLUMNS)
        self.assertEqual(list(frame["exchange"]), ["gate"])
        self.assertEqual(list(frame["symbol_id"]), [0])
        self.assertEqual(list(frame["exchange_symbol"]), ["BTC_USDT"])
        self.assertEqual(list(frame["base_asset"]), ["BTC"])
        self.assertEqual(list(frame["quote_asset"]), ["USDT"])
        self.assertEqual(list(frame["settle_asset"]), ["USDT"])
        self.assertEqual(list(frame["status"]), ["TRADING"])
        self.assertEqual(list(frame["price_tick"]), [0.1])
        self.assertEqual(list(frame["price_decimal_places"]), [1])
        self.assertEqual(list(frame["quantity_step"]), [1.0])
        self.assertEqual(list(frame["quantity_decimal_places"]), [0])
        self.assertEqual(list(frame["min_quantity"]), [1])
        self.assertEqual(list(frame["max_quantity"]), [1000000])
        self.assertEqual(list(frame["max_market_quantity"]), [200000.0])
        self.assertEqual(list(frame["min_notional"]), [None])
        self.assertEqual(list(frame["notional_multiplier"]), [0.0001])
        self.assertEqual(list(frame["price_limit_up"]), [0.5])
        self.assertEqual(list(frame["price_limit_down"]), [0.5])
        self.assertEqual(list(frame["market_price_bound"]), [0.025])

    def test_decimal_size_contract_leaves_quantity_step_unknown(self):
        payload = {
            "name": "ETH_USDT",
            "type": "direct",
            "quanto_multiplier": "0.01",
            "order_price_round": "0.01",
            "order_size_min": 0,
            "order_size_max": 3800000,
            "enable_decimal": True,
        }

        frame = contracts.contracts_to_dataframe([payload])

        self.assertEqual(list(frame["quantity_step"]), [None])
        self.assertEqual(list(frame["quantity_decimal_places"]), [None])


if __name__ == "__main__":
    unittest.main()
