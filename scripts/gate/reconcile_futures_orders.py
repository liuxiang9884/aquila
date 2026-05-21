#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import json
import sys
from dataclasses import replace
from decimal import Decimal, InvalidOperation
from pathlib import Path
from typing import Any, Iterable

import query_gate_account as account


RECOVERED = "Recovered"
MANUAL_INTERVENTION = "ManualIntervention"
TERMINAL_STATUSES = {
    "cancelled",
    "canceled",
    "closed",
    "finished",
    "filled",
    "rejected",
}


def _relabeled(api_requests: list[account.ApiRequest], label: str) -> account.ApiRequest:
    if len(api_requests) != 1:
        raise ValueError(f"expected one ApiRequest for {label}, got {len(api_requests)}")
    return replace(api_requests[0], label=label)


def build_reconcile_query_plan(
    settle: str,
    contract: str,
    finished_limit: int,
) -> list[account.ApiRequest]:
    return [
        _relabeled(
            account.build_order_query_plan(
                settle=settle,
                contract=contract,
                status="open",
                order_id=None,
                limit=finished_limit,
                last_id=None,
            ),
            "open_orders",
        ),
        _relabeled(
            account.build_order_query_plan(
                settle=settle,
                contract=contract,
                status="finished",
                order_id=None,
                limit=finished_limit,
                last_id=None,
            ),
            "finished_orders",
        ),
        _relabeled(
            account.build_position_query_plan(settle=settle, contract=contract),
            "rest_position",
        ),
    ]


def _query_reconcile_facts(
    requester: account.Requester,
    api_requests: Iterable[account.ApiRequest],
    allow_partial: bool,
) -> dict[str, Any]:
    if allow_partial:
        result = account.query_requests(
            requester=requester,
            requests=api_requests,
            allow_partial=True,
        )
        result["allow_partial"] = True
        return result

    results: dict[str, Any] = {}
    errors: dict[str, str] = {}
    for api_request in api_requests:
        result = account.query_requests(
            requester=requester,
            requests=[api_request],
            allow_partial=True,
        )
        if not result["ok"]:
            errors.update(result["errors"])
            break
        results.update(result["results"])
    return {
        "ok": not errors,
        "results": results if not errors else {},
        "errors": errors,
        "allow_partial": False,
    }


def load_local_state_json(value: str | None) -> dict[str, Any]:
    if value is None or value.strip() == "":
        return {}

    text = value.strip()
    if text.startswith("@"):
        text = Path(text[1:]).read_text(encoding="utf-8")
    elif not text.startswith("{") and not text.startswith("["):
        path = Path(text)
        if path.exists():
            text = path.read_text(encoding="utf-8")

    parsed = json.loads(text)
    if isinstance(parsed, list):
        return {"orders": parsed}
    if not isinstance(parsed, dict):
        raise ValueError("local state JSON must be an object or an order list")
    return parsed


def _as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def _local_order_dicts(value: Any) -> list[dict[str, Any]]:
    orders = value.values() if isinstance(value, dict) else _as_list(value)
    return [dict(order) for order in orders if isinstance(order, dict)]


def _rest_order_dicts(value: Any, label: str) -> tuple[list[dict[str, Any]], list[str]]:
    if not isinstance(value, list):
        return [], [f"invalid REST {label}: expected list of order objects"]

    orders: list[dict[str, Any]] = []
    errors: list[str] = []
    for index, order in enumerate(value):
        if not isinstance(order, dict):
            errors.append(f"invalid REST {label}[{index}]: expected order object")
            continue
        orders.append(dict(order))
    return orders, errors


def _local_orders(local_state: dict[str, Any]) -> list[dict[str, Any]]:
    if "orders" in local_state:
        return _local_order_dicts(local_state["orders"])
    if "local_orders" in local_state:
        return _local_order_dicts(local_state["local_orders"])
    return []


