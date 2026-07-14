#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO, Callable


PROJECT_ROOT = Path(__file__).resolve().parents[3]
LEAD_LAG_SCRIPT_DIR = PROJECT_ROOT / "scripts" / "lead_lag"
if str(LEAD_LAG_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(LEAD_LAG_SCRIPT_DIR))

import prepare_gateway_smoke_run as prepare
import run_live_with_guard as guard


FEEDBACK_READY_MARKER = "bitget_order_feedback_subscribe accepted=true code=0"
DEFAULT_FEEDBACK_READY_TIMEOUT_SEC = 30.0
DEFAULT_FEEDBACK_DURATION_SEC = 180
CONFLICTING_EXECUTABLES = {
    "lead_lag_strategy",
    "bitget_data_session",
    "bitget_order_gateway",
    "bitget_order_feedback_session",
    "bitget_gateway_smoke",
    "bitget_order_session_probe",
    "bitget_order_session_rtt_probe",
}


@dataclass(frozen=True)
class BinaryPaths:
    data_session: Path
    gateway: Path
    feedback: Path
    smoke: Path


@dataclass
class LaunchedProcesses:
    processes: dict[str, subprocess.Popen[Any]]
    log_files: list[BinaryIO]

    @property
    def pids(self) -> dict[str, int]:
        return {role: process.pid for role, process in self.processes.items()}

    def close_logs(self) -> None:
        for log_file in self.log_files:
            log_file.close()
        self.log_files.clear()


class ReapingBoundProcessController:
    def __init__(self, launched: LaunchedProcesses, delegate: Any):
        self.launched = launched
        self.delegate = delegate

    def is_running(self, role: str, binding: dict[str, Any]) -> bool:
        process = self.launched.processes.get(role)
        if process is not None and process.pid == binding.get("pid"):
            if process.poll() is not None:
                return False
        return self.delegate.is_running(role, binding)

    def send_signal(
        self, role: str, binding: dict[str, Any], signum: int
    ) -> None:
        self.delegate.send_signal(role, binding, signum)


def find_conflicting_live_processes(
    proc_root: Path = Path("/proc"), own_pid: int | None = None
) -> list[dict[str, Any]]:
    own_pid = os.getpid() if own_pid is None else own_pid
    conflicts: list[dict[str, Any]] = []
    for process_dir in proc_root.iterdir():
        try:
            pid = int(process_dir.name)
        except ValueError:
            continue
        if pid == own_pid:
            continue
        try:
            executable = (process_dir / "exe").resolve(strict=True)
        except (FileNotFoundError, PermissionError, OSError):
            continue
        if executable.name not in CONFLICTING_EXECUTABLES:
            continue
        try:
            command = [
                value.decode("utf-8", errors="replace")
                for value in (process_dir / "cmdline").read_bytes().split(b"\0")
                if value
            ]
        except (FileNotFoundError, PermissionError, OSError):
            command = [str(executable)]
        conflicts.append(
            {
                "pid": pid,
                "executable": executable.name,
                "command": command,
            }
        )
    return sorted(conflicts, key=lambda item: item["pid"])


def assert_no_conflicting_live_processes(
    proc_root: Path = Path("/proc"), own_pid: int | None = None
) -> None:
    conflicts = find_conflicting_live_processes(proc_root, own_pid)
    if not conflicts:
        return
    descriptions = ", ".join(
        f"pid={item['pid']} executable={item['executable']}"
        for item in conflicts
    )
    raise ValueError(f"conflicting live process detected: {descriptions}")


def validate_binary_paths(binaries: BinaryPaths) -> BinaryPaths:
    resolved: dict[str, Path] = {}
    expected_names = {
        "data_session": "bitget_data_session",
        "gateway": "bitget_order_gateway",
        "feedback": "bitget_order_feedback_session",
        "smoke": "bitget_gateway_smoke",
    }
    for role, path in (
        ("data_session", binaries.data_session),
        ("gateway", binaries.gateway),
        ("feedback", binaries.feedback),
        ("smoke", binaries.smoke),
    ):
        candidate = path.expanduser().resolve()
        if candidate.name != expected_names[role]:
            raise ValueError(
                f"{role} binary must be named {expected_names[role]}"
            )
        if not candidate.is_file() or not os.access(candidate, os.X_OK):
            raise ValueError(f"{role} binary is not executable: {candidate}")
        resolved[role] = candidate
    return BinaryPaths(
        data_session=resolved["data_session"],
        gateway=resolved["gateway"],
        feedback=resolved["feedback"],
        smoke=resolved["smoke"],
    )


