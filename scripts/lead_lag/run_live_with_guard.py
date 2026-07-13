#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import math
import os
import re
import signal
import subprocess
import sys
import tomllib
from dataclasses import dataclass, replace
from datetime import datetime
from pathlib import Path
from typing import Any, Callable

from guard_exchange_adapter import (
    GuardCredentialEnvNames,
    GuardExchangeAdapter,
    get_guard_exchange_adapter,
)


SCRIPTS_ROOT = Path(__file__).resolve().parents[1]
if str(SCRIPTS_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_ROOT))

from gate.account import query_gate_account as account  # noqa: E402
from gate.trading import emergency_flatten_futures as flatten  # noqa: E402
from gate.trading.place_futures_order import SignedGateTradingClient  # noqa: E402
from bitget.account import query_bitget_account as bitget_account  # noqa: E402
from bitget.trading import emergency_flatten_futures as bitget_flatten  # noqa: E402
from bitget.trading.place_futures_order import (  # noqa: E402
    SignedBitgetTradingClient,
)


PROJECT_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_TMP_ROOT = Path("/home/liuxiang/tmp")

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
    exchange: str = "gate"
    poll_timeout_sec: float = flatten.DEFAULT_POLL_TIMEOUT_SEC
    poll_interval_sec: float = flatten.DEFAULT_POLL_INTERVAL_SEC
    run_id: str | None = None
    affinity_profile_path: Path | None = None
    affinity_output_dir: Path | None = None
    affinity_gate_market_config: Path | None = None
    affinity_binance_market_config: Path | None = None
    affinity_order_feedback_config: Path | None = None
    affinity_external_configs_applied: bool = False
    api_key_env: str | None = None
    api_secret_env: str | None = None
    api_passphrase_env: str | None = None
    credential_source: str | None = None
    runtime_isolation: dict[str, Any] | None = None


@dataclass(frozen=True)
class AffinityProfile:
    path: Path
    name: str
    numa_node: int
    gate_market_data_cpu: int
    binance_market_data_cpu: int
    strategy_order_owner_cpu: int
    gate_order_feedback_cpu: int
    log_backend_cpu: int
    reserved_core_cpus: list[int]
    preferred_aux_cpus: list[int]


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
    if not math.isfinite(config.poll_timeout_sec):
        raise ValueError("--poll-timeout-sec must be finite")
    if not math.isfinite(config.poll_interval_sec):
        raise ValueError("--poll-interval-sec must be finite")
    if config.poll_timeout_sec < 0:
        raise ValueError("--poll-timeout-sec must be non-negative")
    if config.poll_interval_sec <= 0:
        raise ValueError("--poll-interval-sec must be positive")


def resolve_repo_path(path: Path | str) -> Path:
    resolved = Path(path).expanduser()
    if resolved.is_absolute():
        return resolved
    return (PROJECT_ROOT / resolved).resolve()


def resolve_output_dir(path: Path | str) -> Path:
    resolved = Path(path).expanduser()
    if resolved.is_absolute():
        return resolved
    return (PROJECT_ROOT / resolved).resolve()


def required_table(data: dict[str, Any], name: str) -> dict[str, Any]:
    value = data.get(name)
    if not isinstance(value, dict):
        raise ValueError(f"affinity profile missing [{name}]")
    return value


def required_int(table: dict[str, Any], key: str, section: str) -> int:
    value = table.get(key)
    if not isinstance(value, int):
        raise ValueError(f"affinity profile missing integer {section}.{key}")
    return value


def int_list(table: dict[str, Any], key: str) -> list[int]:
    value = table.get(key, [])
    if not isinstance(value, list) or any(not isinstance(item, int) for item in value):
        raise ValueError(f"affinity profile field {key} must be an integer list")
    return list(value)


def load_affinity_profile(path: Path) -> AffinityProfile:
    resolved_path = resolve_repo_path(path)
    data = tomllib.loads(resolved_path.read_text(encoding="utf-8"))
    profile = required_table(data, "profile")
    core_path = required_table(data, "core_path")
    auxiliary = data.get("auxiliary", {})
    if not isinstance(auxiliary, dict):
        raise ValueError("affinity profile [auxiliary] must be a table")
    name = profile.get("name")
    if not isinstance(name, str) or not name:
        raise ValueError("affinity profile missing profile.name")
    numa_node = required_int(profile, "numa_node", "profile")
    return AffinityProfile(
        path=resolved_path,
        name=name,
        numa_node=numa_node,
        gate_market_data_cpu=required_int(
            core_path, "gate_market_data_cpu", "core_path"
        ),
        binance_market_data_cpu=required_int(
            core_path, "binance_market_data_cpu", "core_path"
        ),
        strategy_order_owner_cpu=required_int(
            core_path, "strategy_order_owner_cpu", "core_path"
        ),
        gate_order_feedback_cpu=required_int(
            core_path, "gate_order_feedback_cpu", "core_path"
        ),
        log_backend_cpu=required_int(core_path, "log_backend_cpu", "core_path"),
        reserved_core_cpus=int_list(auxiliary, "reserved_core_cpus"),
        preferred_aux_cpus=int_list(auxiliary, "preferred_aux_cpus"),
    )


def default_affinity_output_dir(run_id: str | None) -> Path:
    label = run_id or f"lead_lag_live_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    return DEFAULT_TMP_ROOT / label / "configs"


