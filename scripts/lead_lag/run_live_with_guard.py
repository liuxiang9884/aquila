#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import signal
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


GATE_SCRIPT_DIR = Path(__file__).resolve().parents[1] / "gate"
if str(GATE_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(GATE_SCRIPT_DIR))

import emergency_flatten_futures as flatten  # noqa: E402
import query_gate_account as account  # noqa: E402
from place_futures_order import SignedGateTradingClient  # noqa: E402


EXIT_OK = 0
EXIT_CONFIG_ERROR = 2
EXIT_PREFLIGHT_FAILED = 3
EXIT_REST_FAILED = 4
EXIT_EMERGENCY_FLATTENED = 10
EXIT_EMERGENCY_FAILED = 11

STRATEGY_EXIT_CONTINUITY_LOST = 10

FLATTEN_EXIT_OK = flatten.EXIT_OK
FLATTEN_EXIT_NOT_FLAT = flatten.EXIT_NOT_FLAT

PositionSnapshot = flatten.PositionSnapshot
OpenOrder = flatten.OpenOrder

Requester = Callable[[Any], Any]
ProcessRunner = Callable[[list[str]], "ProcessResult"]
StateReader = Callable[[Requester, str, list[str]], "GuardState"]
FlattenRunner = Callable[[flatten.FlattenConfig, Requester, Any], tuple[int, dict[str, Any]]]


@dataclass(frozen=True)
class GuardConfig:
    settle: str
    contracts: list[str]
    strategy_command: list[str]
    poll_timeout_sec: float = flatten.DEFAULT_POLL_TIMEOUT_SEC
    poll_interval_sec: float = flatten.DEFAULT_POLL_INTERVAL_SEC


@dataclass(frozen=True)
class GuardState:
    positions: list[PositionSnapshot]
    open_orders: list[OpenOrder]

    def flat(self) -> bool:
        return flatten.final_state_is_flat(self.positions, self.open_orders)

    def to_summary(self) -> dict[str, Any]:
        return {
            "flat": self.flat(),
            "positions": [position.to_summary() for position in self.positions],
            "open_orders": [order.to_summary() for order in self.open_orders],
        }


@dataclass(frozen=True)
class ProcessResult:
    exit_code: int
    signal_number: int | None = None

    def to_summary(self) -> dict[str, Any]:
        return {
            "exit_code": self.exit_code,
            "signal_number": self.signal_number,
        }


class SystemClock(flatten.SystemClock):
    pass


def normalize_contracts(contracts: list[str]) -> list[str]:
    return flatten.normalize_contracts(contracts)


def validate_config(config: GuardConfig) -> None:
    flatten.normalize_settle(config.settle)
    if not normalize_contracts(config.contracts):
        raise ValueError("at least one --contract is required")
    if not config.strategy_command:
        raise ValueError("strategy command is required after --")
    if config.poll_timeout_sec < 0:
        raise ValueError("--poll-timeout-sec must be non-negative")
    if config.poll_interval_sec <= 0:
        raise ValueError("--poll-interval-sec must be positive")


def query_guard_state(
    requester: Requester,
    settle: str,
    contracts: list[str],
) -> GuardState:
    return GuardState(
        positions=flatten.query_positions(requester, settle, contracts),
        open_orders=flatten.query_open_orders(requester, settle, contracts),
    )


def flatten_config_from_guard(config: GuardConfig) -> flatten.FlattenConfig:
    return flatten.FlattenConfig(
        settle=config.settle,
        scope="allowlist",
        contracts=list(config.contracts),
        confirm_dedicated_account=False,
        dry_run=False,
        poll_timeout_sec=config.poll_timeout_sec,
        poll_interval_sec=config.poll_interval_sec,
        max_position_count=flatten.DEFAULT_MAX_POSITION_COUNT,
    )


def run_emergency_flatten(
    config: flatten.FlattenConfig,
    requester: Requester,
    clock: Any,
) -> tuple[int, dict[str, Any]]:
    return flatten.run_emergency_flatten(config=config, requester=requester, clock=clock)


