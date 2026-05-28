#!/home/liuxiang/dev/pyenv/lx/bin/python

import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "gate"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import discover_gate_ws_ips as discovery
import probe_gate_ws_connect_ip as probe


class DiscoverGateWsIpsTest(unittest.TestCase):
    def test_parse_resolver_specs_keeps_system_and_explicit_udp_resolvers(self):
        specs = discovery.parse_resolver_specs(["system", "1.1.1.1", "8.8.8.8:5353"])

        self.assertEqual([spec.kind for spec in specs], ["system", "udp", "udp"])
        self.assertEqual([spec.label for spec in specs], ["system", "1.1.1.1:53", "8.8.8.8:5353"])
        self.assertEqual(specs[1].address, "1.1.1.1")
        self.assertEqual(specs[1].port, 53)
        self.assertEqual(specs[2].address, "8.8.8.8")
        self.assertEqual(specs[2].port, 5353)

    def test_collect_dns_samples_uses_each_configured_resolver(self):
        records = {}
        specs = discovery.parse_resolver_specs(["system", "1.1.1.1"])
        calls = []

        def fake_resolver(host, port, resolver_spec, timeout):
            calls.append((host, port, resolver_spec.label, timeout))
            if resolver_spec.kind == "system":
                return ["52.198.250.74"]
            return ["52.198.250.74", "52.199.212.24"]

        discovery.collect_dns_samples(
            records,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt/sbe?sbe_schema_id=1",
            port="443",
            duration_sec=0.0,
            interval_sec=5.0,
            resolver_specs=specs,
            resolver=fake_resolver,
            resolver_timeout=1.25,
            now_ns=lambda: 100,
            monotonic=lambda: 10.0,
            sleep=lambda _: None,
        )

        self.assertEqual(
            calls,
            [
                ("fx-ws.gateio.ws", "443", "system", 1.25),
                ("fx-ws.gateio.ws", "443", "1.1.1.1:53", 1.25),
            ],
        )
        first = records["52.198.250.74"]
        self.assertEqual(first["sources"], ["dns_system", "dns_udp_1.1.1.1_53"])
        self.assertEqual(first["dns"]["seen_count"], 2)
        self.assertEqual(first["dns"]["resolvers"], ["system", "1.1.1.1:53"])
        self.assertEqual(
            first["dns"]["resolver_details"],
            [
                {"kind": "system", "address": "", "port": 0, "label": "system"},
                {"kind": "udp", "address": "1.1.1.1", "port": 53, "label": "1.1.1.1:53"},
            ],
        )

    def test_dns_samples_are_aggregated_by_ip(self):
        records = {}

        discovery.add_dns_sample(
            records,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt/sbe?sbe_schema_id=1",
            port="443",
            resolver="system",
            observed_ns=100,
            ips=["52.198.250.74", "52.199.212.24"],
        )
        discovery.add_dns_sample(
            records,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt/sbe?sbe_schema_id=1",
            port="443",
            resolver="system",
            observed_ns=200,
            ips=["52.198.250.74"],
        )

        first = records["52.198.250.74"]
        self.assertEqual(first["status"], "discovered")
        self.assertEqual(first["sources"], ["dns_system"])
        self.assertEqual(first["dns"]["seen_count"], 2)
        self.assertEqual(first["dns"]["first_seen_ns"], 100)
        self.assertEqual(first["dns"]["last_seen_ns"], 200)
        self.assertEqual(first["dns"]["resolvers"], ["system"])
        self.assertFalse(first["selected_for_rtt_probe"])

    def test_history_logs_are_merged_with_dns_records(self):
        records = {}
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "strategy.log"
            log_path.write_text(
                "I2026-05-27 06:34:10 gate_order_session_connected "
                "remote_ip=57.181.9.46 remote_port=443 local_port=42396\n"
                "I2026-05-27 06:34:11 unrelated remote_ip=10.0.0.1\n"
                "I2026-05-27 06:34:12 gate_order_session_connected "
                "remote_ip=57.181.9.46 remote_port=443 local_port=42398\n",
                encoding="utf-8",
            )

            discovery.merge_history_logs(
                records,
                run_id="run-1",
                host="fx-ws.gateio.ws",
                target="/v4/ws/usdt",
                port="443",
                paths=[str(log_path)],
            )

        record = records["57.181.9.46"]
        self.assertEqual(record["sources"], ["history_log"])
        self.assertEqual(record["history"]["seen_count"], 2)
        self.assertEqual(record["history"]["paths"], [str(log_path)])
        self.assertEqual(record["history"]["first_seen_text"], "2026-05-27 06:34:10")
        self.assertEqual(record["history"]["last_seen_text"], "2026-05-27 06:34:12")

    def test_websocket_verification_selects_only_successful_ips(self):
        records = {}
        discovery.ensure_record(
            records,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt",
            port="443",
            ip="52.198.250.74",
        )
        discovery.ensure_record(
            records,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt",
            port="443",
            ip="1.2.3.4",
        )

        def fake_verifier(ip):
            if ip == "52.198.250.74":
                return probe.ProbeResult(
                    connect_ip=ip,
                    host="fx-ws.gateio.ws",
                    port="443",
                    target="/v4/ws/usdt",
                    tls=True,
                    ok=True,
                    websocket_status="HTTP/1.1 101 Switching Protocols",
                    local_endpoint="10.0.0.12:34222",
                    remote_endpoint=f"{ip}:443",
                    tcp_connect_ms=2.79,
                    tls_handshake_ms=6.023,
                    websocket_handshake_ms=4.581,
                    total_ms=32.617,
                )
            return probe.ProbeResult(
                connect_ip=ip,
                host="fx-ws.gateio.ws",
                port="443",
                target="/v4/ws/usdt",
                tls=True,
                ok=False,
                error="TimeoutError: timed out",
            )

        discovery.verify_websocket_candidates(records, fake_verifier)

        ok_record = records["52.198.250.74"]
        self.assertEqual(ok_record["status"], "ws_verified")
        self.assertTrue(ok_record["selected_for_rtt_probe"])
        self.assertEqual(ok_record["websocket_verify"]["http_status"], 101)
        self.assertEqual(ok_record["websocket_verify"]["remote_ip"], "52.198.250.74")
        self.assertEqual(ok_record["websocket_verify"]["remote_port"], 443)
        self.assertEqual(ok_record["websocket_verify"]["tcp_connect_ns"], 2790000)

        rejected = records["1.2.3.4"]
        self.assertEqual(rejected["status"], "rejected")
        self.assertFalse(rejected["selected_for_rtt_probe"])
        self.assertEqual(rejected["rejection_reason"], "websocket_verify_failed")

    def test_login_verification_selects_only_successful_logins(self):
        records = {}
        for ip in ("52.198.250.74", "52.199.212.24", "1.2.3.4"):
            discovery.ensure_record(
                records,
                run_id="run-1",
                host="fx-ws.gateio.ws",
                target="/v4/ws/usdt",
                port="443",
                ip=ip,
            )

        def fake_verifier(ip):
            if ip == "52.198.250.74":
                return probe.ProbeResult(
                    connect_ip=ip,
                    host="fx-ws.gateio.ws",
                    port="443",
                    target="/v4/ws/usdt",
                    tls=True,
                    ok=True,
                    websocket_status="HTTP/1.1 101 Switching Protocols",
                    local_endpoint="10.0.0.12:34222",
                    remote_endpoint=f"{ip}:443",
                    login_ms=8.25,
                    login_ok=True,
                    login_status="success",
                    login_uid="123",
                    login_request_id="ip-probe-login-1",
                    conn_id="conn-1",
                    conn_trace_id="trace-1",
                )
            if ip == "52.199.212.24":
                return probe.ProbeResult(
                    connect_ip=ip,
                    host="fx-ws.gateio.ws",
                    port="443",
                    target="/v4/ws/usdt",
                    tls=True,
                    ok=False,
                    websocket_status="HTTP/1.1 101 Switching Protocols",
                    local_endpoint="10.0.0.12:34224",
                    remote_endpoint=f"{ip}:443",
                    login_ms=7.5,
                    login_ok=False,
                    login_status="fail",
                    error="login rejected",
                )
            return probe.ProbeResult(
                connect_ip=ip,
                host="fx-ws.gateio.ws",
                port="443",
                target="/v4/ws/usdt",
                tls=True,
                ok=False,
                error="TimeoutError: timed out",
            )

        discovery.verify_websocket_candidates(records, fake_verifier, require_login=True)

        ok_record = records["52.198.250.74"]
        self.assertEqual(ok_record["status"], "login_verified")
        self.assertTrue(ok_record["selected_for_rtt_probe"])
        self.assertTrue(ok_record["login_verify"]["attempted"])
        self.assertTrue(ok_record["login_verify"]["ok"])
        self.assertEqual(ok_record["login_verify"]["latency_ns"], 8250000)
        self.assertEqual(ok_record["login_verify"]["status"], "success")
        self.assertEqual(ok_record["login_verify"]["uid"], "123")
        self.assertEqual(ok_record["login_verify"]["conn_id"], "conn-1")

        rejected_login = records["52.199.212.24"]
        self.assertEqual(rejected_login["status"], "rejected")
        self.assertFalse(rejected_login["selected_for_rtt_probe"])
        self.assertEqual(rejected_login["rejection_reason"], "login_verify_failed")
        self.assertTrue(rejected_login["login_verify"]["attempted"])
        self.assertFalse(rejected_login["login_verify"]["ok"])
        self.assertEqual(rejected_login["login_verify"]["error"], "login rejected")

        rejected_ws = records["1.2.3.4"]
        self.assertEqual(rejected_ws["status"], "rejected")
        self.assertEqual(rejected_ws["rejection_reason"], "websocket_verify_failed")
        self.assertFalse(rejected_ws["login_verify"]["attempted"])

    def test_candidate_ip_file_skips_headers_and_deduplicates(self):
        records = {}
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "candidate_ips.txt"
            path.write_text(
                "# schema=aquila.gate.order_session.candidate_ips.v1\n"
                "# host=fx-ws.gateio.ws\n"
                "52.198.250.74\n"
                "\n"
                "52.198.250.74\n"
                "52.199.212.24\n",
                encoding="utf-8",
            )

            discovery.merge_candidate_ip_files(
                records,
                run_id="run-1",
                host="fx-ws.gateio.ws",
                target="/v4/ws/usdt",
                port="443",
                paths=[str(path)],
            )

        self.assertEqual(list(records.keys()), ["52.198.250.74", "52.199.212.24"])
        self.assertEqual(records["52.198.250.74"]["sources"], ["candidate_file"])
        self.assertEqual(records["52.199.212.24"]["sources"], ["candidate_file"])

    def test_writes_jsonl_and_text_outputs(self):
        records = {}
        discovery.ensure_record(
            records,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt",
            port="443",
            ip="52.198.250.74",
        )
        discovery.ensure_record(
            records,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt",
            port="443",
            ip="1.2.3.4",
        )
        records["52.198.250.74"]["status"] = "ws_verified"
        records["52.198.250.74"]["selected_for_rtt_probe"] = True
        records["1.2.3.4"]["status"] = "rejected"
        records["1.2.3.4"]["rejection_reason"] = "websocket_verify_failed"

        jsonl = io.StringIO()
        discovery.write_jsonl(records, jsonl)
        lines = [json.loads(line) for line in jsonl.getvalue().splitlines()]
        self.assertEqual([line["ip"] for line in lines], ["52.198.250.74", "1.2.3.4"])
        self.assertEqual(lines[0]["schema"], "aquila.gate.order_session.ip_discovery.v1")

        txt = io.StringIO()
        discovery.write_candidate_txt(
            records,
            txt,
            run_id="run-1",
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt",
            generated_at_ns=300,
        )
        self.assertEqual(
            txt.getvalue().splitlines(),
            [
                "# schema=aquila.gate.order_session.candidate_ips.v1",
                "# run_id=run-1",
                "# host=fx-ws.gateio.ws",
                "# target=/v4/ws/usdt",
                "# generated_at_ns=300",
                "52.198.250.74",
            ],
        )


if __name__ == "__main__":
    unittest.main()