def toml_scalar(value: int | str | Path) -> str:
    if isinstance(value, int):
        return str(value)
    return json.dumps(str(value), ensure_ascii=False)


SECTION_RE = re.compile(r"^\s*\[([A-Za-z0-9_.-]+)\]\s*(?:#.*)?$")


def write_toml_overlay(
    source_path: Path,
    output_path: Path,
    replacements: dict[tuple[str, str], int | str | Path],
) -> None:
    source_path = resolve_repo_path(source_path)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    pending = set(replacements)
    section = ""
    output_lines: list[str] = []
    for line in source_path.read_text(encoding="utf-8").splitlines(keepends=True):
        section_match = SECTION_RE.match(line)
        if section_match:
            section = section_match.group(1)
        replaced = False
        for section_key, value in replacements.items():
            target_section, key = section_key
            if section != target_section:
                continue
            if re.match(rf"^\s*{re.escape(key)}\s*=", line):
                output_lines.append(f"{key} = {toml_scalar(value)}\n")
                pending.discard(section_key)
                replaced = True
                break
        if not replaced:
            output_lines.append(line)

    if pending:
        missing = ", ".join(f"{section}.{key}" for section, key in sorted(pending))
        raise ValueError(f"{source_path} missing TOML keys: {missing}")
    output_path.write_text("".join(output_lines), encoding="utf-8")


def load_toml(path: Path) -> dict[str, Any]:
    return tomllib.loads(resolve_repo_path(path).read_text(encoding="utf-8"))


def nested_config_path(data: dict[str, Any], section_path: tuple[str, ...]) -> Path:
    current: Any = data
    dotted = ".".join(section_path)
    for key in section_path:
        if not isinstance(current, dict):
            raise ValueError(f"strategy config missing [{dotted}]")
        current = current.get(key)
    if not isinstance(current, dict):
        raise ValueError(f"strategy config missing [{dotted}]")
    config_path = current.get("config")
    if not isinstance(config_path, str) or not config_path:
        raise ValueError(f"strategy config missing {dotted}.config")
    return resolve_repo_path(config_path)


def find_strategy_config_arg(command: list[str]) -> tuple[int, Path, bool] | None:
    for index, arg in enumerate(command):
        if arg == "--config":
            if index + 1 >= len(command):
                raise ValueError("strategy command has --config without a path")
            return index, Path(command[index + 1]), False
        if arg.startswith("--config="):
            return index, Path(arg.split("=", 1)[1]), True
    return None


def strategy_execute_requested(command: list[str]) -> bool:
    return any(
        arg == "--execute" or arg.startswith("--execute=") for arg in command
    )


def validate_bitget_strategy_command(command: list[str]) -> None:
    if not command or Path(command[0]).name != "lead_lag_strategy":
        raise ValueError("Bitget guard requires a direct lead_lag_strategy command")


def bitget_strategy_lag_symbols(command: list[str]) -> list[str]:
    config_arg = find_strategy_config_arg(command)
    if config_arg is None:
        raise ValueError("strategy command requires --config")
    _, strategy_config_path, _ = config_arg
    strategy_data = load_toml(strategy_config_path)
    lead_lag_config_path = nested_config_path(strategy_data, ("strategy",))
    lead_lag_data = load_toml(lead_lag_config_path)
    lead_lag = lead_lag_data.get("lead_lag")
    if not isinstance(lead_lag, dict):
        raise ValueError(f"{lead_lag_config_path} missing [lead_lag]")
    pairs = lead_lag.get("pairs")
    if not isinstance(pairs, list) or not pairs:
        raise ValueError(f"{lead_lag_config_path} missing [[lead_lag.pairs]]")

    symbols: list[str] = []
    for index, pair in enumerate(pairs):
        if not isinstance(pair, dict):
            raise ValueError(f"lead_lag.pairs[{index}] must be a table")
        lag_exchange = pair.get("lag_exchange")
        if not isinstance(lag_exchange, str) or not lag_exchange.strip():
            raise ValueError(f"lead_lag.pairs[{index}] missing lag_exchange")
        if lag_exchange.strip().lower() != "bitget":
            continue
        symbol = pair.get("symbol")
        if not isinstance(symbol, str) or not symbol.strip():
            raise ValueError(f"lead_lag.pairs[{index}] missing symbol")
        symbols.append(symbol)
    if not symbols:
        raise ValueError("strategy config has no Bitget lag symbols")
    return bitget_flatten.normalize_symbols(symbols)


def validate_bitget_guard_contract_scope(
    strategy_command: list[str], guard_contracts: list[str]
) -> list[str]:
    lag_symbols = bitget_strategy_lag_symbols(strategy_command)
    guarded = set(bitget_flatten.normalize_symbols(guard_contracts))
    missing = [symbol for symbol in lag_symbols if symbol not in guarded]
    if missing:
        raise ValueError(
            "Bitget lag symbols outside guard contracts: " + ", ".join(missing)
        )
    return lag_symbols


def order_session_credential_env_names(
    order_session_config_path: Path,
) -> tuple[str, str]:
    data = load_toml(order_session_config_path)
    order_session = data.get("order_session")
    if not isinstance(order_session, dict):
        raise ValueError(f"{order_session_config_path} missing [order_session]")
    credentials = order_session.get("credentials")
    if not isinstance(credentials, dict):
        raise ValueError(
            f"{order_session_config_path} missing [order_session.credentials]"
        )
    api_key_env = credentials.get("api_key_env")
    if not isinstance(api_key_env, str) or not api_key_env:
        raise ValueError(
            f"{order_session_config_path} missing "
            "order_session.credentials.api_key_env"
        )
    api_secret_env = credentials.get("api_secret_env")
    if not isinstance(api_secret_env, str) or not api_secret_env:
        raise ValueError(
            f"{order_session_config_path} missing "
            "order_session.credentials.api_secret_env"
        )
    return api_key_env, api_secret_env