def _to_int(value: Any) -> int | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        return int(str(value).strip())
    except ValueError:
        return None


def _to_decimal(value: Any) -> Decimal | None:
    if isinstance(value, bool) or value is None:
        return None
    try:
        parsed = Decimal(str(value).strip())
    except (InvalidOperation, ValueError):
        return None
    if not parsed.is_finite():
        return None
    return parsed


def _decimal_text(value: Decimal) -> str:
    if value == value.to_integral_value():
        return str(int(value))
    return format(value.normalize(), "f")


def _first_decimal(
    mapping: dict[str, Any],
    keys: Iterable[str],
    label: str,
) -> tuple[Decimal | None, str | None]:
    for key in keys:
        if key in mapping:
            parsed = _to_decimal(mapping[key])
            if parsed is None:
                return None, f"invalid {label}.{key}: expected finite decimal"
            return parsed, None
    return None, None


def _local_position_size(local_state: dict[str, Any]) -> tuple[Decimal, list[str]]:
    sources: list[tuple[str, Decimal]] = []
    errors: list[str] = []

    top_level, error = _first_decimal(
        local_state,
        ("position_size", "signed_position_quantity", "signed_position_size"),
        "local_state",
    )
    if error is not None:
        errors.append(error)
    elif top_level is not None:
        sources.append(("top_level", top_level))

    if "position" in local_state:
        position = local_state.get("position")
        if not isinstance(position, dict):
            errors.append("invalid local position: expected object")
        else:
            position_size, error = _first_decimal(
                position,
                ("size", "quantity", "position_size", "signed_quantity"),
                "local position",
            )
            if error is not None:
                errors.append(error)
            elif position_size is not None:
                sources.append(("position", position_size))

    if "execution_groups" in local_state:
        groups = local_state.get("execution_groups")
        if not isinstance(groups, list):
            errors.append("invalid local execution_groups: expected list")
        else:
            total = Decimal(0)
            for index, group in enumerate(groups):
                if not isinstance(group, dict):
                    errors.append(f"invalid local execution_groups[{index}]: expected object")
                    continue
                group_size, error = _first_decimal(
                    group,
                    ("signed_position_quantity", "position_size", "size", "quantity"),
                    f"local execution_groups[{index}]",
                )
                if error is not None:
                    errors.append(error)
                elif group_size is not None:
                    total += group_size
            sources.append(("execution_groups", total))

    if not sources:
        return Decimal(0), errors

    first_value = sources[0][1]
    conflicting_sources = [(name, value) for name, value in sources if value != first_value]
    if conflicting_sources:
        source_text = ", ".join(
            f"{name}={_decimal_text(value)}" for name, value in sources
        )
        errors.append(f"conflicting local position sources: {source_text}")
    return first_value, errors


def _rest_position_from_result(
    value: Any,
    contract: str,
) -> tuple[dict[str, Any] | None, list[str]]:
    if isinstance(value, dict):
        return dict(value), []
    normalized_contract = account.normalize_contract(contract)
    if not isinstance(value, list):
        return None, ["invalid REST position: expected object or list"]

    errors: list[str] = []
    for index, item in enumerate(value):
        if not isinstance(item, dict):
            errors.append(f"invalid REST position[{index}]: expected object")
            continue
        if str(item.get("contract", "")).upper() == normalized_contract:
            return dict(item), errors
    errors.append(f"invalid REST position: missing contract {normalized_contract}")
    return None, errors


def _rest_position_size(rest_position: dict[str, Any] | None) -> tuple[Decimal, list[str]]:
    if rest_position is None:
        return Decimal(0), []
    value, error = _first_decimal(
        rest_position,
        ("size", "quantity", "position_size"),
        "REST position",
    )
    if error is not None:
        return Decimal(0), [f"invalid REST position: {error}"]
    if value is None:
        return Decimal(0), ["invalid REST position: missing size"]
    return value, []


