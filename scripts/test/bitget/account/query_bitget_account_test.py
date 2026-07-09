#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[3] / "bitget" / "account"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import query_bitget_account as account


class FakeHttpResponse:
    def __init__(self, body=b'{"code":"00000","data":{}}'):
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def read(self):
        return self.body


class QueryBitgetAccountTest(unittest.TestCase):
    def test_signature_matches_bitget_hmac_shape(self):
        headers = account.build_signature_headers(
            api_key="key",
            api_secret="secret",
            passphrase="phrase",
            method="GET",
            request_path="/api/v3/position/current-position",
            query_string="category=USDT-FUTURES&symbol=BTCUSDT",
            timestamp=1684814440729,
        )

        self.assertEqual(headers["ACCESS-KEY"], "key")
        self.assertEqual(headers["ACCESS-PASSPHRASE"], "phrase")
        self.assertEqual(headers["ACCESS-TIMESTAMP"], "1684814440729")
        self.assertEqual(headers["ACCESS-SIGN"], "MEpEsfBcw6yC7EUQ4L5Jd0QDFWgjPsZMrY/uFp2+vnE=")

    def test_account_query_plan_uses_read_only_uta_endpoints(self):
        plan = account.build_account_query_plan()

        self.assertEqual(
            [(request.label, request.endpoint_path, request.query_string) for request in plan],
            [
                ("account_assets", "/api/v3/account/assets", ""),
                ("account_settings", "/api/v3/account/settings", ""),
            ],
        )

    def test_position_query_plan_supports_category_symbol_and_side(self):
        plan = account.build_position_query_plan(
            category="usdt-futures",
            symbol="btc_usdt",
            pos_side="long",
        )

        self.assertEqual(
            [(request.label, request.endpoint_path, request.query_string) for request in plan],
            [
                (
                    "current_positions",
                    "/api/v3/position/current-position",
                    "category=USDT-FUTURES&posSide=long&symbol=BTCUSDT",
                )
            ],
        )

    def test_order_query_plan_lists_open_orders(self):
        plan = account.build_order_query_plan(
            category="usdt-futures",
            symbol="btc_usdt",
            status="open",
            order_id=None,
            client_oid=None,
            start_time=None,
            end_time=None,
            limit=20,
            cursor=None,
        )

        self.assertEqual(
            [(request.label, request.endpoint_path, request.query_string) for request in plan],
            [
                (
                    "open_orders",
                    "/api/v3/trade/unfilled-orders",
                    "category=USDT-FUTURES&limit=20&symbol=BTCUSDT",
                )
            ],
        )

    def test_order_query_plan_lists_history_orders(self):
        plan = account.build_order_query_plan(
            category="usdt-futures",
            symbol=None,
            status="history",
            order_id=None,
            client_oid=None,
            start_time="1700000000000",
            end_time="1700100000000",
            limit=100,
            cursor="abc",
        )

        self.assertEqual(
            [(request.label, request.endpoint_path, request.query_string) for request in plan],
            [
                (
                    "history_orders",
                    "/api/v3/trade/history-orders",
                    "category=USDT-FUTURES&cursor=abc&endTime=1700100000000&limit=100&startTime=1700000000000",
                )
            ],
        )

    def test_order_query_plan_gets_single_order_by_order_id_or_client_oid(self):
        plan = account.build_order_query_plan(
            category="USDT-FUTURES",
            symbol="BTCUSDT",
            status="open",
            order_id="123",
            client_oid="client/abc",
            start_time=None,
            end_time=None,
            limit=None,
            cursor=None,
        )

        self.assertEqual(
            [(request.label, request.endpoint_path, request.query_string) for request in plan],
            [("order_info", "/api/v3/trade/order-info", "clientOid=client%2Fabc&orderId=123")],
        )

    def test_query_requests_aggregates_partial_errors(self):
        requests = [
            account.ApiRequest(label="ok", endpoint_path="/ok"),
            account.ApiRequest(label="fail", endpoint_path="/fail"),
        ]

        def fake_request(api_request):
            if api_request.label == "fail":
                raise RuntimeError("HTTP 401")
            return {"label": api_request.label}

        result = account.query_requests(
            requester=fake_request,
            requests=requests,
            allow_partial=True,
        )

        self.assertFalse(result["ok"])
        self.assertEqual(result["results"], {"ok": {"label": "ok"}})
        self.assertEqual(result["errors"], {"fail": "RuntimeError: HTTP 401"})

    def test_signed_rest_client_sets_bitget_auth_headers(self):
        seen = {}
        original_urlopen = account.urllib.request.urlopen

        def fake_urlopen(request, timeout):
            del timeout
            seen["url"] = request.full_url
            seen["key"] = request.get_header("Access-key")
            seen["passphrase"] = request.get_header("Access-passphrase")
            seen["timestamp"] = request.get_header("Access-timestamp")
            seen["sign"] = request.get_header("Access-sign")
            return FakeHttpResponse()

        account.urllib.request.urlopen = fake_urlopen
        try:
            client = account.SignedBitgetRestClient(
                api_key="key",
                api_secret="secret",
                passphrase="phrase",
                base_url="https://api.example.test",
            )
            client.get_json(
                account.ApiRequest(
                    label="position",
                    endpoint_path="/api/v3/position/current-position",
                    query_string="category=USDT-FUTURES&symbol=BTCUSDT",
                )
            )
        finally:
            account.urllib.request.urlopen = original_urlopen

        self.assertEqual(
            seen["url"],
            "https://api.example.test/api/v3/position/current-position?category=USDT-FUTURES&symbol=BTCUSDT",
        )
        self.assertEqual(seen["key"], "key")
        self.assertEqual(seen["passphrase"], "phrase")
        self.assertIsNotNone(seen["timestamp"])
        self.assertIsNotNone(seen["sign"])


if __name__ == "__main__":
    unittest.main()