def order_gateway_credential_env_names(
    order_gateway_config_path: Path,
) -> tuple[str, str]:
    data = load_toml(order_gateway_config_path)
    order_gateway = data.get("order_gateway")
    if not isinstance(order_gateway, dict):
        raise ValueError(f"{order_gateway_config_path} missing [order_gateway]")
    routes = order_gateway.get("routes")
    if not isinstance(routes, list) or not routes:
        raise ValueError(f"{order_gateway_config_path} missing [[order_gateway.routes]]")
    first_credentials: tuple[str, str] | None = None
    for route_index, route in enumerate(routes):
        if not isinstance(route, dict):
            raise ValueError(
                f"{order_gateway_config_path} route {route_index} must be a table"
            )
        order_session_config = route.get("order_session_config")
        if not isinstance(order_session_config, str) or not order_session_config:
            raise ValueError(
                f"{order_gateway_config_path} route {route_index} missing "
                "order_session_config"
            )
        credentials = order_session_credential_env_names(
            resolve_repo_path(order_session_config)
        )
        if first_credentials is None:
            first_credentials = credentials
        elif credentials != first_credentials:
            raise ValueError(
                f"{order_gateway_config_path} route {route_index} credentials "
                "do not match route 0"
            )
    assert first_credentials is not None
    return first_credentials


def strategy_order_credential_env_names(
    strategy_command: list[str],
) -> GuardCredentialEnvNames | None:
    config_arg = find_strategy_config_arg(strategy_command)
    if config_arg is None:
        return None
    _, strategy_config_path, _ = config_arg
    strategy_data = load_toml(strategy_config_path)
    strategy = strategy_data.get("strategy")
    if not isinstance(strategy, dict):
        raise ValueError(f"{strategy_config_path} missing [strategy]")
    has_order_session = isinstance(strategy.get("order_session"), dict)
    has_order_gateway = isinstance(strategy.get("order_gateway"), dict)
    if has_order_session and has_order_gateway:
        raise ValueError(
            "strategy config must not set both [strategy.order_session] "
            "and [strategy.order_gateway]"
        )
    if has_order_session:
        order_session_config_path = nested_config_path(
            strategy_data, ("strategy", "order_session")
        )
        api_key_env, api_secret_env = order_session_credential_env_names(
            order_session_config_path
        )
        return GuardCredentialEnvNames(
            api_key_env=api_key_env,
            api_secret_env=api_secret_env,
            api_passphrase_env=None,
            source="order_session_config",
        )
    if has_order_gateway:
        order_gateway_config_path = nested_config_path(
            strategy_data, ("strategy", "order_gateway")
        )
        api_key_env, api_secret_env = order_gateway_credential_env_names(
            order_gateway_config_path
        )
        return GuardCredentialEnvNames(
            api_key_env=api_key_env,
            api_secret_env=api_secret_env,
            api_passphrase_env=None,
            source="order_gateway_config",
        )
    return None


def resolve_guard_credential_env_names(
    explicit_api_key: str | None,
    explicit_api_secret: str | None,
    strategy_command: list[str],
) -> GuardCredentialEnvNames:
    if bool(explicit_api_key) != bool(explicit_api_secret):
        raise ValueError("--api-key and --api-secret must be provided together")

    inferred = strategy_order_credential_env_names(strategy_command)
    if explicit_api_key is not None and explicit_api_secret is not None:
        if inferred is not None and (
            explicit_api_key != inferred.api_key_env
            or explicit_api_secret != inferred.api_secret_env
        ):
            raise ValueError(
                "guard REST credentials must match strategy order session "
                f"credentials: explicit {explicit_api_key}/{explicit_api_secret}, "
                f"order session {inferred.api_key_env}/{inferred.api_secret_env}"
            )
        return GuardCredentialEnvNames(
            api_key_env=explicit_api_key,
            api_secret_env=explicit_api_secret,
            api_passphrase_env=None,
            source="explicit",
        )

    if inferred is not None:
        return inferred

    raise ValueError(
        "guard REST credentials require --api-key/--api-secret or a strategy "
        "--config with [strategy.order_session].config or "
        "[strategy.order_gateway].config credentials"
    )


def bitget_order_session_credential_env_names(
    order_session_config_path: Path,
) -> tuple[str, str, str]:
    data = load_toml(order_session_config_path)
    order_session = data.get("order_session")
    if not isinstance(order_session, dict):
        raise ValueError(f"{order_session_config_path} missing [order_session]")
    credentials = order_session.get("credentials")
    if not isinstance(credentials, dict):
        raise ValueError(
            f"{order_session_config_path} missing [order_session.credentials]"
        )

    names: list[str] = []
    for key in ("api_key_env", "api_secret_env", "api_passphrase_env"):
        value = credentials.get(key)
        if not isinstance(value, str) or not value:
            raise ValueError(
                f"{order_session_config_path} missing "
                f"order_session.credentials.{key}"
            )
        names.append(value)
    return names[0], names[1], names[2]