def stop_unbound_processes(
    launched: LaunchedProcesses,
    term_timeout_sec: float = 3.0,
) -> tuple[bool, dict[str, Any]]:
    summary: dict[str, Any] = {"ok": False, "processes": {}}
    ok = True
    for role in ("gateway", "feedback", "data_session"):
        process = launched.processes.get(role)
        if process is None:
            continue
        process_summary: dict[str, Any] = {
            "pid": process.pid,
            "signals": [],
            "result": "not_started",
        }
        if process.poll() is None:
            process.terminate()
            process_summary["signals"].append("SIGTERM")
            try:
                process.wait(timeout=term_timeout_sec)
            except subprocess.TimeoutExpired:
                process.kill()
                process_summary["signals"].append("SIGKILL")
                try:
                    process.wait(timeout=1.0)
                except subprocess.TimeoutExpired:
                    process_summary["result"] = "still_running"
                    ok = False
        if process_summary["result"] == "not_started":
            process_summary["result"] = "stopped"
        summary["processes"][role] = process_summary
    summary["ok"] = ok
    summary["result"] = "stopped" if ok else "stop_failed"
    return ok, summary


def launch_bound_processes(
    runtime: prepare.PreparedRuntimeConfigs,
    binaries: BinaryPaths,
    feedback_duration_sec: int = DEFAULT_FEEDBACK_DURATION_SEC,
    popen_factory: Any = subprocess.Popen,
) -> LaunchedProcesses:
    if feedback_duration_sec <= 0:
        raise ValueError("feedback_duration_sec must be positive")
    launched = LaunchedProcesses(processes={}, log_files=[])
    commands = (
        (
            "data_session",
            [
                str(binaries.data_session),
                "--config",
                str(runtime.data_session_config),
                "--connect",
            ],
        ),
        (
            "feedback",
            [
                str(binaries.feedback),
                "--config",
                str(runtime.feedback_config),
                "--connect",
                "--duration-s",
                str(feedback_duration_sec),
            ],
        ),
        (
            "gateway",
            [
                str(binaries.gateway),
                "--config",
                str(runtime.gateway_config),
                "--connect",
            ],
        ),
    )
    try:
        for role, command in commands:
            output_path = runtime.run_dir / f"{role}.stdout.log"
            output_file = output_path.open("ab", buffering=0)
            launched.log_files.append(output_file)
            launched.processes[role] = popen_factory(
                command,
                cwd=PROJECT_ROOT,
                stdin=subprocess.DEVNULL,
                stdout=output_file,
                stderr=subprocess.STDOUT,
            )
        return launched
    except Exception:
        stop_unbound_processes(launched)
        launched.close_logs()
        raise


def wait_for_feedback_ready(
    runtime: prepare.PreparedRuntimeConfigs,
    launched: LaunchedProcesses,
    timeout_sec: float,
    clock: Any,
) -> None:
    if timeout_sec <= 0:
        raise ValueError("feedback ready timeout must be positive")
    feedback_stdout = runtime.run_dir / "feedback.stdout.log"
    deadline = clock.time() + timeout_sec
    while clock.time() < deadline:
        for role, process in launched.processes.items():
            exit_code = process.poll()
            if exit_code is not None:
                raise RuntimeError(
                    f"{role} exited before feedback ready with code {exit_code}"
                )
        try:
            text = feedback_stdout.read_text(
                encoding="utf-8", errors="replace"
            )
        except FileNotFoundError:
            text = ""
        if FEEDBACK_READY_MARKER in text:
            return
        clock.sleep(min(0.05, deadline - clock.time()))
    raise TimeoutError("Bitget order feedback session did not reach ready")


def attest_launched_processes(
    runtime: prepare.PreparedRuntimeConfigs,
    launched: LaunchedProcesses,
    proc_root: Path,
    runner_command: list[str],
) -> dict[str, Any]:
    pids = launched.pids
    prepare.mark_processes_applied(
        runtime.manifest,
        data_session_pid=pids["data_session"],
        gateway_pid=pids["gateway"],
        feedback_pid=pids["feedback"],
        proc_root=proc_root,
    )
    return prepare.validate_manifest(
        runtime.manifest,
        runner_command=runner_command,
        require_applied=True,
        proc_root=proc_root,
    )


def quiesce_pipeline_processes(
    runtime: prepare.PreparedRuntimeConfigs,
    launched: LaunchedProcesses,
    clock: Any,
    proc_root: Path,
) -> tuple[bool, dict[str, Any]]:
    manifest = prepare.read_manifest(runtime.manifest)
    if manifest.get("external_configs_applied") is True and isinstance(
        manifest.get("processes"), dict
    ):
        controller = ReapingBoundProcessController(
            launched, guard.LinuxBoundProcessController(proc_root)
        )
        return guard.quiesce_bitget_processes(
            manifest,
            controller=controller,
            clock=clock,
        )
    return stop_unbound_processes(launched)


