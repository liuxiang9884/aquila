#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import unittest
from decimal import Decimal
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import dedent

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import run_live_with_guard as guard


class FakeStateReader:
    def __init__(self, states):
        self.states = list(states)
        self.calls = []

    def __call__(self, requester, settle, contracts):
        self.calls.append((settle, tuple(contracts)))
        if not self.states:
            raise AssertionError("unexpected state read")
        return self.states.pop(0)


class FakeProcessRunner:
    def __init__(self, result):
        self.result = result
        self.commands = []

    def __call__(self, command):
        self.commands.append(list(command))
        return self.result


class FakeFlattenRunner:
    def __init__(self, result):
        self.result = result
        self.calls = []

    def __call__(self, config, requester, clock):
        self.calls.append(config)
        return self.result


class RecordingFlattenConfigBuilder:
    def __init__(self, result):
        self.result = result
        self.calls = []

    def __call__(self, config):
        self.calls.append(config)
        return self.result


def flat_state(contract="BTC_USDT"):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(contract=contract, size=0, pending_orders=0)
        ],
        open_orders=[],
    )


def open_order_state(contract="BTC_USDT"):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(contract=contract, size=0, pending_orders=0)
        ],
        open_orders=[guard.OpenOrder(contract=contract, order_id="12345")],
    )


def position_state(contract="BTC_USDT", size=1):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(contract=contract, size=size, pending_orders=0)
        ],
        open_orders=[],
    )


def residual_value_state(contract="RAVE_USDT"):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(
                contract=contract,
                size=Decimal("0"),
                pending_orders=0,
                value=Decimal("0.11248"),
                margin=Decimal("0.022496"),
            )
        ],
        open_orders=[],
    )


def config(command=None):
    return guard.GuardConfig(
        settle="usdt",
        contracts=["BTC_USDT"],
        strategy_command=command
        or ["./build/debug/tools/lead_lag_strategy", "--execute"],
        poll_timeout_sec=3.0,
        poll_interval_sec=0.5,
    )


def write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dedent(text).strip() + "\n", encoding="utf-8")


def write_affinity_profile(path: Path) -> None:
    write_text(
        path,
        """
        [profile]
        name = "test_node0"
        numa_node = 0

        [core_path]
        gate_market_data_cpu = 2
        binance_market_data_cpu = 3
        strategy_order_owner_cpu = 4
        gate_order_feedback_cpu = 6
        log_backend_cpu = 5

        [auxiliary]
        reserved_core_cpus = [2, 3, 4, 6]
        preferred_aux_cpus = [7, 8, 9, 10]
        """,
    )


