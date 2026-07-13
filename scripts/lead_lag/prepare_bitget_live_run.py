#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import re
import sys
import tomllib
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any

import run_live_with_guard as guard


MANIFEST_SCHEMA = "aquila.bitget_lead_lag_live_manifest.v2"
TMP_ROOT = Path("/home/liuxiang/tmp")
PROC_ROOT = Path("/proc")
RUN_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")
PROCESS_SPECS = {
    "gateway": "bitget_order_gateway",
    "feedback": "bitget_order_feedback_session",
}


@dataclass(frozen=True)
class PreparedRuntimeConfigs:
    run_id: str
    strategy_config: Path
    gateway_config: Path
    feedback_config: Path
    manifest: Path
    gateway_shm: str
    feedback_shm: str
    route_count: int


def validate_run_id(run_id: str) -> str:
    if not RUN_ID_PATTERN.fullmatch(run_id) or run_id in {".", ".."}:
        raise ValueError("run_id must contain only letters, digits, '.', '_', or '-'")
    return run_id


def expected_config_dir(run_id: str) -> Path:
    return (TMP_ROOT / validate_run_id(run_id) / "configs").resolve()


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


def load_toml(path: Path) -> dict[str, Any]:
    try:
        return tomllib.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise ValueError(
            f"run isolation: cannot read TOML {path}: {type(exc).__name__}: {exc}"
        ) from exc


def feedback_credential_env_names(path: Path) -> tuple[str, str, str]:
    data = load_toml(path)
    session = required_dict(data, "order_feedback_session", "order_feedback_session")
    credentials = required_dict(
        session,
        "credentials",
        "order_feedback_session.credentials",
    )
    return tuple(
        required_string(
            credentials,
            key,
            "order_feedback_session.credentials",
        )
        for key in ("api_key_env", "api_secret_env", "api_passphrase_env")
    )


def _trading_contract(
    session: dict[str, Any],
    section: str,
) -> tuple[str, str, str, str, str, str, bool]:
    websocket = required_dict(session, "websocket", f"{section}.websocket")
    endpoint = required_dict(
        websocket,
        "endpoint",
        f"{section}.websocket.endpoint",
    )
    enable_tls = endpoint.get("enable_tls")
    if not isinstance(enable_tls, bool):
        raise ValueError(
            f"run isolation: missing {section}.websocket.endpoint.enable_tls"
        )
    port = endpoint.get("port")
    if isinstance(port, bool) or not isinstance(port, (str, int)):
        raise ValueError(f"run isolation: missing {section}.websocket.endpoint.port")
    return (
        required_string(session, "category", section).strip().lower(),
        required_string(session, "position_mode", section).strip().lower(),
        required_string(session, "margin_mode", section).strip().lower(),
        required_string(endpoint, "host", f"{section}.websocket.endpoint")
        .strip()
        .lower(),
        str(port),
        required_string(endpoint, "target", f"{section}.websocket.endpoint"),
        enable_tls,
    )


def _gateway_order_session(gateway: dict[str, Any]) -> dict[str, Any]:
    routes = gateway.get("routes")
    if (
        not isinstance(routes, list)
        or len(routes) != 1
        or not isinstance(routes[0], dict)
    ):
        raise ValueError("run isolation: gateway requires exactly one route")
    session_path_value = required_string(
        routes[0], "order_session_config", "order_gateway.routes[0]"
    )
    session_path = guard.resolve_repo_path(session_path_value)
    session_data = load_toml(session_path)
    return required_dict(session_data, "order_session", "order_session")


def _read_process_start_time(proc_root: Path, pid: int) -> int:
    try:
        stat = (proc_root / str(pid) / "stat").read_text(encoding="utf-8")
    except Exception as exc:
        raise ValueError(
            f"run isolation: cannot read process {pid} stat: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    command_end = stat.rfind(")")
    if command_end < 0:
        raise ValueError(f"run isolation: process {pid} stat is malformed")
    fields = stat[command_end + 1 :].split()
    if len(fields) <= 19:
        raise ValueError(f"run isolation: process {pid} stat is malformed")
    try:
        return int(fields[19])
    except ValueError as exc:
        raise ValueError(
            f"run isolation: process {pid} start time is malformed"
        ) from exc


def _read_null_delimited(path: Path, label: str) -> list[str]:
    try:
        raw = path.read_bytes()
        return [
            value.decode("utf-8")
            for value in raw.split(b"\0")
            if value
        ]
    except Exception as exc:
        raise ValueError(
            f"run isolation: cannot read {label}: {type(exc).__name__}: {exc}"
        ) from exc


