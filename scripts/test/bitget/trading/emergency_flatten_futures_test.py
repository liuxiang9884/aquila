#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import unittest
from decimal import Decimal
from pathlib import Path


SCRIPTS_ROOT = Path(__file__).resolve().parents[3]
SCRIPT_DIR = SCRIPTS_ROOT / "bitget" / "trading"
ACCOUNT_SCRIPT_DIR = SCRIPTS_ROOT / "bitget" / "account"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
if str(ACCOUNT_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(ACCOUNT_SCRIPT_DIR))

import emergency_flatten_futures as flatten


def allowlist_config(**overrides):
    values = {
        "category": "USDT-FUTURES",
        "scope": "allowlist",
        "symbols": ["BTC_USDT"],
        "confirm_dedicated_account": False,
        "dry_run": True,
        "poll_timeout_sec": 3.0,
        "poll_interval_sec": 0.5,
        "max_position_count": 8,
    }
    values.update(overrides)
    return flatten.FlattenConfig(**values)


def dedicated_config(**overrides):
    values = {
        "category": "USDT-FUTURES",
        "scope": "dedicated-account",
        "symbols": [],
        "confirm_dedicated_account": True,
        "dry_run": True,
        "poll_timeout_sec": 3.0,
        "poll_interval_sec": 0.5,
        "max_position_count": 8,
    }
    values.update(overrides)
    return flatten.FlattenConfig(**values)


def position_data(
    symbol="BTCUSDT",
    pos_side="long",
    total="0.001",
    available="0.001",
    frozen="0",
    margin_mode="crossed",
):
    return {
        "symbol": symbol,
        "posSide": pos_side,
        "total": total,
        "available": available,
        "frozen": frozen,
        "marginMode": margin_mode,
    }


def order_data(symbol="BTCUSDT", order_id="11", client_oid="a-11"):
    return {
        "symbol": symbol,
        "orderId": order_id,
        "clientOid": client_oid,
    }


class RecordingRequester:
    def __init__(self, open_orders, positions):
        self.open_orders = open_orders
        self.positions = positions
        self.requests = []

    def __call__(self, request):
        self.requests.append(request)
        if request.method != "GET":
            raise AssertionError(f"unexpected mutating request: {request}")
        if request.endpoint_path.endswith("unfilled-orders"):
            return {"list": list(self.open_orders)}
        if request.endpoint_path.endswith("current-position"):
            return {"list": list(self.positions)}
        raise AssertionError(f"unexpected request: {request}")


class EmergencyFlattenFuturesTest(unittest.TestCase):
    def test_position_is_flat_only_when_total_available_and_frozen_are_zero(self):
        position = flatten.PositionSnapshot(
            symbol="BTCUSDT",
            pos_side="long",
            total=Decimal("0"),
            available=Decimal("0"),
            frozen=Decimal("0.001"),
            margin_mode="crossed",
        )

        self.assertFalse(position.flat())

    def test_final_state_requires_no_open_orders(self):
        position = flatten.PositionSnapshot(
            symbol="BTCUSDT",
            pos_side="long",
            total=Decimal("0"),
            available=Decimal("0"),
            frozen=Decimal("0"),
            margin_mode="crossed",
        )
        order = flatten.OpenOrder(
            symbol="BTCUSDT", order_id="11", client_oid="a-11"
        )

        self.assertFalse(flatten.final_state_is_flat([position], [order]))
        self.assertTrue(flatten.final_state_is_flat([position], []))

    def test_allowlist_scope_requires_symbol(self):
        with self.assertRaisesRegex(flatten.ScopeRefused, "allowlist"):
            flatten.validate_config(allowlist_config(symbols=[]))

    def test_dedicated_account_requires_explicit_confirmation(self):
        with self.assertRaisesRegex(
            flatten.ScopeRefused, "confirm-dedicated-account"
        ):
            flatten.validate_config(
                dedicated_config(confirm_dedicated_account=False)
            )

    def test_rejects_invalid_poll_config(self):
        for config, message in (
            (allowlist_config(poll_timeout_sec=-1), "poll-timeout-sec"),
            (allowlist_config(poll_interval_sec=0), "poll-interval-sec"),
            (allowlist_config(max_position_count=0), "max-position-count"),
        ):
            with self.subTest(message=message):
                with self.assertRaisesRegex(flatten.ScopeRefused, message):
                    flatten.validate_config(config)

    def test_parse_open_orders_rejects_symbol_outside_allowlist(self):
        with self.assertRaisesRegex(flatten.RestFailure, "outside allowlist"):
            flatten.parse_open_orders(
                {"list": [order_data(symbol="ETHUSDT")]}, {"BTCUSDT"}
            )

    def test_parse_open_orders_requires_order_identity(self):
        raw = order_data()
        raw["orderId"] = ""
        raw["clientOid"] = ""

        with self.assertRaisesRegex(flatten.RestFailure, "missing order identity"):
            flatten.parse_open_orders({"list": [raw]}, {"BTCUSDT"})

    def test_parse_positions_uses_decimal_fields(self):
        positions = flatten.parse_positions(
            {"list": [position_data()]}, {"BTCUSDT"}
        )

        self.assertEqual(positions[0].total, Decimal("0.001"))
        self.assertEqual(positions[0].available, Decimal("0.001"))
        self.assertEqual(positions[0].frozen, Decimal("0"))

    def test_parse_positions_rejects_non_finite_quantity(self):
        with self.assertRaisesRegex(flatten.RestFailure, "finite decimal"):
            flatten.parse_positions(
                {"list": [position_data(total="NaN")]}, {"BTCUSDT"}
            )

    def test_parse_positions_rejects_invalid_side_and_margin_mode(self):
        for field, value, message in (
            ("posSide", "net", "posSide"),
            ("marginMode", "portfolio", "marginMode"),
        ):
            raw = position_data()
            raw[field] = value
            with self.subTest(field=field):
                with self.assertRaisesRegex(flatten.RestFailure, message):
                    flatten.parse_positions({"list": [raw]}, {"BTCUSDT"})

    def test_allowlist_dry_run_is_read_only_and_reports_plan(self):
        requester = RecordingRequester(
            open_orders=[order_data()], positions=[position_data()]
        )

        exit_code, summary = flatten.run_emergency_flatten(
            config=allowlist_config(), requester=requester
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertTrue(summary["ok"])
        self.assertEqual(summary["result"], "dry_run")
        self.assertEqual(summary["scope"]["symbols"], ["BTCUSDT"])
        self.assertEqual(summary["plan"]["open_orders_to_cancel"][0]["order_id"], "11")
        self.assertEqual(summary["plan"]["positions_to_close"][0]["side"], "sell")
        self.assertTrue(all(request.method == "GET" for request in requester.requests))

    def test_dedicated_dry_run_discovers_symbols(self):
        requester = RecordingRequester(
            open_orders=[order_data(symbol="ETHUSDT")],
            positions=[position_data(symbol="BTCUSDT")],
        )

        exit_code, summary = flatten.run_emergency_flatten(
            config=dedicated_config(), requester=requester
        )

        self.assertEqual(exit_code, flatten.EXIT_OK)
        self.assertEqual(summary["scope"]["symbols"], ["BTCUSDT", "ETHUSDT"])


if __name__ == "__main__":
    unittest.main()
