#!/home/liuxiang/dev/pyenv/lx/bin/python

import argparse
import csv
import json
import socket
import struct
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class TcpPacket:
    ts_ns: int
    src_ip: str
    dst_ip: str
    src_port: int
    dst_port: int
    seq: int
    ack: int
    payload: bytes


@dataclass(frozen=True)
class WebSocketRequest:
    local_id: str
    pcap_request_ns: int
    src_port: int
    seq_end: int


@dataclass(frozen=True)
class WebSocketAckResponse:
    local_id: str
    pcap_ack_response_ns: int
    dst_port: int
    x_in_time: int | None
    x_out_time: int | None
    gate_x_in_to_x_out_ms: float | None
    conn_id: str
    conn_trace_id: str
    trace_id: str


@dataclass(frozen=True)
class AlignmentRow:
    session: str
    group: str
    ip: str
    contract: str
    local_id: str
    req_seq: str
    ack_ms: float
    pcap_request_to_ack_ms: float | None
    gate_x_in_to_x_out_ms: float | None
    residual_ms: float | None
    gate_share: float | None
    ack_response_to_ack_receive_us: float | None
    tcp_ack_to_response_us: float | None
    tcp_ack_same_as_response: bool
    pcap_request_ns: int | None
    tcp_ack_ns: int | None
    pcap_ack_response_ns: int | None
    x_in_time: int | None
    x_out_time: int | None
    conn_id: str
    conn_trace_id: str
    trace_id: str
    src_port: int | None
    tcp_info_retrans: str
    tcp_info_total_retrans: str
    tcp_info_unacked: str
    tcp_notsent_bytes: str

    def as_csv_dict(self) -> dict[str, str]:
        return {
            "session": self.session,
            "group": self.group,
            "ip": self.ip,
            "contract": self.contract,
            "local_id": self.local_id,
            "req_seq": self.req_seq,
            "ack_ms": format_float(self.ack_ms),
            "pcap_request_to_ack_ms": format_float(self.pcap_request_to_ack_ms),
            "gate_x_in_to_x_out_ms": format_float(self.gate_x_in_to_x_out_ms),
            "residual_ms": format_float(self.residual_ms),
            "gate_share": format_float(self.gate_share, digits=4),
            "ack_response_to_ack_receive_us": format_float(
                self.ack_response_to_ack_receive_us
            ),
            "tcp_ack_to_response_us": format_float(self.tcp_ack_to_response_us),
            "tcp_ack_same_as_response": "true" if self.tcp_ack_same_as_response else "false",
            "pcap_request_ns": optional_int(self.pcap_request_ns),
            "tcp_ack_ns": optional_int(self.tcp_ack_ns),
            "pcap_ack_response_ns": optional_int(self.pcap_ack_response_ns),
            "x_in_time": optional_int(self.x_in_time),
            "x_out_time": optional_int(self.x_out_time),
            "conn_id": self.conn_id,
            "conn_trace_id": self.conn_trace_id,
            "trace_id": self.trace_id,
            "src_port": optional_int(self.src_port),
            "tcp_info_retrans": self.tcp_info_retrans,
            "tcp_info_total_retrans": self.tcp_info_total_retrans,
            "tcp_info_unacked": self.tcp_info_unacked,
            "tcp_notsent_bytes": self.tcp_notsent_bytes,
        }


@dataclass(frozen=True)
class AnalysisResult:
    rows: list[AlignmentRow]
    sample_count: int
    matched_count: int
    request_count: int
    ack_response_count: int
    packet_count: int


ALIGNMENT_CSV_FIELDNAMES = [
    "session",
    "group",
    "ip",
    "contract",
    "local_id",
    "req_seq",
    "ack_ms",
    "pcap_request_to_ack_ms",
    "gate_x_in_to_x_out_ms",
    "residual_ms",
    "gate_share",
    "ack_response_to_ack_receive_us",
    "tcp_ack_to_response_us",
    "tcp_ack_same_as_response",
    "pcap_request_ns",
    "tcp_ack_ns",
    "pcap_ack_response_ns",
    "x_in_time",
    "x_out_time",
    "conn_id",
    "conn_trace_id",
    "trace_id",
    "src_port",
    "tcp_info_retrans",
    "tcp_info_total_retrans",
    "tcp_info_unacked",
    "tcp_notsent_bytes",
]


