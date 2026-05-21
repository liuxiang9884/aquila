#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import unittest

import emergency_flatten_futures as flatten


class FakeClock:
    def __init__(self):
        self.now = 1000.0
        self.sleeps = []

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


def allowlist_config(**overrides):
    values = {
        "settle": "usdt",
        "scope": "allowlist",
        "contracts": ["BTC_USDT"],
        "confirm_dedicated_account": False,
        "dry_run": False,
        "poll_timeout_sec": 0.0,
        "poll_interval_sec": 1.0,
        "max_position_count": 8,
    }
    values.update(overrides)
    return flatten.FlattenConfig(**values)


class EmergencyFlattenFuturesTest(unittest.TestCase):
    def test_allowlist_scope_requires_contract(self):
        def fail_request(api_request):
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(contracts=[]),
            requester=fail_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_SCOPE_REFUSED)
        self.assertEqual(summary["result"], "scope_refused")
        self.assertIn("allowlist", summary["errors"][0])

    def test_dedicated_account_requires_confirmation(self):
        def fail_request(api_request):
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(scope="dedicated-account", contracts=[]),
            requester=fail_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_SCOPE_REFUSED)
        self.assertEqual(summary["result"], "scope_refused")
        self.assertIn("confirm-dedicated-account", summary["errors"][0])

    def test_dry_run_does_not_send_mutating_requests(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return [{"id": "12345", "contract": "BTC_USDT"}]
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 2}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(dry_run=True),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["result"], "dry_run")
        self.assertEqual([call.method for call in calls], ["GET", "GET"])
        self.assertEqual(summary["plan"]["open_orders_to_cancel"][0]["order_id"], "12345")
        self.assertEqual(summary["plan"]["positions_to_close"][0]["size"], 2)

    def test_cancel_before_close_ordering(self):
        calls = []
        order_query_count = 0
        position_query_count = 0

        def fake_request(api_request):
            nonlocal order_query_count, position_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                order_query_count += 1
                return [{"id": "12345", "contract": "BTC_USDT"}] if order_query_count == 1 else []
            if api_request.method == "DELETE":
                return {"id": "12345", "status": "finished", "finish_as": "cancelled"}
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                position_query_count += 1
                return {
                    "contract": "BTC_USDT",
                    "size": 2 if position_query_count == 1 else 0,
                    "pending_orders": 0,
                }
            if api_request.method == "POST":
                return {"id": "54321", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        methods = [call.method for call in calls]
        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["result"], "verified_flat")
        self.assertLess(methods.index("DELETE"), methods.index("POST"))
        self.assertEqual(summary["orders_cancelled"][0]["order_id"], "12345")
        self.assertEqual(summary["close_orders_submitted"][0]["contract"], "BTC_USDT")

    def test_reduce_only_market_close_payload_direction_and_size(self):
        calls = []
        position_query_count = 0

        def fake_request(api_request):
            nonlocal position_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                position_query_count += 1
                return {
                    "contract": "BTC_USDT",
                    "size": -3 if position_query_count == 1 else 0,
                    "pending_orders": 0,
                }
            if api_request.method == "POST":
                return {"id": "54321", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        close_posts = [call for call in calls if call.method == "POST"]
        payload = json.loads(close_posts[0].body)
        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(payload["contract"], "BTC_USDT")
        self.assertEqual(payload["size"], 3)
        self.assertEqual(payload["price"], "0")
        self.assertEqual(payload["tif"], "ioc")
        self.assertTrue(payload["reduce_only"])
        self.assertEqual(summary["close_orders_submitted"][0]["side"], "buy")
        self.assertEqual(summary["close_orders_submitted"][0]["size"], 3)

    def test_idempotent_flat_account_sends_no_mutating_requests(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 0, "pending_orders": 0}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["result"], "verified_flat")
        self.assertFalse(any(call.method in {"DELETE", "POST"} for call in calls))

    def test_timeout_returns_not_flat_exit_classification(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 2, "pending_orders": 0}
            if api_request.method == "POST":
                return {"id": "54321", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(poll_timeout_sec=0.0),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_NOT_FLAT)
        self.assertEqual(summary["result"], "not_flat")
        self.assertEqual(summary["close_orders_submitted"][0]["contract"], "BTC_USDT")
        self.assertEqual(summary["final_positions"][0]["size"], 2)

    def test_max_position_count_guard_refuses_dedicated_account(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                return [
                    {"contract": "BTC_USDT", "size": 1},
                    {"contract": "ETH_USDT", "size": -1},
                ]
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(
                scope="dedicated-account",
                contracts=[],
                confirm_dedicated_account=True,
                max_position_count=1,
            ),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_SCOPE_REFUSED)
        self.assertEqual(summary["result"], "scope_refused")
        self.assertFalse(any(call.method in {"DELETE", "POST"} for call in calls))
        self.assertIn("max-position-count", summary["errors"][0])

    def test_dedicated_account_reports_open_order_discovery_limitation(self):
        def fake_request(api_request):
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                return []
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(
                scope="dedicated-account",
                contracts=[],
                confirm_dedicated_account=True,
            ),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertIn("all-open-orders-not-discoverable", summary["scope"]["limitations"])


if __name__ == "__main__":
    unittest.main()
