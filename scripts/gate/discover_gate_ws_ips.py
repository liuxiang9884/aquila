#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
from dataclasses import dataclass
import glob
import ipaddress
import json
import re
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Callable, TextIO

import probe_gate_ws_connect_ip as probe


SCHEMA = "aquila.gate.order_session.ip_discovery.v1"
TXT_SCHEMA = "aquila.gate.order_session.candidate_ips.v1"
DEFAULT_TARGET = probe.DEFAULT_TARGET
DEFAULT_TIMEOUT = probe.DEFAULT_TIMEOUT
DEFAULT_RESOLVER_TIMEOUT = 2.0
CONNECTED_TAG = "gate_order_session_connected"
DNS_TYPE_A = 1
DNS_TYPE_AAAA = 28
DNS_CLASS_IN = 1


@dataclass(frozen=True)
class DnsResolverSpec:
    kind: str
    address: str
    port: int
    label: str


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
        "resolver_details": [],
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
    resolver_detail: dict | None = None,
    source: str | None = None,
    observed_ns: int,
    ips: list[str],
) -> None:
    source_name = source or f"dns_{resolver}"
    for ip in ips:
        record = ensure_record(
            records, run_id=run_id, host=host, target=target, port=port, ip=ip
        )
        add_source(record, source_name)
        dns = record["dns"]
        dns["seen_count"] += 1
        if dns["first_seen_ns"] == 0:
            dns["first_seen_ns"] = observed_ns
        dns["last_seen_ns"] = observed_ns
        if resolver not in dns["resolvers"]:
            dns["resolvers"].append(resolver)
        if (
            resolver_detail is not None
            and resolver_detail not in dns["resolver_details"]
        ):
            dns["resolver_details"].append(resolver_detail)


def format_resolver_label(address: str, port: int) -> str:
    parsed = ipaddress.ip_address(address)
    if parsed.version == 6:
        return f"[{parsed}]:{port}"
    return f"{parsed}:{port}"


def parse_resolver_port(port_text: str) -> int:
    try:
        port = int(port_text)
    except ValueError as exc:
        raise ValueError("resolver port must be an integer") from exc
    if not 1 <= port <= 65535:
        raise ValueError("resolver port must be in [1, 65535]")
    return port


def parse_udp_resolver_spec(value: str) -> DnsResolverSpec:
    if value.startswith("["):
        end = value.find("]")
        if end < 0:
            raise ValueError(f"invalid resolver endpoint: {value}")
        address = value[1:end]
        rest = value[end + 1 :]
        resolver_port = 53
        if rest:
            if not rest.startswith(":"):
                raise ValueError(f"invalid resolver endpoint: {value}")
            resolver_port = parse_resolver_port(rest[1:])
    else:
        try:
            address = str(ipaddress.ip_address(value))
            resolver_port = 53
        except ValueError:
            address_text, sep, port_text = value.rpartition(":")
            if sep == "":
                raise ValueError(f"resolver must be 'system' or an IP[:port]: {value}")
            address = str(ipaddress.ip_address(address_text))
            resolver_port = parse_resolver_port(port_text)

    normalized = str(ipaddress.ip_address(address))
    return DnsResolverSpec(
        kind="udp",
        address=normalized,
        port=resolver_port,
        label=format_resolver_label(normalized, resolver_port),
    )


def parse_resolver_specs(values: list[str]) -> list[DnsResolverSpec]:
    specs: list[DnsResolverSpec] = []
    seen: set[str] = set()
    for raw_value in values or ["system"]:
        value = raw_value.strip()
        if not value:
            continue
        if value.lower() == "system":
            spec = DnsResolverSpec(kind="system", address="", port=0, label="system")
        else:
            spec = parse_udp_resolver_spec(value)
        if spec.label in seen:
            continue
        seen.add(spec.label)
        specs.append(spec)
    if not specs:
        specs.append(DnsResolverSpec(kind="system", address="", port=0, label="system"))
    return specs


def resolver_source_name(spec: DnsResolverSpec) -> str:
    if spec.kind == "system":
        return "dns_system"
    address = spec.address.replace(":", "_")
    return f"dns_udp_{address}_{spec.port}"


def resolver_detail(spec: DnsResolverSpec) -> dict:
    return {
        "kind": spec.kind,
        "address": spec.address,
        "port": spec.port,
        "label": spec.label,
    }


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


def encode_dns_name(host: str) -> bytes:
    labels = host.rstrip(".").split(".")
    encoded = bytearray()
    for label in labels:
        if not label:
            raise ValueError(f"invalid DNS host: {host}")
        raw_label = label.encode("ascii")
        if len(raw_label) > 63:
            raise ValueError(f"DNS label is too long: {label}")
        encoded.append(len(raw_label))
        encoded.extend(raw_label)
    encoded.append(0)
    return bytes(encoded)


def build_dns_query(host: str, query_type: int, query_id: int) -> bytes:
    header = struct.pack("!HHHHHH", query_id, 0x0100, 1, 0, 0, 0)
    question = encode_dns_name(host)
    question += struct.pack("!HH", query_type, DNS_CLASS_IN)
    return header + question


def skip_dns_name(message: bytes, offset: int) -> int:
    while True:
        if offset >= len(message):
            raise ValueError("truncated DNS name")
        length = message[offset]
        if length & 0xC0 == 0xC0:
            if offset + 1 >= len(message):
                raise ValueError("truncated DNS compression pointer")
            return offset + 2
        if length & 0xC0:
            raise ValueError("unsupported DNS label format")
        offset += 1
        if length == 0:
            return offset
        offset += length
        if offset > len(message):
            raise ValueError("truncated DNS label")


