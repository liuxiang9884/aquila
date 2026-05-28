#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import glob
import ipaddress
import json
import re
import socket
import sys
import time
from pathlib import Path
from typing import Callable, TextIO

import probe_gate_ws_connect_ip as probe


SCHEMA = "aquila.gate.order_session.ip_discovery.v1"
TXT_SCHEMA = "aquila.gate.order_session.candidate_ips.v1"
DEFAULT_TARGET = probe.DEFAULT_TARGET
DEFAULT_TIMEOUT = probe.DEFAULT_TIMEOUT
CONNECTED_TAG = "gate_order_session_connected"


def make_run_id(now: float | None = None) -> str:
    timestamp = time.gmtime(time.time() if now is None else now)
    return time.strftime("%Y%m%d_%H%M%S_gate_ip_discovery", timestamp)


def normalize_ip(ip: str) -> str:
    return str(ipaddress.ip_address(ip.strip()))


def split_endpoint(endpoint: str) -> tuple[str, int]:
    if not endpoint:
        return "", 0
    host, sep, port_text = endpoint.rpartition(":")
    if sep == "":
        return endpoint, 0
    try:
        return host.strip("[]"), int(port_text)
    except ValueError:
        return host.strip("[]"), 0


def ms_to_ns(value: float | None) -> int:
    if value is None:
        return 0
    return int(round(value * 1_000_000))


def parse_http_status(status_line: str) -> int:
    match = re.search(r"\s(\d{3})(?:\s|$)", status_line)
    if match is None:
        return 0
    return int(match.group(1))


def empty_dns_summary() -> dict:
    return {
        "seen_count": 0,
        "first_seen_ns": 0,
        "last_seen_ns": 0,
        "resolvers": [],
    }


def empty_history_summary() -> dict:
    return {
        "seen_count": 0,
        "paths": [],
        "first_seen_text": "",
        "last_seen_text": "",
    }


def empty_websocket_summary() -> dict:
    return {
        "attempted": False,
        "ok": False,
        "remote_ip": "",
        "remote_port": 0,
        "local_ip": "",
        "local_port": 0,
        "tcp_connect_ns": 0,
        "tls_handshake_ns": 0,
        "websocket_handshake_ns": 0,
        "total_ns": 0,
        "http_status": 0,
        "error": "",
    }


def ensure_record(
    records: dict[str, dict],
    *,
    run_id: str,
    host: str,
    target: str,
    port: str,
    ip: str,
) -> dict:
    normalized = normalize_ip(ip)
    existing = records.get(normalized)
    if existing is not None:
        return existing

    record = {
        "schema": SCHEMA,
        "run_id": run_id,
        "host": host,
        "target": target,
        "port": int(port),
        "ip": normalized,
        "status": "discovered",
        "sources": [],
        "dns": empty_dns_summary(),
        "history": empty_history_summary(),
        "websocket_verify": empty_websocket_summary(),
        "selected_for_rtt_probe": False,
        "rejection_reason": "",
    }
    records[normalized] = record
    return record


def add_source(record: dict, source: str) -> None:
    if source not in record["sources"]:
        record["sources"].append(source)


def add_dns_sample(
    records: dict[str, dict],
    *,
    run_id: str,
    host: str,
    target: str,
    port: str,
    resolver: str,
    observed_ns: int,
    ips: list[str],
) -> None:
    source = f"dns_{resolver}"
    for ip in ips:
        record = ensure_record(
            records, run_id=run_id, host=host, target=target, port=port, ip=ip
        )
        add_source(record, source)
        dns = record["dns"]
        dns["seen_count"] += 1
        if dns["first_seen_ns"] == 0:
            dns["first_seen_ns"] = observed_ns
        dns["last_seen_ns"] = observed_ns
        if resolver not in dns["resolvers"]:
            dns["resolvers"].append(resolver)


def resolve_system_ips(host: str, port: str) -> list[str]:
    results = socket.getaddrinfo(host, int(port), type=socket.SOCK_STREAM)
    ips: list[str] = []
    seen: set[str] = set()
    for result in results:
        sockaddr = result[4]
        if len(sockaddr) < 1:
            continue
        try:
            ip = normalize_ip(str(sockaddr[0]))
        except ValueError:
            continue
        if ip not in seen:
            seen.add(ip)
            ips.append(ip)
    return ips