def _read_process_environment(proc_root: Path, pid: int) -> dict[str, str]:
    values = _read_null_delimited(
        proc_root / str(pid) / "environ", f"process {pid} environment"
    )
    environment: dict[str, str] = {}
    for value in values:
        if "=" not in value:
            continue
        key, item = value.split("=", 1)
        environment[key] = item
    return environment


def _process_credential_values(
    proc_root: Path,
    pid: int,
    credential_env_names: tuple[str, str, str],
) -> tuple[str, str, str]:
    environment = _read_process_environment(proc_root, pid)
    values: list[str] = []
    for env_name in credential_env_names:
        value = environment.get(env_name)
        if not value:
            raise ValueError(
                f"run isolation: process {pid} missing credential env {env_name}"
            )
        values.append(value)
    return values[0], values[1], values[2]


def _process_config_arg(command: list[str], pid: int) -> Path:
    config_arg = guard.find_strategy_config_arg(command)
    if config_arg is None:
        raise ValueError(f"run isolation: process {pid} command requires --config")
    config_path = config_arg[1].expanduser()
    if not config_path.is_absolute():
        raise ValueError(
            f"run isolation: process {pid} command requires absolute --config"
        )
    return config_path.resolve()


def _build_process_binding(
    proc_root: Path,
    pid: int,
    expected_executable: str,
    expected_config: Path,
) -> dict[str, Any]:
    if isinstance(pid, bool) or not isinstance(pid, int) or pid <= 0:
        raise ValueError("run isolation: process PID must be a positive integer")
    process_dir = proc_root / str(pid)
    start_time_ticks = _read_process_start_time(proc_root, pid)
    try:
        executable_path = (process_dir / "exe").resolve(strict=True)
    except Exception as exc:
        raise ValueError(
            f"run isolation: cannot read process {pid} executable: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    if executable_path.name != expected_executable:
        raise ValueError(
            f"run isolation: process {pid} executable must be {expected_executable}"
        )
    command = _read_null_delimited(
        process_dir / "cmdline", f"process {pid} command line"
    )
    if not command or Path(command[0]).name != expected_executable:
        raise ValueError(
            f"run isolation: process {pid} command must launch {expected_executable}"
        )
    if "--connect" not in command:
        raise ValueError(f"run isolation: process {pid} command requires --connect")
    if "--validate-only" in command:
        raise ValueError(
            f"run isolation: process {pid} command cannot use --validate-only"
        )
    actual_config = _process_config_arg(command, pid)
    expected_config = expected_config.expanduser().resolve()
    if actual_config != expected_config:
        raise ValueError(
            f"run isolation: process {pid} config mismatch: "
            f"expected {expected_config}, got {actual_config}"
        )
    return {
        "pid": pid,
        "start_time_ticks": start_time_ticks,
        "executable": expected_executable,
        "config": str(expected_config),
    }


def _validate_process_binding(
    binding: Any,
    role: str,
    expected_config: Path,
    proc_root: Path,
) -> dict[str, Any]:
    if not isinstance(binding, dict):
        raise ValueError(f"run isolation: manifest missing process binding {role}")
    expected_executable = PROCESS_SPECS[role]
    if binding.get("executable") != expected_executable:
        raise ValueError(f"run isolation: {role} executable binding mismatch")
    if binding.get("config") != str(expected_config.expanduser().resolve()):
        raise ValueError(f"run isolation: {role} config binding mismatch")
    actual = _build_process_binding(
        proc_root,
        binding.get("pid"),
        expected_executable,
        expected_config,
    )
    if binding.get("start_time_ticks") != actual["start_time_ticks"]:
        raise ValueError(f"run isolation: {role} process start time changed")
    return actual


def validate_bound_process_credentials(
    manifest: dict[str, Any],
    credential_env_names: tuple[str, str, str],
    expected_values: tuple[str, str, str],
    proc_root: Path = PROC_ROOT,
) -> None:
    processes = manifest.get("processes")
    if not isinstance(processes, dict):
        raise ValueError("run isolation: manifest missing process bindings")
    proc_root = proc_root.expanduser().resolve()
    for role in PROCESS_SPECS:
        binding = processes.get(role)
        if not isinstance(binding, dict):
            raise ValueError(f"run isolation: manifest missing process binding {role}")
        config_value = binding.get("config")
        if not isinstance(config_value, str) or not config_value:
            raise ValueError(f"run isolation: {role} config binding mismatch")
        _validate_process_binding(
            binding,
            role,
            Path(config_value),
            proc_root,
        )
        actual_values = _process_credential_values(
            proc_root,
            binding.get("pid"),
            credential_env_names,
        )
        if actual_values != expected_values:
            raise ValueError(
                f"run isolation: {role} process credential values do not match guard"
            )


def _manifest_path(manifest: dict[str, Any], key: str, config_dir: Path) -> Path:
    raw_path = manifest.get(key)
    if not isinstance(raw_path, str) or not raw_path:
        raise ValueError(f"run isolation: manifest missing {key}")
    path = Path(raw_path).expanduser().resolve()
    if path.parent != config_dir:
        raise ValueError(f"run isolation: {key} must be inside {config_dir}")
    return path


def _read_manifest(manifest_path: Path) -> dict[str, Any]:
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise ValueError(
            f"run isolation: cannot read manifest {manifest_path}: "
            f"{type(exc).__name__}: {exc}"
        ) from exc
    if not isinstance(manifest, dict):
        raise ValueError("run isolation: manifest must be a JSON object")
    return manifest


def _validate_manifest(
    manifest_path: Path,
    strategy_command: list[str] | None,
    require_applied: bool,
    proc_root: Path = PROC_ROOT,
) -> dict[str, Any]:
    manifest_path = manifest_path.expanduser().resolve()
    manifest = _read_manifest(manifest_path)
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise ValueError("run isolation: unsupported manifest schema")
    run_id = manifest.get("run_id")
    if not isinstance(run_id, str):
        raise ValueError("run isolation: manifest missing run_id")
    config_dir = expected_config_dir(run_id)
    if manifest_path.parent != config_dir:
        raise ValueError(f"run isolation: manifest must be inside {config_dir}")

    strategy_path = _manifest_path(manifest, "strategy_config", config_dir)
    gateway_path = _manifest_path(manifest, "gateway_config", config_dir)
    feedback_path = _manifest_path(manifest, "feedback_config", config_dir)
    expected_gateway_shm = f"aquila_bitget_order_gateway_{run_id}"
    expected_feedback_shm = f"aquila_bitget_order_feedback_{run_id}"
    if manifest.get("gateway_shm") != expected_gateway_shm:
        raise ValueError("run isolation: gateway_shm is not run-specific")
    if manifest.get("feedback_shm") != expected_feedback_shm:
        raise ValueError("run isolation: feedback_shm is not run-specific")
    if manifest.get("route_count") != 1:
        raise ValueError("run isolation: route_count must be 1")
    applied = manifest.get("external_configs_applied")
    if not isinstance(applied, bool):
        raise ValueError("run isolation: external_configs_applied must be boolean")
    if require_applied and not applied:
        raise ValueError("run isolation: external configs are not marked applied")

    strategy_data = load_toml(strategy_path)
    strategy = required_dict(strategy_data, "strategy", "strategy")
    order_gateway = required_dict(
        strategy, "order_gateway", "strategy.order_gateway"
    )
    feedback = required_dict(strategy, "feedback", "strategy.feedback")
    if feedback.get("enabled", True) is not True:
        raise ValueError("run isolation: strategy.feedback.enabled must be true")
    poll_budget = feedback.get("poll_budget", 32)
    if (
        isinstance(poll_budget, bool)
        or not isinstance(poll_budget, int)
        or poll_budget <= 0
    ):
        raise ValueError("run isolation: strategy.feedback.poll_budget must be positive")
    strategy_gateway_path = Path(
        required_string(order_gateway, "config", "strategy.order_gateway")
    ).resolve()
    if strategy_gateway_path != gateway_path:
        raise ValueError("run isolation: strategy gateway config path mismatch")
    strategy_feedback_shm = required_string(
        feedback, "shm_name", "strategy.feedback"
    )
    if strategy_feedback_shm != expected_feedback_shm:
        raise ValueError("run isolation: strategy feedback SHM mismatch")

    gateway_data = load_toml(gateway_path)
    gateway = required_dict(gateway_data, "order_gateway", "order_gateway")
    routes = gateway.get("routes")
    if (
        gateway.get("route_count") != 1
        or not isinstance(routes, list)
        or len(routes) != 1
    ):
        raise ValueError("run isolation: gateway route_count must be 1")
    if required_string(gateway, "shm_name", "order_gateway") != expected_gateway_shm:
        raise ValueError("run isolation: gateway SHM mismatch")

    feedback_data = load_toml(feedback_path)
    feedback_session = required_dict(
        feedback_data,
        "order_feedback_session",
        "order_feedback_session",
    )
    feedback_shm = required_dict(
        feedback_session,
        "shm",
        "order_feedback_session.shm",
    )
    feedback_shm_name = required_string(
        feedback_shm, "shm_name", "order_feedback_session.shm"
    )
    if feedback_shm_name != expected_feedback_shm:
        raise ValueError("run isolation: feedback session SHM mismatch")

    gateway_credentials = guard.bitget_order_gateway_credential_env_names(gateway_path)
    feedback_credentials = feedback_credential_env_names(feedback_path)
    if gateway_credentials != feedback_credentials:
        raise ValueError("run isolation: gateway and feedback credentials do not match")
    gateway_contract = _trading_contract(
        _gateway_order_session(gateway),
        "order_session",
    )
    feedback_contract = _trading_contract(
        feedback_session,
        "order_feedback_session",
    )
    if gateway_contract != feedback_contract:
        raise ValueError(
            "run isolation: gateway and feedback trading contract mismatch"
        )

    if applied:
        processes = manifest.get("processes")
        if not isinstance(processes, dict):
            raise ValueError("run isolation: manifest missing process bindings")
        proc_root = proc_root.expanduser().resolve()
        _validate_process_binding(
            processes.get("gateway"), "gateway", gateway_path, proc_root
        )
        _validate_process_binding(
            processes.get("feedback"), "feedback", feedback_path, proc_root
        )

    if strategy_command is not None:
        config_arg = guard.find_strategy_config_arg(strategy_command)
        if config_arg is None:
            raise ValueError("run isolation: strategy command requires --config")
        _, command_config, _ = config_arg
        if guard.resolve_repo_path(command_config) != strategy_path:
            raise ValueError("run isolation: strategy command config path mismatch")
    return manifest


def validate_bitget_run_isolation(
    manifest_path: Path,
    strategy_command: list[str],
    proc_root: Path = PROC_ROOT,
) -> dict[str, Any]:
    return _validate_manifest(
        manifest_path=manifest_path,
        strategy_command=strategy_command,
        require_applied=True,
        proc_root=proc_root,
    )


def _atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    temporary_path = path.with_suffix(path.suffix + ".tmp")
    temporary_path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary_path.replace(path)


def prepare_runtime_configs(
    run_id: str,
    strategy_source: Path,
    gateway_source: Path,
    feedback_source: Path,
    output_dir: Path,
) -> PreparedRuntimeConfigs:
    run_id = validate_run_id(run_id)
    output_dir = output_dir.expanduser().resolve()
    if output_dir != expected_config_dir(run_id):
        raise ValueError(
            f"output_dir must be the fresh-run config directory {expected_config_dir(run_id)}"
        )

    strategy_source = strategy_source.expanduser().resolve()
    gateway_source = gateway_source.expanduser().resolve()
    feedback_source = feedback_source.expanduser().resolve()
    gateway_data = load_toml(gateway_source)
    gateway = required_dict(gateway_data, "order_gateway", "order_gateway")
    routes = gateway.get("routes")
    if (
        gateway.get("route_count") != 1
        or not isinstance(routes, list)
        or len(routes) != 1
        or not isinstance(routes[0], dict)
    ):
        raise ValueError("route_count must be 1 for Bitget live V1")
    order_session_source = guard.resolve_repo_path(
        required_string(
            routes[0],
            "order_session_config",
            "order_gateway.routes[0]",
        )
    )
    strategy_data = load_toml(strategy_source)
    strategy = required_dict(strategy_data, "strategy", "strategy")
    lead_lag_source = guard.resolve_repo_path(
        required_string(strategy, "config", "strategy")
    )

    output_dir.mkdir(parents=True, exist_ok=True)
    strategy_config = output_dir / f"strategy__{strategy_source.name}"
    gateway_config = output_dir / f"bitget_gateway__{gateway_source.name}"
    feedback_config = output_dir / f"bitget_feedback__{feedback_source.name}"
    manifest_path = output_dir / "bitget_live_manifest.json"
    generated_paths = (
        strategy_config,
        gateway_config,
        feedback_config,
        manifest_path,
    )
    if any(path.exists() for path in generated_paths):
        raise ValueError("fresh-run output already exists; choose a new run_id")

    gateway_shm = f"aquila_bitget_order_gateway_{run_id}"
    feedback_shm = f"aquila_bitget_order_feedback_{run_id}"
    guard.write_toml_overlay(
        gateway_source,
        gateway_config,
        {
            ("order_gateway", "shm_name"): gateway_shm,
            ("order_gateway.routes", "order_session_config"): str(
                order_session_source
            ),
        },
    )
    guard.write_toml_overlay(
        feedback_source,
        feedback_config,
        {("order_feedback_session.shm", "shm_name"): feedback_shm},
    )
    guard.write_toml_overlay(
        strategy_source,
        strategy_config,
        {
            ("strategy", "config"): str(lead_lag_source),
            ("strategy.order_gateway", "config"): str(gateway_config),
            ("strategy.feedback", "shm_name"): feedback_shm,
        },
    )
    manifest = {
        "schema": MANIFEST_SCHEMA,
        "run_id": run_id,
        "strategy_config": str(strategy_config),
        "gateway_config": str(gateway_config),
        "feedback_config": str(feedback_config),
        "gateway_shm": gateway_shm,
        "feedback_shm": feedback_shm,
        "route_count": 1,
        "external_configs_applied": False,
    }
    _atomic_write_json(manifest_path, manifest)
    return PreparedRuntimeConfigs(
        run_id=run_id,
        strategy_config=strategy_config,
        gateway_config=gateway_config,
        feedback_config=feedback_config,
        manifest=manifest_path,
        gateway_shm=gateway_shm,
        feedback_shm=feedback_shm,
        route_count=1,
    )


def mark_external_configs_applied(
    manifest_path: Path,
    gateway_pid: int,
    feedback_pid: int,
    proc_root: Path = PROC_ROOT,
) -> dict[str, Any]:
    manifest_path = manifest_path.expanduser().resolve()
    manifest = _validate_manifest(
        manifest_path=manifest_path,
        strategy_command=None,
        require_applied=False,
        proc_root=proc_root,
    )
    config_dir = expected_config_dir(manifest["run_id"])
    gateway_path = _manifest_path(manifest, "gateway_config", config_dir)
    feedback_path = _manifest_path(manifest, "feedback_config", config_dir)
    proc_root = proc_root.expanduser().resolve()
    gateway_binding = _build_process_binding(
        proc_root,
        gateway_pid,
        PROCESS_SPECS["gateway"],
        gateway_path,
    )
    feedback_binding = _build_process_binding(
        proc_root,
        feedback_pid,
        PROCESS_SPECS["feedback"],
        feedback_path,
    )
    credential_env_names = guard.bitget_order_gateway_credential_env_names(
        gateway_path
    )
    gateway_credentials = _process_credential_values(
        proc_root, gateway_pid, credential_env_names
    )
    feedback_credentials = _process_credential_values(
        proc_root, feedback_pid, credential_env_names
    )
    if gateway_credentials != feedback_credentials:
        raise ValueError(
            "run isolation: gateway and feedback process credential values do not match"
        )
    updated = dict(manifest)
    updated["processes"] = {
        "gateway": gateway_binding,
        "feedback": feedback_binding,
    }
    updated["external_configs_applied"] = True
    _atomic_write_json(manifest_path, updated)
    return updated


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare and attest an isolated Bitget LeadLag live run."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--run-id", required=True)
    prepare_parser.add_argument("--strategy-config", type=Path, required=True)
    prepare_parser.add_argument("--gateway-config", type=Path, required=True)
    prepare_parser.add_argument("--feedback-config", type=Path, required=True)
    prepare_parser.add_argument("--output-dir", type=Path)
    mark_parser = subparsers.add_parser("mark-applied")
    mark_parser.add_argument("--runtime-manifest", type=Path, required=True)
    mark_parser.add_argument("--gateway-pid", type=int, required=True)
    mark_parser.add_argument("--feedback-pid", type=int, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.command == "prepare":
        output_dir = args.output_dir or expected_config_dir(args.run_id)
        result = prepare_runtime_configs(
            run_id=args.run_id,
            strategy_source=args.strategy_config,
            gateway_source=args.gateway_config,
            feedback_source=args.feedback_config,
            output_dir=output_dir,
        )
        payload = {
            key: str(value) if isinstance(value, Path) else value
            for key, value in asdict(result).items()
        }
    else:
        payload = mark_external_configs_applied(
            args.runtime_manifest,
            gateway_pid=args.gateway_pid,
            feedback_pid=args.feedback_pid,
        )
    print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
