#!/home/liuxiang/dev/pyenv/lx/bin/python

from dataclasses import dataclass
from typing import Any, Callable


@dataclass(frozen=True)
class GuardCredentialEnvNames:
    api_key_env: str
    api_secret_env: str
    api_passphrase_env: str | None
    source: str


@dataclass(frozen=True)
class GuardExchangeAdapter:
    name: str
    credential_resolver: Callable[..., GuardCredentialEnvNames]
    requester_factory: Callable[..., Any]
    state_reader: Callable[..., Any]
    flatten_config_builder: Callable[[Any], Any]
    flatten_runner: Callable[[Any, Any, Any], tuple[int, dict[str, Any]]]


def get_guard_exchange_adapter(
    exchange: str,
    gate_adapter: GuardExchangeAdapter,
    bitget_adapter: GuardExchangeAdapter | None = None,
) -> GuardExchangeAdapter:
    normalized = exchange.strip().lower()
    if normalized == "gate":
        return gate_adapter
    if normalized == "bitget" and bitget_adapter is not None:
        return bitget_adapter
    raise ValueError(f"unsupported guard exchange: {exchange}")


if __name__ == "__main__":
    raise SystemExit("guard_exchange_adapter is a library module")