def optional_int(value: int | None) -> str:
    return "" if value is None else str(value)


def format_float(value: float | None, *, digits: int = 3) -> str:
    return "" if value is None else f"{value:.{digits}f}"


def percentile(values: list[float], quantile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, int((len(ordered) - 1) * quantile))
    return ordered[index]


def ip_text(raw: bytes) -> str:
    return socket.inet_ntoa(raw)


def parse_pcap(path: Path) -> list[TcpPacket]:
    packets: list[TcpPacket] = []
    with path.open("rb") as input_file:
        global_header = input_file.read(24)
        if len(global_header) != 24:
            raise ValueError("short pcap global header")
        magic = global_header[:4]
        if magic == b"\xd4\xc3\xb2\xa1":
            endian = "<"
            ns_resolution = False
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian = ">"
            ns_resolution = False
        elif magic == b"\x4d\x3c\xb2\xa1":
            endian = "<"
            ns_resolution = True
        elif magic == b"\xa1\xb2\x3c\x4d":
            endian = ">"
            ns_resolution = True
        else:
            raise ValueError(f"unsupported pcap magic {magic.hex()}")
        linktype = struct.unpack(endian + "I", global_header[20:24])[0]
        if linktype != 1:
            raise ValueError(f"expected Ethernet pcap, linktype={linktype}")

        while True:
            packet_header = input_file.read(16)
            if not packet_header:
                break
            if len(packet_header) != 16:
                raise ValueError("truncated pcap packet header")
            ts_sec, ts_fraction, incl_len, _orig_len = struct.unpack(
                endian + "IIII", packet_header
            )
            frame = input_file.read(incl_len)
            if len(frame) != incl_len:
                raise ValueError("truncated pcap packet")
            packet = parse_ethernet_ipv4_tcp_frame(
                ts_sec * 1_000_000_000
                + ts_fraction * (1 if ns_resolution else 1000),
                frame,
            )
            if packet is not None:
                packets.append(packet)
    return packets


def parse_ethernet_ipv4_tcp_frame(ts_ns: int, frame: bytes) -> TcpPacket | None:
    if len(frame) < 14:
        return None
    eth_type = struct.unpack("!H", frame[12:14])[0]
    offset = 14
    if eth_type == 0x8100 and len(frame) >= 18:
        eth_type = struct.unpack("!H", frame[16:18])[0]
        offset = 18
    if eth_type != 0x0800 or len(frame) < offset + 20:
        return None

    ip = frame[offset:]
    ihl = (ip[0] & 0x0F) * 4
    if len(ip) < ihl or ip[9] != 6:
        return None
    total_len = struct.unpack("!H", ip[2:4])[0]
    tcp_start = offset + ihl
    if len(frame) < tcp_start + 20:
        return None

    tcp = frame[tcp_start:]
    src_port, dst_port, seq, ack, off_flags = struct.unpack("!HHIIH", tcp[:14])
    tcp_hlen = ((off_flags >> 12) & 0xF) * 4
    payload_start = tcp_start + tcp_hlen
    payload_end = min(offset + total_len, len(frame))
    if payload_start > payload_end:
        return None
    return TcpPacket(
        ts_ns=ts_ns,
        src_ip=ip_text(ip[12:16]),
        dst_ip=ip_text(ip[16:20]),
        src_port=src_port,
        dst_port=dst_port,
        seq=seq,
        ack=ack,
        payload=frame[payload_start:payload_end],
    )


def parse_websocket_frames(payload: bytes) -> Iterable[tuple[int, bytes]]:
    index = 0
    while index + 2 <= len(payload):
        first = payload[index]
        second = payload[index + 1]
        opcode = first & 0x0F
        masked = bool(second & 0x80)
        length = second & 0x7F
        position = index + 2
        if length == 126:
            if position + 2 > len(payload):
                break
            length = struct.unpack("!H", payload[position : position + 2])[0]
            position += 2
        elif length == 127:
            if position + 8 > len(payload):
                break
            length = struct.unpack("!Q", payload[position : position + 8])[0]
            position += 8
        mask = b""
        if masked:
            if position + 4 > len(payload):
                break
            mask = payload[position : position + 4]
            position += 4
        if position + length > len(payload):
            break
        data = payload[position : position + length]
        if masked:
            data = bytes(byte ^ mask[i & 3] for i, byte in enumerate(data))
        yield opcode, data
        index = position + length