def atomic_write_summary(path: Path, summary: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def run_prepared_pipeline(
    *,
    runtime: prepare.PreparedRuntimeConfigs,
    binaries: BinaryPaths,
    requester: Any,
    credential_env_names: tuple[str, str, str],
    process_launcher: Callable[[prepare.PreparedRuntimeConfigs, BinaryPaths], Any],
    process_attester: Callable[[prepare.PreparedRuntimeConfigs, Any, Path], dict[str, Any]],
    readiness_waiter: Callable[[prepare.PreparedRuntimeConfigs, Any, float, Any], None],
    smoke_runner: Callable[[list[str]], guard.ProcessResult],
    process_quiescer: Callable[
        [prepare.PreparedRuntimeConfigs, Any, Any, Path], tuple[bool, dict[str, Any]]
    ],
    state_reader: Callable[..., guard.GuardState],
    flatten_runner: Callable[..., tuple[int, dict[str, Any]]],
    flatten_config_builder: Callable[[guard.GuardConfig], Any],
    clock: Any,
    proc_root: Path,
    feedback_ready_timeout_sec: float = DEFAULT_FEEDBACK_READY_TIMEOUT_SEC,
) -> tuple[int, dict[str, Any]]:
    runner_command = [
        str(binaries.smoke),
        "--config",
        str(runtime.smoke_config),
        "--execute",
    ]
    runtime_isolation = prepare.read_manifest(runtime.manifest)
    launched: Any | None = None

    def run_smoke_after_launch(_: list[str]) -> guard.ProcessResult:
        nonlocal launched
        launched = process_launcher(runtime, binaries)
        applied = process_attester(runtime, launched, proc_root)
        runtime_isolation.clear()
        runtime_isolation.update(applied)
        readiness_waiter(
            runtime, launched, feedback_ready_timeout_sec, clock
        )
        return smoke_runner(runner_command)

    def stop_pipeline_processes(current_clock: Any) -> tuple[bool, dict[str, Any]]:
        if launched is None:
            return True, {"ok": True, "result": "not_started", "processes": {}}
        return process_quiescer(runtime, launched, current_clock, proc_root)

    config = guard.GuardConfig(
        settle="usdt",
        contracts=["BTC_USDT"],
        strategy_command=runner_command,
        exchange="bitget",
        run_id=runtime.run_id,
        api_key_env=credential_env_names[0],
        api_secret_env=credential_env_names[1],
        api_passphrase_env=credential_env_names[2],
        credential_source="gateway_smoke_manifest",
        runtime_isolation=runtime_isolation,
    )
    try:
        exit_code, summary = guard.run_guarded_live(
            config=config,
            requester=requester,
            process_runner=run_smoke_after_launch,
            flatten_runner=flatten_runner,
            flatten_config_builder=flatten_config_builder,
            state_reader=state_reader,
            clock=clock,
            process_quiescer=stop_pipeline_processes,
        )
        try:
            atomic_write_summary(runtime.run_dir / "guard_summary.json", summary)
        except Exception as exc:
            summary["ok"] = False
            summary["result"] = "guard_summary_write_failed"
            summary["exit_code"] = guard.EXIT_CONFIG_ERROR
            summary["errors"].append(
                f"guard summary write failed: {type(exc).__name__}: {exc}"
            )
            exit_code = guard.EXIT_CONFIG_ERROR
        return exit_code, summary
    finally:
        if launched is not None:
            launched.close_logs()


def credential_env_names_from_gateway(gateway_config: Path) -> tuple[str, str, str]:
    return guard.bitget_order_gateway_credential_env_names(gateway_config)


def require_credentials(
    names: tuple[str, str, str]
) -> tuple[str, str, str]:
    values: list[str] = []
    for name in names:
        value = os.getenv(name)
        if not value:
            raise ValueError(f"missing credential environment variable {name}")
        values.append(value)
    return values[0], values[1], values[2]


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run one fresh Bitget gateway smoke under strict stop-and-flat."
    )
    parser.add_argument("--run-id", required=True)
    parser.add_argument(
        "--data-session-config",
        type=Path,
        default=PROJECT_ROOT / "config/data_sessions/bitget_gateway_smoke.toml",
    )
    parser.add_argument(
        "--gateway-config",
        type=Path,
        default=PROJECT_ROOT / "config/order_gateways/bitget_order_gateway.toml",
    )
    parser.add_argument(
        "--feedback-config",
        type=Path,
        default=(
            PROJECT_ROOT
            / "config/order_feedback/bitget_order_feedback_session.toml"
        ),
    )
    parser.add_argument(
        "--smoke-config",
        type=Path,
        default=(
            PROJECT_ROOT
            / "config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml"
        ),
    )
    parser.add_argument(
        "--data-session-binary",
        type=Path,
        default=PROJECT_ROOT / "build/release/tools/bitget_data_session",
    )
    parser.add_argument(
        "--gateway-binary",
        type=Path,
        default=PROJECT_ROOT / "build/release/tools/bitget_order_gateway",
    )
    parser.add_argument(
        "--feedback-binary",
        type=Path,
        default=PROJECT_ROOT / "build/release/tools/bitget_order_feedback_session",
    )
    parser.add_argument(
        "--smoke-binary",
        type=Path,
        default=PROJECT_ROOT / "build/release/tools/bitget_gateway_smoke",
    )
    parser.add_argument(
        "--feedback-ready-timeout-sec",
        type=float,
        default=DEFAULT_FEEDBACK_READY_TIMEOUT_SEC,
    )
    parser.add_argument(
        "--feedback-duration-sec", type=int, default=DEFAULT_FEEDBACK_DURATION_SEC
    )
    parser.add_argument("--rest-timeout-sec", type=float, default=10.0)
    parser.add_argument("--execute", action="store_true")
    parser.add_argument(
        "--pretty", action=argparse.BooleanOptionalAction, default=True
    )
    return parser.parse_args(argv)