def collect_dns_samples(
    records: dict[str, dict],
    *,
    run_id: str,
    host: str,
    target: str,
    port: str,
    duration_sec: float,
    interval_sec: float,
    resolver: Callable[[str, str], list[str]] = resolve_system_ips,
    now_ns: Callable[[], int] = time.time_ns,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    deadline = monotonic() + max(0.0, duration_sec)
    while True:
        observed_ns = now_ns()
        add_dns_sample(
            records,
            run_id=run_id,
            host=host,
            target=target,
            port=port,
            resolver="system",
            observed_ns=observed_ns,
            ips=resolver(host, port),
        )
        remaining = deadline - monotonic()
        if remaining <= 0:
            break
        sleep(min(interval_sec, remaining))


HISTORY_TS_RE = re.compile(r"^[A-Z](\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")
REMOTE_IP_RE = re.compile(r"\bremote_ip=([0-9A-Fa-f:.]+)\b")


def parse_history_line(line: str) -> tuple[str, str] | None:
    if CONNECTED_TAG not in line:
        return None
    ip_match = REMOTE_IP_RE.search(line)
    if ip_match is None:
        return None
    timestamp_match = HISTORY_TS_RE.search(line)
    timestamp = timestamp_match.group(1) if timestamp_match is not None else ""
    return normalize_ip(ip_match.group(1)), timestamp


def expand_history_paths(patterns: list[str]) -> list[str]:
    paths: list[str] = []
    seen: set[str] = set()
    for pattern in patterns:
        matches = sorted(glob.glob(pattern))
        for match in matches or [pattern]:
            if match not in seen:
                seen.add(match)
                paths.append(match)
    return paths


def merge_history_logs(
    records: dict[str, dict],
    *,
    run_id: str,
    host: str,
    target: str,
    port: str,
    paths: list[str],
) -> None:
    for path_text in expand_history_paths(paths):
        path = Path(path_text)
        if not path.exists():
            continue
        with path.open("r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                parsed = parse_history_line(line)
                if parsed is None:
                    continue
                ip, timestamp = parsed
                record = ensure_record(
                    records,
                    run_id=run_id,
                    host=host,
                    target=target,
                    port=port,
                    ip=ip,
                )
                add_source(record, "history_log")
                history = record["history"]
                history["seen_count"] += 1
                if path_text not in history["paths"]:
                    history["paths"].append(path_text)
                if timestamp:
                    if not history["first_seen_text"]:
                        history["first_seen_text"] = timestamp
                    history["last_seen_text"] = timestamp


def apply_websocket_result(record: dict, result: probe.ProbeResult) -> None:
    remote_ip, remote_port = split_endpoint(result.remote_endpoint)
    local_ip, local_port = split_endpoint(result.local_endpoint)
    record["websocket_verify"] = {
        "attempted": True,
        "ok": result.ok,
        "remote_ip": remote_ip,
        "remote_port": remote_port,
        "local_ip": local_ip,
        "local_port": local_port,
        "tcp_connect_ns": ms_to_ns(result.tcp_connect_ms),
        "tls_handshake_ns": ms_to_ns(result.tls_handshake_ms),
        "websocket_handshake_ns": ms_to_ns(result.websocket_handshake_ms),
        "total_ns": ms_to_ns(result.total_ms),
        "http_status": parse_http_status(result.websocket_status),
        "error": result.error,
    }
    if result.ok:
        record["status"] = "ws_verified"
        record["selected_for_rtt_probe"] = True
        record["rejection_reason"] = ""
        return
    record["status"] = "rejected"
    record["selected_for_rtt_probe"] = False
    record["rejection_reason"] = "websocket_verify_failed"


def verify_websocket_candidates(
    records: dict[str, dict],
    verifier: Callable[[str], probe.ProbeResult],
) -> None:
    for ip in list(records.keys()):
        apply_websocket_result(records[ip], verifier(ip))


def ordered_records(records: dict[str, dict]) -> list[dict]:
    selected = [record for record in records.values() if record["selected_for_rtt_probe"]]
    unselected = [
        record for record in records.values() if not record["selected_for_rtt_probe"]
    ]
    return selected + unselected


def write_jsonl(records: dict[str, dict], handle: TextIO) -> None:
    for record in ordered_records(records):
        handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True))
        handle.write("\n")


