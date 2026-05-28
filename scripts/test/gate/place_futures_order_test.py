#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "gate"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import place_futures_order as orders


class FakeHttpResponse:
    def __init__(self, body=b"{}"):
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def read(self):
        return self.body


class PlaceFuturesOrderTest(unittest.TestCase):
    def test_build_order_payload_uses_positive_size_for_buy(self):
        payload = orders.build_order_payload(
            contract="BTC_USDT",
            side="buy",
            size=1,
            price="1",
            tif="gtc",
            text="t-aquila-rest-test",
        )

        self.assertEqual(
            payload,
            {
                "contract": "BTC_USDT",
                "size": 1,
                "iceberg": 0,
                "price": "1",
                "tif": "gtc",
                "text": "t-aquila-rest-test",
            },
        )

    def test_build_order_payload_uses_negative_size_for_sell(self):
        payload = orders.build_order_payload(
            contract="BTC_USDT",
            side="sell",
            size=3,
            price="1000000",
            tif="gtc",
            text="t-aquila-rest-test",
            reduce_only=True,
        )

        self.assertEqual(payload["size"], -3)
        self.assertTrue(payload["reduce_only"])

    def test_build_order_payload_allows_max_configured_size(self):
        payload = orders.build_order_payload(
            contract="BTC_USDT",
            side="buy",
            size=orders.MAX_ORDER_SIZE,
            price="1",
            tif="gtc",
            text="t-aquila-rest-test",
        )

        self.assertEqual(payload["size"], 5)

    def test_build_order_payload_rejects_size_above_max_risk_limit(self):
        with self.assertRaisesRegex(ValueError, "size must be <= 5"):
            orders.build_order_payload(
                contract="BTC_USDT",
                side="buy",
                size=6,
                price="1",
                tif="gtc",
                text="t-aquila-rest-test",
            )

    def test_build_order_payload_rejects_sell_size_above_max_risk_limit(self):
        with self.assertRaisesRegex(ValueError, "size must be <= 5"):
            orders.build_order_payload(
                contract="BTC_USDT",
                side="sell",
                size=6,
                price="1000000",
                tif="gtc",
                text="t-aquila-rest-test",
            )

    def test_build_order_payload_rejects_market_price_without_ioc(self):
        with self.assertRaisesRegex(ValueError, "price=0 requires tif=ioc"):
            orders.build_order_payload(
                contract="BTC_USDT",
                side="buy",
                size=1,
                price="0",
                tif="gtc",
                text="t-aquila-rest-test",
            )

    def test_build_order_payload_rejects_invalid_text_prefix(self):
        with self.assertRaisesRegex(ValueError, "must start with t-"):
            orders.build_order_payload(
                contract="BTC_USDT",
                side="buy",
                size=1,
                price="1",
                tif="gtc",
                text="aquila-rest-test",
            )

    def test_build_place_order_request_serializes_stable_json_body(self):
        api_request = orders.build_place_order_request(
            settle="usdt",
            payload={
                "contract": "BTC_USDT",
                "size": 1,
                "iceberg": 0,
                "price": "1",
                "tif": "gtc",
                "text": "t-aquila-rest-test",
            },
        )

        self.assertEqual(api_request.method, "POST")
        self.assertEqual(api_request.endpoint_path, "/futures/usdt/orders")
        self.assertEqual(api_request.query_string, "")
        self.assertEqual(
            api_request.body,
            '{"contract":"BTC_USDT","size":1,"iceberg":0,"price":"1","tif":"gtc","text":"t-aquila-rest-test"}',
        )

    def test_build_place_order_request_serializes_raw_decimal_size(self):
        api_request = orders.build_place_order_request(
            settle="usdt",
            payload={
                "contract": "RAVE_USDT",
                "size": orders.RawJsonNumber("-0.2"),
                "iceberg": 0,
                "price": "0",
                "tif": "ioc",
                "text": "t-aquila-rest-test",
                "reduce_only": True,
            },
        )

        self.assertIn('"size":-0.2', api_request.body)
        self.assertEqual(json.loads(api_request.body)["size"], -0.2)

    def test_raw_json_number_rejects_non_json_number_text(self):
        invalid_values = ["", "01", "1e-1", "0.2, \"price\":\"1\"", "NaN"]

        for value in invalid_values:
            with self.subTest(value=value):
                with self.assertRaisesRegex(ValueError, "invalid raw JSON number"):
                    orders.RawJsonNumber(value)

    def test_dry_run_does_not_call_client(self):
        def fail_request(api_request):
            raise AssertionError(f"unexpected request: {api_request}")

        result = orders.place_order(
            requester=fail_request,
            settle="usdt",
            payload=orders.build_order_payload(
                contract="BTC_USDT",
                side="buy",
                size=1,
                price="1",
                tif="gtc",
                text="t-aquila-rest-test",
            ),
            execute=False,
            keep_open=False,
        )

        self.assertFalse(result["executed"])
        self.assertEqual(result["place_request"]["method"], "POST")
        self.assertNotIn("place_response", result)

    def test_execute_places_order_and_cancels_by_returned_id(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "POST":
                return {"id": 12345, "status": "open"}
            if api_request.method == "DELETE":
                return {"id": 12345, "status": "finished", "finish_as": "cancelled"}
            raise AssertionError(f"unexpected method: {api_request.method}")

        result = orders.place_order(
            requester=fake_request,
            settle="usdt",
            payload=orders.build_order_payload(
                contract="BTC_USDT",
                side="buy",
                size=1,
                price="1",
                tif="gtc",
                text="t-aquila-rest-test",
            ),
            execute=True,
            keep_open=False,
        )

        self.assertTrue(result["executed"])
        self.assertEqual([call.method for call in calls], ["POST", "DELETE"])
        self.assertEqual(calls[1].endpoint_path, "/futures/usdt/orders/12345")
        self.assertEqual(result["cancel_response"]["finish_as"], "cancelled")

    def test_execute_keep_open_skips_cancel(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            return {"id": 12345, "status": "open"}

        result = orders.place_order(
            requester=fake_request,
            settle="usdt",
            payload=orders.build_order_payload(
                contract="BTC_USDT",
                side="buy",
                size=1,
                price="1",
                tif="gtc",
                text="t-aquila-rest-test",
            ),
            execute=True,
            keep_open=True,
        )

        self.assertEqual([call.method for call in calls], ["POST"])
        self.assertNotIn("cancel_response", result)

    def test_cancel_order_deletes_by_order_id(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            return {"id": 12345, "status": "finished", "finish_as": "cancelled"}

        result = orders.cancel_order(
            requester=fake_request,
            settle="usdt",
            order_id="12345",
        )

        self.assertEqual([call.method for call in calls], ["DELETE"])
        self.assertEqual(calls[0].endpoint_path, "/futures/usdt/orders/12345")
        self.assertEqual(result["cancel_request"]["method"], "DELETE")
        self.assertEqual(result["cancel_response"]["finish_as"], "cancelled")

    def test_cancel_order_escapes_text_order_id(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            return {"text": "t-aquila/rest-test", "status": "finished"}

        orders.cancel_order(
            requester=fake_request,
            settle="usdt",
            order_id="t-aquila/rest-test",
        )

        self.assertEqual(calls[0].endpoint_path, "/futures/usdt/orders/t-aquila%2Frest-test")

    def test_signed_trading_client_requests_decimal_size_payloads(self):
        seen = {}
        original_urlopen = orders.urllib.request.urlopen

        def fake_urlopen(request, timeout):
            del timeout
            seen["size_decimal"] = request.get_header("X-gate-size-decimal")
            return FakeHttpResponse()

        orders.urllib.request.urlopen = fake_urlopen
        try:
            client = orders.SignedGateTradingClient(
                api_key="key",
                api_secret="secret",
                base_url="https://api.example.test/api/v4",
            )
            client.request_json(
                orders.ApiRequest(
                    method="POST",
                    endpoint_path="/futures/usdt/orders",
                    body='{"contract":"RAVE_USDT","size":0.2}',
                )
            )
        finally:
            orders.urllib.request.urlopen = original_urlopen

        self.assertEqual(seen["size_decimal"], "1")

    def test_json_result_is_serializable(self):
        result = orders.place_order(
            requester=lambda api_request: {"id": 12345},
            settle="usdt",
            payload=orders.build_order_payload(
                contract="BTC_USDT",
                side="buy",
                size=1,
                price="1",
                tif="gtc",
                text="t-aquila-rest-test",
            ),
            execute=False,
            keep_open=False,
        )

        json.dumps(result)


if __name__ == "__main__":
    unittest.main()
