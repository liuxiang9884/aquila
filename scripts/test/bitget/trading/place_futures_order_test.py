#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import unittest
from decimal import Decimal
from pathlib import Path
from unittest.mock import patch


SCRIPTS_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_DIR = SCRIPTS_ROOT / "bitget" / "trading"
ACCOUNT_SCRIPT_DIR = SCRIPTS_ROOT / "bitget" / "account"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
if str(ACCOUNT_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(ACCOUNT_SCRIPT_DIR))

import place_futures_order as orders


class FakeHttpResponse:
    def __init__(self, body=b'\x7b"code":"00000","msg":"success","data":\x7b\x7d\x7d'):
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        return False

    def read(self):
        return self.body


class PlaceFuturesOrderTest(unittest.TestCase):
    def test_build_reduce_only_market_close_request(self):
        request = orders.build_place_order_request(
            category="USDT-FUTURES",
            symbol="BTC_USDT",
            qty=Decimal("0.001"),
            side="sell",
            margin_mode="crossed",
            client_oid="a-flat-1700000000000-0",
            reduce_only=True,
        )

        self.assertEqual(request.method, "POST")
        self.assertEqual(request.endpoint_path, "/api/v3/trade/place-order")
        self.assertEqual(
            json.loads(request.body),
            {
                "category": "USDT-FUTURES",
                "symbol": "BTCUSDT",
                "qty": "0.001",
                "side": "sell",
                "orderType": "market",
                "reduceOnly": "yes",
                "marginMode": "crossed",
                "clientOid": "a-flat-1700000000000-0",
            },
        )

    def test_build_cancel_request_prefers_exchange_order_id(self):
        request = orders.build_cancel_order_request(
            category="USDT-FUTURES",
            symbol="BTCUSDT",
            order_id="12345",
            client_oid="a-7",
        )

        self.assertEqual(request.endpoint_path, "/api/v3/trade/cancel-order")
        self.assertEqual(
            json.loads(request.body),
            {
                "orderId": "12345",
                "category": "USDT-FUTURES",
                "symbol": "BTCUSDT",
            },
        )

    def test_build_cancel_request_uses_client_oid_without_exchange_id(self):
        request = orders.build_cancel_order_request(
            category="usdt-futures",
            symbol="btc_usdt",
            order_id=None,
            client_oid="a-7",
        )

        self.assertEqual(json.loads(request.body)["clientOid"], "a-7")

    def test_build_cancel_request_requires_an_identity(self):
        with self.assertRaisesRegex(ValueError, "order_id or client_oid"):
            orders.build_cancel_order_request(
                category="USDT-FUTURES",
                symbol="BTCUSDT",
                order_id=None,
                client_oid=None,
            )

    def test_rejects_non_positive_quantity(self):
        for qty in (Decimal("0"), Decimal("-0.001")):
            with self.subTest(qty=qty):
                with self.assertRaisesRegex(ValueError, "qty must be positive"):
                    orders.build_place_order_request(
                        category="USDT-FUTURES",
                        symbol="BTCUSDT",
                        qty=qty,
                        side="buy",
                        margin_mode="crossed",
                        client_oid="a-flat-1-0",
                        reduce_only=True,
                    )

    def test_rejects_invalid_side_margin_mode_and_client_oid(self):
        common = {
            "category": "USDT-FUTURES",
            "symbol": "BTCUSDT",
            "qty": Decimal("0.001"),
            "side": "buy",
            "margin_mode": "crossed",
            "client_oid": "a-flat-1-0",
            "reduce_only": True,
        }
        for field, value, message in (
            ("side", "hold", "side must be buy or sell"),
            ("margin_mode", "portfolio", "margin_mode"),
            ("client_oid", "", "client_oid"),
            ("client_oid", "x" * 33, "client_oid"),
        ):
            with self.subTest(field=field, value=value):
                fields = dict(common)
                fields[field] = value
                with self.assertRaisesRegex(ValueError, message):
                    orders.build_place_order_request(**fields)

    def test_client_oid_accepts_bitget_protocol_characters(self):
        self.assertEqual(
            orders.validate_client_oid("a.test/ABC:xyz_9-0"),
            "a.test/ABC:xyz_9-0",
        )

    def test_client_oid_rejects_characters_outside_bitget_protocol(self):
        for client_oid in ("a oid", "a#oid", "a+oid", "a-é"):
            with self.subTest(client_oid=client_oid):
                with self.assertRaisesRegex(ValueError, "unsupported characters"):
                    orders.validate_client_oid(client_oid)

    def test_validate_response_returns_data(self):
        data = orders.validate_uta_response(
            {"code": "00000", "msg": "success", "data": {"orderId": "9"}}
        )

        self.assertEqual(data, {"orderId": "9"})

    def test_validate_response_rejects_non_success_code(self):
        with self.assertRaisesRegex(RuntimeError, "Bitget REST code=40010"):
            orders.validate_uta_response(
                {"code": "40010", "msg": "request timed out"}
            )

    def test_validate_response_rejects_missing_data(self):
        with self.assertRaisesRegex(RuntimeError, "missing data"):
            orders.validate_uta_response({"code": "00000", "msg": "success"})

    @patch("urllib.request.urlopen")
    def test_signed_client_posts_body_and_passphrase_header(self, urlopen):
        urlopen.return_value = FakeHttpResponse(
            b'{"code":"00000","msg":"success","data":{"orderId":"9"}}'
        )
        client = orders.SignedBitgetTradingClient(
            api_key="key",
            api_secret="secret",
            api_passphrase="passphrase",
            base_url="https://example.test",
            timeout=3.0,
        )
        api_request = orders.build_place_order_request(
            category="USDT-FUTURES",
            symbol="BTCUSDT",
            qty=Decimal("0.001"),
            side="sell",
            margin_mode="crossed",
            client_oid="a-flat-1700000000000-0",
            reduce_only=True,
        )

        result = client.request_json(api_request)

        self.assertEqual(result, {"orderId": "9"})
        request = urlopen.call_args.args[0]
        self.assertEqual(request.method, "POST")
        self.assertEqual(request.data.decode("utf-8"), api_request.body)
        self.assertEqual(request.get_header("Access-passphrase"), "passphrase")
        self.assertEqual(urlopen.call_args.kwargs["timeout"], 3.0)

    def test_dry_run_does_not_call_requester(self):
        def fail_request(api_request):
            raise AssertionError(f"unexpected request: {api_request}")

        result = orders.place_order(
            requester=fail_request,
            request=orders.build_place_order_request(
                category="USDT-FUTURES",
                symbol="BTCUSDT",
                qty=Decimal("0.001"),
                side="sell",
                margin_mode="crossed",
                client_oid="a-flat-1700000000000-0",
                reduce_only=True,
            ),
            execute=False,
        )

        self.assertFalse(result["executed"])
        self.assertNotIn("response", result)


if __name__ == "__main__":
    unittest.main()