def _rest_pending_orders(rest_position: dict[str, Any] | None) -> tuple[Decimal, list[str]]:
    if rest_position is None:
        return Decimal(0), []
    value, error = _first_decimal(
        rest_position,
        ("pending_orders", "pending_order_count"),
        "REST position",
    )
    if error is not None:
        return Decimal(0), [f"invalid REST position: {error}"]
    return Decimal(0) if value is None else value, []


def _order_is_finished(order: dict[str, Any]) -> bool:
    explicit = order.get("is_finished")
    if isinstance(explicit, bool):
        return explicit
    status = str(order.get("status", "")).strip().lower()
    return status in TERMINAL_STATUSES


def _pending_local_orders(
    local_orders: list[dict[str, Any]],
    contract: str,
    strategy_id: int,
) -> list[dict[str, Any]]:
    normalized_contract = account.normalize_contract(contract)
    pending = []
    for order in local_orders:
        local_id = _to_int(order.get("local_order_id"))
        if local_id is None or local_id == 0:
            continue
        if (local_id >> 56) != strategy_id:
            continue
        order_contract = str(order.get("contract", normalized_contract)).upper()
        if order_contract != normalized_contract:
            continue
        if not _order_is_finished(order):
            pending.append(order)
    return pending


def _exchange_order_id(order: dict[str, Any]) -> str:
    for key in ("exchange_order_id", "id", "order_id"):
        value = order.get(key)
        if value is not None and str(value).strip():
            return str(value).strip()
    return ""


def _parse_text_local_order_id(text: Any) -> int | None:
    text_value = "" if text is None else str(text).strip()
    if not text_value.startswith("t-"):
        return None
    return _to_int(text_value[2:])


def _map_remote_orders(
    pending_local_orders: list[dict[str, Any]],
    open_orders: list[dict[str, Any]],
    finished_orders: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], list[str]]:
    local_by_id = {
        _to_int(order.get("local_order_id")): order
        for order in pending_local_orders
        if _to_int(order.get("local_order_id")) is not None
    }
    local_by_exchange_id = {
        _exchange_order_id(order): order
        for order in pending_local_orders
        if _exchange_order_id(order)
    }

    mapped_orders: list[dict[str, Any]] = []
    mapped_local_ids: set[int] = set()
    mapped_open_indexes: set[int] = set()
    duplicate_local_ids: set[int] = set()

    remote_sources = [("open_orders", open_orders), ("finished_orders", finished_orders)]
    for source, remote_orders in remote_sources:
        for index, remote_order in enumerate(remote_orders):
            local_order = None
            matched_by = ""
            text_local_id = _parse_text_local_order_id(remote_order.get("text"))
            if text_local_id is not None and text_local_id in local_by_id:
                local_order = local_by_id[text_local_id]
                matched_by = "text"
            else:
                remote_exchange_id = _exchange_order_id(remote_order)
                if remote_exchange_id and remote_exchange_id in local_by_exchange_id:
                    local_order = local_by_exchange_id[remote_exchange_id]
                    matched_by = "exchange_order_id"

            if local_order is None:
                continue

            local_id = _to_int(local_order.get("local_order_id"))
            if local_id is None:
                continue
            if local_id in mapped_local_ids:
                duplicate_local_ids.add(local_id)
            mapped_local_ids.add(local_id)
            if source == "open_orders":
                mapped_open_indexes.add(index)
            mapped_orders.append(
                {
                    "local_order_id": local_id,
                    "matched_by": matched_by,
                    "remote_source": source,
                    "local_order": local_order,
                    "remote_order": remote_order,
                }
            )

    unmapped_local_orders = [
        order
        for order in pending_local_orders
        if _to_int(order.get("local_order_id")) not in mapped_local_ids
    ]
    unmapped_remote_orders = [
        order for index, order in enumerate(open_orders) if index not in mapped_open_indexes
    ]
    duplicate_errors = [
        f"duplicate remote facts for local_order_id={local_id}"
        for local_id in sorted(duplicate_local_ids)
    ]
    return mapped_orders, unmapped_local_orders, unmapped_remote_orders, duplicate_errors


