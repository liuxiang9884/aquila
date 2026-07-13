#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import os
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


def write_fake_process(
    proc_root: Path,
    pid: int,
    executable: str,
    config: Path,
    credentials: tuple[str, str, str] = (
        "key-value",
        "secret-value",
        "passphrase-value",
    ),
    start_time_ticks: int = 100,
    config_argument: str | None = None,
) -> None:
    process_dir = proc_root / str(pid)
    process_dir.mkdir(parents=True, exist_ok=True)
    binary = proc_root / "bin" / executable
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.touch(exist_ok=True)
    executable_link = process_dir / "exe"
    executable_link.unlink(missing_ok=True)
    executable_link.symlink_to(binary)
    (process_dir / "cmdline").write_bytes(
        b"\0".join(
            value.encode("utf-8")
            for value in (
                str(binary),
                "--config",
                config_argument or str(config),
                "--connect",
            )
        )
        + b"\0"
    )
    environment = {
        "BITGET_TEST_KEY": credentials[0],
        "BITGET_TEST_SECRET": credentials[1],
        "BITGET_TEST_PASSPHRASE": credentials[2],
    }
    (process_dir / "environ").write_bytes(
        b"\0".join(
            f"{key}={value}".encode("utf-8")
            for key, value in environment.items()
        )
        + b"\0"
    )
    stat_tail = ["S"] + ["0"] * 18 + [str(start_time_ticks)]
    (process_dir / "stat").write_text(
        f"{pid} ({executable}) " + " ".join(stat_tail) + "\n",
        encoding="utf-8",
    )


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
            [order_session]
            category = "usdt-futures"
            position_mode = "one_way_mode"
            margin_mode = "crossed"

            [order_session.credentials]
            api_key_env = "BITGET_TEST_KEY"
            api_secret_env = "BITGET_TEST_SECRET"
            api_passphrase_env = "BITGET_TEST_PASSPHRASE"

            [order_session.websocket.endpoint]
            host = "vip-ws-uta.bitget.com"
            port = "443"
            target = "/v3/ws/private"
            enable_tls = true
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
        [order_feedback_session]
        category = "usdt-futures"
        position_mode = "one_way_mode"
        margin_mode = "crossed"

        [order_feedback_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "BITGET_TEST_PASSPHRASE"

        [order_feedback_session.websocket.endpoint]
        host = "vip-ws-uta.bitget.com"
        port = "443"
        target = "/v3/ws/private"
        enable_tls = true

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
        self.proc_root = self.root / "proc"
        self.gateway_pid = 1101
        self.feedback_pid = 1102

    def tearDown(self):
        self.temp_dir.cleanup()

    def mark_applied(self, result):
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
        )
        write_fake_process(
            self.proc_root,
            self.feedback_pid,
            "bitget_order_feedback_session",
            result.feedback_config,
        )
        return prepare.mark_external_configs_applied(
            result.manifest,
            gateway_pid=self.gateway_pid,
            feedback_pid=self.feedback_pid,
            proc_root=self.proc_root,
        )

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

        manifest = self.mark_applied(result)
        validated = guard.validate_bitget_run_isolation(
            result.manifest,
            ["lead_lag_strategy", "--config", str(result.strategy_config), "--execute"],
            proc_root=self.proc_root,
        )

        self.assertTrue(manifest["external_configs_applied"])
        self.assertEqual(validated["run_id"], self.run_id)

    def test_mark_applied_binds_live_processes_without_persisting_secrets(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
        )
        write_fake_process(
            self.proc_root,
            self.feedback_pid,
            "bitget_order_feedback_session",
            result.feedback_config,
        )

        manifest = prepare.mark_external_configs_applied(
            result.manifest,
            gateway_pid=self.gateway_pid,
            feedback_pid=self.feedback_pid,
            proc_root=self.proc_root,
        )
        validated = guard.validate_bitget_run_isolation(
            result.manifest,
            ["lead_lag_strategy", "--config", str(result.strategy_config), "--execute"],
            proc_root=self.proc_root,
        )

        self.assertEqual(manifest["schema"], prepare.MANIFEST_SCHEMA)
        self.assertEqual(manifest["processes"]["gateway"]["pid"], self.gateway_pid)
        self.assertEqual(
            validated["processes"]["feedback"]["start_time_ticks"], 100
        )
        serialized = json.dumps(manifest)
        self.assertNotIn("key-value", serialized)
        self.assertNotIn("secret-value", serialized)
        self.assertNotIn("passphrase-value", serialized)

    def test_guard_rejects_reused_bound_pid(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
        )
        write_fake_process(
            self.proc_root,
            self.feedback_pid,
            "bitget_order_feedback_session",
            result.feedback_config,
        )
        prepare.mark_external_configs_applied(
            result.manifest,
            gateway_pid=self.gateway_pid,
            feedback_pid=self.feedback_pid,
            proc_root=self.proc_root,
        )
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
            start_time_ticks=101,
        )

        with self.assertRaisesRegex(ValueError, "start time"):
            guard.validate_bitget_run_isolation(
                result.manifest,
                [
                    "lead_lag_strategy",
                    "--config",
                    str(result.strategy_config),
                    "--execute",
                ],
                proc_root=self.proc_root,
            )

    def test_mark_applied_rejects_process_credential_value_mismatch(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
        )
        write_fake_process(
            self.proc_root,
            self.feedback_pid,
            "bitget_order_feedback_session",
            result.feedback_config,
            credentials=("different-key", "secret-value", "passphrase-value"),
        )

        with self.assertRaisesRegex(ValueError, "credential values"):
            prepare.mark_external_configs_applied(
                result.manifest,
                gateway_pid=self.gateway_pid,
                feedback_pid=self.feedback_pid,
                proc_root=self.proc_root,
            )

    def test_mark_applied_rejects_process_config_mismatch(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.feedback_config,
        )
        write_fake_process(
            self.proc_root,
            self.feedback_pid,
            "bitget_order_feedback_session",
            result.feedback_config,
        )

        with self.assertRaisesRegex(ValueError, "config mismatch"):
            prepare.mark_external_configs_applied(
                result.manifest,
                gateway_pid=self.gateway_pid,
                feedback_pid=self.feedback_pid,
                proc_root=self.proc_root,
            )

    def test_mark_applied_rejects_relative_process_config_argument(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
            config_argument=os.path.relpath(result.gateway_config, guard.PROJECT_ROOT),
        )
        write_fake_process(
            self.proc_root,
            self.feedback_pid,
            "bitget_order_feedback_session",
            result.feedback_config,
        )

        with self.assertRaisesRegex(ValueError, "absolute --config"):
            prepare.mark_external_configs_applied(
                result.manifest,
                gateway_pid=self.gateway_pid,
                feedback_pid=self.feedback_pid,
                proc_root=self.proc_root,
            )

    def test_guard_rejects_bound_process_credential_change(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        self.mark_applied(result)
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
            credentials=("changed-key", "secret-value", "passphrase-value"),
        )
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=guard.resolve_bitget_guard_credential_env_names,
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
                proc_root=self.proc_root,
            )

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("credential values", summary["errors"][0])

    def test_guard_summary_records_validated_runtime_isolation(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        self.mark_applied(result)
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

        def stop_fake_processes(command):
            del command
            (self.proc_root / str(self.gateway_pid) / "stat").unlink()
            (self.proc_root / str(self.feedback_pid) / "stat").unlink()
            return guard.ProcessResult(exit_code=0)

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
                process_runner=stop_fake_processes,
                proc_root=self.proc_root,
            )

        self.assertEqual(exit_code, guard.EXIT_OK)
        self.assertTrue(summary["runtime_isolation"]["validated"])
        self.assertEqual(
            summary["runtime_isolation"]["manifest"], str(result.manifest)
        )
        self.assertEqual(
            summary["runtime_isolation"]["gateway_shm"], result.gateway_shm
        )
        self.assertTrue(summary["quiescence"]["ok"])

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
            self.mark_applied(result)

    def test_mark_applied_rejects_feedback_trading_contract_mismatch(self):
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
                'margin_mode = "crossed"',
                'margin_mode = "isolated"',
            ),
            encoding="utf-8",
        )

        with self.assertRaisesRegex(ValueError, "trading contract"):
            self.mark_applied(result)

    def test_guard_rejects_fixed_shm_for_bitget_execute(self):
        strategy, gateway, feedback = write_runtime_fixture_graph(self.source_dir)
        result = prepare.prepare_runtime_configs(
            run_id=self.run_id,
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
        self.mark_applied(result)
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
                proc_root=self.proc_root,
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
            self.mark_applied(result)

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
            self.mark_applied(result)

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
        self.mark_applied(result)
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

        exit_code, summary = guard.run_from_args(
            args, adapter=adapter, proc_root=self.proc_root
        )

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("ETHUSDT", summary["errors"][0])
        self.assertIn("guard contracts", summary["errors"][0])


if __name__ == "__main__":
    unittest.main()
