#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest

import query_gate_account as account


class QueryGateAccountTest(unittest.TestCase):
    def test_signature_matches_gate_rest_docs_example(self):
        headers = account.build_signature_headers(
            api_key="key",
            api_secret="secret",
            method="GET",
            request_path="/api/v4/futures/orders",
            query_string="contract=BTC_USD&status=finished&limit=50",
            body="",
            timestamp=1541993715,
        )

        self.assertEqual(headers["KEY"], "key")
        self.assertEqual(headers["Timestamp"], "1541993715")
        self.assertEqual(
            headers["SIGN"],
            "55f84ea195d6fe57ce62464daaa7c3c02fa9d1dde954e4c898289c9a2407a3d6fb3faf24deff16790d726b66ac9f74526668b13bd01029199cc4fcc522418b8a",
        )

    def test_query_plan_uses_read_only_account_and_fee_endpoints(self):
        plan = account.build_query_plan(
            settle="usdt",
            currency="USDT",
            currency_pair="BTC_USDT",
            futures_contracts=["BTC_USDT", "ETH_USDT"],
        )

        self.assertEqual(
            [(request.label, request.endpoint_path, request.query_string) for request in plan],
            [
                ("wallet_total_balance", "/wallet/total_balance", "currency=USDT"),
                ("futures_account", "/futures/usdt/accounts", ""),
                ("wallet_fee", "/wallet/fee", "currency_pair=BTC_USDT&settle=USDT"),
                ("futures_fee_BTC_USDT", "/futures/usdt/fee", "contract=BTC_USDT"),
                ("futures_fee_ETH_USDT", "/futures/usdt/fee", "contract=ETH_USDT"),
            ],
        )

    def test_query_account_info_aggregates_responses(self):
        calls = []

        def fake_request(api_request):
            calls.append((api_request.label, api_request.endpoint_path, api_request.query_string))
            return {"label": api_request.label}

        result = account.query_account_info(
            requester=fake_request,
            settle="usdt",
            currency="USDT",
            currency_pair=None,
            futures_contracts=[],
        )

        self.assertTrue(result["ok"])
        self.assertEqual(result["errors"], {})
        self.assertEqual(
            calls,
            [
                ("wallet_total_balance", "/wallet/total_balance", "currency=USDT"),
                ("futures_account", "/futures/usdt/accounts", ""),
                ("wallet_fee", "/wallet/fee", "settle=USDT"),
                ("futures_fee", "/futures/usdt/fee", ""),
            ],
        )
        self.assertEqual(result["results"]["futures_account"], {"label": "futures_account"})

    def test_query_account_info_can_keep_partial_results(self):
        def fake_request(api_request):
            if api_request.label == "futures_account":
                raise RuntimeError("HTTP 401: invalid key")
            return {"label": api_request.label}

        result = account.query_account_info(
            requester=fake_request,
            allow_partial=True,
            settle="usdt",
            currency="USDT",
            currency_pair=None,
            futures_contracts=[],
        )

        self.assertFalse(result["ok"])
        self.assertIn("wallet_total_balance", result["results"])
        self.assertEqual(result["errors"], {"futures_account": "RuntimeError: HTTP 401: invalid key"})


if __name__ == "__main__":
    unittest.main()