def bitget_order_gateway_credential_env_names(
    order_gateway_config_path: Path,
) -> tuple[str, str, str]:
    data = load_toml(order_gateway_config_path)
    order_gateway = data.get("order_gateway")
    if not isinstance(order_gateway, dict):
        raise ValueError(f"{order_gateway_config_path} missing [order_gateway]")
    routes = order_gateway.get("routes")
    if not isinstance(routes, list) or not routes:
        raise ValueError(f"{order_gateway_config_path} missing [[order_gateway.routes]]")

    first_credentials: tuple[str, str, str] | None = None
    for route_index, route in enumerate(routes):
        if not isinstance(route, dict):
            raise ValueError(
                f"{order_gateway_config_path} route {route_index} must be a table"
            )
        order_session_config = route.get("order_session_config")
        if not isinstance(order_session_config, str) or not order_session_config:
            raise ValueError(
                f"{order_gateway_config_path} route {route_index} missing "
                "order_session_config"
            )
        credentials = bitget_order_session_credential_env_names(
            resolve_repo_path(order_session_config)
        )
        if first_credentials is None:
            first_credentials = credentials
        elif credentials != first_credentials:
            raise ValueError(
                f"{order_gateway_config_path} route {route_index} credentials "
                "do not match route 0"
            )
    assert first_credentials is not None
    return first_credentials


def bitget_strategy_order_credential_env_names(
    strategy_command: list[str],
) -> GuardCredentialEnvNames | None:
    config_arg = find_strategy_config_arg(strategy_command)
    if config_arg is None:
        return None
    _, strategy_config_path, _ = config_arg
    strategy_data = load_toml(strategy_config_path)
    strategy = strategy_data.get("strategy")
    if not isinstance(strategy, dict):
        raise ValueError(f"{strategy_config_path} missing [strategy]")
    if isinstance(strategy.get("order_session"), dict):
        raise ValueError("Bitget guarded live requires [strategy.order_gateway]")
    if not isinstance(strategy.get("order_gateway"), dict):
        return None

    order_gateway_config_path = nested_config_path(
        strategy_data, ("strategy", "order_gateway")
    )
    api_key_env, api_secret_env, api_passphrase_env = (
        bitget_order_gateway_credential_env_names(order_gateway_config_path)
    )
    return GuardCredentialEnvNames(
        api_key_env=api_key_env,
        api_secret_env=api_secret_env,
        api_passphrase_env=api_passphrase_env,
        source="order_gateway_config",
    )


def resolve_bitget_guard_credential_env_names(
    explicit_api_key: str | None,
    explicit_api_secret: str | None,
    explicit_api_passphrase: str | None,
    strategy_command: list[str],
) -> GuardCredentialEnvNames:
    explicit_values = (
        explicit_api_key,
        explicit_api_secret,
        explicit_api_passphrase,
    )
    if any(explicit_values) and not all(explicit_values):
        raise ValueError(
            "--api-key, --api-secret, and --api-passphrase must be provided together"
        )

    inferred = bitget_strategy_order_credential_env_names(strategy_command)
    if all(explicit_values):
        assert explicit_api_key is not None
        assert explicit_api_secret is not None
        assert explicit_api_passphrase is not None
        explicit = (
            explicit_api_key,
            explicit_api_secret,
            explicit_api_passphrase,
        )
        if inferred is not None and explicit != (
            inferred.api_key_env,
            inferred.api_secret_env,
            inferred.api_passphrase_env,
        ):
            raise ValueError(
                "guard REST credentials must match strategy order gateway "
                "credentials"
            )
        return GuardCredentialEnvNames(
            api_key_env=explicit_api_key,
            api_secret_env=explicit_api_secret,
            api_passphrase_env=explicit_api_passphrase,
            source="explicit",
        )
    if inferred is not None:
        return inferred
    raise ValueError(
        "Bitget guard REST credentials require --api-key/--api-secret/"
        "--api-passphrase or a strategy --config with "
        "[strategy.order_gateway].config credentials"
    )


def replace_strategy_config_arg(
    command: list[str], config_index: int, config_path: Path, inline_arg: bool
) -> list[str]:
    rewritten = list(command)
    if inline_arg:
        rewritten[config_index] = f"--config={config_path}"
    else:
        rewritten[config_index + 1] = str(config_path)
    return rewritten


def overlay_path(output_dir: Path, label: str, source_path: Path) -> Path:
    return output_dir / f"{label}__{source_path.name}"


def write_runtime_config_overlay(
    source_path: Path,
    output_dir: Path,
    label: str,
    execution_section: str,
    bind_cpu_id: int,
    log_backend_cpu: int,
) -> Path:
    destination = overlay_path(output_dir, label, resolve_repo_path(source_path))
    write_toml_overlay(
        source_path,
        destination,
        {
            ("log", "backend_cpu_affinity"): log_backend_cpu,
            (execution_section, "bind_cpu_id"): bind_cpu_id,
        },
    )
    return destination