def parse_dns_response_ips(message: bytes, expected_query_id: int) -> list[str]:
    if len(message) < 12:
        raise ValueError("truncated DNS header")
    query_id, flags, question_count, answer_count, _, _ = struct.unpack(
        "!HHHHHH", message[:12]
    )
    if query_id != expected_query_id:
        return []
    if not flags & 0x8000:
        return []
    if flags & 0x000F:
        return []

    offset = 12
    for _ in range(question_count):
        offset = skip_dns_name(message, offset)
        if offset + 4 > len(message):
            raise ValueError("truncated DNS question")
        offset += 4

    ips: list[str] = []
    seen: set[str] = set()
    for _ in range(answer_count):
        offset = skip_dns_name(message, offset)
        if offset + 10 > len(message):
            raise ValueError("truncated DNS answer")
        answer_type, answer_class, _, rdlength = struct.unpack(
            "!HHIH", message[offset : offset + 10]
        )
        offset += 10
        if offset + rdlength > len(message):
            raise ValueError("truncated DNS rdata")
        rdata = message[offset : offset + rdlength]
        offset += rdlength
        ip = ""
        if answer_class == DNS_CLASS_IN and answer_type == DNS_TYPE_A and rdlength == 4:
            ip = normalize_ip(socket.inet_ntop(socket.AF_INET, rdata))
        elif (
            answer_class == DNS_CLASS_IN
            and answer_type == DNS_TYPE_AAAA
            and rdlength == 16
        ):
            ip = normalize_ip(socket.inet_ntop(socket.AF_INET6, rdata))
        if ip and ip not in seen:
            seen.add(ip)
            ips.append(ip)
    return ips


def resolve_dns_udp_ips(
    host: str, resolver_address: str, resolver_port: int, timeout: float
) -> list[str]:
    parsed_address = ipaddress.ip_address(resolver_address)
    family = socket.AF_INET6 if parsed_address.version == 6 else socket.AF_INET
    ips: list[str] = []
    seen: set[str] = set()
    for query_type in (DNS_TYPE_A, DNS_TYPE_AAAA):
        query_id = (time.time_ns() ^ query_type) & 0xFFFF
        query = build_dns_query(host, query_type, query_id)
        with socket.socket(family, socket.SOCK_DGRAM) as sock:
            sock.settimeout(timeout)
            sock.sendto(query, (resolver_address, resolver_port))
            response, _ = sock.recvfrom(4096)
        for ip in parse_dns_response_ips(response, query_id):
            if ip not in seen:
                seen.add(ip)
                ips.append(ip)
    return ips


def resolve_ips_with_spec(
    host: str, port: str, spec: DnsResolverSpec, timeout: float
) -> list[str]:
    if spec.kind == "system":
        return resolve_system_ips(host, port)
    if spec.kind == "udp":
        return resolve_dns_udp_ips(host, spec.address, spec.port, timeout)
    raise ValueError(f"unsupported resolver kind: {spec.kind}")


def collect_dns_samples(
    records: dict[str, dict],
    *,
    run_id: str,
    host: str,
    target: str,
    port: str,
    duration_sec: float,
    interval_sec: float,
    resolver_specs: list[DnsResolverSpec] | None = None,
    resolver: Callable[
        [str, str, DnsResolverSpec, float], list[str]
    ] = resolve_ips_with_spec,
    resolver_timeout: float = DEFAULT_RESOLVER_TIMEOUT,
    now_ns: Callable[[], int] = time.time_ns,
    monotonic: Callable[[], float] = time.monotonic,
    sleep: Callable[[float], None] = time.sleep,
) -> None:
    specs = resolver_specs or parse_resolver_specs(["system"])
    deadline = monotonic() + max(0.0, duration_sec)
    while True:
        observed_ns = now_ns()
        for spec in specs:
            try:
                ips = resolver(host, port, spec, resolver_timeout)
            except Exception as exc:
                print(
                    f"[WARN] DNS resolver failed resolver={spec.label}: {exc}",
                    file=sys.stderr,
                )
                continue
            add_dns_sample(
                records,
                run_id=run_id,
                host=host,
                target=target,
                port=port,
                resolver=spec.label,
                resolver_detail=resolver_detail(spec),
                source=resolver_source_name(spec),
                observed_ns=observed_ns,
                ips=ips,
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
    parser.add_argument(
        "--resolver",
        action="append",
        default=["system"],
        help=(
            "DNS resolver to sample. Use 'system' or IP[:port]. Repeat to sample "
            "multiple resolvers; defaults to system."
        ),
    )
    parser.add_argument(
        "--resolver-timeout",
        type=float,
        default=DEFAULT_RESOLVER_TIMEOUT,
        help="Timeout in seconds for each explicit UDP DNS query.",
    )
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
    if args.resolver_timeout <= 0:
        return "--resolver-timeout must be positive"
    try:
        int(args.port)
    except ValueError:
        return "--port must be an integer"
    try:
        parse_resolver_specs(args.resolver)
    except ValueError as exc:
        return f"--resolver invalid: {exc}"
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
    resolver_specs = parse_resolver_specs(args.resolver)
    collect_dns_samples(
        records,
        run_id=run_id,
        host=args.host,
        target=args.target,
        port=args.port,
        duration_sec=args.duration_sec,
        interval_sec=args.interval_sec,
        resolver_specs=resolver_specs,
        resolver_timeout=args.resolver_timeout,
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
        "resolvers": [spec.label for spec in resolver_specs],
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
