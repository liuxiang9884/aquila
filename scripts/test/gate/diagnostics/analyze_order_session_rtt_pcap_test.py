#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import json
import socket
import struct
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[3] / "gate" / "diagnostics"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import analyze_order_session_rtt_pcap as rtt_pcap


def websocket_frame(payload: bytes, *, masked: bool) -> bytes:
    first = 0x81
    length = len(payload)
    if length < 126:
        header = bytes([first, length | (0x80 if masked else 0)])
    elif length <= 0xFFFF:
        header = bytes([first, 126 | (0x80 if masked else 0)]) + struct.pack("!H", length)
    else:
        header = bytes([first, 127 | (0x80 if masked else 0)]) + struct.pack("!Q", length)
    if not masked:
        return header + payload
    mask = b"\x01\x02\x03\x04"
    encoded = bytes(byte ^ mask[index & 3] for index, byte in enumerate(payload))
    return header + mask + encoded


def tcp_packet(
    *,
    src_ip: str,
    dst_ip: str,
    src_port: int,
    dst_port: int,
    seq: int,
    ack: int,
    payload: bytes,
) -> bytes:
    eth = b"\x00" * 12 + struct.pack("!H", 0x0800)
    total_len = 20 + 20 + len(payload)
    ip = struct.pack(
        "!BBHHHBBH4s4s",
        0x45,
        0,
        total_len,
        0,
        0,
        64,
        6,
        0,
        socket.inet_aton(src_ip),
        socket.inet_aton(dst_ip),
    )
    tcp = struct.pack(
        "!HHIIHHHH",
        src_port,
        dst_port,
        seq,
        ack,
        (5 << 12) | 0x18,
        65535,
        0,
        0,
    )
    return eth + ip + tcp + payload


def write_pcap(path: Path, packets: list[tuple[int, bytes]]) -> None:
    with path.open("wb") as output:
        output.write(struct.pack("<IHHIIII", 0xA1B2C3D4, 2, 4, 0, 0, 262144, 1))
        for ts_ns, packet in packets:
            output.write(
                struct.pack(
                    "<IIII",
                    ts_ns // 1_000_000_000,
                    (ts_ns % 1_000_000_000) // 1000,
                    len(packet),
                    len(packet),
                )
            )
            output.write(packet)


class AnalyzeOrderSessionRttPcapTest(unittest.TestCase):
    def test_aligns_gate_x_time_with_no_tls_websocket_ack(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            pcap_path = base / "sample.pcap"
            sample_path = base / "samples.csv"

            local_id = "123456"
            request_payload = json.dumps(
                {
                    "channel": "futures.order_place",
                    "payload": {"req_param": {"text": f"t-{local_id}"}},
                    "request_id": "99",
                },
                separators=(",", ":"),
            ).encode()
            ack_payload = json.dumps(
                {
                    "header": {
                        "channel": "futures.order_place",
                        "x_in_time": 1_000_000,
                        "x_out_time": 1_005_500,
                        "conn_id": "conn-a",
                        "conn_trace_id": "conn-trace",
                        "trace_id": "trace-a",
                    },
                    "data": {
                        "result": {
                            "req_param": {"text": f"t-{local_id}"},
                        }
                    },
                    "request_id": "99",
                    "ack": True,
                },
                separators=(",", ":"),
            ).encode()

            request_frame = websocket_frame(request_payload, masked=True)
            ack_frame = websocket_frame(ack_payload, masked=False)
            request_ts_ns = 10_000_000_000
            response_ts_ns = request_ts_ns + 6_000_000
            write_pcap(
                pcap_path,
                [
                    (
                        request_ts_ns,
                        tcp_packet(
                            src_ip="10.0.1.103",
                            dst_ip="10.0.1.154",
                            src_port=51000,
                            dst_port=80,
                            seq=1000,
                            ack=5000,
                            payload=request_frame,
                        ),
                    ),
                    (
                        response_ts_ns,
                        tcp_packet(
                            src_ip="10.0.1.154",
                            dst_ip="10.0.1.103",
                            src_port=80,
                            dst_port=51000,
                            seq=5000,
                            ack=1000 + len(request_frame),
                            payload=ack_frame,
                        ),
                    ),
                ],
            )

            with sample_path.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(
                    output,
                    fieldnames=[
                        "session",
                        "group",
                        "ip",
                        "contract",
                        "local_id",
                        "req_seq",
                        "ack_rtt_ns",
                        "write_complete_ns",
                        "ack_recv_ns",
                        "tcp_info_retrans",
                        "tcp_info_total_retrans",
                        "tcp_info_unacked",
                        "tcp_notsent_bytes",
                    ],
                )
                writer.writeheader()
                writer.writerow(
                    {
                        "session": "private-00",
                        "group": "private-10.0.1.154",
                        "ip": "10.0.1.154",
                        "contract": "SUI_USDT",
                        "local_id": local_id,
                        "req_seq": "7",
                        "ack_rtt_ns": "6030000",
                        "write_complete_ns": str(request_ts_ns - 5_000),
                        "ack_recv_ns": str(response_ts_ns + 25_000),
                        "tcp_info_retrans": "0",
                        "tcp_info_total_retrans": "0",
                        "tcp_info_unacked": "0",
                        "tcp_notsent_bytes": "0",
                    }
                )

            result = rtt_pcap.analyze_order_session_rtt_pcap(
                sample_csv=sample_path,
                pcap_path=pcap_path,
                local_ip="10.0.1.103",
                remote_ip="10.0.1.154",
            )

        self.assertEqual(result.matched_count, 1)
        row = result.rows[0]
        summary = rtt_pcap.format_summary(result, tail_thresholds=[5.0])
        self.assertEqual(row.local_id, local_id)
        self.assertEqual(row.group, "private-10.0.1.154")
        self.assertEqual(row.ip, "10.0.1.154")
        self.assertAlmostEqual(row.pcap_request_to_ack_ms, 6.0)
        self.assertAlmostEqual(row.gate_x_in_to_x_out_ms, 5.5)
        self.assertAlmostEqual(row.residual_ms, 0.5)
        self.assertAlmostEqual(row.ack_response_to_ack_receive_us, 25.0)
        self.assertTrue(row.tcp_ack_same_as_response)
        self.assertAlmostEqual(row.tcp_ack_to_response_us, 0.0)
        self.assertEqual(row.conn_id, "conn-a")
        self.assertIn("by_ip={'10.0.1.154': 1}", summary)


if __name__ == "__main__":
    unittest.main()
