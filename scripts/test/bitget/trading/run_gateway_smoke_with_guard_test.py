#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import json
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory


PROJECT_ROOT = Path(__file__).resolve().parents[4]
BITGET_SCRIPT_DIR = PROJECT_ROOT / "scripts" / "bitget" / "trading"
LEAD_LAG_SCRIPT_DIR = PROJECT_ROOT / "scripts" / "lead_lag"
for script_dir in (BITGET_SCRIPT_DIR, LEAD_LAG_SCRIPT_DIR):
    if str(script_dir) not in sys.path:
        sys.path.insert(0, str(script_dir))

import prepare_gateway_smoke_run as prepare
import run_gateway_smoke_with_guard as pipeline
import run_live_with_guard as guard


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class FakeLaunchedProcesses:
    def __init__(self):
        self.pids = {
            "data_session": 3101,
            "gateway": 3102,
            "feedback": 3103,
        }
        self.logs_closed = False

    def close_logs(self):
        self.logs_closed = True


class FakePopenProcess:
    next_pid = 4100

    def __init__(self):
        type(self).next_pid += 1
        self.pid = type(self).next_pid

    def poll(self):
        return None


class FakeExitedPopenProcess:
    pid = 4201

    def __init__(self):
        self.poll_calls = 0

    def poll(self):
        self.poll_calls += 1
        return 0


class FakeAlwaysRunningController:
    def __init__(self):
        self.is_running_calls = 0

    def is_running(self, role, binding):
        del role, binding
        self.is_running_calls += 1
        return True

    def send_signal(self, role, binding, signum):
        del role, binding, signum


def flat_state():
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(
                contract="BTC_USDT", size=0, pending_orders=0
            )
        ],
        open_orders=[],
    )


def nonflat_state():
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(
                contract="BTC_USDT", size=1, pending_orders=0
            )
        ],
        open_orders=[],
    )


class RunGatewaySmokeWithGuardTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = TemporaryDirectory(dir="/home/liuxiang/tmp")
        self.run_dir = Path(self.temp_dir.name)
        self.manifest = self.run_dir / "manifest.json"
        self.manifest.write_text(
            json.dumps(
                {
                    "schema": prepare.MANIFEST_SCHEMA,
                    "run_id": self.run_dir.name,
                    "external_configs_applied": False,
                }
            ),
            encoding="utf-8",
        )
        self.runtime = prepare.PreparedRuntimeConfigs(
            run_id=self.run_dir.name,
            run_dir=self.run_dir,
            data_session_config=self.run_dir / "data.toml",
            gateway_config=self.run_dir / "gateway.toml",
            feedback_config=self.run_dir / "feedback.toml",
            order_session_config=self.run_dir / "order_session.toml",
            smoke_config=self.run_dir / "smoke.toml",
            manifest=self.manifest,
            market_data_shm="market",
            gateway_shm="gateway",
            feedback_shm="feedback",
            data_session_log=self.run_dir / "data.log",
            gateway_log=self.run_dir / "gateway.log",
            feedback_log=self.run_dir / "feedback.log",
            client_oid_run_namespace="0123456789AB",
        )
        self.binaries = pipeline.BinaryPaths(
            data_session=Path("/bin/bitget_data_session"),
            gateway=Path("/bin/bitget_order_gateway"),
            feedback=Path("/bin/bitget_order_feedback_session"),
            smoke=Path("/bin/bitget_gateway_smoke"),
        )
        self.events = []
        self.launched = FakeLaunchedProcesses()

    def tearDown(self):
        self.temp_dir.cleanup()

    def state_reader(self, requester, settle, contracts):
        del requester, settle, contracts
        phase = "preflight" if "launch" not in self.events else "final"
        self.events.append(phase)
        return flat_state()

    def process_launcher(self, runtime, binaries):
        del runtime, binaries
        self.events.append("launch")
        return self.launched

    def process_attester(self, runtime, launched, proc_root):
        del runtime, launched, proc_root
        self.events.append("attest")
        return {
            "schema": prepare.MANIFEST_SCHEMA,
            "run_id": self.runtime.run_id,
            "external_configs_applied": True,
            "processes": {},
        }

    def readiness_waiter(self, runtime, launched, timeout_sec, clock):
        del runtime, launched, timeout_sec, clock
        self.events.append("ready")

    def smoke_runner(self, command):
        self.events.append("smoke")
        self.assertEqual(command[-1], "--execute")
        return guard.ProcessResult(exit_code=0)

    def evidence_validator(self, runtime):
        del runtime
        self.events.append("evidence")

    def process_quiescer(self, runtime, launched, clock, proc_root):
        del runtime, launched, clock, proc_root
        self.events.append("quiesce")
        return True, {"ok": True, "result": "stopped"}

    def flatten_runner(self, config, requester, clock):
        del config, requester, clock
        self.events.append("flatten")
        return guard.FLATTEN_EXIT_OK, {"ok": True, "result": "verified_flat"}

    def execute_pipeline(self, **overrides):
        arguments = {
            "runtime": self.runtime,
            "binaries": self.binaries,
            "requester": object(),
            "credential_env_names": (
                "BITGET_TEST_KEY",
                "BITGET_TEST_SECRET",
                "BITGET_TEST_PASSPHRASE",
            ),
            "process_launcher": self.process_launcher,
            "process_attester": self.process_attester,
            "readiness_waiter": self.readiness_waiter,
            "smoke_runner": self.smoke_runner,
            "evidence_validator": self.evidence_validator,
            "process_quiescer": self.process_quiescer,
            "state_reader": self.state_reader,
            "flatten_runner": self.flatten_runner,
            "flatten_config_builder": lambda config: config,
            "clock": FakeClock(),
            "proc_root": self.run_dir / "proc",
        }
        arguments.update(overrides)
        return pipeline.run_prepared_pipeline(**arguments)

    def test_flat_preflight_happens_before_process_launch(self):
        exit_code, summary = self.execute_pipeline()

        self.assertEqual(exit_code, guard.EXIT_OK)
        self.assertEqual(summary["result"], "normal_exit_flat")
        self.assertEqual(
            self.events,
            [
                "preflight",
                "launch",
                "attest",
                "ready",
                "smoke",
                "evidence",
                "quiesce",
                "final",
            ],
        )
        self.assertTrue(self.launched.logs_closed)

    def test_feedback_ready_timeout_never_runs_smoke_and_flattens(self):
        def timeout(*args):
            del args
            self.events.append("ready")
            raise TimeoutError("feedback not ready")

        exit_code, summary = self.execute_pipeline(readiness_waiter=timeout)

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertEqual(summary["result"], "strategy_exception_flattened")
        self.assertNotIn("smoke", self.events)
        self.assertEqual(
            self.events,
            ["preflight", "launch", "attest", "ready", "quiesce", "flatten"],
        )

    def test_partial_launch_failure_quiesces_before_flatten(self):
        def fail_after_partial_launch(runtime, binaries):
            del runtime, binaries
            self.events.append("launch")
            raise pipeline.ProcessLaunchError(
                self.launched, RuntimeError("feedback spawn failed")
            )

        exit_code, summary = self.execute_pipeline(
            process_launcher=fail_after_partial_launch
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertEqual(summary["result"], "strategy_exception_flattened")
        self.assertEqual(
            self.events, ["preflight", "launch", "quiesce", "flatten"]
        )
        self.assertTrue(self.launched.logs_closed)

    def test_invalid_runner_evidence_quiesces_before_flatten(self):
        def reject_evidence(runtime):
            del runtime
            self.events.append("evidence")
            raise ValueError("summary.json missing")

        exit_code, summary = self.execute_pipeline(
            evidence_validator=reject_evidence
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertEqual(summary["result"], "strategy_exception_flattened")
        self.assertEqual(
            self.events,
            [
                "preflight",
                "launch",
                "attest",
                "ready",
                "smoke",
                "evidence",
                "quiesce",
                "flatten",
            ],
        )

    def test_nonflat_preflight_never_launches_or_flattens(self):
        def read_nonflat(requester, settle, contracts):
            del requester, settle, contracts
            self.events.append("preflight")
            return nonflat_state()

        exit_code, summary = self.execute_pipeline(state_reader=read_nonflat)

        self.assertEqual(exit_code, guard.EXIT_PREFLIGHT_FAILED)
        self.assertEqual(summary["result"], "preflight_not_flat")
        self.assertEqual(self.events, ["preflight"])
        self.assertFalse(self.launched.logs_closed)

    def test_wait_for_feedback_ready_accepts_explicit_subscribe_ack(self):
        (self.runtime.run_dir / "feedback.stdout.log").write_text(
            pipeline.FEEDBACK_READY_MARKER + "\n", encoding="utf-8"
        )
        launched = pipeline.LaunchedProcesses(
            processes={
                "data_session": FakePopenProcess(),
                "gateway": FakePopenProcess(),
                "feedback": FakePopenProcess(),
            },
            log_files=[],
        )

        pipeline.wait_for_feedback_ready(
            self.runtime, launched, timeout_sec=1.0, clock=FakeClock()
        )

    def test_runner_evidence_rejects_missing_artifacts(self):
        with self.assertRaisesRegex(ValueError, "summary.json"):
            pipeline.validate_runner_evidence(self.runtime)

    def test_runner_evidence_accepts_no_fill_ack_and_terminal(self):
        entry_id = 504403158265495553
        (self.runtime.run_dir / "summary.json").write_text(
            json.dumps(
                {
                    "run_id": self.runtime.run_id,
                    "final_result": "no_fill",
                    "failure_reason": "none",
                    "entry_local_order_id": entry_id,
                    "entry_acked": True,
                    "entry_terminal": True,
                    "entry_filled_quantity": 0,
                    "close_required": False,
                    "close_local_order_id": 0,
                    "close_acked": False,
                    "close_terminal": False,
                    "close_filled_quantity": 0,
                }
            ),
            encoding="utf-8",
        )
        with (self.runtime.run_dir / "order_event.csv").open(
            "w", newline="", encoding="utf-8"
        ) as output:
            writer = csv.DictWriter(
                output, fieldnames=pipeline.ORDER_EVENT_FIELDS
            )
            writer.writeheader()
            writer.writerows(
                [
                    {
                        "run_id": self.runtime.run_id,
                        "event_source": "runner",
                        "event_kind": "order_submitted",
                        "order_role": "entry",
                        "local_order_id": entry_id,
                        "quantity": "0.0001",
                    },
                    {
                        "run_id": self.runtime.run_id,
                        "event_source": "feedback",
                        "event_kind": "feedback_terminal",
                        "order_role": "entry",
                        "local_order_id": entry_id,
                        "feedback_kind": "cancelled",
                    },
                    {
                        "run_id": self.runtime.run_id,
                        "event_source": "gateway",
                        "event_kind": "gateway_response",
                        "order_role": "entry",
                        "local_order_id": entry_id,
                        "response_kind": "ack",
                    },
                ]
            )

        pipeline.validate_runner_evidence(self.runtime)

    def test_bound_controller_reaps_exited_popen_before_proc_check(self):
        process = FakeExitedPopenProcess()
        launched = pipeline.LaunchedProcesses(
            processes={"feedback": process}, log_files=[]
        )
        delegate = FakeAlwaysRunningController()
        controller = pipeline.ReapingBoundProcessController(
            launched, delegate
        )

        self.assertFalse(
            controller.is_running(
                "feedback", {"pid": process.pid, "start_time_ticks": 1}
            )
        )
        self.assertEqual(process.poll_calls, 1)
        self.assertEqual(delegate.is_running_calls, 0)

    def test_launch_bound_processes_uses_absolute_configs_and_connect(self):
        calls = []

        def popen(command, **kwargs):
            calls.append((list(command), kwargs))
            return FakePopenProcess()

        launched = pipeline.launch_bound_processes(
            self.runtime,
            self.binaries,
            feedback_duration_sec=90,
            popen_factory=popen,
        )
        try:
            self.assertEqual(
                [Path(call[0][0]).name for call in calls],
                [
                    "bitget_data_session",
                    "bitget_order_feedback_session",
                    "bitget_order_gateway",
                ],
            )
            for command, kwargs in calls:
                self.assertIn("--connect", command)
                config_index = command.index("--config") + 1
                self.assertTrue(Path(command[config_index]).is_absolute())
                self.assertEqual(kwargs["cwd"], pipeline.PROJECT_ROOT)
            self.assertIn("--duration-s", calls[1][0])
            self.assertIn("90", calls[1][0])
        finally:
            launched.close_logs()

    def test_existing_bitget_gateway_process_blocks_fresh_run(self):
        proc_root = self.run_dir / "conflict-proc"
        process_dir = proc_root / "5101"
        process_dir.mkdir(parents=True)
        binary = self.run_dir / "bin" / "bitget_order_gateway"
        binary.parent.mkdir(parents=True)
        binary.touch()
        (process_dir / "exe").symlink_to(binary)
        (process_dir / "cmdline").write_bytes(
            f"{binary}\0--connect\0".encode("utf-8")
        )

        with self.assertRaisesRegex(ValueError, "conflicting live process"):
            pipeline.assert_no_conflicting_live_processes(proc_root=proc_root)


if __name__ == "__main__":
    unittest.main()