def local_id_from_text(text: object) -> str | None:
    if not isinstance(text, str):
        return None
    if not text.startswith("t-"):
        return None
    return text[2:]


def extract_local_id(message: object) -> str | None:
    if not isinstance(message, dict):
        return None
    payload = message.get("payload")
    if isinstance(payload, dict):
        req_param = payload.get("req_param")
        if isinstance(req_param, dict):
            local_id = local_id_from_text(req_param.get("text"))
            if local_id:
                return local_id
    data = message.get("data")
    if isinstance(data, dict):
        result = data.get("result")
        if isinstance(result, dict):
            req_param = result.get("req_param")
            if isinstance(req_param, dict):
                local_id = local_id_from_text(req_param.get("text"))
                if local_id:
                    return local_id
            local_id = local_id_from_text(result.get("text"))
            if local_id:
                return local_id
    return None


def order_place_messages(packet: TcpPacket) -> Iterable[dict[str, object]]:
    for opcode, data in parse_websocket_frames(packet.payload):
        if opcode != 1:
            continue
        try:
            message = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if not isinstance(message, dict):
            continue
        header = message.get("header")
        channel = message.get("channel")
        if isinstance(header, dict):
            channel = channel or header.get("channel")
        if channel == "futures.order_place":
            yield message


def extract_stream_events(
    packets: list[TcpPacket], *, local_ip: str, remote_ip: str
) -> tuple[dict[str, WebSocketRequest], dict[str, WebSocketAckResponse]]:
    requests: dict[str, WebSocketRequest] = {}
    responses: dict[str, WebSocketAckResponse] = {}
    for packet in packets:
        from_local = packet.src_ip == local_ip and packet.dst_ip == remote_ip
        to_local = packet.src_ip == remote_ip and packet.dst_ip == local_ip
        if not packet.payload or not (from_local or to_local):
            continue
        for message in order_place_messages(packet):
            local_id = extract_local_id(message)
            if not local_id:
                continue
            if from_local:
                requests.setdefault(
                    local_id,
                    WebSocketRequest(
                        local_id=local_id,
                        pcap_request_ns=packet.ts_ns,
                        src_port=packet.src_port,
                        seq_end=(packet.seq + len(packet.payload)) & 0xFFFFFFFF,
                    ),
                )
            elif to_local and message.get("ack") is True:
                header = message.get("header")
                header = header if isinstance(header, dict) else {}
                x_in_time = parse_optional_int(header.get("x_in_time"))
                x_out_time = parse_optional_int(header.get("x_out_time"))
                gate_ms = None
                if x_in_time is not None and x_out_time is not None:
                    gate_ms = (x_out_time - x_in_time) / 1000.0
                responses.setdefault(
                    local_id,
                    WebSocketAckResponse(
                        local_id=local_id,
                        pcap_ack_response_ns=packet.ts_ns,
                        dst_port=packet.dst_port,
                        x_in_time=x_in_time,
                        x_out_time=x_out_time,
                        gate_x_in_to_x_out_ms=gate_ms,
                        conn_id=str(header.get("conn_id", "")),
                        conn_trace_id=str(header.get("conn_trace_id", "")),
                        trace_id=str(header.get("trace_id", "")),
                    ),
                )
    return requests, responses


def parse_optional_int(value: object) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def first_tcp_ack_packet(
    packets: list[TcpPacket],
    *,
    request: WebSocketRequest,
    response: WebSocketAckResponse,
    local_ip: str,
    remote_ip: str,
) -> TcpPacket | None:
    for packet in packets:
        if packet.src_ip != remote_ip or packet.dst_ip != local_ip:
            continue
        if packet.dst_port != request.src_port:
            continue
        if packet.ts_ns < request.pcap_request_ns:
            continue
        if packet.ack >= request.seq_end:
            return packet
        if packet.ts_ns > response.pcap_ack_response_ns:
            return None
    return None


