#!/home/liuxiang/dev/pyenv/lx/bin/python

import sys
import unittest
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import guard_exchange_adapter as adapters


def fake_callable(*args, **kwargs):
    return args, kwargs


class GuardExchangeAdapterTest(unittest.TestCase):
    def test_gate_adapter_preserves_supplied_operations(self):
        adapter = adapters.GuardExchangeAdapter(
            name="gate",
            credential_resolver=fake_callable,
            requester_factory=fake_callable,
            state_reader=fake_callable,
            flatten_config_builder=fake_callable,
            flatten_runner=fake_callable,
        )

        resolved = adapters.get_guard_exchange_adapter(
            "gate", gate_adapter=adapter
        )

        self.assertIs(resolved, adapter)
        self.assertEqual(resolved.name, "gate")

    def test_unknown_exchange_is_rejected(self):
        adapter = adapters.GuardExchangeAdapter(
            name="gate",
            credential_resolver=fake_callable,
            requester_factory=fake_callable,
            state_reader=fake_callable,
            flatten_config_builder=fake_callable,
            flatten_runner=fake_callable,
        )

        with self.assertRaisesRegex(ValueError, "unsupported guard exchange"):
            adapters.get_guard_exchange_adapter("unknown", gate_adapter=adapter)

    def test_credential_names_allow_gate_without_passphrase(self):
        credentials = adapters.GuardCredentialEnvNames(
            api_key_env="GATE_KEY",
            api_secret_env="GATE_SECRET",
            api_passphrase_env=None,
            source="order_gateway_config",
        )

        self.assertIsNone(credentials.api_passphrase_env)


if __name__ == "__main__":
    unittest.main()
