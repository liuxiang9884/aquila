#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import os
import sys
import tomllib
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory


PROJECT_ROOT = Path(__file__).resolve().parents[4]
SCRIPT_DIR = PROJECT_ROOT / "scripts" / "bitget" / "trading"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import prepare_gateway_smoke_run as prepare


def write_fake_process(
    proc_root: Path,
    pid: int,
    executable: str,
    config: Path,
    credentials: tuple[str, str, str] = (
        "key-value",
        "secret-value",
        "passphrase-value",
    ),
    start_time_ticks: int = 100,
) -> None:
    process_dir = proc_root / str(pid)
    process_dir.mkdir(parents=True, exist_ok=True)
    binary = proc_root / "bin" / executable
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.touch(exist_ok=True)
    executable_link = process_dir / "exe"
    executable_link.unlink(missing_ok=True)
    executable_link.symlink_to(binary)
    (process_dir / "cmdline").write_bytes(
        b"\0".join(
            value.encode("utf-8")
            for value in (
                str(binary),
                "--config",
                str(config),
                "--connect",
            )
        )
        + b"\0"
    )
    environment = {
        "BITGET_TEST_KEY": credentials[0],
        "BITGET_TEST_SECRET": credentials[1],
        "BITGET_TEST_PASSPHRASE": credentials[2],
    }
    (process_dir / "environ").write_bytes(
        b"\0".join(
            f"{key}={value}".encode("utf-8")
            for key, value in environment.items()
        )
        + b"\0"
    )
    stat_tail = ["S"] + ["0"] * 18 + [str(start_time_ticks)]
    (process_dir / "stat").write_text(
        f"{pid} ({executable}) " + " ".join(stat_tail) + "\n",
        encoding="utf-8",
    )


class PrepareGatewaySmokeRunTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = TemporaryDirectory(dir="/home/liuxiang/tmp")
        self.run_dir = Path(self.temp_dir.name)
        self.run_id = self.run_dir.name
        self.config_dir = self.run_dir / "configs"
        self.proc_root = self.run_dir / "proc"
        self.data_pid = 2101
        self.gateway_pid = 2102
        self.feedback_pid = 2103
        self.data_source = (
            PROJECT_ROOT / "config/data_sessions/bitget_gateway_smoke.toml"
        )
        self.gateway_source = (
            PROJECT_ROOT / "config/order_gateways/bitget_order_gateway.toml"
        )
        self.feedback_source = (
            PROJECT_ROOT
            / "config/order_feedback/bitget_order_feedback_session.toml"
        )
        self.smoke_source = (
            PROJECT_ROOT
            / "config/gateway_smoke/bitget_btcusdt_gateway_smoke.toml"
        )

    def tearDown(self):
        self.temp_dir.cleanup()

    def prepare(self):
        return prepare.prepare_runtime_configs(
            run_id=self.run_id,
            data_session_source=self.data_source,
            gateway_source=self.gateway_source,
            feedback_source=self.feedback_source,
            smoke_source=self.smoke_source,
            output_dir=self.config_dir,
        )

    def write_processes(
        self,
        result,
        gateway_credentials=("key-value", "secret-value", "passphrase-value"),
        feedback_credentials=("key-value", "secret-value", "passphrase-value"),
    ):
        write_fake_process(
            self.proc_root,
            self.data_pid,
            "bitget_data_session",
            result.data_session_config,
        )
        write_fake_process(
            self.proc_root,
            self.gateway_pid,
            "bitget_order_gateway",
            result.gateway_config,
            gateway_credentials,
        )
        write_fake_process(
            self.proc_root,
            self.feedback_pid,
            "bitget_order_feedback_session",
            result.feedback_config,
            feedback_credentials,
        )

    def test_prepare_generates_four_run_specific_configs(self):
        result = self.prepare()

        self.assertEqual(
            result.market_data_shm,
            f"aquila_bitget_market_data_{self.run_id}",
        )
        self.assertEqual(
            result.gateway_shm,
            f"aquila_bitget_order_gateway_{self.run_id}",
        )
        self.assertEqual(
            result.feedback_shm,
            f"aquila_bitget_order_feedback_{self.run_id}",
        )
        self.assertEqual(result.manifest.parent, self.config_dir)
        data = tomllib.loads(result.data_session_config.read_text(encoding="utf-8"))
        gateway = tomllib.loads(result.gateway_config.read_text(encoding="utf-8"))
        feedback = tomllib.loads(result.feedback_config.read_text(encoding="utf-8"))
        smoke = tomllib.loads(result.smoke_config.read_text(encoding="utf-8"))
        self.assertEqual(data["data_shm_sink"]["shm_name"], result.market_data_shm)
        self.assertEqual(gateway["order_gateway"]["shm_name"], result.gateway_shm)
        self.assertEqual(
            feedback["order_feedback_session"]["shm"]["shm_name"],
            result.feedback_shm,
        )
        self.assertEqual(smoke["gateway_smoke"]["run_id"], self.run_id)
        self.assertEqual(smoke["market_data"]["shm_name"], result.market_data_shm)
        self.assertEqual(smoke["order_gateway"]["shm_name"], result.gateway_shm)
        self.assertEqual(smoke["feedback"]["shm_name"], result.feedback_shm)
        self.assertEqual(smoke["output"]["run_dir"], str(self.run_dir))

    def test_prepare_rejects_existing_fresh_run_output(self):
        self.prepare()

        with self.assertRaisesRegex(ValueError, "fresh-run output already exists"):
            self.prepare()

    def test_validate_rejects_config_digest_change(self):
        result = self.prepare()
        result.smoke_config.write_text(
            result.smoke_config.read_text(encoding="utf-8") + "\n# tampered\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            prepare.validate_manifest(result.manifest, require_applied=False)

    def test_validate_rejects_runner_config_mismatch(self):
        result = self.prepare()

        with self.assertRaisesRegex(ValueError, "runner command config mismatch"):
            prepare.validate_manifest(
                result.manifest,
                runner_command=[
                    "/opt/aquila/bitget_gateway_smoke",
                    "--config",
                    str(result.gateway_config),
                    "--execute",
                ],
                require_applied=False,
            )

    def test_mark_applied_binds_three_processes_without_secrets(self):
        result = self.prepare()
        self.write_processes(result)

        manifest = prepare.mark_processes_applied(
            result.manifest,
            data_session_pid=self.data_pid,
            gateway_pid=self.gateway_pid,
            feedback_pid=self.feedback_pid,
            proc_root=self.proc_root,
        )

        self.assertEqual(
            set(manifest["processes"]),
            {"data_session", "gateway", "feedback"},
        )
        self.assertTrue(manifest["external_configs_applied"])
        serialized = json.dumps(manifest)
        self.assertNotIn("key-value", serialized)
        self.assertNotIn("secret-value", serialized)
        self.assertNotIn("passphrase-value", serialized)
        validated = prepare.validate_manifest(
            result.manifest,
            runner_command=[
                "/opt/aquila/bitget_gateway_smoke",
                "--config",
                str(result.smoke_config),
                "--execute",
            ],
            require_applied=True,
            proc_root=self.proc_root,
        )
        self.assertEqual(validated["run_id"], self.run_id)

    def test_mark_applied_rejects_process_credential_mismatch(self):
        result = self.prepare()
        self.write_processes(
            result,
            feedback_credentials=(
                "different-key",
                "secret-value",
                "passphrase-value",
            ),
        )

        with self.assertRaisesRegex(ValueError, "credential values"):
            prepare.mark_processes_applied(
                result.manifest,
                data_session_pid=self.data_pid,
                gateway_pid=self.gateway_pid,
                feedback_pid=self.feedback_pid,
                proc_root=self.proc_root,
            )

    def test_validate_rejects_reused_data_session_pid(self):
        result = self.prepare()
        self.write_processes(result)
        prepare.mark_processes_applied(
            result.manifest,
            data_session_pid=self.data_pid,
            gateway_pid=self.gateway_pid,
            feedback_pid=self.feedback_pid,
            proc_root=self.proc_root,
        )
        write_fake_process(
            self.proc_root,
            self.data_pid,
            "bitget_data_session",
            result.data_session_config,
            start_time_ticks=101,
        )

        with self.assertRaisesRegex(ValueError, "start time changed"):
            prepare.validate_manifest(
                result.manifest,
                require_applied=True,
                proc_root=self.proc_root,
            )


if __name__ == "__main__":
    unittest.main()
