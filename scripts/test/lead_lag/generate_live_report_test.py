#!/home/liuxiang/dev/pyenv/lx/bin/python

import csv
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import generate_live_report as report


def write_file(path: Path, text: str) -> None:
    path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")


def write_config(path: Path) -> None:
    write_file(
        path,
        """
        [lead_lag]
        name = "lead_lag"
        version = "1.0"

        [[lead_lag.pairs]]
        symbol = "PROVE_USDT"
        symbol_id = 4
        lead_exchange = "binance"
        lag_exchange = "gate"
        lag_taker_fee = 0.00016

        [lead_lag.pairs.execute]
        open_notional = 100.0
        trailing_stop = 0.01
        open_slippage = 3
        close_slippage = 3
        """,
    )


def write_catalog(path: Path) -> None:
    path.write_text(
        "symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,"
        "settle_asset,product_type,status,contract_type,price_tick,"
        "price_decimal_places,quantity_step,quantity_decimal_places,"
        "min_quantity,max_quantity,max_market_quantity,min_notional,"
        "notional_multiplier,price_limit_up,price_limit_down,"
        "market_price_bound\n"
        "4,PROVE_USDT,gate,PROVE_USDT,PROVE,USDT,USDT,"
        "linear_perpetual,TRADING,direct,0.0001,4,1.0,0,"
        "1.0,68000.0,45000.0,,10.0,0.1,0.1,0.035\n",
        encoding="utf-8",
    )