def analyze_order_session_rtt_pcap(
    *,
    sample_csv: Path,
    pcap_path: Path,
    local_ip: str,
    remote_ip: str,
) -> AnalysisResult:
    packets = parse_pcap(pcap_path)
    requests, responses = extract_stream_events(
        packets, local_ip=local_ip, remote_ip=remote_ip
    )
    rows: list[AlignmentRow] = []
    with sample_csv.open(newline="", encoding="utf-8") as input_file:
        sample_rows = list(csv.DictReader(input_file))
    for sample in sample_rows:
        local_id = sample["local_id"]
        request = requests.get(local_id)
        response = responses.get(local_id)
        pcap_request_to_ack_ms = None
        residual_ms = None
        gate_share = None
        ack_response_to_ack_receive_us = None
        tcp_ack_to_response_us = None
        tcp_ack_same_as_response = False
        tcp_ack_ns = None
        if request is not None and response is not None:
            pcap_request_to_ack_ms = (
                response.pcap_ack_response_ns - request.pcap_request_ns
            ) / 1_000_000.0
            if response.gate_x_in_to_x_out_ms is not None:
                residual_ms = pcap_request_to_ack_ms - response.gate_x_in_to_x_out_ms
                if pcap_request_to_ack_ms > 0:
                    gate_share = response.gate_x_in_to_x_out_ms / pcap_request_to_ack_ms
            ack_response_to_ack_receive_us = (
                int(sample["ack_recv_ns"]) - response.pcap_ack_response_ns
            ) / 1000.0
            tcp_ack = first_tcp_ack_packet(
                packets,
                request=request,
                response=response,
                local_ip=local_ip,
                remote_ip=remote_ip,
            )
            if tcp_ack is not None:
                tcp_ack_ns = tcp_ack.ts_ns
                tcp_ack_to_response_us = (
                    response.pcap_ack_response_ns - tcp_ack.ts_ns
                ) / 1000.0
                tcp_ack_same_as_response = tcp_ack.ts_ns == response.pcap_ack_response_ns

        rows.append(
            AlignmentRow(
                session=sample.get("session", ""),
                group=sample.get("group", ""),
                ip=sample.get("ip", ""),
                contract=sample.get("contract", ""),
                local_id=local_id,
                req_seq=sample.get("req_seq", ""),
                ack_ms=int(sample["ack_rtt_ns"]) / 1_000_000.0,
                pcap_request_to_ack_ms=pcap_request_to_ack_ms,
                gate_x_in_to_x_out_ms=(
                    response.gate_x_in_to_x_out_ms if response is not None else None
                ),
                residual_ms=residual_ms,
                gate_share=gate_share,
                ack_response_to_ack_receive_us=ack_response_to_ack_receive_us,
                tcp_ack_to_response_us=tcp_ack_to_response_us,
                tcp_ack_same_as_response=tcp_ack_same_as_response,
                pcap_request_ns=request.pcap_request_ns if request is not None else None,
                tcp_ack_ns=tcp_ack_ns,
                pcap_ack_response_ns=(
                    response.pcap_ack_response_ns if response is not None else None
                ),
                x_in_time=response.x_in_time if response is not None else None,
                x_out_time=response.x_out_time if response is not None else None,
                conn_id=response.conn_id if response is not None else "",
                conn_trace_id=response.conn_trace_id if response is not None else "",
                trace_id=response.trace_id if response is not None else "",
                src_port=request.src_port if request is not None else None,
                tcp_info_retrans=sample.get("tcp_info_retrans", ""),
                tcp_info_total_retrans=sample.get("tcp_info_total_retrans", ""),
                tcp_info_unacked=sample.get("tcp_info_unacked", ""),
                tcp_notsent_bytes=sample.get("tcp_notsent_bytes", ""),
            )
        )
    return AnalysisResult(
        rows=rows,
        sample_count=len(sample_rows),
        matched_count=sum(
            1 for row in rows if row.pcap_request_to_ack_ms is not None
        ),
        request_count=len(requests),
        ack_response_count=len(responses),
        packet_count=len(packets),
    )


def alignment_csv_fieldnames() -> list[str]:
    return ALIGNMENT_CSV_FIELDNAMES.copy()


def write_alignment_csv(result: AnalysisResult, output_path: Path) -> None:
    with output_path.open("w", newline="", encoding="utf-8") as output_file:
        writer = csv.DictWriter(output_file, fieldnames=alignment_csv_fieldnames())
        writer.writeheader()
        for row in result.rows:
            writer.writerow(row.as_csv_dict())


