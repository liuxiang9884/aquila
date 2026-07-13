#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import tomllib
import unittest
from decimal import Decimal
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import dedent
from unittest.mock import patch


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import prepare_bitget_live_run as prepare
import run_live_with_guard as guard


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dedent(text).strip() + "\n", encoding="utf-8")


def write_runtime_fixture_graph(
    base: Path,
    route_count: int = 1,
    lag_symbols: tuple[str, ...] = ("BTC_USDT",),
):
    sessions = []
    for route in range(route_count):
        session = base / f"bitget_order_session_{route}.toml"
        write_text(
            session,
            """
            [order_session.credentials]
            api_key_env = "BITGET_TEST_KEY"
            api_secret_env = "BITGET_TEST_SECRET"
            api_passphrase_env = "BITGET_TEST_PASSPHRASE"
            """,
        )
        sessions.append(session)

    gateway = base / "bitget_order_gateway.toml"
    routes = "\n\n".join(
        dedent(
            f"""
            [[order_gateway.routes]]
            name = "route{route}"
            order_session_config = "{session}"
            worker_cpu_id = 16
            """
        ).strip()
        for route, session in enumerate(sessions)
    )
    write_text(
        gateway,
        f"""
        [order_gateway]
        name = "bitget_order_gateway"
        shm_name = "aquila_bitget_order_gateway"
        route_count = {route_count}
        command_queue_capacity = 4096
        event_queue_capacity = 8192
        startup_ready_timeout_s = 30

        {routes}
        """,
    )

    feedback = base / "bitget_order_feedback.toml"
    write_text(
        feedback,
        """
        [order_feedback_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "BITGET_TEST_PASSPHRASE"

        [order_feedback_session.shm]
        shm_name = "aquila_bitget_order_feedback"
        channel_name = "orders"
        """,
    )

    lead_lag = base / "lead_lag.toml"
    pairs = "\n\n".join(
        dedent(
            f"""
            [[lead_lag.pairs]]
            symbol = "{symbol}"
            lead_exchange = "binance"
            lag_exchange = "bitget"
            """
        ).strip()
        for symbol in lag_symbols
    )
    write_text(lead_lag, pairs)

    strategy = base / "bitget_strategy.toml"
    write_text(
        strategy,
        f"""
        [strategy]
        mode = "live"
        config = "{lead_lag}"

        [strategy.order_gateway]
        config = "{gateway}"

        [strategy.feedback]
        enabled = true
        shm_name = "aquila_bitget_order_feedback"
        channel_name = "orders"
        """,
    )
    return strategy, gateway, feedback


class PrepareBitgetLiveRunTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = TemporaryDirectory(dir="/home/liuxiang/tmp")
        self.root = Path(self.temp_dir.name)
        self.run_id = self.root.name
        self.source_dir = self.root / "source"
        self.output_dir = self.root / "configs"

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_prepare_generates_run_specific_gateway_and_feedback_shm(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)

        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )

        self.assertEqual(
            result.gateway_shm,
            f"aquila_bitget_order_gateway_{self.run_id}",
        )
        self.assertEqual(
            result.feedback_shm,
            f"aquila_bitget_order_feedback_{self.run_id}",
        )
        strategy_data = tomllib.loads(
            result.strategy_config.read_text(encoding="utf-8")
        )
        self.assertEqual(
            strategy_data["strategy"]["feedback"]["shm_name"],
            result.feedback_shm,
        )
        self.assertEqual(
            strategy_data["strategy"]["order_gateway"]["config"],
            str(result.gateway_config),
        )
        manifest = json.loads(result.manifest.read_text(encoding="utf-8"))
        self.assertFalse(manifest["external_configs_applied"])

    def test_prepare_rejects_route_count_greater_than_one(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(
            self.source_dir, route_count=2
        )

        with self.assertRaisesRegex(ValueError, "route_count must be 1"):
            prepare.prepare_runtime_configs(
                run_id=self.run_id,
                strategy_source=strategy,
                gateway_source=gateway,
                feedback_source=feedback,
                output_dir=self.output_dir,
            )

    def test_mark_applied_revalidates_configs_then_guard_accepts_manifest(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )

        manifest = prepare.mark_external_configs_applied(result.manifest)
        validated = guard.validate_bitget_run_isolation(
            result.manifest,
            ["lead_lag_strategy", "--config", str(result.strategy_config), "--execute"],
        )

        self.assertTrue(manifest["external_configs_applied"])
        self.assertEqual(validated["run_id"], self.run_id)

    def test_guard_summary_records_validated_runtime_isolation(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        prepare.mark_external_configs_applied(result.manifest)
        flat_state = guard.GuardState(
            positions=[
                guard.PositionSnapshot(
                    contract="BTC_USDT",
                    size=Decimal("0"),
                    pending_orders=0,
                )
            ],
            open_orders=[],
        )
        states = [flat_state, flat_state]
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=guard.resolve_bitget_guard_credential_env_names,
            requester_factory=lambda *args: object(),
            state_reader=lambda requester, settle, contracts: states.pop(0),
            flatten_config_builder=lambda config: {},
            flatten_runner=lambda config, requester, clock: (0, {}),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--runtime-manifest",
                str(result.manifest),
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
                "--config",
                str(result.strategy_config),
                "--execute",
            ]
        )

        with patch.dict(
            "os.environ",
            {
                "BITGET_TEST_KEY": "key-value",
                "BITGET_TEST_SECRET": "secret-value",
                "BITGET_TEST_PASSPHRASE": "passphrase-value",
            },
        ):
            exit_code, summary = guard.run_from_args(
                args,
                adapter=adapter,
                process_runner=lambda command: guard.ProcessResult(exit_code=0),
            )

        self.assertEqual(exit_code, guard.EXIT_OK)
        self.assertTrue(summary["runtime_isolation"]["validated"])
        self.assertEqual(
            summary["runtime_isolation"]["manifest"], str(result.manifest)
        )
        self.assertEqual(
            summary["runtime_isolation"]["gateway_shm"], result.gateway_shm
        )

    def test_mark_applied_rejects_feedback_credentials_mismatch(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        feedback_text = result.feedback_config.read_text(encoding="utf-8")
        result.feedback_config.write_text(
            feedback_text.replace(
                "BITGET_TEST_PASSPHRASE", "OTHER_BITGET_PASSPHRASE"
            ),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(ValueError, "credentials"):
            prepare.mark_external_configs_applied(result.manifest)

    def test_guard_rejects_fixed_shm_for_bitget_execute(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        prepare.mark_external_configs_applied(result.manifest)
        manifest = json.loads(result.manifest.read_text(encoding="utf-8"))
        manifest["gateway_shm"] = "aquila_bitget_order_gateway"
        result.manifest.write_text(json.dumps(manifest), encoding="utf-8")

        with self.assertRaisesRegex(ValueError, "run isolation"):
            guard.validate_bitget_run_isolation(
                result.manifest,
                [
                    "lead_lag_strategy",
                    "--config",
                    str(result.strategy_config),
                    "--execute",
                ],
            )

    def test_mark_applied_rejects_disabled_strategy_feedback(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        strategy_text = result.strategy_config.read_text(encoding="utf-8")
        result.strategy_config.write_text(
            strategy_text.replace("enabled = true", "enabled = false"),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(ValueError, "feedback.enabled"):
            prepare.mark_external_configs_applied(result.manifest)

    def test_mark_applied_rejects_zero_strategy_feedback_poll_budget(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        strategy_text = result.strategy_config.read_text(encoding="utf-8")
        result.strategy_config.write_text(
            strategy_text.replace(
                "enabled = true",
                "enabled = true\npoll_budget = 0",
            ),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(ValueError, "feedback.poll_budget"):
            prepare.mark_external_configs_applied(result.manifest)

    def test_guard_rejects_contract_scope_smaller_than_bitget_lag_symbols(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(
            self.source_dir,
            lag_symbols=("BTC_USDT", "ETH_USDT"),
        )
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        prepare.mark_external_configs_applied(result.manifest)
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=lambda **kwargs: self.fail(
                "credentials must not be read before contract scope validation"
            ),
            requester_factory=lambda *args: self.fail("requester must not be created"),
            state_reader=lambda *args: self.fail("REST state must not be read"),
            flatten_config_builder=lambda config: {},
            flatten_runner=lambda config, requester, clock: (0, {}),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--runtime-manifest",
                str(result.manifest),
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
                "--config",
                str(result.strategy_config),
                "--execute",
            ]
        )

        exit_code, summary = guard.run_from_args(args, adapter=adapter)

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("ETHUSDT", summary["errors"][0])
        self.assertIn("guard contracts", summary["errors"][0])


if __name__ == "__main__":
    unittest.main()
