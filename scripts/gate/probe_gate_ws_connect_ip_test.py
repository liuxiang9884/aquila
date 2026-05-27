#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest

import probe_gate_ws_connect_ip as probe


class ProbeGateWsConnectIpTest(unittest.TestCase):
    def test_login_signature_matches_cpp_vector(self):
        self.assertEqual(
            probe.build_login_signature("secret", "futures.login", 1700000000),
            "f39035057b3528fc2c5aff4b9cfa9f43673c88d3ff823c5546860817320"
            "5809999a8b45d7ed898ebf49c15a4f6e5131de175ded143be5eeb58431f"
            "600e1d4085",
        )

    def test_handshake_uses_logical_host_header(self):
        request = probe.build_handshake_request(
            host="fx-ws.gateio.ws",
            target="/v4/ws/usdt",
            websocket_key="fixed-key",
            extra_headers=["X-Gate-Size-Decimal: 1"],
        )

        self.assertIn(b"GET /v4/ws/usdt HTTP/1.1\r\n", request)
        self.assertIn(b"Host: fx-ws.gateio.ws\r\n", request)
        self.assertIn(b"X-Gate-Size-Decimal: 1\r\n", request)
        self.assertNotIn(b"Host: 57.181.9.46\r\n", request)

    def test_client_text_frame_is_masked_and_round_trips(self):
        frame = probe.encode_client_text_frame("hello", mask_key=b"\x01\x02\x03\x04")

        self.assertEqual(frame[0], 0x81)
        self.assertEqual(frame[1], 0x80 | 5)
        self.assertEqual(frame[2:6], b"\x01\x02\x03\x04")
        decoded = bytes(value ^ b"\x01\x02\x03\x04"[index % 4] for index, value in enumerate(frame[6:]))
        self.assertEqual(decoded, b"hello")

    def test_parse_login_response_success(self):
        result = probe.parse_login_response(
            '{"request_id":"login-1","header":{"status":"200","conn_id":"c1"},'
            '"data":{"result":{"uid":"14446887"}}}'
        )

        self.assertTrue(result.ok)
        self.assertEqual(result.uid, "14446887")
        self.assertEqual(result.request_id, "login-1")
        self.assertEqual(result.conn_id, "c1")

    def test_parse_login_response_error(self):
        result = probe.parse_login_response(
            '{"request_id":"login-1","header":{"status":"401"},'
            '"data":{"errs":{"label":"INVALID_KEY","message":"bad key"}}}'
        )

        self.assertFalse(result.ok)
        self.assertEqual(result.status, "401")
        self.assertIn("INVALID_KEY", result.detail)

    def test_parse_args_uses_port_name(self):
        args = probe.parse_args(["--connect-ip", "57.181.9.46", "--port", "443"])

        self.assertEqual(args.connect_ip, ["57.181.9.46"])
        self.assertEqual(args.port, "443")


if __name__ == "__main__":
    unittest.main()