def run_strategy_process(command: list[str]) -> ProcessResult:
    process = subprocess.Popen(command)
    received_signal: int | None = None
    old_sigint = signal.getsignal(signal.SIGINT)
    old_sigterm = signal.getsignal(signal.SIGTERM)

    def stop_child(signum: int, frame: Any) -> None:
        del frame
        nonlocal received_signal
        received_signal = signum
        if process.poll() is None:
            process.terminate()

    signal.signal(signal.SIGINT, stop_child)
    signal.signal(signal.SIGTERM, stop_child)
    try:
        exit_code = process.wait()
        if received_signal is not None and exit_code == 0:
            exit_code = 128 + received_signal
        return ProcessResult(exit_code=exit_code, signal_number=received_signal)
    finally:
        signal.signal(signal.SIGINT, old_sigint)
        signal.signal(signal.SIGTERM, old_sigterm)


def initial_summary(config: GuardConfig) -> dict[str, Any]:
    return {
        "ok": False,
        "result": "not_started",
        "settle": flatten.normalize_settle(config.settle),
        "contracts": list(config.contracts),
        "strategy_command": list(config.strategy_command),
        "preflight": None,
        "strategy": None,
        "final_check": None,
        "flatten": None,
        "errors": [],
    }


def run_flatten_for_reason(
    summary: dict[str, Any],
    config: GuardConfig,
    requester: Requester,
    clock: Any,
    reason: str,
    flatten_runner: FlattenRunner,
) -> tuple[int, dict[str, Any]]:
    flatten_exit_code, flatten_summary = flatten_runner(
        flatten_config_from_guard(config), requester, clock
    )
    summary["flatten"] = flatten_summary
    if flatten_exit_code == flatten.EXIT_OK and flatten_summary.get("ok") is True:
        summary["ok"] = False
        summary["result"] = f"{reason}_flattened"
        summary["exit_code"] = EXIT_EMERGENCY_FLATTENED
        return EXIT_EMERGENCY_FLATTENED, summary

    summary["ok"] = False
    summary["result"] = f"{reason}_flatten_failed"
    summary["exit_code"] = EXIT_EMERGENCY_FAILED
    summary["errors"].append(
        f"emergency flatten failed with exit_code={flatten_exit_code}"
    )
    return EXIT_EMERGENCY_FAILED, summary