class GenerateLiveReportTest(unittest.TestCase):
    def test_generates_report_directory_from_live_log(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n\n`local_order_id`\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_ticker_id=10624277384126 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                I2026-05-25 02:29:35.105563683 1:1 order_session.h:LogGatePlaceOrderSent:466] gate_order_send_ok type=place local_order_id=288230376151711749 request_sequence=6 encoded_request_id=144115188075855878 contract=PROVE_USDT side=kBuy quantity=36 price=0.2714 tif=kImmediateOrCancel reduce_only=false inflight=5 request_send_local_ns=1779676175105554883
                I2026-05-25 02:29:35.105566021 1:1 strategy.h:LogStrategyOrderSubmitted:1] lead_lag_order_submitted local_order_id=288230376151711749 trigger_ticker_id=10624277384126 trigger_exchange=kBinance trigger_symbol_id=4 trigger_exchange_ns=1779676175105510000 trigger_local_ns=1779676175105520000 on_book_ticker_entry_ns=1779676175105530000 signal_decision_ns=1779676175105540000 symbol=PROVE_USDT symbol_id=4 signal_role=kLead order_role=entry action=kOpenLong side=kBuy reduce_only=false position_id=1 position_event=kEntrySubmit position_direction=kLong entry_local_order_id=288230376151711749 quantity=36 quantity_text=36 raw_price=0.2711 order_price=0.2714 price_text=0.2714 slippage_ticks=3 price_tick=0.0001 target_open_notional=100 estimated_notional=97.704 active_groups=1 place_status=kOk
                I2026-05-25 02:29:35.111554171 1:1 order_session.h:LogGateOrderResponse:517] gate_order_response kind=kAck local_order_id=288230376151711749 exchange_order_id=0 request_sequence=6 channel=2 http_status=200 error_label_hash=0 error_label= error_message= local_receive_ns=1779676175111547348 exchange_ns=1779676175107083000 exchange_x_in_ns=1779676175107000000 exchange_x_out_ns=1779676175107083000 exchange_x_in_to_x_out_ns=83000 exchange_to_local_ns=4464348
                W2026-05-25 02:29:35.112000000 1:1 order_session.h:LogOrderLatencyDiagnostic:1] gate_order_ack_latency_diagnostic reason=kAckRttThreshold local_order_id=288230376151711749 request_sequence=6 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 ack_exchange_ns=1779676175107083000 ack_rtt_ns=5992465 send_to_first_after_hook_ns=1000 send_to_first_drive_read_ns=3499001 drive_read_duration_ns=1300001 max_observed_drive_read_duration_ns=1300001 inflight_at_send=7
                I2026-05-25 02:29:35.117544030 1:1 strategy.h:LogStrategyOrderFeedback:272] lead_lag_order_feedback kind=kFilled local_order_id=288230376151711749 exchange_order_id=260082878984634644 cumulative_filled_quantity=36 left_quantity=0 cancelled_quantity=0 fill_price=0.2714 role=kTaker finish_reason=kUnknown reject_reason=kUnknown
                I2026-05-25 02:29:35.117545430 1:1 strategy.h:LogStrategyOrderFinished:335] lead_lag_order_finished local_order_id=288230376151711749 symbol_id=4 symbol=PROVE_USDT status=kFilled reduce_only=false position_id=1 position_direction=kLong order_role=entry entry_local_order_id=288230376151711749 order_finished_local_ns=1779676175117545430 quantity=36 cumulative_filled_quantity=36 average_fill_price=0.2714 last_fill_price=0.2714 exchange_order_id=260082878984634644 active_groups=1 request_send_local_ns=1779676175105554883 ack_local_receive_ns=1779676175111547348 response_local_receive_ns=0 ack_exchange_ns=1779676175107083000 response_exchange_ns=0 accepted_exchange_ns=0 finish_exchange_ns=1779676175113000000 ack_rtt_ns=5992465 response_rtt_ns=0 ack_exchange_to_local_ns=4464348 response_exchange_to_local_ns=0 exchange_lifecycle_ns=0
                """,
            )

            result = report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
            )

            report_dir = output_root / "run-1"
            self.assertEqual(result.report_dir, report_dir)
            for name in (
                "signal.csv",
                "order_detail.csv",
                "position.csv",
                "latency.csv",
                "lead_lag_live_report_csv_schema.md",
                "report.md",
            ):
                self.assertTrue((report_dir / name).exists(), name)

            with (report_dir / "signal.csv").open(newline="", encoding="utf-8") as input_file:
                signal_row = next(csv.DictReader(input_file))
            with (report_dir / "latency.csv").open(newline="", encoding="utf-8") as input_file:
                latency_row = next(csv.DictReader(input_file))
            report_text = (report_dir / "report.md").read_text(encoding="utf-8")

        self.assertEqual(signal_row["run_id"], "run-1")
        self.assertEqual(signal_row["signal_index"], "1")
        self.assertEqual(signal_row["log_time"], "2026-05-25 02:29:35.105545332")
        self.assertEqual(signal_row["trigger_exchange"], "kBinance")
        self.assertEqual(signal_row["trigger_local_ns"], "1779676175105520000")
        self.assertEqual(signal_row["bbo_to_strategy_ns"], "10000")
        self.assertEqual(signal_row["strategy_to_signal_ns"], "10000")
        self.assertEqual(signal_row["signal_to_request_send_ns"], "14883")
        self.assertEqual(signal_row["trigger_to_request_send_ns"], "34883")
        self.assertEqual(signal_row["local_order_id"], "288230376151711749")
        self.assertEqual(signal_row["exchange_order_id"], "260082878984634644")
        self.assertEqual(signal_row["order_role"], "entry")
        self.assertEqual(signal_row["order_position_id"], "1")
        self.assertEqual(signal_row["status"], "kFilled")
        self.assertEqual(latency_row["latency_diagnostic_reason"], "kAckRttThreshold")
        self.assertEqual(latency_row["ack_exchange_x_in_to_x_out_ns"], "83000")
        self.assertEqual(latency_row["exchange_lifecycle_ns"], "5917000")
        self.assertIn("- signal: `1`", report_text)
        self.assertIn("- submitted order: `1`", report_text)
        self.assertIn("- latency diagnostic outliers: `1`", report_text)
        self.assertIn("- exchange Ack-to-finish min: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish median: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish avg: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish p95: `5.917 ms`", report_text)
        self.assertIn("- exchange Ack-to-finish max: `5.917 ms`", report_text)
        self.assertIn("- Gate Ack x_in-to-x_out min: `0.083 ms`", report_text)
        self.assertIn("- Gate Ack x_in-to-x_out max: `0.083 ms`", report_text)
        self.assertIn(
            "| 288230376151711749 | PROVE_USDT | kFilled | kUnknown | 5.917 | 5.998 | 11.991 |",
            report_text,
        )
        self.assertIn("- 字段参考: `lead_lag_live_report_csv_schema.md`", report_text)

    def test_refuses_to_overwrite_existing_report_without_flag(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            output_root = base / "reports"
            (output_root / "run-1").mkdir(parents=True)
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_ticker_id=1 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                """,
            )

            with self.assertRaises(FileExistsError):
                report.generate_live_report(
                    log_path=log_path,
                    config_path=config_path,
                    instrument_catalog_path=catalog_path,
                    run_id="run-1",
                    output_root=output_root,
                    schema_path=schema_path,
                )

    def test_report_includes_affinity_summary_from_guard_stdout(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            log_path = base / "run.log"
            config_path = base / "strategy.toml"
            catalog_path = base / "catalog.csv"
            schema_path = base / "schema.md"
            guard_stdout_path = base / "guard.stdout"
            generated_strategy_path = base / "tmp" / "configs" / "strategy.toml"
            generated_strategy_path.parent.mkdir(parents=True)
            generated_strategy_path.write_text("[strategy]\n", encoding="utf-8")
            output_root = base / "reports"
            write_config(config_path)
            write_catalog(catalog_path)
            schema_path.write_text("# schema\n", encoding="utf-8")
            write_file(
                log_path,
                """
                I2026-05-25 02:29:35.105545332 1:1 strategy.h:LogStrategySignalTriggered:155] lead_lag_signal_triggered trigger_ticker_id=1 trigger_exchange=kBinance trigger_symbol_id=4 symbol=PROVE_USDT symbol_id=4 role=kLead action=kOpenLong side=kBuy reduce_only=false position_id=0 raw_price=0.2711
                """,
            )
            guard_stdout_path.write_text(
                """
                noisy child log {"ignored": true}
                {
                  "affinity": {
                    "profile_name": "lead_lag_requested_12symbols_node0",
                    "affinity_split": true,
                    "output_dir": "/home/liuxiang/tmp/run-1/configs",
                    "core_path": {
                      "gate_market_data_cpu": 2,
                      "binance_market_data_cpu": 3,
                      "strategy_order_owner_cpu": 4,
                      "gate_order_feedback_cpu": 6,
                      "log_backend_cpu": 5
                    },
                    "generated_configs": {
                      "strategy": "%s"
                    }
                  }
                }
                """
                % generated_strategy_path,
                encoding="utf-8",
            )

            report.generate_live_report(
                log_path=log_path,
                config_path=config_path,
                instrument_catalog_path=catalog_path,
                run_id="run-1",
                output_root=output_root,
                schema_path=schema_path,
                guard_stdout_path=guard_stdout_path,
            )

            report_text = (output_root / "run-1" / "report.md").read_text(
                encoding="utf-8"
            )
            copied_config_exists = (
                output_root
                / "run-1"
                / "runtime_configs"
                / "strategy__strategy.toml"
            ).exists()

        self.assertIn("- affinity profile: `lead_lag_requested_12symbols_node0`", report_text)
        self.assertIn("- affinity split: `true`", report_text)
        self.assertIn("- strategy_order_owner_cpu: `4`", report_text)
        self.assertIn("- generated strategy config:", report_text)
        self.assertTrue(copied_config_exists)


if __name__ == "__main__":
    unittest.main()
