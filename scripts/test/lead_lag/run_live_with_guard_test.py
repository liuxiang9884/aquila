#!/home/liuxiang/dev/pyenv/lx/bin/python

import os
import sys
import unittest
from decimal import Decimal
from io import StringIO
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import dedent
from unittest.mock import patch

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


class FakeClock:
    def __init__(self):
        self.now = 0.0
        self.sleeps = []

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class FakeBoundProcessController:
    def __init__(self, alive, exit_on_signal=None):
        self.alive = dict(alive)
        self.exit_on_signal = dict(exit_on_signal or {})
        self.signals = []

    def is_running(self, role, binding):
        del binding
        return self.alive[role]

    def send_signal(self, role, binding, signum):
        del binding
        self.signals.append((role, signum))
        if self.exit_on_signal.get(role) == signum:
            self.alive[role] = False


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
    def test_bitget_quiescence_allows_gateway_self_exit_and_stops_feedback(self):
        controller = FakeBoundProcessController(
            {"gateway": False, "feedback": True},
            {"feedback": guard.signal.SIGTERM},
        )
        manifest = {
            "processes": {
                "gateway": {"pid": 101, "start_time_ticks": 1},
                "feedback": {"pid": 102, "start_time_ticks": 2},
            }
        }

        ok, summary = guard.quiesce_bitget_processes(
            manifest,
            controller=controller,
            clock=FakeClock(),
            gateway_grace_sec=1.0,
            term_timeout_sec=1.0,
            kill_timeout_sec=1.0,
            poll_interval_sec=0.1,
        )

        self.assertTrue(ok)
        self.assertTrue(summary["ok"])
        self.assertEqual(controller.signals, [("feedback", guard.signal.SIGTERM)])

    def test_bitget_quiescence_stops_optional_data_session(self):
        controller = FakeBoundProcessController(
            {"gateway": False, "feedback": True, "data_session": True},
            {
                "feedback": guard.signal.SIGTERM,
                "data_session": guard.signal.SIGTERM,
            },
        )
        manifest = {
            "processes": {
                "data_session": {"pid": 100, "start_time_ticks": 1},
                "gateway": {"pid": 101, "start_time_ticks": 2},
                "feedback": {"pid": 102, "start_time_ticks": 3},
            }
        }

        ok, summary = guard.quiesce_bitget_processes(
            manifest,
            controller=controller,
            clock=FakeClock(),
            gateway_grace_sec=1.0,
            term_timeout_sec=1.0,
            kill_timeout_sec=1.0,
            poll_interval_sec=0.1,
        )

        self.assertTrue(ok)
        self.assertEqual(
            set(summary["processes"]),
            {"gateway", "feedback", "data_session"},
        )
        self.assertEqual(
            controller.signals,
            [
                ("feedback", guard.signal.SIGTERM),
                ("data_session", guard.signal.SIGTERM),
            ],
        )

    def test_bitget_quiescence_escalates_gateway_to_kill(self):
        controller = FakeBoundProcessController(
            {"gateway": True, "feedback": True},
            {
                "gateway": guard.signal.SIGKILL,
                "feedback": guard.signal.SIGTERM,
            },
        )

        ok, summary = guard.quiesce_bitget_processes(
            {
                "processes": {
                    "gateway": {"pid": 101, "start_time_ticks": 1},
                    "feedback": {"pid": 102, "start_time_ticks": 2},
                }
            },
            controller=controller,
            clock=FakeClock(),
            gateway_grace_sec=0.1,
            term_timeout_sec=0.1,
            kill_timeout_sec=0.1,
            poll_interval_sec=0.1,
        )

        self.assertTrue(ok)
        self.assertEqual(
            controller.signals,
            [
                ("gateway", guard.signal.SIGTERM),
                ("gateway", guard.signal.SIGKILL),
                ("feedback", guard.signal.SIGTERM),
            ],
        )
        self.assertEqual(summary["processes"]["gateway"]["result"], "stopped")

    def test_bitget_quiescence_failure_is_fail_closed(self):
        controller = FakeBoundProcessController(
            {"gateway": True, "feedback": True},
        )

        ok, summary = guard.quiesce_bitget_processes(
            {
                "processes": {
                    "gateway": {"pid": 101, "start_time_ticks": 1},
                    "feedback": {"pid": 102, "start_time_ticks": 2},
                }
            },
            controller=controller,
            clock=FakeClock(),
            gateway_grace_sec=0.1,
            term_timeout_sec=0.1,
            kill_timeout_sec=0.1,
            poll_interval_sec=0.1,
        )

        self.assertFalse(ok)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["processes"]["gateway"]["result"], "still_running")
        self.assertIn(("feedback", guard.signal.SIGKILL), controller.signals)

    def test_bitget_execute_rejects_nonproduction_rest_base_url(self):
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=lambda **kwargs: self.fail(
                "credentials must not be read before endpoint validation"
            ),
            requester_factory=lambda *args: self.fail("requester must not be created"),
            state_reader=FakeStateReader([]),
            flatten_config_builder=RecordingFlattenConfigBuilder({}),
            flatten_runner=FakeFlattenRunner((0, {})),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--base-url",
                "https://example.invalid",
                "--runtime-manifest",
                "/home/liuxiang/tmp/unused.json",
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
                "--config",
                "/home/liuxiang/tmp/unused.toml",
                "--execute",
            ]
        )

        exit_code, summary = guard.run_from_args(args, adapter=adapter)

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("production REST base URL", summary["errors"][0])

    def test_bitget_execute_requires_runtime_manifest_before_credentials(self):
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=lambda **kwargs: self.fail(
                "credentials must not be read before run isolation validation"
            ),
            requester_factory=lambda *args: self.fail("requester must not be created"),
            state_reader=FakeStateReader([]),
            flatten_config_builder=RecordingFlattenConfigBuilder({}),
            flatten_runner=FakeFlattenRunner((0, {})),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
                "--execute",
            ]
        )

        exit_code, summary = guard.run_from_args(args, adapter=adapter)

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("--runtime-manifest", summary["errors"][0])

    def test_bitget_execute_equals_requires_runtime_manifest_before_credentials(self):
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=lambda **kwargs: self.fail(
                "credentials must not be read before run isolation validation"
            ),
            requester_factory=lambda *args: self.fail("requester must not be created"),
            state_reader=FakeStateReader([]),
            flatten_config_builder=RecordingFlattenConfigBuilder({}),
            flatten_runner=FakeFlattenRunner((0, {})),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
                "--execute=1",
            ]
        )

        exit_code, summary = guard.run_from_args(args, adapter=adapter)

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("--runtime-manifest", summary["errors"][0])

    def test_bitget_rejects_strategy_wrapper_before_credentials(self):
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=lambda **kwargs: self.fail(
                "credentials must not be read before command validation"
            ),
            requester_factory=lambda *args: self.fail("requester must not be created"),
            state_reader=FakeStateReader([]),
            flatten_config_builder=RecordingFlattenConfigBuilder({}),
            flatten_runner=FakeFlattenRunner((0, {})),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--contract",
                "BTC_USDT",
                "--",
                "bash",
                "-c",
                "lead_lag_strategy --execute",
            ]
        )

        exit_code, summary = guard.run_from_args(args, adapter=adapter)

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("direct lead_lag_strategy", summary["errors"][0])

    def test_run_from_args_uses_selected_adapter_and_passphrase(self):
        resolver_calls = []
        requester_calls = []
        requester = object()

        def resolve_credentials(**kwargs):
            resolver_calls.append(kwargs)
            return guard.GuardCredentialEnvNames(
                api_key_env="BITGET_TEST_KEY",
                api_secret_env="BITGET_TEST_SECRET",
                api_passphrase_env="BITGET_TEST_PASSPHRASE",
                source="explicit",
            )

        def make_requester(api_key, api_secret, api_passphrase, base_url, timeout):
            requester_calls.append(
                (api_key, api_secret, api_passphrase, base_url, timeout)
            )
            return requester

        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=resolve_credentials,
            requester_factory=make_requester,
            state_reader=FakeStateReader([flat_state(), flat_state()]),
            flatten_config_builder=RecordingFlattenConfigBuilder({}),
            flatten_runner=FakeFlattenRunner(
                (guard.FLATTEN_EXIT_OK, {"ok": True, "result": "verified_flat"})
            ),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--api-key",
                "BITGET_TEST_KEY",
                "--api-secret",
                "BITGET_TEST_SECRET",
                "--api-passphrase",
                "BITGET_TEST_PASSPHRASE",
                "--base-url",
                "https://bitget.invalid",
                "--timeout",
                "1.5",
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
            ]
        )

        with patch.dict(
            os.environ,
            {
                "BITGET_TEST_KEY": "key-value",
                "BITGET_TEST_SECRET": "secret-value",
                "BITGET_TEST_PASSPHRASE": "passphrase-value",
            },
            clear=False,
        ):
            exit_code, summary = guard.run_from_args(
                args,
                adapter=adapter,
                process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=0)),
            )

        self.assertEqual(exit_code, guard.EXIT_OK)
        self.assertEqual(summary["exchange"], "bitget")
        self.assertEqual(summary["credentials"]["api_passphrase_env"], "BITGET_TEST_PASSPHRASE")
        self.assertEqual(
            resolver_calls,
            [
                {
                    "explicit_api_key": "BITGET_TEST_KEY",
                    "explicit_api_secret": "BITGET_TEST_SECRET",
                    "explicit_api_passphrase": "BITGET_TEST_PASSPHRASE",
                    "strategy_command": ["lead_lag_strategy"],
                }
            ],
        )
        self.assertEqual(
            requester_calls,
            [
                (
                    "key-value",
                    "secret-value",
                    "passphrase-value",
                    "https://bitget.invalid",
                    1.5,
                )
            ],
        )

    def test_run_from_args_rejects_empty_passphrase_env(self):
        credentials = guard.GuardCredentialEnvNames(
            api_key_env="BITGET_TEST_KEY",
            api_secret_env="BITGET_TEST_SECRET",
            api_passphrase_env="BITGET_TEST_PASSPHRASE",
            source="explicit",
        )
        adapter = guard.GuardExchangeAdapter(
            name="bitget",
            credential_resolver=lambda **kwargs: credentials,
            requester_factory=lambda *args: self.fail(
                "requester must not be created with an empty passphrase"
            ),
            state_reader=FakeStateReader([]),
            flatten_config_builder=RecordingFlattenConfigBuilder({}),
            flatten_runner=FakeFlattenRunner((0, {})),
        )
        args = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
            ]
        )

        with patch.dict(
            os.environ,
            {
                "BITGET_TEST_KEY": "key-value",
                "BITGET_TEST_SECRET": "secret-value",
                "BITGET_TEST_PASSPHRASE": "",
            },
            clear=False,
        ):
            exit_code, summary = guard.run_from_args(args, adapter=adapter)

        self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
        self.assertEqual(summary["result"], "config_error")
        self.assertIn("BITGET_TEST_PASSPHRASE", summary["errors"][0])

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

    def test_bitget_guard_state_uses_conservative_flat_snapshot(self):
        calls = []
        open_order_responses = [
            {"list": [], "cursor": ""},
            {
                "list": [
                    {
                        "symbol": "BTCUSDT",
                        "orderId": "late-order",
                        "clientOid": "a-late-order",
                    }
                ],
                "cursor": "",
            },
        ]

        def fake_request(api_request):
            calls.append(api_request.endpoint_path)
            if api_request.endpoint_path.endswith("unfilled-orders"):
                return open_order_responses.pop(0)
            if api_request.endpoint_path.endswith("current-position"):
                return {
                    "list": [
                        {
                            "symbol": "BTCUSDT",
                            "posSide": "long",
                            "holdMode": "one_way_mode",
                            "marginMode": "crossed",
                            "total": "0",
                            "available": "0",
                            "frozen": "0",
                        }
                    ]
                }
            raise AssertionError(f"unexpected request: {api_request}")

        state = guard.bitget_query_guard_state(
            fake_request,
            "usdt",
            ["BTC_USDT"],
        )

        self.assertFalse(state.flat())
        self.assertEqual(state.open_orders[0].order_id, "late-order")
        self.assertEqual(
            calls,
            [
                "/api/v3/trade/unfilled-orders",
                "/api/v3/position/current-position",
                "/api/v3/trade/unfilled-orders",
            ],
        )

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

    def test_non_finite_poll_config_is_rejected_before_rest_or_strategy(self):
        for field in ("poll_timeout_sec", "poll_interval_sec"):
            for value in (float("nan"), float("inf"), float("-inf")):
                with self.subTest(field=field, value=value):
                    values = {
                        "settle": "usdt",
                        "contracts": ["BTC_USDT"],
                        "strategy_command": ["lead_lag_strategy"],
                        "poll_timeout_sec": 3.0,
                        "poll_interval_sec": 0.5,
                    }
                    values[field] = value

                    exit_code, summary = guard.run_guarded_live(
                        config=guard.GuardConfig(**values),
                        requester=lambda request: self.fail(
                            "REST must not run for invalid poll config"
                        ),
                        process_runner=lambda command: self.fail(
                            "strategy must not run for invalid poll config"
                        ),
                        state_reader=lambda *args: self.fail(
                            "state reader must not run for invalid poll config"
                        ),
                    )

                    self.assertEqual(exit_code, guard.EXIT_CONFIG_ERROR)
                    self.assertEqual(summary["result"], "config_error")
                    self.assertIn("finite", summary["errors"][0])

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

    def test_final_rest_check_runs_only_after_process_quiescence(self):
        quiesced = False

        def quiescer(clock):
            del clock
            nonlocal quiesced
            quiesced = True
            return True, {"ok": True, "result": "stopped"}

        states = [flat_state(), flat_state()]

        def state_reader(requester, settle, contracts):
            del requester, settle, contracts
            if len(states) == 1:
                self.assertTrue(quiesced)
            return states.pop(0)

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=0)),
            flatten_runner=FakeFlattenRunner(
                (guard.FLATTEN_EXIT_OK, {"ok": True})
            ),
            state_reader=state_reader,
            process_quiescer=quiescer,
        )

        self.assertEqual(exit_code, guard.EXIT_OK)
        self.assertTrue(summary["quiescence"]["ok"])

    def test_quiescence_failure_blocks_final_rest_and_flatten(self):
        state_reader = FakeStateReader([flat_state()])
        flatten_runner = FakeFlattenRunner(
            (guard.FLATTEN_EXIT_OK, {"ok": True, "result": "verified_flat"})
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=1)),
            flatten_runner=flatten_runner,
            state_reader=state_reader,
            process_quiescer=lambda clock: (
                False,
                {"ok": False, "result": "still_running"},
            ),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FAILED)
        self.assertEqual(summary["result"], "process_quiescence_failed")
        self.assertEqual(len(state_reader.calls), 1)
        self.assertEqual(flatten_runner.calls, [])

    def test_strategy_exception_quiesces_before_flatten(self):
        events = []

        def raise_strategy(command):
            del command
            events.append("strategy")
            raise RuntimeError("injected strategy failure")

        def quiescer(clock):
            del clock
            events.append("quiescence")
            return True, {"ok": True, "result": "stopped"}

        def flatten_runner(flatten_config, requester, clock):
            del flatten_config, requester, clock
            events.append("flatten")
            return guard.FLATTEN_EXIT_OK, {"ok": True, "result": "verified_flat"}

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=raise_strategy,
            flatten_runner=flatten_runner,
            state_reader=FakeStateReader([flat_state()]),
            process_quiescer=quiescer,
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertEqual(events, ["strategy", "quiescence", "flatten"])
        self.assertEqual(summary["result"], "strategy_exception_flattened")

    def test_strategy_process_escalates_ignored_termination_to_kill(self):
        handlers = {}
        now = 0.0

        class IgnoringChild:
            def __init__(self):
                self.terminate_calls = 0
                self.kill_calls = 0

            def poll(self):
                return -guard.signal.SIGKILL if self.kill_calls else None

            def terminate(self):
                self.terminate_calls += 1

            def kill(self):
                self.kill_calls += 1

        child = IgnoringChild()

        def install_handler(signum, handler):
            previous = handlers.get(signum, guard.signal.SIG_DFL)
            handlers[signum] = handler
            return previous

        signal_delivered = False

        def sleep(seconds):
            nonlocal now, signal_delivered
            if not signal_delivered:
                signal_delivered = True
                handlers[guard.signal.SIGTERM](guard.signal.SIGTERM, None)
            now += seconds

        with patch.object(guard.signal, "signal", side_effect=install_handler):
            result = guard.run_strategy_process(
                ["lead_lag_strategy"],
                popen_factory=lambda command: child,
                monotonic=lambda: now,
                sleeper=sleep,
                terminate_grace_sec=0.1,
                poll_interval_sec=0.05,
            )

        self.assertEqual(child.terminate_calls, 1)
        self.assertEqual(child.kill_calls, 1)
        self.assertEqual(result.exit_code, -guard.signal.SIGKILL)
        self.assertEqual(result.signal_number, guard.signal.SIGTERM)

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

    def test_flatten_exception_returns_structured_emergency_failure(self):
        def raise_flatten_error(flatten_config, requester, clock):
            raise RuntimeError("injected REST transport failure")

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=1)),
            flatten_runner=raise_flatten_error,
            state_reader=FakeStateReader([flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FAILED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "strategy_exit_flatten_failed")
        self.assertEqual(summary["flatten"]["result"], "exception")
        self.assertIn("injected REST transport failure", summary["errors"][0])

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

    def test_parse_args_accepts_bitget_passphrase(self):
        parsed = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--api-key",
                "BITGET_TEST_KEY",
                "--api-secret",
                "BITGET_TEST_SECRET",
                "--api-passphrase",
                "BITGET_TEST_PASSPHRASE",
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
                "--execute",
            ]
        )

        self.assertEqual(parsed.api_passphrase, "BITGET_TEST_PASSPHRASE")
        self.assertEqual(guard.config_from_args(parsed).exchange, "bitget")

    def test_contract_help_is_exchange_neutral(self):
        with patch("sys.stdout", new_callable=StringIO) as stdout:
            with self.assertRaises(SystemExit) as raised:
                guard.parse_args(["--help"])

        self.assertEqual(raised.exception.code, 0)
        self.assertIn("futures contract or symbol", stdout.getvalue())
        self.assertNotIn("Gate futures contract", stdout.getvalue())

    def test_parse_args_defers_default_base_url_to_exchange_adapter(self):
        parsed = guard.parse_args(
            [
                "--exchange",
                "bitget",
                "--contract",
                "BTC_USDT",
                "--",
                "lead_lag_strategy",
            ]
        )

        self.assertIsNone(parsed.base_url)

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
