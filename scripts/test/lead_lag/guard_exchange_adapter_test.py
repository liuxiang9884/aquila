#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import dedent


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
SCRIPTS_ROOT = Path(__file__).resolve().parents[2]
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
if str(SCRIPTS_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_ROOT))

import guard_exchange_adapter as adapters
import run_live_with_guard as guard


def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dedent(text).strip() + "\n", encoding="utf-8")


def write_bitget_config_graph(base, route1_passphrase="BITGET_TEST_PASSPHRASE"):
    session0 = base / "session0.toml"
    session1 = base / "session1.toml"
    gateway = base / "gateway.toml"
    strategy = base / "strategy.toml"
    write_text(
        session0,
        """
        [order_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "BITGET_TEST_PASSPHRASE"
        """,
    )
    write_text(
        session1,
        f"""
        [order_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "{route1_passphrase}"
        """,
    )
    write_text(
        gateway,
        f"""
        [order_gateway]
        route_count = 2

        [[order_gateway.routes]]
        name = "route0"
        order_session_config = "{session0}"

        [[order_gateway.routes]]
        name = "route1"
        order_session_config = "{session1}"
        """,
    )
    write_text(
        strategy,
        f"""
        [strategy.order_gateway]
        config = "{gateway}"
        """,
    )
    return strategy, gateway


def fake_callable(*args, **kwargs):
    return args, kwargs


class GuardExchangeAdapterTest(unittest.TestCase):
    def test_gate_adapter_preserves_supplied_operations(self):
        adapter = adapters.GuardExchangeAdapter(
            name="gate",
            credential_resolver=fake_callable,
            requester_factory=fake_callable,
            state_reader=fake_callable,
            flatten_config_builder=fake_callable,
            flatten_runner=fake_callable,
        )

        resolved = adapters.get_guard_exchange_adapter(
            "gate", gate_adapter=adapter
        )

        self.assertIs(resolved, adapter)
        self.assertEqual(resolved.name, "gate")

    def test_unknown_exchange_is_rejected(self):
        adapter = adapters.GuardExchangeAdapter(
            name="gate",
            credential_resolver=fake_callable,
            requester_factory=fake_callable,
            state_reader=fake_callable,
            flatten_config_builder=fake_callable,
            flatten_runner=fake_callable,
        )

        with self.assertRaisesRegex(ValueError, "unsupported guard exchange"):
            adapters.get_guard_exchange_adapter("unknown", gate_adapter=adapter)

    def test_credential_names_allow_gate_without_passphrase(self):
        credentials = adapters.GuardCredentialEnvNames(
            api_key_env="GATE_KEY",
            api_secret_env="GATE_SECRET",
            api_passphrase_env=None,
            source="order_gateway_config",
        )

        self.assertIsNone(credentials.api_passphrase_env)

    def test_gate_and_bitget_trading_modules_coexist(self):
        from bitget.trading import emergency_flatten_futures as bitget_flatten
        from bitget.trading import place_futures_order as bitget_orders

        self.assertNotEqual(Path(bitget_flatten.__file__), Path(guard.flatten.__file__))
        self.assertEqual(
            bitget_orders.build_place_order_request(
                category="USDT-FUTURES",
                symbol="BTCUSDT",
                qty=__import__("decimal").Decimal("0.001"),
                side="sell",
                margin_mode="crossed",
                client_oid="a-flat-1-0",
                reduce_only=True,
            ).endpoint_path,
            "/api/v3/trade/place-order",
        )

    def test_bitget_gateway_credentials_include_matching_passphrase(self):
        with TemporaryDirectory() as tmp:
            strategy, _ = write_bitget_config_graph(Path(tmp))

            credentials = guard.resolve_bitget_guard_credential_env_names(
                explicit_api_key=None,
                explicit_api_secret=None,
                explicit_api_passphrase=None,
                strategy_command=[
                    "lead_lag_strategy",
                    "--config",
                    str(strategy),
                    "--execute",
                ],
            )

        self.assertEqual(credentials.api_key_env, "BITGET_TEST_KEY")
        self.assertEqual(credentials.api_secret_env, "BITGET_TEST_SECRET")
        self.assertEqual(credentials.api_passphrase_env, "BITGET_TEST_PASSPHRASE")

    def test_bitget_routes_reject_mixed_passphrase(self):
        with TemporaryDirectory() as tmp:
            _, gateway = write_bitget_config_graph(
                Path(tmp), route1_passphrase="OTHER_PASSPHRASE"
            )

            with self.assertRaisesRegex(ValueError, "route 1 credentials"):
                guard.bitget_order_gateway_credential_env_names(gateway)

    def test_bitget_adapter_queries_flat_state(self):
        def fake_request(request):
            if request.endpoint_path.endswith("unfilled-orders"):
                return {"list": []}
            if request.endpoint_path.endswith("current-position"):
                return {"list": []}
            raise AssertionError(f"unexpected request: {request}")

        state = guard.bitget_query_guard_state(
            fake_request, "usdt", ["BTC_USDT"]
        )

        self.assertTrue(state.flat())
        self.assertEqual(state.positions, [])
        self.assertEqual(state.open_orders, [])

    def test_bitget_adapter_builds_allowlist_flatten_config(self):
        config = guard.GuardConfig(
            settle="usdt",
            contracts=["BTC_USDT"],
            strategy_command=["lead_lag_strategy", "--execute"],
            exchange="bitget",
            poll_timeout_sec=5.0,
            poll_interval_sec=0.25,
        )

        flatten_config = guard.bitget_flatten_config_from_guard(config)

        self.assertEqual(flatten_config.category, "USDT-FUTURES")
        self.assertEqual(flatten_config.scope, "allowlist")
        self.assertEqual(flatten_config.symbols, ["BTC_USDT"])
        self.assertFalse(flatten_config.dry_run)

    def test_runtime_adapter_registers_bitget(self):
        adapter = guard.get_runtime_guard_adapter("bitget")

        self.assertEqual(adapter.name, "bitget")
        self.assertIsNotNone(adapter.requester_factory)


if __name__ == "__main__":
    unittest.main()