class RunLiveWithGuardTest(unittest.TestCase):
    def test_query_guard_state_treats_position_not_found_as_flat_contract(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/ZEC_USDT"):
                raise RuntimeError('HTTP 400: {"label":"POSITION_NOT_FOUND"}')
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            raise AssertionError(f"unexpected request: {api_request}")

        state = guard.query_guard_state(fake_request, "usdt", ["ZEC_USDT"])

        self.assertTrue(state.flat())
        self.assertEqual(state.positions[0].contract, "ZEC_USDT")
        self.assertEqual(state.positions[0].size, 0)
        self.assertEqual(state.positions[0].pending_orders, 0)
        self.assertEqual(state.open_orders, [])
        self.assertEqual(len(calls), 2)

    def test_preflight_not_flat_refuses_to_start_or_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([open_order_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_PREFLIGHT_FAILED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "preflight_not_flat")
        self.assertEqual(process.commands, [])
        self.assertEqual(flatten.calls, [])
        self.assertEqual(summary["preflight"]["open_orders"][0]["order_id"], "12345")

    def test_preflight_residual_value_refuses_to_start_or_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([residual_value_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_PREFLIGHT_FAILED)
        self.assertEqual(summary["result"], "preflight_not_flat")
        self.assertEqual(process.commands, [])
        self.assertEqual(flatten.calls, [])
        self.assertFalse(summary["preflight"]["flat"])
        self.assertEqual(summary["preflight"]["positions"][0]["value"], "0.11248")

    def test_normal_exit_and_flat_final_state_returns_ok(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state(), flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_OK)
        self.assertTrue(summary["ok"])
        self.assertEqual(summary["result"], "normal_exit_flat")
        self.assertEqual(process.commands, [config().strategy_command])
        self.assertEqual(flatten.calls, [])

    def test_nonzero_strategy_exit_runs_flatten_and_reports_handoff(self):
        process = FakeProcessRunner(
            guard.ProcessResult(exit_code=guard.STRATEGY_EXIT_CONTINUITY_LOST)
        )
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "strategy_exit_flattened")
        self.assertEqual(summary["strategy"]["exit_code"], guard.STRATEGY_EXIT_CONTINUITY_LOST)
        self.assertEqual(len(flatten.calls), 1)
        self.assertEqual(flatten.calls[0].contracts, ["BTC_USDT"])
        self.assertFalse(flatten.calls[0].dry_run)

    def test_nonzero_exit_uses_injected_flatten_config_builder(self):
        flatten_config = {"exchange": "test"}
        builder = RecordingFlattenConfigBuilder(flatten_config)
        flatten_runner = FakeFlattenRunner(
            (guard.FLATTEN_EXIT_OK, {"ok": True, "result": "verified_flat"})
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=1)),
            flatten_runner=flatten_runner,
            flatten_config_builder=builder,
            state_reader=FakeStateReader([flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertEqual(builder.calls, [config()])
        self.assertEqual(flatten_runner.calls, [flatten_config])
        self.assertEqual(summary["exchange"], "gate")

    def test_normal_exit_with_nonflat_final_state_runs_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state(), position_state(size=2)]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "final_check_flattened")
        self.assertEqual(summary["final_check"]["positions"][0]["size"], 2)
        self.assertEqual(len(flatten.calls), 1)

    def test_normal_exit_with_residual_value_final_state_runs_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state("RAVE_USDT"), residual_value_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "final_check_flattened")
        self.assertFalse(summary["final_check"]["flat"])
        self.assertEqual(summary["final_check"]["positions"][0]["value"], "0.11248")
        self.assertEqual(len(flatten.calls), 1)

    def test_flatten_failure_returns_emergency_failed(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=1))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_NOT_FLAT,
                {"ok": False, "result": "not_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FAILED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "strategy_exit_flatten_failed")
        self.assertEqual(summary["flatten"]["result"], "not_flat")

    def test_parse_args_uses_separator_for_strategy_command(self):
        parsed = guard.parse_args(
            [
                "--contract",
                "btc_usdt",
                "--poll-timeout-sec",
                "5",
                "--",
                "./build/debug/tools/lead_lag_strategy",
                "--execute",
                "--duration-sec",
                "10",
            ]
        )
        config = guard.config_from_args(parsed)

        self.assertEqual(parsed.exchange, "gate")
        self.assertEqual(config.exchange, "gate")
        self.assertEqual(config.contracts, ["BTC_USDT"])
        self.assertEqual(config.poll_timeout_sec, 5.0)
        self.assertEqual(
            config.strategy_command,
            [
                "./build/debug/tools/lead_lag_strategy",
                "--execute",
                "--duration-sec",
                "10",
            ],
        )

    def test_resolves_guard_credentials_from_strategy_order_session_config(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            strategy_path = base / "strategy.toml"
            order_session_path = base / "order_session.toml"
            write_text(
                strategy_path,
                f"""
                [strategy.order_session]
                config = "{order_session_path}"
                """,
            )
            write_text(
                order_session_path,
                """
                [order_session.credentials]
                api_key_env = "GATE_PROBE_KEY"
                api_secret_env = "GATE_PROBE_SECRET"
                """,
            )

            credentials = guard.resolve_guard_credential_env_names(
                explicit_api_key=None,
                explicit_api_secret=None,
                strategy_command=[
                    "./build/debug/tools/lead_lag_strategy",
                    "--config",
                    str(strategy_path),
                    "--execute",
                ],
            )

            self.assertEqual(credentials.api_key_env, "GATE_PROBE_KEY")
            self.assertEqual(credentials.api_secret_env, "GATE_PROBE_SECRET")
            self.assertEqual(credentials.source, "order_session_config")

    def test_resolves_guard_credentials_from_strategy_order_gateway_config(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            strategy_path = base / "strategy.toml"
            order_gateway_path = base / "order_gateway.toml"
            order_session_path = base / "order_session.toml"
            write_text(
                strategy_path,
                f"""
                [strategy.order_gateway]
                config = "{order_gateway_path}"
                """,
            )
            write_text(
                order_gateway_path,
                f"""
                [order_gateway]
                route_count = 1

                [[order_gateway.routes]]
                name = "route0"
                order_session_config = "{order_session_path}"
                worker_cpu_id = 16
                """,
            )
            write_text(
                order_session_path,
                """
                [order_session.credentials]
                api_key_env = "GATE_PROBE_KEY"
                api_secret_env = "GATE_PROBE_SECRET"
                """,
            )

            credentials = guard.resolve_guard_credential_env_names(
                explicit_api_key=None,
                explicit_api_secret=None,
                strategy_command=[
                    "./build/debug/tools/lead_lag_strategy",
                    "--config",
                    str(strategy_path),
                    "--execute",
                ],
            )

            self.assertEqual(credentials.api_key_env, "GATE_PROBE_KEY")
            self.assertEqual(credentials.api_secret_env, "GATE_PROBE_SECRET")
            self.assertEqual(credentials.source, "order_gateway_config")

    def test_explicit_guard_credentials_must_match_order_session_config(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            strategy_path = base / "strategy.toml"
            order_session_path = base / "order_session.toml"
            write_text(
                strategy_path,
                f"""
                [strategy.order_session]
                config = "{order_session_path}"
                """,
            )
            write_text(
                order_session_path,
                """
                [order_session.credentials]
                api_key_env = "GATE_PROBE_KEY"
                api_secret_env = "GATE_PROBE_SECRET"
                """,
            )

            with self.assertRaisesRegex(ValueError, "guard REST credentials"):
                guard.resolve_guard_credential_env_names(
                    explicit_api_key="GATE_TEST_KEY",
                    explicit_api_secret="GATE_TEST_SECRET",
                    strategy_command=[
                        "./build/debug/tools/lead_lag_strategy",
                        "--config",
                        str(strategy_path),
                    ],
                )

    def test_matching_explicit_guard_credentials_are_allowed(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            strategy_path = base / "strategy.toml"
            order_session_path = base / "order_session.toml"
            write_text(
                strategy_path,
                f"""
                [strategy.order_session]
                config = "{order_session_path}"
                """,
            )
            write_text(
                order_session_path,
                """
                [order_session.credentials]
                api_key_env = "GATE_PROBE_KEY"
                api_secret_env = "GATE_PROBE_SECRET"
                """,
            )

            credentials = guard.resolve_guard_credential_env_names(
                explicit_api_key="GATE_PROBE_KEY",
                explicit_api_secret="GATE_PROBE_SECRET",
                strategy_command=[
                    "./build/debug/tools/lead_lag_strategy",
                    "--config",
                    str(strategy_path),
                ],
            )

            self.assertEqual(credentials.api_key_env, "GATE_PROBE_KEY")
            self.assertEqual(credentials.api_secret_env, "GATE_PROBE_SECRET")
            self.assertEqual(credentials.source, "explicit")

    def test_partial_explicit_guard_credentials_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "must be provided together"):
            guard.resolve_guard_credential_env_names(
                explicit_api_key="GATE_PROBE_KEY",
                explicit_api_secret=None,
                strategy_command=["./build/debug/tools/lead_lag_strategy"],
            )

    def test_affinity_profile_rewrites_strategy_command_to_temp_overlays(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            profile_path = base / "profile.toml"
            strategy_path = base / "strategy.toml"
            data_reader_path = base / "data_reader.toml"
            order_session_path = base / "order_session.toml"
            output_dir = base / "out" / "configs"
            write_affinity_profile(profile_path)
            write_text(
                strategy_path,
                f"""
                [log]
                backend_cpu_affinity = 99

                [strategy.loop]
                bind_cpu_id = 99

                [strategy.data_reader]
                config = "{data_reader_path}"

                [strategy.order_session]
                config = "{order_session_path}"
                """,
            )
            write_text(
                data_reader_path,
                """
                [log]
                backend_cpu_affinity = 99

                [data_reader.execution_policy]
                bind_cpu_id = 99
                """,
            )
            write_text(
                order_session_path,
                """
                [log]
                backend_cpu_affinity = 99

                [order_session.websocket.execution_policy]
                bind_cpu_id = 99
                """,
            )
            process = FakeProcessRunner(guard.ProcessResult(exit_code=0))

            exit_code, summary = guard.run_guarded_live(
                config=guard.GuardConfig(
                    settle="usdt",
                    contracts=["BTC_USDT"],
                    strategy_command=[
                        "./build/debug/tools/lead_lag_strategy",
                        "--config",
                        str(strategy_path),
                        "--execute",
                    ],
                    affinity_profile_path=profile_path,
                    affinity_output_dir=output_dir,
                ),
                requester=lambda request: {},
                process_runner=process,
                flatten_runner=FakeFlattenRunner(
                    (guard.FLATTEN_EXIT_OK, {"ok": True})
                ),
                state_reader=FakeStateReader([flat_state(), flat_state()]),
            )

            self.assertEqual(exit_code, guard.EXIT_OK)
            generated = summary["affinity"]["generated_configs"]
            rewritten_strategy = Path(generated["strategy"])
            rewritten_data_reader = Path(generated["strategy_data_reader"])
            rewritten_order_session = Path(generated["gate_order_session"])
            self.assertEqual(
                process.commands[0],
                [
                    "./build/debug/tools/lead_lag_strategy",
                    "--config",
                    str(rewritten_strategy),
                    "--execute",
                ],
            )
            self.assertIn("backend_cpu_affinity = 5", rewritten_strategy.read_text())
            self.assertIn("bind_cpu_id = 4", rewritten_strategy.read_text())
            self.assertIn(f'config = "{rewritten_data_reader}"', rewritten_strategy.read_text())
            self.assertIn(f'config = "{rewritten_order_session}"', rewritten_strategy.read_text())
            self.assertIn("backend_cpu_affinity = 5", rewritten_data_reader.read_text())
            self.assertIn("bind_cpu_id = 4", rewritten_data_reader.read_text())
            self.assertIn("backend_cpu_affinity = 5", rewritten_order_session.read_text())
            self.assertIn("bind_cpu_id = 4", rewritten_order_session.read_text())

    def test_affinity_profile_generates_market_and_feedback_overlays(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            profile_path = base / "profile.toml"
            output_dir = base / "out" / "configs"
            strategy_path = base / "strategy.toml"
            data_reader_path = base / "data_reader.toml"
            order_session_path = base / "order_session.toml"
            gate_market_path = base / "gate_data.toml"
            binance_market_path = base / "binance_data.toml"
            feedback_path = base / "feedback.toml"
            write_affinity_profile(profile_path)
            write_text(
                strategy_path,
                f"""
                [log]
                backend_cpu_affinity = 99

                [strategy.loop]
                bind_cpu_id = 99

                [strategy.data_reader]
                config = "{data_reader_path}"

                [strategy.order_session]
                config = "{order_session_path}"
                """,
            )
            write_text(
                data_reader_path,
                """
                [log]
                backend_cpu_affinity = 99

                [data_reader.execution_policy]
                bind_cpu_id = 99
                """,
            )
            write_text(
                order_session_path,
                """
                [log]
                backend_cpu_affinity = 99

                [order_session.websocket.execution_policy]
                bind_cpu_id = 99
                """,
            )
            for path, section in [
                (gate_market_path, "data_session.websocket.execution_policy"),
                (binance_market_path, "data_session.websocket.execution_policy"),
                (feedback_path, "order_feedback_session.websocket.execution_policy"),
            ]:
                write_text(
                    path,
                    f"""
                    [log]
                    backend_cpu_affinity = 99

                    [{section}]
                    bind_cpu_id = 99
                    """,
                )
            process = FakeProcessRunner(guard.ProcessResult(exit_code=0))

            exit_code, summary = guard.run_guarded_live(
                config=guard.GuardConfig(
                    settle="usdt",
                    contracts=["BTC_USDT"],
                    strategy_command=[
                        "./build/debug/tools/lead_lag_strategy",
                        "--config",
                        str(strategy_path),
                    ],
                    affinity_profile_path=profile_path,
                    affinity_output_dir=output_dir,
                    affinity_gate_market_config=gate_market_path,
                    affinity_binance_market_config=binance_market_path,
                    affinity_order_feedback_config=feedback_path,
                ),
                requester=lambda request: {},
                process_runner=process,
                flatten_runner=FakeFlattenRunner(
                    (guard.FLATTEN_EXIT_OK, {"ok": True})
                ),
                state_reader=FakeStateReader([flat_state(), flat_state()]),
            )

            self.assertEqual(exit_code, guard.EXIT_OK)
            self.assertFalse(summary["affinity"]["affinity_split"])
            self.assertEqual(
                summary["affinity"]["applied_configs"],
                ["strategy", "strategy_data_reader", "gate_order_session"],
            )
            self.assertEqual(
                summary["affinity"]["generated_only_configs"],
                [
                    "binance_market_data",
                    "gate_market_data",
                    "gate_order_feedback",
                ],
            )
            generated = summary["affinity"]["generated_configs"]
            gate_market = Path(generated["gate_market_data"])
            binance_market = Path(generated["binance_market_data"])
            feedback = Path(generated["gate_order_feedback"])
            self.assertIn("backend_cpu_affinity = 5", gate_market.read_text())
            self.assertIn("bind_cpu_id = 2", gate_market.read_text())
            self.assertIn("backend_cpu_affinity = 5", binance_market.read_text())
            self.assertIn("bind_cpu_id = 3", binance_market.read_text())
            self.assertIn("backend_cpu_affinity = 5", feedback.read_text())
            self.assertIn("bind_cpu_id = 6", feedback.read_text())

    def test_affinity_summary_can_mark_external_overlays_applied(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            profile_path = base / "profile.toml"
            output_dir = base / "out" / "configs"
            strategy_path = base / "strategy.toml"
            data_reader_path = base / "data_reader.toml"
            order_session_path = base / "order_session.toml"
            gate_market_path = base / "gate_data.toml"
            binance_market_path = base / "binance_data.toml"
            feedback_path = base / "feedback.toml"
            write_affinity_profile(profile_path)
            write_text(
                strategy_path,
                f"""
                [log]
                backend_cpu_affinity = 99

                [strategy.loop]
                bind_cpu_id = 99

                [strategy.data_reader]
                config = "{data_reader_path}"

                [strategy.order_session]
                config = "{order_session_path}"
                """,
            )
            write_text(
                data_reader_path,
                """
                [log]
                backend_cpu_affinity = 99

                [data_reader.execution_policy]
                bind_cpu_id = 99
                """,
            )
            write_text(
                order_session_path,
                """
                [log]
                backend_cpu_affinity = 99

                [order_session.websocket.execution_policy]
                bind_cpu_id = 99
                """,
            )
            for path, section in [
                (gate_market_path, "data_session.websocket.execution_policy"),
                (binance_market_path, "data_session.websocket.execution_policy"),
                (feedback_path, "order_feedback_session.websocket.execution_policy"),
            ]:
                write_text(
                    path,
                    f"""
                    [log]
                    backend_cpu_affinity = 99

                    [{section}]
                    bind_cpu_id = 99
                    """,
                )

            exit_code, summary = guard.run_guarded_live(
                config=guard.GuardConfig(
                    settle="usdt",
                    contracts=["BTC_USDT"],
                    strategy_command=[
                        "./build/debug/tools/lead_lag_strategy",
                        "--config",
                        str(strategy_path),
                    ],
                    affinity_profile_path=profile_path,
                    affinity_output_dir=output_dir,
                    affinity_gate_market_config=gate_market_path,
                    affinity_binance_market_config=binance_market_path,
                    affinity_order_feedback_config=feedback_path,
                    affinity_external_configs_applied=True,
                ),
                requester=lambda request: {},
                process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=0)),
                flatten_runner=FakeFlattenRunner(
                    (guard.FLATTEN_EXIT_OK, {"ok": True})
                ),
                state_reader=FakeStateReader([flat_state(), flat_state()]),
            )

            self.assertEqual(exit_code, guard.EXIT_OK)
            self.assertTrue(summary["affinity"]["affinity_split"])
            self.assertEqual(summary["affinity"]["generated_only_configs"], [])
            self.assertIn(
                "gate_order_feedback", summary["affinity"]["applied_configs"]
            )

    def test_prepare_affinity_only_generates_summary_without_running_process(self):
        with TemporaryDirectory() as tmp:
            base = Path(tmp)
            profile_path = base / "profile.toml"
            strategy_path = base / "strategy.toml"
            data_reader_path = base / "data_reader.toml"
            order_session_path = base / "order_session.toml"
            output_dir = base / "out" / "configs"
            write_affinity_profile(profile_path)
            write_text(
                strategy_path,
                f"""
                [log]
                backend_cpu_affinity = 99

                [strategy.loop]
                bind_cpu_id = 99

                [strategy.data_reader]
                config = "{data_reader_path}"

                [strategy.order_session]
                config = "{order_session_path}"
                """,
            )
            write_text(
                data_reader_path,
                """
                [log]
                backend_cpu_affinity = 99

                [data_reader.execution_policy]
                bind_cpu_id = 99
                """,
            )
            write_text(
                order_session_path,
                """
                [log]
                backend_cpu_affinity = 99

                [order_session.websocket.execution_policy]
                bind_cpu_id = 99
                """,
            )

            exit_code, summary = guard.prepare_affinity_only(
                guard.GuardConfig(
                    settle="usdt",
                    contracts=["BTC_USDT"],
                    strategy_command=[
                        "./build/debug/tools/lead_lag_strategy",
                        "--config",
                        str(strategy_path),
                    ],
                    affinity_profile_path=profile_path,
                    affinity_output_dir=output_dir,
                )
            )

            self.assertEqual(exit_code, guard.EXIT_OK)
            self.assertTrue(summary["ok"])
            self.assertEqual(summary["result"], "affinity_prepared")
            self.assertIn("strategy", summary["affinity"]["generated_configs"])


if __name__ == "__main__":
    unittest.main()
