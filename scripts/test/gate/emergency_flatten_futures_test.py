#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import unittest
from decimal import Decimal
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "gate"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

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


def dedicated_config(**overrides):
    values = {
        "settle": "usdt",
        "scope": "dedicated-account",
        "contracts": [],
        "confirm_dedicated_account": True,
        "dry_run": False,
        "poll_timeout_sec": 0.0,
        "poll_interval_sec": 1.0,
        "max_position_count": 8,
    }
    values.update(overrides)
    return flatten.FlattenConfig(**values)


def position_payload(contract="BTC_USDT", size=0, pending_orders=0):
    payload = {"contract": contract, "size": size, "pending_orders": pending_orders}
    if Decimal(str(size)) == 0:
        payload["value"] = "0"
        payload["margin"] = "0"
    return payload


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

    def test_poll_time_parameters_reject_nan_and_inf(self):
        invalid_configs = [
            allowlist_config(poll_timeout_sec=float("nan")),
            allowlist_config(poll_timeout_sec=float("inf")),
            allowlist_config(poll_timeout_sec=float("-inf")),
            allowlist_config(poll_interval_sec=float("nan")),
            allowlist_config(poll_interval_sec=float("inf")),
            allowlist_config(poll_interval_sec=float("-inf")),
        ]

        def fail_request(api_request):
            raise AssertionError(f"unexpected request: {api_request}")

        for config in invalid_configs:
            with self.subTest(config=config):
                exit_code, summary = flatten.run_emergency_flatten(
                    config=config,
                    requester=fail_request,
                    clock=FakeClock(),
                )

                self.assertEqual(exit_code, flatten.EXIT_SCOPE_REFUSED)
                self.assertEqual(summary["result"], "scope_refused")

    def test_poll_sleep_is_clipped_to_remaining_timeout(self):
        clock = FakeClock()
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 1, "pending_orders": 0}
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            raise AssertionError(f"unexpected request: {api_request}")

        verified, polls, final_positions, final_open_orders = flatten.poll_until_flat(
            requester=fake_request,
            settle="usdt",
            contracts=["BTC_USDT"],
            timeout_sec=2.5,
            interval_sec=10.0,
            clock=clock,
        )

        self.assertFalse(verified)
        self.assertEqual(polls, 2)
        self.assertEqual(clock.sleeps, [2.5])
        self.assertEqual(final_positions[0].size, 1)
        self.assertEqual(final_open_orders, [])

    def test_dry_run_does_not_send_mutating_requests(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return [{"id": "12345", "contract": "BTC_USDT"}]
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 2, "pending_orders": 0}
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
                return position_payload(size=2 if position_query_count == 1 else 0)
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
                return position_payload(size=-3 if position_query_count == 1 else 0)
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

    def test_decimal_position_size_closes_with_decimal_order_size(self):
        calls = []
        position_query_count = 0

        def fake_request(api_request):
            nonlocal position_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/RAVE_USDT"):
                position_query_count += 1
                return {
                    "contract": "RAVE_USDT",
                    "size": "0.2" if position_query_count == 1 else "0",
                    "pending_orders": 0,
                    "value": "0.11248" if position_query_count == 1 else "0",
                    "margin": "0.022496" if position_query_count == 1 else "0",
                }
            if api_request.method == "POST":
                return {"id": "decimal-close", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(contracts=["RAVE_USDT"]),
            requester=fake_request,
            clock=FakeClock(),
        )

        close_posts = [call for call in calls if call.method == "POST"]
        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(len(close_posts), 1)
        self.assertIn('"size":-0.2', close_posts[0].body)
        self.assertEqual(summary["close_orders_submitted"][0]["signed_size"], "-0.2")
        self.assertEqual(summary["close_orders_submitted"][0]["size"], "0.2")

    def test_zero_integer_size_with_residual_value_is_not_flat(self):
        self.assertFalse(
            flatten.final_state_is_flat(
                [
                    flatten.PositionSnapshot(
                        contract="RAVE_USDT",
                        size=Decimal("0"),
                        pending_orders=0,
                        value=Decimal("0.11248"),
                        margin=Decimal("0.022496"),
                    )
                ],
                [],
            )
        )

    def test_idempotent_flat_account_sends_no_mutating_requests(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return position_payload(size=0)
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

    def test_residual_value_without_size_returns_not_flat_without_close(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/RAVE_USDT"):
                return {
                    "contract": "RAVE_USDT",
                    "size": 0,
                    "pending_orders": 0,
                    "value": "0.11248",
                    "margin": "0.022496",
                }
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(contracts=["RAVE_USDT"], poll_timeout_sec=0.0),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_NOT_FLAT)
        self.assertEqual(summary["result"], "not_flat")
        self.assertFalse(any(call.method == "POST" for call in calls))
        self.assertEqual(summary["final_positions"][0]["size"], 0)
        self.assertEqual(summary["final_positions"][0]["value"], "0.11248")
        self.assertEqual(summary["final_positions"][0]["margin"], "0.022496")

    def test_invalid_residual_value_returns_rest_failed_exit_code(self):
        def fake_request(api_request):
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/RAVE_USDT"):
                return {
                    "contract": "RAVE_USDT",
                    "size": 0,
                    "pending_orders": 0,
                    "value": "bad",
                }
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(contracts=["RAVE_USDT"], poll_timeout_sec=0.0),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("RAVE_USDT.value", summary["errors"][0])

    def test_missing_residual_fields_for_zero_size_returns_rest_failed_exit_code(self):
        def fake_request(api_request):
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/RAVE_USDT"):
                return {
                    "contract": "RAVE_USDT",
                    "size": 0,
                    "pending_orders": 0,
                }
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(contracts=["RAVE_USDT"], poll_timeout_sec=0.0),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("RAVE_USDT.value", summary["errors"][0])

    def test_max_position_count_guard_refuses_dedicated_account(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                return [
                    {"contract": "BTC_USDT", "size": 1, "pending_orders": 0},
                    {"contract": "ETH_USDT", "size": -1, "pending_orders": 0},
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

    def test_dedicated_account_queries_and_cancels_all_open_orders_without_positions(self):
        calls = []
        positions_query_count = 0
        order_query_count = 0

        def fake_request(api_request):
            nonlocal positions_query_count, order_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                positions_query_count += 1
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                order_query_count += 1
                self.assertEqual(api_request.query_string, "status=open")
                return [{"id": "999", "contract": "DOGE_USDT"}] if order_query_count == 1 else []
            if api_request.method == "DELETE":
                return {"id": "999", "status": "finished", "finish_as": "cancelled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=dedicated_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["result"], "verified_flat")
        self.assertEqual(summary["scope"]["contracts"], ["DOGE_USDT"])
        self.assertEqual(summary["scope"]["limitations"], [])
        self.assertEqual(summary["orders_cancelled"][0]["order_id"], "999")
        self.assertGreaterEqual(positions_query_count, 2)
        self.assertTrue(any(call.method == "DELETE" for call in calls))

    def test_dedicated_account_dry_run_uses_all_positions_and_all_open_orders(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                return [
                    {"contract": "BTC_USDT", "size": -2, "pending_orders": 0},
                    position_payload(contract="ETH_USDT", size=0),
                ]
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                self.assertEqual(api_request.query_string, "status=open")
                return [{"id": "123", "contract": "SOL_USDT"}]
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=dedicated_config(dry_run=True),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["result"], "dry_run")
        self.assertEqual(summary["scope"]["contracts"], ["BTC_USDT", "SOL_USDT"])
        self.assertEqual(summary["plan"]["open_orders_to_cancel"][0]["order_id"], "123")
        self.assertEqual(summary["plan"]["positions_to_close"][0]["contract"], "BTC_USDT")
        self.assertFalse(any(call.method in {"DELETE", "POST"} for call in calls))

    def test_dedicated_account_dry_run_counts_residual_position_separately(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                return [
                    {
                        "contract": "RAVE_USDT",
                        "size": 0,
                        "pending_orders": 0,
                        "value": "0.11248",
                        "margin": "0.022496",
                    }
                ]
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=dedicated_config(dry_run=True),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["scope"]["contracts"], ["RAVE_USDT"])
        self.assertEqual(summary["scope"]["discovered_non_zero_position_count"], 0)
        self.assertEqual(summary["scope"]["discovered_residual_position_count"], 1)
        self.assertEqual(summary["plan"]["positions_to_close"], [])
        self.assertFalse(any(call.method in {"DELETE", "POST"} for call in calls))

    def test_dedicated_account_cancels_all_orders_before_closing_all_positions(self):
        calls = []
        positions_query_count = 0
        order_query_count = 0

        def fake_request(api_request):
            nonlocal positions_query_count, order_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                positions_query_count += 1
                size = 2 if positions_query_count < 3 else 0
                return [
                    position_payload(contract="BTC_USDT", size=size),
                    position_payload(contract="ETH_USDT", size=0),
                ]
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                order_query_count += 1
                self.assertEqual(api_request.query_string, "status=open")
                return [{"id": "abc", "contract": "SOL_USDT"}] if order_query_count == 1 else []
            if api_request.method == "DELETE":
                return {"id": "abc", "status": "finished", "finish_as": "cancelled"}
            if api_request.method == "POST":
                return {"id": "close-1", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=dedicated_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        methods = [call.method for call in calls]
        close_posts = [call for call in calls if call.method == "POST"]
        close_payload = json.loads(close_posts[0].body)
        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["scope"]["contracts"], ["BTC_USDT", "SOL_USDT"])
        self.assertLess(methods.index("DELETE"), methods.index("POST"))
        self.assertEqual(close_payload["contract"], "BTC_USDT")
        self.assertEqual(close_payload["size"], -2)
        self.assertTrue(close_payload["reduce_only"])

    def test_requester_failure_returns_rest_failed_exit_code(self):
        def failing_request(api_request):
            raise RuntimeError("HTTP 500: unavailable")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=failing_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("HTTP 500", summary["errors"][0])

    def test_allowlist_mismatched_open_order_contract_returns_rest_failed_without_mutation(self):
        calls = []
        order_query_count = 0

        def fake_request(api_request):
            nonlocal order_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                order_query_count += 1
                return [{"id": "777", "contract": "ETH_USDT"}] if order_query_count == 1 else []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": 0, "pending_orders": 0}
            if api_request.method == "DELETE":
                return {"id": "777", "status": "finished", "finish_as": "cancelled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("contract mismatch", summary["errors"][0])
        self.assertFalse(any(call.method in {"DELETE", "POST"} for call in calls))

    def test_allowlist_mismatched_position_contract_returns_rest_failed_without_close(self):
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
                    "contract": "ETH_USDT",
                    "size": 1 if position_query_count == 1 else 0,
                    "pending_orders": 0,
                }
            if api_request.method == "POST":
                return {"id": "bad-close", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("contract mismatch", summary["errors"][0])
        self.assertFalse(any(call.method == "POST" for call in calls))

    def test_allowlist_position_list_with_extra_contract_returns_rest_failed_without_close(self):
        calls = []
        position_query_count = 0

        def fake_request(api_request):
            nonlocal position_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                position_query_count += 1
                if position_query_count == 1:
                    return [
                        {"contract": "BTC_USDT", "size": 1, "pending_orders": 0},
                        {"contract": "ETH_USDT", "size": 0, "pending_orders": 0},
                    ]
                return {"contract": "BTC_USDT", "size": 0, "pending_orders": 0}
            if api_request.method == "POST":
                return {"id": "bad-close", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("contract mismatch", summary["errors"][0])
        self.assertFalse(any(call.method in {"DELETE", "POST"} for call in calls))

    def test_malformed_open_orders_response_returns_rest_failed_exit_code(self):
        def fake_request(api_request):
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return {"id": "not-a-list"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("expected list", summary["errors"][0])

    def test_malformed_positions_response_returns_rest_failed_exit_code(self):
        def fake_request(api_request):
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/BTC_USDT"):
                return {"contract": "BTC_USDT", "size": "bad", "pending_orders": 0}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("expected finite decimal", summary["errors"][0])

    def test_missing_pending_orders_in_final_verification_returns_rest_failed(self):
        calls = []
        position_query_count = 0

        def fake_request(api_request):
            nonlocal position_query_count
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions"):
                position_query_count += 1
                if position_query_count == 1:
                    return [{"contract": "BTC_USDT", "size": 1, "pending_orders": 0}]
                return [{"contract": "BTC_USDT", "size": 0, "value": "0", "margin": "0"}]
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            if api_request.method == "POST":
                return {"id": "close-1", "status": "finished", "finish_as": "filled"}
            raise AssertionError(f"unexpected request: {api_request}")

        exit_code, summary = flatten.run_emergency_flatten(
            config=dedicated_config(),
            requester=fake_request,
            clock=FakeClock(),
        )

        self.assertEqual(exit_code, flatten.EXIT_REST_FAILED)
        self.assertEqual(summary["result"], "rest_failed")
        self.assertIn("pending_orders", summary["errors"][0])


if __name__ == "__main__":
    unittest.main()
