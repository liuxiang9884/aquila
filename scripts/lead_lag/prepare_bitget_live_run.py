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


MANIFEST_SCHEMA = "aquila.bitget_lead_lag_live_manifest.v1"
TMP_ROOT = Path("/home/liuxiang/tmp")
RUN_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]*$")


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
) -> dict[str, Any]:
    return _validate_manifest(
        manifest_path=manifest_path,
        strategy_command=strategy_command,
        require_applied=True,
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

    gateway_source = gateway_source.expanduser().resolve()
    gateway_data = load_toml(gateway_source)
    gateway = required_dict(gateway_data, "order_gateway", "order_gateway")
    routes = gateway.get("routes")
    if gateway.get("route_count") != 1 or not isinstance(routes, list) or len(routes) != 1:
        raise ValueError("route_count must be 1 for Bitget live V1")

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
        {("order_gateway", "shm_name"): gateway_shm},
    )
    guard.write_toml_overlay(
        feedback_source.expanduser().resolve(),
        feedback_config,
        {("order_feedback_session.shm", "shm_name"): feedback_shm},
    )
    guard.write_toml_overlay(
        strategy_source.expanduser().resolve(),
        strategy_config,
        {
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


def mark_external_configs_applied(manifest_path: Path) -> dict[str, Any]:
    manifest_path = manifest_path.expanduser().resolve()
    manifest = _validate_manifest(
        manifest_path=manifest_path,
        strategy_command=None,
        require_applied=False,
    )
    updated = dict(manifest)
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
        payload = mark_external_configs_applied(args.runtime_manifest)
    print(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
