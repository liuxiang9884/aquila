#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import hashlib
import json
import re
import sys
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


PROJECT_ROOT = Path(__file__).resolve().parents[3]
LEAD_LAG_SCRIPT_DIR = PROJECT_ROOT / "scripts" / "lead_lag"
if str(LEAD_LAG_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(LEAD_LAG_SCRIPT_DIR))

import prepare_bitget_live_run as lead_prepare
import run_live_with_guard as guard


MANIFEST_SCHEMA = "aquila.bitget_gateway_smoke_manifest.v1"
RUNNER_EXECUTABLE = "bitget_gateway_smoke"
TMP_ROOT = Path("/home/liuxiang/tmp")
PROC_ROOT = Path("/proc")
RUN_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
PROCESS_SPECS = {
    "data_session": "bitget_data_session",
    "gateway": "bitget_order_gateway",
    "feedback": "bitget_order_feedback_session",
}
CONFIG_KEYS = (
    "data_session_config",
    "gateway_config",
    "feedback_config",
    "smoke_config",
)


@dataclass(frozen=True)
class PreparedRuntimeConfigs:
    run_id: str
    run_dir: Path
    data_session_config: Path
    gateway_config: Path
    feedback_config: Path
    smoke_config: Path
    manifest: Path
    market_data_shm: str
    gateway_shm: str
    feedback_shm: str
    data_session_log: Path
    gateway_log: Path
    feedback_log: Path


def validate_run_id(run_id: str) -> str:
    if (
        not RUN_ID_PATTERN.fullmatch(run_id)
        or run_id in {".", ".."}
        or len(run_id) > 96
    ):
        raise ValueError(
            "run_id must be 1-96 letters, digits, '.', '_', or '-'"
        )
    return run_id


def expected_run_dir(run_id: str) -> Path:
    return (TMP_ROOT / validate_run_id(run_id)).resolve()


def expected_config_dir(run_id: str) -> Path:
    return expected_run_dir(run_id) / "configs"


def load_toml(path: Path) -> dict[str, Any]:
    try:
        value = tomllib.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise ValueError(
            f"run isolation: cannot read TOML {path}: {type(exc).__name__}: {exc}"
        ) from exc
    if not isinstance(value, dict):
        raise ValueError(f"run isolation: TOML {path} must contain a table")
    return value


def required_dict(mapping: dict[str, Any], key: str, label: str) -> dict[str, Any]:
    value = mapping.get(key)
    if not isinstance(value, dict):
        raise ValueError(f"run isolation: missing [{label}]")
    return value