def prepare_affinity_overlays(
    config: GuardConfig,
) -> tuple[GuardConfig, dict[str, Any]]:
    if config.affinity_profile_path is None:
        return config, {}
    profile = load_affinity_profile(config.affinity_profile_path)
    output_dir = resolve_output_dir(
        config.affinity_output_dir or default_affinity_output_dir(config.run_id)
    )
    generated_configs: dict[str, str] = {}
    applied_configs: list[str] = []
    strategy_command = list(config.strategy_command)

    config_arg = find_strategy_config_arg(strategy_command)
    if config_arg is not None:
        config_index, strategy_source, inline_arg = config_arg
        strategy_source = resolve_repo_path(strategy_source)
        strategy_data = load_toml(strategy_source)
        data_reader_source = nested_config_path(
            strategy_data, ("strategy", "data_reader")
        )
        order_session_source = nested_config_path(
            strategy_data, ("strategy", "order_session")
        )
        data_reader_overlay = write_runtime_config_overlay(
            data_reader_source,
            output_dir,
            "strategy_data_reader",
            "data_reader.execution_policy",
            profile.strategy_order_owner_cpu,
            profile.log_backend_cpu,
        )
        order_session_overlay = write_runtime_config_overlay(
            order_session_source,
            output_dir,
            "gate_order_session",
            "order_session.websocket.execution_policy",
            profile.strategy_order_owner_cpu,
            profile.log_backend_cpu,
        )
        strategy_overlay = overlay_path(output_dir, "strategy", strategy_source)
        write_toml_overlay(
            strategy_source,
            strategy_overlay,
            {
                ("log", "backend_cpu_affinity"): profile.log_backend_cpu,
                ("strategy.loop", "bind_cpu_id"): profile.strategy_order_owner_cpu,
                ("strategy.data_reader", "config"): data_reader_overlay,
                ("strategy.order_session", "config"): order_session_overlay,
            },
        )
        strategy_command = replace_strategy_config_arg(
            strategy_command, config_index, strategy_overlay, inline_arg
        )
        generated_configs.update(
            {
                "strategy": str(strategy_overlay),
                "strategy_data_reader": str(data_reader_overlay),
                "gate_order_session": str(order_session_overlay),
            }
        )
        applied_configs.extend(
            ["strategy", "strategy_data_reader", "gate_order_session"]
        )

    if config.affinity_gate_market_config is not None:
        gate_market_overlay = write_runtime_config_overlay(
            config.affinity_gate_market_config,
            output_dir,
            "gate_market_data",
            "data_session.websocket.execution_policy",
            profile.gate_market_data_cpu,
            profile.log_backend_cpu,
        )
        generated_configs["gate_market_data"] = str(gate_market_overlay)
    if config.affinity_binance_market_config is not None:
        binance_market_overlay = write_runtime_config_overlay(
            config.affinity_binance_market_config,
            output_dir,
            "binance_market_data",
            "data_session.websocket.execution_policy",
            profile.binance_market_data_cpu,
            profile.log_backend_cpu,
        )
        generated_configs["binance_market_data"] = str(binance_market_overlay)
    if config.affinity_order_feedback_config is not None:
        feedback_overlay = write_runtime_config_overlay(
            config.affinity_order_feedback_config,
            output_dir,
            "gate_order_feedback",
            "order_feedback_session.websocket.execution_policy",
            profile.gate_order_feedback_cpu,
            profile.log_backend_cpu,
        )
        generated_configs["gate_order_feedback"] = str(feedback_overlay)

    external_configs = sorted(
        key
        for key in (
            "gate_market_data",
            "binance_market_data",
            "gate_order_feedback",
        )
        if key in generated_configs
    )
    if config.affinity_external_configs_applied:
        applied_configs.extend(external_configs)
    generated_only_configs = sorted(
        key for key in generated_configs if key not in set(applied_configs)
    )
    full_split_roles = {
        "strategy",
        "strategy_data_reader",
        "gate_order_session",
        "gate_market_data",
        "binance_market_data",
        "gate_order_feedback",
    }
    applied_set = set(applied_configs)
    summary = {
        "profile": str(profile.path),
        "profile_name": profile.name,
        "numa_node": profile.numa_node,
        "affinity_split": full_split_roles.issubset(applied_set),
        "output_dir": str(output_dir),
        "core_path": {
            "gate_market_data_cpu": profile.gate_market_data_cpu,
            "binance_market_data_cpu": profile.binance_market_data_cpu,
            "strategy_order_owner_cpu": profile.strategy_order_owner_cpu,
            "gate_order_feedback_cpu": profile.gate_order_feedback_cpu,
            "log_backend_cpu": profile.log_backend_cpu,
        },
        "reserved_core_cpus": list(profile.reserved_core_cpus),
        "preferred_aux_cpus": list(profile.preferred_aux_cpus),
        "generated_configs": generated_configs,
        "applied_configs": applied_configs,
        "generated_only_configs": generated_only_configs,
    }
    return replace(config, strategy_command=strategy_command), summary


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


def bitget_category_from_settle(settle: str) -> str:
    normalized = settle.strip().lower()
    if normalized != "usdt":
        raise ValueError(f"unsupported Bitget settle: {settle}")
    return "USDT-FUTURES"


def bitget_query_guard_state(
    requester: Requester,
    settle: str,
    contracts: list[str],
) -> GuardState:
    category = bitget_category_from_settle(settle)
    symbols = bitget_flatten.normalize_symbols(contracts)
    positions, open_orders = bitget_flatten.query_flat_snapshot(
        requester,
        category,
        symbols,
    )
    return GuardState(
        positions=positions,
        open_orders=open_orders,
    )


def bitget_flatten_config_from_guard(
    config: GuardConfig,
) -> bitget_flatten.FlattenConfig:
    return bitget_flatten.FlattenConfig(
        category=bitget_category_from_settle(config.settle),
        scope="allowlist",
        symbols=list(config.contracts),
        confirm_dedicated_account=False,
        dry_run=False,
        poll_timeout_sec=config.poll_timeout_sec,
        poll_interval_sec=config.poll_interval_sec,
        max_position_count=bitget_flatten.DEFAULT_MAX_POSITION_COUNT,
    )