def write_candidate_txt(
    records: dict[str, dict],
    handle: TextIO,
    *,
    run_id: str,
    host: str,
    target: str,
    generated_at_ns: int,
) -> None:
    handle.write(f"# schema={TXT_SCHEMA}\n")
    handle.write(f"# run_id={run_id}\n")
    handle.write(f"# host={host}\n")
    handle.write(f"# target={target}\n")
    handle.write(f"# generated_at_ns={generated_at_ns}\n")
    for record in ordered_records(records):
        if record["selected_for_rtt_probe"]:
            handle.write(f"{record['ip']}\n")


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Discover Gate WebSocket candidate connect_ip values for later "
            "OrderSession RTT probes. This script never sends orders."
        )
    )
    parser.add_argument("--host", default=probe.DEFAULT_HOST)
    parser.add_argument("--port", default=probe.DEFAULT_PORT)
    parser.add_argument("--target", default=DEFAULT_TARGET)
    parser.add_argument("--duration-sec", type=float, default=180.0)
    parser.add_argument("--interval-sec", type=float, default=5.0)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument("--run-id", default="")
    parser.add_argument(
        "--history-log",
        action="append",
        default=[],
        help="Historical log path or glob to scan for gate_order_session_connected remote_ip.",
    )
    parser.add_argument(
        "--no-verify-websocket",
        action="store_true",
        help="Skip pinned WebSocket handshake verification.",
    )
    parser.add_argument("--plain", action="store_true", help="Disable TLS.")
    parser.add_argument("--output", required=True, help="JSONL output path.")
    parser.add_argument(
        "--text-output", required=True, help="candidate_ips.txt output path."
    )
    return parser.parse_args(argv)


def validate_args(args: argparse.Namespace) -> str:
    if args.duration_sec < 0:
        return "--duration-sec must be non-negative"
    if args.interval_sec <= 0:
        return "--interval-sec must be positive"
    if args.timeout <= 0:
        return "--timeout must be positive"
    try:
        int(args.port)
    except ValueError:
        return "--port must be an integer"
    return ""


def make_probe_verifier(args: argparse.Namespace) -> Callable[[str], probe.ProbeResult]:
    def verifier(ip: str) -> probe.ProbeResult:
        return probe.probe_connect_ip(
            connect_ip=ip,
            host=args.host,
            port=args.port,
            target=args.target,
            timeout=args.timeout,
            use_tls=not args.plain,
            login=False,
            api_key=None,
            api_secret=None,
            size_decimal_header=True,
        )

    return verifier


def main() -> int:
    args = parse_args(sys.argv[1:])
    error = validate_args(args)
    if error:
        print(f"[FAIL] {error}", file=sys.stderr)
        return 2

    run_id = args.run_id or make_run_id()
    records: dict[str, dict] = {}
    collect_dns_samples(
        records,
        run_id=run_id,
        host=args.host,
        target=args.target,
        port=args.port,
        duration_sec=args.duration_sec,
        interval_sec=args.interval_sec,
    )
    merge_history_logs(
        records,
        run_id=run_id,
        host=args.host,
        target=args.target,
        port=args.port,
        paths=args.history_log,
    )
    if not args.no_verify_websocket:
        verify_websocket_candidates(records, make_probe_verifier(args))

    output_path = Path(args.output)
    text_output_path = Path(args.text_output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    text_output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as handle:
        write_jsonl(records, handle)
    generated_at_ns = time.time_ns()
    with text_output_path.open("w", encoding="utf-8") as handle:
        write_candidate_txt(
            records,
            handle,
            run_id=run_id,
            host=args.host,
            target=args.target,
            generated_at_ns=generated_at_ns,
        )

    selected = sum(1 for record in records.values() if record["selected_for_rtt_probe"])
    summary = {
        "run_id": run_id,
        "records": len(records),
        "selected_for_rtt_probe": selected,
        "output": str(output_path),
        "text_output": str(text_output_path),
    }
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    if not records:
        return 1
    if not args.no_verify_websocket and selected == 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
