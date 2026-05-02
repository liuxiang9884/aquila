#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest

import query_futures_contracts as contracts


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

        self.assertEqual(list(frame["symbol_id"]), [0])
        self.assertEqual(list(frame["symbol"]), ["BTC_USDT"])
        self.assertEqual(list(frame["settle"]), ["usdt"])
        self.assertEqual(list(frame["price_tick"]), [0.1])
        self.assertEqual(list(frame["price_decimal_places"]), [1])
        self.assertEqual(list(frame["contract_multiplier"]), [0.0001])
        self.assertEqual(list(frame["order_size_min"]), [1])
        self.assertEqual(list(frame["order_size_max"]), [1000000])
        self.assertEqual(list(frame["enable_decimal"]), [False])
        self.assertEqual(list(frame["market_order_size_max"]), [200000.0])
        self.assertEqual(list(frame["market_order_slip_ratio"]), [0.025])
        self.assertEqual(list(frame["orders_limit"]), [100])
        self.assertEqual(list(frame["status"]), ["trading"])


if __name__ == "__main__":
    unittest.main()