def run_bitget_emergency_flatten(
    config: bitget_flatten.FlattenConfig,
    requester: Requester,
    clock: Any,
) -> tuple[int, dict[str, Any]]:
    return bitget_flatten.run_emergency_flatten(
        config=config, requester=requester, clock=clock
    )


def resolve_gate_adapter_credentials(
    *,
    explicit_api_key: str | None,
    explicit_api_secret: str | None,
    explicit_api_passphrase: str | None,
    strategy_command: list[str],
) -> GuardCredentialEnvNames:
    if explicit_api_passphrase is not None:
        raise ValueError("Gate guard does not accept --api-passphrase")
    return resolve_guard_credential_env_names(
        explicit_api_key=explicit_api_key,
        explicit_api_secret=explicit_api_secret,
        strategy_command=strategy_command,
    )


def gate_requester_factory(
    api_key: str,
    api_secret: str,
    api_passphrase: str | None,
    base_url: str,
    timeout: float,
) -> Requester:
    if api_passphrase is not None:
        raise ValueError("Gate requester does not use an API passphrase")
    return SignedGateTradingClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=base_url,
        timeout=timeout,
    ).request_json


def bitget_requester_factory(
    api_key: str,
    api_secret: str,
    api_passphrase: str | None,
    base_url: str,
    timeout: float,
) -> Requester:
    if api_passphrase is None:
        raise ValueError("Bitget requester requires an API passphrase")
    return SignedBitgetTradingClient(
        api_key=api_key,
        api_secret=api_secret,
        api_passphrase=api_passphrase,
        base_url=base_url,
        timeout=timeout,
    ).request_json


def gate_guard_exchange_adapter() -> GuardExchangeAdapter:
    return GuardExchangeAdapter(
        name="gate",
        credential_resolver=resolve_gate_adapter_credentials,
        requester_factory=gate_requester_factory,
        state_reader=query_guard_state,
        flatten_config_builder=flatten_config_from_guard,
        flatten_runner=run_emergency_flatten,
    )


def bitget_guard_exchange_adapter() -> GuardExchangeAdapter:
    return GuardExchangeAdapter(
        name="bitget",
        credential_resolver=resolve_bitget_guard_credential_env_names,
        requester_factory=bitget_requester_factory,
        state_reader=bitget_query_guard_state,
        flatten_config_builder=bitget_flatten_config_from_guard,
        flatten_runner=run_bitget_emergency_flatten,
    )


def get_runtime_guard_adapter(exchange: str) -> GuardExchangeAdapter:
    return get_guard_exchange_adapter(
        exchange,
        gate_adapter=gate_guard_exchange_adapter(),
        bitget_adapter=bitget_guard_exchange_adapter(),
    )


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
        "exchange": config.exchange,
        "run_id": config.run_id,
        "settle": flatten.normalize_settle(config.settle),
        "contracts": list(config.contracts),
        "strategy_command": list(config.strategy_command),
        "credentials": {
            "api_key_env": config.api_key_env or "",
            "api_secret_env": config.api_secret_env or "",
            "api_passphrase_env": config.api_passphrase_env or "",
            "source": config.credential_source or "",
        },
        "runtime_isolation": config.runtime_isolation,
        "affinity": None,
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
    flatten_config_builder: Callable[[GuardConfig], Any],
) -> tuple[int, dict[str, Any]]:
    try:
        flatten_exit_code, flatten_summary = flatten_runner(
            flatten_config_builder(config), requester, clock
        )
    except Exception as exc:
        error = f"emergency flatten raised {type(exc).__name__}: {exc}"
        summary["flatten"] = {
            "ok": False,
            "result": "exception",
            "error": error,
        }
        summary["ok"] = False
        summary["result"] = f"{reason}_flatten_failed"
        summary["exit_code"] = EXIT_EMERGENCY_FAILED
        summary["errors"].append(error)
        return EXIT_EMERGENCY_FAILED, summary
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


def prepare_affinity_only(config: GuardConfig) -> tuple[int, dict[str, Any]]:
    summary = initial_summary(config)
    try:
        validate_config(config)
        if config.affinity_profile_path is None:
            raise ValueError("--affinity-profile is required with --prepare-affinity-only")
        config, affinity_summary = prepare_affinity_overlays(config)
    except Exception as exc:
        summary["result"] = "config_error"
        summary["exit_code"] = EXIT_CONFIG_ERROR
        summary["errors"].append(f"affinity overlay failed: {type(exc).__name__}: {exc}")
        return EXIT_CONFIG_ERROR, summary

    summary = initial_summary(config)
    summary["affinity"] = affinity_summary
    summary["ok"] = True
    summary["result"] = "affinity_prepared"
    summary["exit_code"] = EXIT_OK
    return EXIT_OK, summary