def format_summary(result: AnalysisResult, *, tail_thresholds: Iterable[float]) -> str:
    lines = [
        f"samples={result.sample_count} matched={result.matched_count} "
        f"requests={result.request_count} ack_responses={result.ack_response_count} "
        f"packets={result.packet_count}"
    ]
    fields = [
        ("ack_ms", lambda row: row.ack_ms),
        ("pcap_request_to_ack_ms", lambda row: row.pcap_request_to_ack_ms),
        ("gate_x_in_to_x_out_ms", lambda row: row.gate_x_in_to_x_out_ms),
        ("residual_ms", lambda row: row.residual_ms),
        ("ack_response_to_ack_receive_us", lambda row: row.ack_response_to_ack_receive_us),
    ]
    for name, getter in fields:
        values = [value for row in result.rows if (value := getter(row)) is not None]
        lines.append(
            f"{name} p50={format_float(percentile(values, 0.50))} "
            f"p95={format_float(percentile(values, 0.95))} "
            f"p99={format_float(percentile(values, 0.99))} "
            f"max={format_float(max(values) if values else None)}"
        )
    for threshold in tail_thresholds:
        tails = [row for row in result.rows if row.ack_ms > threshold]
        pcap_sum = sum(row.pcap_request_to_ack_ms or 0.0 for row in tails)
        gate_sum = sum(row.gate_x_in_to_x_out_ms or 0.0 for row in tails)
        residual_values = [
            row.residual_ms for row in tails if row.residual_ms is not None
        ]
        gate_share_values = [
            row.gate_share for row in tails if row.gate_share is not None
        ]
        lines.append(f"tails_gt_{threshold:g}ms count={len(tails)}")
        lines.append(
            f"  gate_sum_ms={format_float(gate_sum)} "
            f"pcap_sum_ms={format_float(pcap_sum)} "
            f"gate_share_sum={format_float(gate_sum / pcap_sum if pcap_sum else None, digits=4)}"
        )
        lines.append(
            f"  residual_ms_p50={format_float(percentile(residual_values, 0.50))} "
            f"p95={format_float(percentile(residual_values, 0.95))} "
            f"max={format_float(max(residual_values) if residual_values else None)}"
        )
        lines.append(
            f"  gate_share_p50={format_float(percentile(gate_share_values, 0.50), digits=4)} "
            f"min={format_float(min(gate_share_values) if gate_share_values else None, digits=4)}"
        )
        lines.append(f"  by_session={dict(sorted(Counter(row.session for row in tails).items()))}")
        lines.append(f"  by_group={dict(sorted(Counter(row.group for row in tails).items()))}")
        lines.append(f"  by_ip={dict(sorted(Counter(row.ip for row in tails).items()))}")
        lines.append(f"  by_conn_id={dict(sorted(Counter(row.conn_id for row in tails).items()))}")
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Align Gate OrderSession RTT samples with no-TLS pcap and Gate x_in/x_out times."
    )
    parser.add_argument("--samples", type=Path, required=True, help="order_session_rtt_samples.csv")
    parser.add_argument("--pcap", type=Path, required=True, help="tcpdump pcap file")
    parser.add_argument("--local-ip", required=True, help="local host IP in the pcap")
    parser.add_argument("--remote-ip", required=True, help="Gate private remote IP in the pcap")
    parser.add_argument("--output", type=Path, help="Optional aligned CSV output path")
    parser.add_argument("--summary-output", type=Path, help="Optional summary text output path")
    parser.add_argument(
        "--tail-threshold-ms",
        type=float,
        action="append",
        default=[5.0, 10.0],
        help="Tail threshold for summary. Can be repeated.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = analyze_order_session_rtt_pcap(
        sample_csv=args.samples,
        pcap_path=args.pcap,
        local_ip=args.local_ip,
        remote_ip=args.remote_ip,
    )
    if args.output is not None:
        write_alignment_csv(result, args.output)
    summary = format_summary(result, tail_thresholds=args.tail_threshold_ms)
    print(summary)
    if args.summary_output is not None:
        args.summary_output.write_text(summary + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