def required_string(mapping: dict[str, Any], key: str, label: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value:
        raise ValueError(f"run isolation: missing {label}.{key}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65_536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def read_manifest(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise ValueError(
            f"run isolation: cannot read manifest {path}: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    if not isinstance(value, dict):
        raise ValueError("run isolation: manifest must be a JSON object")
    return value


def manifest_config_path(
    manifest: dict[str, Any], key: str, config_dir: Path
) -> Path:
    configs = manifest.get("configs")
    if not isinstance(configs, dict):
        raise ValueError("run isolation: manifest missing configs")
    entry = configs.get(key)
    if not isinstance(entry, dict):
        raise ValueError(f"run isolation: manifest missing configs.{key}")
    raw_path = entry.get("path")
    if not isinstance(raw_path, str) or not raw_path:
        raise ValueError(f"run isolation: manifest missing configs.{key}.path")
    path = Path(raw_path).expanduser()
    if not path.is_absolute():
        raise ValueError(f"run isolation: configs.{key}.path must be absolute")
    path = path.resolve()
    if path.parent != config_dir:
        raise ValueError(f"run isolation: configs.{key}.path escaped config dir")
    expected_digest = entry.get("sha256")
    if not isinstance(expected_digest, str) or len(expected_digest) != 64:
        raise ValueError(f"run isolation: manifest missing configs.{key}.sha256")
    if sha256_file(path) != expected_digest:
        raise ValueError(f"run isolation: configs.{key} digest mismatch")
    return path


def process_config_path(command: list[str], role: str) -> Path:
    config_arg = guard.find_strategy_config_arg(command)
    if config_arg is None:
        raise ValueError(f"run isolation: {role} command requires --config")
    path = config_arg[1].expanduser()
    if not path.is_absolute():
        raise ValueError(
            f"run isolation: {role} command requires absolute --config"
        )
    return path.resolve()


def validate_runner_command(command: list[str], smoke_config: Path) -> None:
    if not command or Path(command[0]).name != RUNNER_EXECUTABLE:
        raise ValueError(
            f"run isolation: runner command must launch {RUNNER_EXECUTABLE}"
        )
    if "--execute" not in command:
        raise ValueError("run isolation: runner command requires --execute")
    if "--preflight-only" in command or "--validate-only" in command:
        raise ValueError("run isolation: runner command has conflicting run mode")
    if process_config_path(command, "runner") != smoke_config:
        raise ValueError("run isolation: runner command config mismatch")


def validate_binding(
    binding: Any,
    role: str,
    expected_config: Path,
    proc_root: Path,
) -> dict[str, Any]:
    if not isinstance(binding, dict):
        raise ValueError(f"run isolation: manifest missing process binding {role}")
    expected_executable = PROCESS_SPECS[role]
    if binding.get("executable") != expected_executable:
        raise ValueError(f"run isolation: {role} executable changed")
    if binding.get("config") != str(expected_config):
        raise ValueError(f"run isolation: {role} config binding changed")
    pid = binding.get("pid")
    actual = lead_prepare._build_process_binding(
        proc_root, pid, expected_executable, expected_config
    )
    if binding.get("start_time_ticks") != actual["start_time_ticks"]:
        raise ValueError(f"run isolation: {role} process start time changed")
    return actual


def validate_runtime_configs(
    manifest: dict[str, Any],
    data_path: Path,
    gateway_path: Path,
    feedback_path: Path,
    smoke_path: Path,
) -> tuple[str, str, str]:
    run_id = manifest["run_id"]
    market_data_shm = f"aquila_bitget_market_data_{run_id}"
    gateway_shm = f"aquila_bitget_order_gateway_{run_id}"
    feedback_shm = f"aquila_bitget_order_feedback_{run_id}"
    if manifest.get("market_data_shm") != market_data_shm:
        raise ValueError("run isolation: market_data_shm is not run-specific")
    if manifest.get("gateway_shm") != gateway_shm:
        raise ValueError("run isolation: gateway_shm is not run-specific")
    if manifest.get("feedback_shm") != feedback_shm:
        raise ValueError("run isolation: feedback_shm is not run-specific")
    if manifest.get("route_count") != 1:
        raise ValueError("run isolation: route_count must be 1")
    if manifest.get("contract") != "BTC_USDT":
        raise ValueError("run isolation: contract must be BTC_USDT")
    if manifest.get("runner_executable") != RUNNER_EXECUTABLE:
        raise ValueError("run isolation: runner executable mismatch")

    data = load_toml(data_path)
    data_session = required_dict(data, "data_session", "data_session")
    if data_session.get("subscribe_symbols") != ["BTCUSDT"]:
        raise ValueError("run isolation: data session must only subscribe BTCUSDT")
    if data_session.get("feeds") != ["book_ticker"]:
        raise ValueError("run isolation: data session must only publish book_ticker")
    data_shm = required_dict(data, "data_shm_sink", "data_shm_sink")
    if required_string(data_shm, "shm_name", "data_shm_sink") != market_data_shm:
        raise ValueError("run isolation: data session SHM mismatch")
    if data_shm.get("create") is not True or data_shm.get("remove_existing") is not False:
        raise ValueError("run isolation: data session requires fresh create without removal")

    gateway_data = load_toml(gateway_path)
    gateway = required_dict(gateway_data, "order_gateway", "order_gateway")
    routes = gateway.get("routes")
    if (
        gateway.get("route_count") != 1
        or not isinstance(routes, list)
        or len(routes) != 1
        or not isinstance(routes[0], dict)
    ):
        raise ValueError("run isolation: gateway route_count must be 1")
    if required_string(gateway, "shm_name", "order_gateway") != gateway_shm:
        raise ValueError("run isolation: gateway SHM mismatch")

    feedback_data = load_toml(feedback_path)
    feedback = required_dict(
        feedback_data, "order_feedback_session", "order_feedback_session"
    )
    feedback_runtime = required_dict(
        feedback, "shm", "order_feedback_session.shm"
    )
    if (
        required_string(
            feedback_runtime, "shm_name", "order_feedback_session.shm"
        )
        != feedback_shm
    ):
        raise ValueError("run isolation: feedback SHM mismatch")
    if feedback_runtime.get("create") is not True or feedback_runtime.get(
        "remove_existing"
    ) is not False:
        raise ValueError("run isolation: feedback requires fresh create without removal")

    smoke = load_toml(smoke_path)
    probe = required_dict(smoke, "gateway_smoke", "gateway_smoke")
    if (
        probe.get("run_id") != run_id
        or probe.get("symbol") != "BTC_USDT"
        or probe.get("exchange_symbol") != "BTCUSDT"
        or probe.get("strategy_id") != 7
        or probe.get("side") != "buy"
        or probe.get("quantity") != 0.0001
        or probe.get("route_id") != 0
    ):
        raise ValueError("run isolation: smoke trading contract mismatch")
    smoke_market = required_dict(smoke, "market_data", "market_data")
    smoke_gateway = required_dict(smoke, "order_gateway", "order_gateway")
    smoke_feedback = required_dict(smoke, "feedback", "feedback")
    smoke_output = required_dict(smoke, "output", "output")
    if required_string(smoke_market, "shm_name", "market_data") != market_data_shm:
        raise ValueError("run isolation: smoke market data SHM mismatch")
    if required_string(smoke_gateway, "shm_name", "order_gateway") != gateway_shm:
        raise ValueError("run isolation: smoke gateway SHM mismatch")
    if smoke_gateway.get("route_count") != 1:
        raise ValueError("run isolation: smoke route_count must be 1")
    if required_string(smoke_feedback, "shm_name", "feedback") != feedback_shm:
        raise ValueError("run isolation: smoke feedback SHM mismatch")
    if Path(required_string(smoke_output, "run_dir", "output")).resolve() != expected_run_dir(
        run_id
    ):
        raise ValueError("run isolation: smoke output run_dir mismatch")

    gateway_credentials = guard.bitget_order_gateway_credential_env_names(
        gateway_path
    )
    feedback_credentials = lead_prepare.feedback_credential_env_names(feedback_path)
    if gateway_credentials != feedback_credentials:
        raise ValueError("run isolation: gateway and feedback credentials do not match")
    gateway_contract = lead_prepare._trading_contract(
        lead_prepare._gateway_order_session(gateway), "order_session"
    )
    feedback_contract = lead_prepare._trading_contract(
        feedback, "order_feedback_session"
    )
    if gateway_contract != feedback_contract:
        raise ValueError("run isolation: gateway and feedback trading contract mismatch")
    return market_data_shm, gateway_shm, feedback_shm


def validate_manifest(
    manifest_path: Path,
    runner_command: list[str] | None = None,
    require_applied: bool = True,
    proc_root: Path = PROC_ROOT,
) -> dict[str, Any]:
    manifest_path = manifest_path.expanduser().resolve()
    manifest = read_manifest(manifest_path)
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise ValueError("run isolation: unsupported manifest schema")
    run_id = manifest.get("run_id")
    if not isinstance(run_id, str):
        raise ValueError("run isolation: manifest missing run_id")
    run_id = validate_run_id(run_id)
    config_dir = expected_config_dir(run_id)
    if manifest_path.parent != config_dir:
        raise ValueError(f"run isolation: manifest must be inside {config_dir}")

    paths = {
        key: manifest_config_path(manifest, key, config_dir) for key in CONFIG_KEYS
    }
    validate_runtime_configs(
        manifest,
        paths["data_session_config"],
        paths["gateway_config"],
        paths["feedback_config"],
        paths["smoke_config"],
    )
    applied = manifest.get("external_configs_applied")
    if not isinstance(applied, bool):
        raise ValueError("run isolation: external_configs_applied must be boolean")
    if require_applied and not applied:
        raise ValueError("run isolation: external configs are not marked applied")
    if applied:
        processes = manifest.get("processes")
        if not isinstance(processes, dict):
            raise ValueError("run isolation: manifest missing process bindings")
        proc_root = proc_root.expanduser().resolve()
        validate_binding(
            processes.get("data_session"),
            "data_session",
            paths["data_session_config"],
            proc_root,
        )
        validate_binding(
            processes.get("gateway"),
            "gateway",
            paths["gateway_config"],
            proc_root,
        )
        validate_binding(
            processes.get("feedback"),
            "feedback",
            paths["feedback_config"],
            proc_root,
        )
    if runner_command is not None:
        validate_runner_command(runner_command, paths["smoke_config"])
    return manifest


def prepare_runtime_configs(
    run_id: str,
    data_session_source: Path,
    gateway_source: Path,
    feedback_source: Path,
    smoke_source: Path,
    output_dir: Path,
) -> PreparedRuntimeConfigs:
    run_id = validate_run_id(run_id)
    run_dir = expected_run_dir(run_id)
    output_dir = output_dir.expanduser().resolve()
    if output_dir != expected_config_dir(run_id):
        raise ValueError(
            f"output_dir must be the fresh-run config directory {expected_config_dir(run_id)}"
        )
    data_session_source = data_session_source.expanduser().resolve()
    gateway_source = gateway_source.expanduser().resolve()
    feedback_source = feedback_source.expanduser().resolve()
    smoke_source = smoke_source.expanduser().resolve()

    gateway_source_data = load_toml(gateway_source)
    gateway_source_table = required_dict(
        gateway_source_data, "order_gateway", "order_gateway"
    )
    routes = gateway_source_table.get("routes")
    if (
        gateway_source_table.get("route_count") != 1
        or not isinstance(routes, list)
        or len(routes) != 1
        or not isinstance(routes[0], dict)
    ):
        raise ValueError("route_count must be 1 for Bitget gateway smoke")
    order_session_source = guard.resolve_repo_path(
        required_string(
            routes[0], "order_session_config", "order_gateway.routes[0]"
        )
    )

    try:
        run_dir.mkdir(parents=False, exist_ok=False)
    except FileExistsError as exc:
        raise ValueError(
            "fresh-run directory already exists; choose a new run_id"
        ) from exc
    output_dir.mkdir(parents=False, exist_ok=False)
    data_session_config = output_dir / f"data__{data_session_source.name}"
    gateway_config = output_dir / f"gateway__{gateway_source.name}"
    feedback_config = output_dir / f"feedback__{feedback_source.name}"
    smoke_config = output_dir / f"smoke__{smoke_source.name}"
    manifest_path = output_dir / "bitget_gateway_smoke_manifest.json"
    generated = (
        data_session_config,
        gateway_config,
        feedback_config,
        smoke_config,
        manifest_path,
    )
    if any(path.exists() for path in generated):
        raise ValueError("fresh-run output already exists; choose a new run_id")

    market_data_shm = f"aquila_bitget_market_data_{run_id}"
    gateway_shm = f"aquila_bitget_order_gateway_{run_id}"
    feedback_shm = f"aquila_bitget_order_feedback_{run_id}"
    data_session_log = run_dir / "bitget_data_session.log"
    gateway_log = run_dir / "bitget_order_gateway.log"
    feedback_log = run_dir / "bitget_order_feedback_session.log"
    guard.write_toml_overlay(
        data_session_source,
        data_session_config,
        {
            ("log", "file_sink_name"): str(data_session_log),
            ("data_shm_sink", "shm_name"): market_data_shm,
        },
    )
    guard.write_toml_overlay(
        gateway_source,
        gateway_config,
        {
            ("log", "file_sink_name"): str(gateway_log),
            ("order_gateway", "shm_name"): gateway_shm,
            ("order_gateway.routes", "order_session_config"): str(
                order_session_source
            ),
        },
    )
    guard.write_toml_overlay(
        feedback_source,
        feedback_config,
        {
            ("log", "file_sink_name"): str(feedback_log),
            ("order_feedback_session.shm", "shm_name"): feedback_shm,
        },
    )
    guard.write_toml_overlay(
        smoke_source,
        smoke_config,
        {
            ("gateway_smoke", "run_id"): run_id,
            ("market_data", "shm_name"): market_data_shm,
            ("order_gateway", "shm_name"): gateway_shm,
            ("feedback", "shm_name"): feedback_shm,
            ("output", "run_dir"): str(run_dir),
        },
    )
    configs = {
        "data_session_config": data_session_config,
        "gateway_config": gateway_config,
        "feedback_config": feedback_config,
        "smoke_config": smoke_config,
    }
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "run_id": run_id,
        "runner_executable": RUNNER_EXECUTABLE,
        "contract": "BTC_USDT",
        "route_count": 1,
        "market_data_shm": market_data_shm,
        "gateway_shm": gateway_shm,
        "feedback_shm": feedback_shm,
        "configs": {
            key: {"path": str(path), "sha256": sha256_file(path)}
            for key, path in configs.items()
        },
        "logs": {
            "data_session": str(data_session_log),
            "gateway": str(gateway_log),
            "feedback": str(feedback_log),
        },
        "external_configs_applied": False,
    }
    atomic_write_json(manifest_path, manifest)
    validate_manifest(manifest_path, require_applied=False)
    return PreparedRuntimeConfigs(
        run_id=run_id,
        run_dir=run_dir,
        data_session_config=data_session_config,
        gateway_config=gateway_config,
        feedback_config=feedback_config,
        smoke_config=smoke_config,
        manifest=manifest_path,
        market_data_shm=market_data_shm,
        gateway_shm=gateway_shm,
        feedback_shm=feedback_shm,
        data_session_log=data_session_log,
        gateway_log=gateway_log,
        feedback_log=feedback_log,
    )


def mark_processes_applied(
    manifest_path: Path,
    data_session_pid: int,
    gateway_pid: int,
    feedback_pid: int,
    proc_root: Path = PROC_ROOT,
) -> dict[str, Any]:
    manifest_path = manifest_path.expanduser().resolve()
    manifest = validate_manifest(
        manifest_path, require_applied=False, proc_root=proc_root
    )
    if manifest["external_configs_applied"]:
        raise ValueError("run isolation: process bindings already applied")
    config_dir = expected_config_dir(manifest["run_id"])
    paths = {
        key: manifest_config_path(manifest, key, config_dir) for key in CONFIG_KEYS
    }
    proc_root = proc_root.expanduser().resolve()
    bindings = {
        "data_session": lead_prepare._build_process_binding(
            proc_root,
            data_session_pid,
            PROCESS_SPECS["data_session"],
            paths["data_session_config"],
        ),
        "gateway": lead_prepare._build_process_binding(
            proc_root,
            gateway_pid,
            PROCESS_SPECS["gateway"],
            paths["gateway_config"],
        ),
        "feedback": lead_prepare._build_process_binding(
            proc_root,
            feedback_pid,
            PROCESS_SPECS["feedback"],
            paths["feedback_config"],
        ),
    }
    credential_env_names = guard.bitget_order_gateway_credential_env_names(
        paths["gateway_config"]
    )
    if credential_env_names != lead_prepare.feedback_credential_env_names(
        paths["feedback_config"]
    ):
        raise ValueError("run isolation: process credential env names do not match")
    gateway_credentials = lead_prepare._process_credential_values(
        proc_root, gateway_pid, credential_env_names
    )
    feedback_credentials = lead_prepare._process_credential_values(
        proc_root, feedback_pid, credential_env_names
    )
    if gateway_credentials != feedback_credentials:
        raise ValueError(
            "run isolation: gateway and feedback process credential values do not match"
        )
    updated = dict(manifest)
    updated["processes"] = bindings
    updated["external_configs_applied"] = True
    atomic_write_json(manifest_path, updated)
    return validate_manifest(
        manifest_path,
        require_applied=True,
        proc_root=proc_root,
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare and attest one isolated Bitget gateway smoke run."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--run-id", required=True)
    prepare_parser.add_argument("--data-session-config", type=Path, required=True)
    prepare_parser.add_argument("--gateway-config", type=Path, required=True)
    prepare_parser.add_argument("--feedback-config", type=Path, required=True)
    prepare_parser.add_argument("--smoke-config", type=Path, required=True)
    prepare_parser.add_argument("--output-dir", type=Path)
    mark_parser = subparsers.add_parser("mark-applied")
    mark_parser.add_argument("--runtime-manifest", type=Path, required=True)
    mark_parser.add_argument("--data-session-pid", type=int, required=True)
    mark_parser.add_argument("--gateway-pid", type=int, required=True)
    mark_parser.add_argument("--feedback-pid", type=int, required=True)
    validate_parser = subparsers.add_parser("validate")
    validate_parser.add_argument("--runtime-manifest", type=Path, required=True)
    validate_parser.add_argument("--allow-unapplied", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.command == "prepare":
            output_dir = args.output_dir or expected_config_dir(args.run_id)
            result = prepare_runtime_configs(
                run_id=args.run_id,
                data_session_source=args.data_session_config,
                gateway_source=args.gateway_config,
                feedback_source=args.feedback_config,
                smoke_source=args.smoke_config,
                output_dir=output_dir,
            )
            print(json.dumps(asdict(result), default=str, indent=2, sort_keys=True))
            return 0
        if args.command == "mark-applied":
            result = mark_processes_applied(
                args.runtime_manifest,
                data_session_pid=args.data_session_pid,
                gateway_pid=args.gateway_pid,
                feedback_pid=args.feedback_pid,
            )
            print(json.dumps(result, indent=2, sort_keys=True))
            return 0
        result = validate_manifest(
            args.runtime_manifest,
            require_applied=not args.allow_unapplied,
        )
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0
    except Exception as exc:
        print(f"config_error={type(exc).__name__}: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