def run_guarded_live(
    config: GuardConfig,
    requester: Requester,
    process_runner: ProcessRunner = run_strategy_process,
    flatten_runner: FlattenRunner = run_emergency_flatten,
    flatten_config_builder: Callable[[GuardConfig], Any] = flatten_config_from_guard,
    state_reader: StateReader = query_guard_state,
    clock: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    clock = SystemClock() if clock is None else clock
    config = GuardConfig(
        settle=config.settle,
        contracts=normalize_contracts(config.contracts),
        strategy_command=list(config.strategy_command),
        exchange=config.exchange,
        poll_timeout_sec=config.poll_timeout_sec,
        poll_interval_sec=config.poll_interval_sec,
        run_id=config.run_id,
        affinity_profile_path=config.affinity_profile_path,
        affinity_output_dir=config.affinity_output_dir,
        affinity_gate_market_config=config.affinity_gate_market_config,
        affinity_binance_market_config=config.affinity_binance_market_config,
        affinity_order_feedback_config=config.affinity_order_feedback_config,
        affinity_external_configs_applied=config.affinity_external_configs_applied,
        api_key_env=config.api_key_env,
        api_secret_env=config.api_secret_env,
        api_passphrase_env=config.api_passphrase_env,
        credential_source=config.credential_source,
        runtime_isolation=config.runtime_isolation,
    )
    summary = initial_summary(config)
    try:
        validate_config(config)
    except ValueError as exc:
        summary["result"] = "config_error"
        summary["exit_code"] = EXIT_CONFIG_ERROR
        summary["errors"].append(str(exc))
        return EXIT_CONFIG_ERROR, summary

    if config.affinity_profile_path is not None:
        try:
            config, affinity_summary = prepare_affinity_overlays(config)
        except Exception as exc:
            summary["result"] = "config_error"
            summary["exit_code"] = EXIT_CONFIG_ERROR
            summary["errors"].append(f"affinity overlay failed: {type(exc).__name__}: {exc}")
            return EXIT_CONFIG_ERROR, summary
        summary = initial_summary(config)
        summary["affinity"] = affinity_summary

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
        summary["errors"].append(
            "preflight found open orders, positions, residual exposure, or pending orders"
        )
        return EXIT_PREFLIGHT_FAILED, summary

    try:
        process_result = process_runner(config.strategy_command)
    except Exception as exc:
        summary["strategy"] = {"exception": f"{type(exc).__name__}: {exc}"}
        summary["errors"].append("strategy command raised before exit")
        return run_flatten_for_reason(
            summary,
            config,
            requester,
            clock,
            "strategy_exception",
            flatten_runner,
            flatten_config_builder,
        )

    summary["strategy"] = process_result.to_summary()
    if process_result.exit_code != 0:
        return run_flatten_for_reason(
            summary,
            config,
            requester,
            clock,
            "strategy_exit",
            flatten_runner,
            flatten_config_builder,
        )

    try:
        final_state = state_reader(requester, config.settle, config.contracts)
        summary["final_check"] = final_state.to_summary()
    except Exception as exc:
        summary["errors"].append(f"final REST check failed: {type(exc).__name__}: {exc}")
        return run_flatten_for_reason(
            summary,
            config,
            requester,
            clock,
            "final_check_rest_failed",
            flatten_runner,
            flatten_config_builder,
        )

    if not final_state.flat():
        return run_flatten_for_reason(
            summary,
            config,
            requester,
            clock,
            "final_check",
            flatten_runner,
            flatten_config_builder,
        )

    summary["ok"] = True
    summary["result"] = "normal_exit_flat"
    summary["exit_code"] = EXIT_OK
    return EXIT_OK, summary


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run LeadLag live orders behind REST preflight and stop-and-flat guardrails."
    )
    parser.add_argument("--exchange", choices=("gate", "bitget"), default="gate")
    parser.add_argument("--api-key")
    parser.add_argument("--api-secret")
    parser.add_argument("--api-passphrase")
    parser.add_argument(
        "--base-url",
        help="REST base URL. Defaults to the selected exchange endpoint.",
    )
    parser.add_argument("--timeout", type=float, default=account.DEFAULT_TIMEOUT)
    parser.add_argument("--settle", default="usdt")
    parser.add_argument(
        "--contract",
        action="append",
        default=[],
        help="Guarded futures contract or symbol. Can be repeated.",
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
    parser.add_argument("--run-id")
    parser.add_argument("--runtime-manifest", type=Path)
    parser.add_argument("--affinity-profile", type=Path)
    parser.add_argument("--affinity-output-dir", type=Path)
    parser.add_argument("--affinity-gate-market-config", type=Path)
    parser.add_argument("--affinity-binance-market-config", type=Path)
    parser.add_argument("--affinity-order-feedback-config", type=Path)
    parser.add_argument(
        "--prepare-affinity-only",
        action="store_true",
        help="Generate affinity overlay TOMLs, print the summary, and exit.",
    )
    parser.add_argument(
        "--affinity-external-configs-applied",
        action="store_true",
        help=(
            "Mark generated market-data and order-feedback overlays as already "
            "used by their external processes."
        ),
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
        exchange=args.exchange,
        poll_timeout_sec=args.poll_timeout_sec,
        poll_interval_sec=args.poll_interval_sec,
        run_id=args.run_id,
        affinity_profile_path=args.affinity_profile,
        affinity_output_dir=args.affinity_output_dir,
        affinity_gate_market_config=args.affinity_gate_market_config,
        affinity_binance_market_config=args.affinity_binance_market_config,
        affinity_order_feedback_config=args.affinity_order_feedback_config,
        affinity_external_configs_applied=args.affinity_external_configs_applied,
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


def default_rest_base_url(exchange: str) -> str:
    if exchange == "gate":
        return account.DEFAULT_BASE_URL
    if exchange == "bitget":
        return bitget_account.DEFAULT_BASE_URL
    raise ValueError(f"unsupported guard exchange: {exchange}")


def validate_bitget_run_isolation(
    manifest_path: Path,
    strategy_command: list[str],
) -> dict[str, Any]:
    from prepare_bitget_live_run import validate_bitget_run_isolation as validate

    return validate(manifest_path, strategy_command)


def run_from_args(
    args: argparse.Namespace,
    adapter: GuardExchangeAdapter | None = None,
    process_runner: ProcessRunner = run_strategy_process,
    clock: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    config = config_from_args(args)
    if args.prepare_affinity_only:
        return prepare_affinity_only(config)

    if config.exchange == "bitget":
        try:
            validate_bitget_strategy_command(config.strategy_command)
        except ValueError as exc:
            summary = initial_summary(config)
            summary["result"] = "config_error"
            summary["exit_code"] = EXIT_CONFIG_ERROR
            summary["errors"].append(str(exc))
            return EXIT_CONFIG_ERROR, summary

    if config.exchange == "bitget" and strategy_execute_requested(
        config.strategy_command
    ):
        if args.runtime_manifest is None:
            summary = initial_summary(config)
            summary["result"] = "config_error"
            summary["exit_code"] = EXIT_CONFIG_ERROR
            summary["errors"].append(
                "Bitget --execute requires --runtime-manifest for fresh-run isolation"
            )
            return EXIT_CONFIG_ERROR, summary
        try:
            manifest = validate_bitget_run_isolation(
                args.runtime_manifest,
                config.strategy_command,
            )
            lag_symbols = validate_bitget_guard_contract_scope(
                config.strategy_command,
                config.contracts,
            )
            if config.run_id is not None and config.run_id != manifest["run_id"]:
                raise ValueError(
                    f"--run-id {config.run_id} does not match runtime manifest "
                    f"{manifest['run_id']}"
                )
            config = replace(
                config,
                run_id=manifest["run_id"],
                runtime_isolation={
                    **manifest,
                    "strategy_lag_symbols": lag_symbols,
                    "manifest": str(args.runtime_manifest.expanduser().resolve()),
                    "validated": True,
                },
            )
        except Exception as exc:
            summary = initial_summary(config)
            summary["result"] = "config_error"
            summary["exit_code"] = EXIT_CONFIG_ERROR
            summary["errors"].append(
                f"run isolation validation failed: {type(exc).__name__}: {exc}"
            )
            return EXIT_CONFIG_ERROR, summary

    try:
        adapter = (
            get_runtime_guard_adapter(config.exchange)
            if adapter is None
            else adapter
        )
        if adapter.name != config.exchange:
            raise ValueError(
                f"adapter {adapter.name} does not match exchange {config.exchange}"
            )
    except Exception as exc:
        summary = initial_summary(config)
        summary["result"] = "config_error"
        summary["exit_code"] = EXIT_CONFIG_ERROR
        summary["errors"].append(
            f"adapter resolution failed: {type(exc).__name__}: {exc}"
        )
        return EXIT_CONFIG_ERROR, summary

    try:
        credential_env_names = adapter.credential_resolver(
            explicit_api_key=args.api_key,
            explicit_api_secret=args.api_secret,
            explicit_api_passphrase=args.api_passphrase,
            strategy_command=config.strategy_command,
        )
    except Exception as exc:
        summary = initial_summary(config)
        summary["result"] = "config_error"
        summary["exit_code"] = EXIT_CONFIG_ERROR
        summary["errors"].append(
            f"credential resolution failed: {type(exc).__name__}: {exc}"
        )
        return EXIT_CONFIG_ERROR, summary

    config = replace(
        config,
        api_key_env=credential_env_names.api_key_env,
        api_secret_env=credential_env_names.api_secret_env,
        api_passphrase_env=credential_env_names.api_passphrase_env,
        credential_source=credential_env_names.source,
    )

    api_key = os.getenv(credential_env_names.api_key_env)
    if not api_key:
        return EXIT_CONFIG_ERROR, missing_env_summary(
            config, credential_env_names.api_key_env
        )
    api_secret = os.getenv(credential_env_names.api_secret_env)
    if not api_secret:
        return EXIT_CONFIG_ERROR, missing_env_summary(
            config, credential_env_names.api_secret_env
        )
    api_passphrase = None
    if credential_env_names.api_passphrase_env is not None:
        api_passphrase = os.getenv(credential_env_names.api_passphrase_env)
        if not api_passphrase:
            return EXIT_CONFIG_ERROR, missing_env_summary(
                config, credential_env_names.api_passphrase_env
            )

    try:
        requester = adapter.requester_factory(
            api_key,
            api_secret,
            api_passphrase,
            args.base_url or default_rest_base_url(config.exchange),
            args.timeout,
        )
    except Exception as exc:
        summary = initial_summary(config)
        summary["result"] = "config_error"
        summary["exit_code"] = EXIT_CONFIG_ERROR
        summary["errors"].append(
            f"requester creation failed: {type(exc).__name__}: {exc}"
        )
        return EXIT_CONFIG_ERROR, summary

    return run_guarded_live(
        config=config,
        requester=requester,
        process_runner=process_runner,
        flatten_runner=adapter.flatten_runner,
        flatten_config_builder=adapter.flatten_config_builder,
        state_reader=adapter.state_reader,
        clock=clock,
    )


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    exit_code, summary = run_from_args(args)
    print_summary(summary, args.pretty)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