def _manual_reason(
    *,
    queries: dict[str, Any],
    validation_errors: list[str],
    position_match: bool,
    local_position: Decimal,
    rest_position: Decimal,
    rest_pending_orders: Decimal,
    mapped_orders: list[dict[str, Any]],
    unmapped_local_orders: list[dict[str, Any]],
    unmapped_remote_orders: list[dict[str, Any]],
    duplicate_errors: list[str],
) -> str:
    if not queries["ok"]:
        labels = ", ".join(sorted(queries["errors"].keys()))
        return f"REST query failed: {labels}"
    if validation_errors:
        return validation_errors[0]
    if duplicate_errors:
        return "; ".join(duplicate_errors)
    if not position_match:
        return (
            "position mismatch: "
            f"local={_decimal_text(local_position)} rest={_decimal_text(rest_position)}"
        )
    if rest_pending_orders != 0:
        return f"REST pending_orders not zero: {_decimal_text(rest_pending_orders)}"
    if unmapped_remote_orders:
        return "unmapped remote open order"
    if unmapped_local_orders:
        return "local pending order missing REST fact"
    if mapped_orders:
        return "mapped pending/open orders require manual review"
    return ""


def reconcile_futures_orders(
    requester: account.Requester,
    local_state: dict[str, Any] | None = None,
    *,
    settle: str = "usdt",
    contract: str = "BTC_USDT",
    strategy_id: int = 4,
    finished_limit: int = 50,
    window_sec: int = 300,
    allow_partial: bool = False,
) -> dict[str, Any]:
    if finished_limit <= 0:
        raise ValueError("finished_limit must be positive")
    if window_sec <= 0:
        raise ValueError("window_sec must be positive")
    if strategy_id < 0 or strategy_id > 255:
        raise ValueError("strategy_id must be in [0, 255]")

    normalized_contract = account.normalize_contract(contract)
    local_state = {} if local_state is None else local_state
    local_orders = _local_orders(local_state)
    pending_local_orders = _pending_local_orders(local_orders, normalized_contract, strategy_id)
    local_position, local_validation_errors = _local_position_size(local_state)

    query_plan = build_reconcile_query_plan(
        settle=settle,
        contract=normalized_contract,
        finished_limit=finished_limit,
    )
    queries = _query_reconcile_facts(
        requester=requester,
        api_requests=query_plan,
        allow_partial=allow_partial,
    )
    results = queries["results"]
    validation_errors = list(local_validation_errors)
    open_orders: list[dict[str, Any]] = []
    finished_orders: list[dict[str, Any]] = []
    rest_position: dict[str, Any] | None = None
    rest_position_size = Decimal(0)
    rest_pending_orders = Decimal(0)

    if "open_orders" in results:
        open_orders, errors = _rest_order_dicts(results["open_orders"], "open_orders")
        validation_errors.extend(errors)
    elif queries["ok"]:
        validation_errors.append("invalid REST open_orders: missing response")

    if "finished_orders" in results:
        finished_orders, errors = _rest_order_dicts(results["finished_orders"], "finished_orders")
        validation_errors.extend(errors)
    elif queries["ok"]:
        validation_errors.append("invalid REST finished_orders: missing response")

    if "rest_position" in results:
        rest_position, errors = _rest_position_from_result(
            results["rest_position"],
            normalized_contract,
        )
        validation_errors.extend(errors)
        rest_position_size, errors = _rest_position_size(rest_position)
        validation_errors.extend(errors)
        rest_pending_orders, errors = _rest_pending_orders(rest_position)
        validation_errors.extend(errors)
    elif queries["ok"]:
        validation_errors.append("invalid REST position: missing response")

    rest_position_valid = not any(error.startswith("invalid REST position") for error in validation_errors)
    local_position_valid = not any(error.startswith("invalid local") for error in validation_errors)
    local_position_valid = local_position_valid and not any(
        error.startswith("conflicting local position") for error in validation_errors
    )
    position_match = rest_position_valid and local_position_valid and local_position == rest_position_size

    (
        mapped_orders,
        unmapped_local_orders,
        unmapped_remote_orders,
        duplicate_errors,
    ) = _map_remote_orders(
        pending_local_orders=pending_local_orders,
        open_orders=open_orders,
        finished_orders=finished_orders,
    )

    reason = _manual_reason(
        queries=queries,
        validation_errors=validation_errors,
        position_match=position_match,
        local_position=local_position,
        rest_position=rest_position_size,
        rest_pending_orders=rest_pending_orders,
        mapped_orders=mapped_orders,
        unmapped_local_orders=unmapped_local_orders,
        unmapped_remote_orders=unmapped_remote_orders,
        duplicate_errors=duplicate_errors,
    )
    state = RECOVERED if reason == "" else MANUAL_INTERVENTION

    return {
        "state": state,
        "manual_intervention_reason": reason,
        "mapped_orders": mapped_orders,
        "unmapped_local_orders": unmapped_local_orders,
        "unmapped_remote_orders": unmapped_remote_orders,
        "rest_position": rest_position,
        "position_match": position_match,
        "queries": queries,
        "validation_errors": validation_errors,
        "local_position_size": _decimal_text(local_position),
        "rest_position_size": _decimal_text(rest_position_size),
        "strategy_id": strategy_id,
        "contract": normalized_contract,
        "settle": account.normalize_settle_path(settle),
        "finished_limit": finished_limit,
        "window_sec": window_sec,
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read-only Gate futures order and position reconcile helper."
    )
    account.add_common_args(parser)
    parser.add_argument("--contract", default="BTC_USDT", help="Gate futures contract.")
    parser.add_argument("--strategy-id", type=int, default=4, help="LeadLag strategy id lane.")
    parser.add_argument(
        "--local-state-json",
        default="{}",
        help="Local state JSON object/list, @path, or plain file path.",
    )
    parser.add_argument(
        "--finished-limit",
        type=int,
        default=50,
        help="Bounded order list limit for open and finished REST facts.",
    )
    parser.add_argument(
        "--window-sec",
        type=int,
        default=300,
        help="Diagnostic recovery window seconds for summary metadata.",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    api_key = account.get_env_value(args.api_key)
    if api_key is None:
        print(f"[FAIL] missing env var {args.api_key}", file=sys.stderr)
        return 2
    api_secret = account.get_env_value(args.api_secret)
    if api_secret is None:
        print(f"[FAIL] missing env var {args.api_secret}", file=sys.stderr)
        return 2

    try:
        local_state = load_local_state_json(args.local_state_json)
    except Exception as exc:
        print(f"[FAIL] invalid --local-state-json: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2

    client = account.SignedGateRestClient(
        api_key=api_key,
        api_secret=api_secret,
        base_url=args.base_url,
        timeout=args.timeout,
    )
    try:
        summary = reconcile_futures_orders(
            requester=client.get_json,
            settle=args.settle,
            contract=args.contract,
            strategy_id=args.strategy_id,
            local_state=local_state,
            finished_limit=args.finished_limit,
            window_sec=args.window_sec,
            allow_partial=args.allow_partial,
        )
    except Exception as exc:  # pragma: no cover - utility script
        print(f"[FAIL] {type(exc).__name__}: {exc}", file=sys.stderr)
        return 1

    indent = 2 if args.pretty else None
    print(json.dumps(summary, ensure_ascii=False, indent=indent, sort_keys=True))
    return 0 if summary["state"] == RECOVERED else 1


if __name__ == "__main__":
    sys.exit(main())