def run_guarded_live(
    config: GuardConfig,
    requester: Requester,
    process_runner: ProcessRunner = run_strategy_process,
    flatten_runner: FlattenRunner = run_emergency_flatten,
    state_reader: StateReader = query_guard_state,
    clock: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    clock = SystemClock() if clock is None else clock
    config = GuardConfig(
        settle=config.settle,
        contracts=normalize_contracts(config.contracts),
        strategy_command=list(config.strategy_command),
        poll_timeout_sec=config.poll_timeout_sec,
        poll_interval_sec=config.poll_interval_sec,
    )
    summary = initial_summary(config)
    try:
        validate_config(config)
    except ValueError as exc:
        summary["result"] = "config_error"
        summary["exit_code"] = EXIT_CONFIG_ERROR
        summary["errors"].append(str(exc))
        return EXIT_CONFIG_ERROR, summary

    try:
        preflight = state_reader(requester, config.settle, config.contracts)
        summary["preflight"] = preflight.to_summary()
    except Exception as exc:
        summary["result"] = "preflight_rest_failed"
        summary["exit_code"] = EXIT_REST_FAILED
        summary["errors"].append(f"{type(exc).__name__}: {exc}")
        return EXIT_REST_FAILED, summary

    if not preflight.flat():
        summary["result"] = "preflight_not_flat"
        summary["exit_code"] = EXIT_PREFLIGHT_FAILED
        summary["errors"].append("preflight found open orders, positions, or pending orders")
        return EXIT_PREFLIGHT_FAILED, summary

    try:
        process_result = process_runner(config.strategy_command)
    except Exception as exc:
        summary["strategy"] = {"exception": f"{type(exc).__name__}: {exc}"}
        summary["errors"].append("strategy command raised before exit")
        return run_flatten_for_reason(
            summary, config, requester, clock, "strategy_exception", flatten_runner
        )

    summary["strategy"] = process_result.to_summary()
    if process_result.exit_code != 0:
        return run_flatten_for_reason(
            summary, config, requester, clock, "strategy_exit", flatten_runner
        )

    try:
        final_state = state_reader(requester, config.settle, config.contracts)
        summary["final_check"] = final_state.to_summary()
    except Exception as exc:
        summary["errors"].append(f"final REST check failed: {type(exc).__name__}: {exc}")
        return run_flatten_for_reason(
            summary, config, requester, clock, "final_check_rest_failed", flatten_runner
        )

    if not final_state.flat():
        return run_flatten_for_reason(
            summary, config, requester, clock, "final_check", flatten_runner
        )

    summary["ok"] = True
    summary["result"] = "normal_exit_flat"
    summary["exit_code"] = EXIT_OK
    return EXIT_OK, summary


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run LeadLag live orders behind REST preflight and stop-and-flat guardrails."
    )
    parser.add_argument("--api-key", default=account.DEFAULT_API_KEY_ENV)
    parser.add_argument("--api-secret", default=account.DEFAULT_API_SECRET_ENV)
    parser.add_argument("--base-url", default=account.DEFAULT_BASE_URL)
    parser.add_argument("--timeout", type=float, default=account.DEFAULT_TIMEOUT)
    parser.add_argument("--settle", default="usdt")
    parser.add_argument(
        "--contract",
        action="append",
        default=[],
        help="Gate futures contract in the guarded allowlist. Can be repeated.",
    )
    parser.add_argument(
        "--poll-timeout-sec",
        type=float,
        default=flatten.DEFAULT_POLL_TIMEOUT_SEC,
    )
    parser.add_argument(
        "--poll-interval-sec",
        type=float,
        default=flatten.DEFAULT_POLL_INTERVAL_SEC,
    )
    parser.add_argument(
        "--pretty",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Pretty-print JSON summary.",
    )
    parser.add_argument("strategy_command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.strategy_command and args.strategy_command[0] == "--":
        args.strategy_command = args.strategy_command[1:]
    return args


def config_from_args(args: argparse.Namespace) -> GuardConfig:
    return GuardConfig(
        settle=args.settle,
        contracts=normalize_contracts(args.contract),
        strategy_command=list(args.strategy_command),
        poll_timeout_sec=args.poll_timeout_sec,
        poll_interval_sec=args.poll_interval_sec,
    )


def print_summary(summary: dict[str, Any], pretty: bool) -> None:
    indent = 2 if pretty else None
    print(json.dumps(summary, ensure_ascii=False, indent=indent, sort_keys=True))


def missing_env_summary(config: GuardConfig, env_name: str) -> dict[str, Any]:
    summary = initial_summary(config)
    summary["result"] = "config_error"
    summary["exit_code"] = EXIT_CONFIG_ERROR
    summary["errors"].append(f"missing env var {env_name}")
    return summary


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    config = config_from_args(args)

    api_key = account.get_env_value(args.api_key)
    if api_key is None:
        summary = missing_env_summary(config, args.api_key)
        print_summary(summary, args.pretty)
        return EXIT_CONFIG_ERROR
    api_secret = account.get_env_value(args.api_secret)
    if api_secret is None:
        summary = missing_env_summary(config, args.api_secret)
        print_summary(summary, args.pretty)
        return EXIT_CONFIG_ERROR

    client = SignedGateTradingClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=args.base_url,
        timeout=args.timeout,
    )
    exit_code, summary = run_guarded_live(
        config=config,
        requester=client.request_json,
        process_runner=run_strategy_process,
        flatten_runner=run_emergency_flatten,
        state_reader=query_guard_state,
        clock=SystemClock(),
    )
    print_summary(summary, args.pretty)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
