#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import unittest
from decimal import Decimal
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "gate"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import run_futures_order_smoke as smoke


class FakeClock:
    def __init__(self):
        self.now = 1000.0
        self.sleeps = []

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class SmokeRunnerTest(unittest.TestCase):
    def test_limit_price_uses_buy_margin_and_tick_round_up(self):
        price = smoke.limit_price_from_ticker(
            last_price=Decimal("81000.01"),
            tick=Decimal("0.1"),
            side="buy",
            margin_bps=Decimal("5"),
        )

        self.assertEqual(price, "81040.6")

    def test_refuses_to_place_when_position_limit_would_be_exceeded(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 2}
            if api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.endpoint_path.endswith("/contracts/BTC_USDT"):
                return {"order_price_round": "0.1"}
            if api_request.endpoint_path.endswith("/tickers"):
                return [{"contract": "BTC_USDT", "last": "81000"}]
            raise AssertionError(f"unexpected request: {api_request}")

        runner = smoke.SmokeRunner(
            requester=fake_request,
            settle="usdt",
            contract="BTC_USDT",
            clock=FakeClock(),
        )

        with self.assertRaisesRegex(RuntimeError, "max open size"):
            runner.run_once(
                iteration=1,
                size=1,
                max_open_size=2,
                fill_timeout=60,
                poll_interval=1,
                margin_bps=Decimal("5"),
                execute=True,
            )

        self.assertFalse(any(call.method == "POST" for call in calls))

    def test_unfilled_order_is_cancelled_after_timeout(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 0}
            if api_request.endpoint_path == "/futures/usdt/orders" and api_request.method == "GET":
                return []
            if api_request.endpoint_path.endswith("/contracts/BTC_USDT"):
                return {"order_price_round": "0.1"}
            if api_request.endpoint_path.endswith("/tickers"):
                return [{"contract": "BTC_USDT", "last": "81000"}]
            if api_request.method == "POST":
                return {"id": 12345, "status": "open", "left": 1}
            if api_request.endpoint_path.endswith("/orders/12345") and api_request.method == "GET":
                return {"id": 12345, "status": "open", "left": 1}
            if api_request.endpoint_path.endswith("/orders/12345") and api_request.method == "DELETE":
                return {"id": 12345, "status": "finished", "finish_as": "cancelled", "left": 1}
            raise AssertionError(f"unexpected request: {api_request}")

        runner = smoke.SmokeRunner(
            requester=fake_request,
            settle="usdt",
            contract="BTC_USDT",
            clock=FakeClock(),
        )

        result = runner.run_once(
            iteration=1,
            size=1,
            max_open_size=2,
            fill_timeout=2,
            poll_interval=1,
            margin_bps=Decimal("5"),
            execute=True,
        )

        self.assertEqual(result["outcome"], "cancelled")
        self.assertTrue(any(call.method == "DELETE" for call in calls))
        self.assertFalse(
            any(call.method == "POST" and '"reduce_only":true' in call.body for call in calls)
        )

    def test_filled_order_is_closed_with_reduce_only_market_order(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 0}
            if api_request.endpoint_path == "/futures/usdt/orders" and api_request.method == "GET":
                return []
            if api_request.endpoint_path.endswith("/contracts/BTC_USDT"):
                return {"order_price_round": "0.1"}
            if api_request.endpoint_path.endswith("/tickers"):
                return [{"contract": "BTC_USDT", "last": "81000"}]
            if api_request.method == "POST" and '"reduce_only":true' not in api_request.body:
                return {"id": 12345, "status": "finished", "finish_as": "filled", "left": 0}
            if api_request.endpoint_path.endswith("/orders/12345") and api_request.method == "GET":
                return {"id": 12345, "status": "finished", "finish_as": "filled", "left": 0}
            if api_request.method == "POST" and '"reduce_only":true' in api_request.body:
                return {"id": 12346, "status": "finished", "finish_as": "filled", "left": 0}
            raise AssertionError(f"unexpected request: {api_request}")

        runner = smoke.SmokeRunner(
            requester=fake_request,
            settle="usdt",
            contract="BTC_USDT",
            clock=FakeClock(),
        )

        result = runner.run_once(
            iteration=1,
            size=1,
            max_open_size=2,
            fill_timeout=60,
            poll_interval=1,
            margin_bps=Decimal("5"),
            execute=True,
        )

        self.assertEqual(result["outcome"], "filled_and_closed")
        close_posts = [
            call for call in calls if call.method == "POST" and '"reduce_only":true' in call.body
        ]
        self.assertEqual(len(close_posts), 1)
        self.assertIn('"price":"0"', close_posts[0].body)
        self.assertIn('"tif":"ioc"', close_posts[0].body)
        self.assertIn('"size":-1', close_posts[0].body)


if __name__ == "__main__":
    unittest.main()