def run_from_args(args: argparse.Namespace) -> tuple[int, dict[str, Any]]:
    if not args.execute:
        raise ValueError("--execute is required; gateway smoke has no resume mode")
    assert_no_conflicting_live_processes()
    binaries = validate_binary_paths(
        BinaryPaths(
            data_session=args.data_session_binary,
            gateway=args.gateway_binary,
            feedback=args.feedback_binary,
            smoke=args.smoke_binary,
        )
    )
    credential_env_names = credential_env_names_from_gateway(args.gateway_config)
    api_key, api_secret, api_passphrase = require_credentials(
        credential_env_names
    )
    runtime = prepare.prepare_runtime_configs(
        run_id=args.run_id,
        data_session_source=args.data_session_config,
        gateway_source=args.gateway_config,
        feedback_source=args.feedback_config,
        smoke_source=args.smoke_config,
        output_dir=prepare.expected_config_dir(args.run_id),
    )
    adapter = guard.bitget_guard_exchange_adapter()
    requester = adapter.requester_factory(
        api_key,
        api_secret,
        api_passphrase,
        guard.default_rest_base_url("bitget"),
        args.rest_timeout_sec,
    )
    clock = guard.SystemClock()
    runner_command = [
        str(binaries.smoke),
        "--config",
        str(runtime.smoke_config),
        "--execute",
    ]

    def launcher(
        prepared: prepare.PreparedRuntimeConfigs, paths: BinaryPaths
    ) -> LaunchedProcesses:
        return launch_bound_processes(
            prepared,
            paths,
            feedback_duration_sec=args.feedback_duration_sec,
        )

    def attester(
        prepared: prepare.PreparedRuntimeConfigs,
        launched: LaunchedProcesses,
        proc_root: Path,
    ) -> dict[str, Any]:
        return attest_launched_processes(
            prepared, launched, proc_root, runner_command
        )

    return run_prepared_pipeline(
        runtime=runtime,
        binaries=binaries,
        requester=requester,
        credential_env_names=credential_env_names,
        process_launcher=launcher,
        process_attester=attester,
        readiness_waiter=wait_for_feedback_ready,
        smoke_runner=guard.run_strategy_process,
        process_quiescer=quiesce_pipeline_processes,
        state_reader=adapter.state_reader,
        flatten_runner=adapter.flatten_runner,
        flatten_config_builder=adapter.flatten_config_builder,
        clock=clock,
        proc_root=Path("/proc"),
        feedback_ready_timeout_sec=args.feedback_ready_timeout_sec,
    )


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        exit_code, summary = run_from_args(args)
    except Exception as exc:
        exit_code = guard.EXIT_CONFIG_ERROR
        summary = {
            "ok": False,
            "result": "config_error",
            "exit_code": exit_code,
            "errors": [f"{type(exc).__name__}: {exc}"],
        }
    print(
        json.dumps(
            summary,
            ensure_ascii=False,
            indent=2 if args.pretty else None,
            sort_keys=True,
        )
    )
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
