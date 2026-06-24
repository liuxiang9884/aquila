#!/home/liuxiang/dev/pyenv/lx/bin/python

import json
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[2] / "lead_lag"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import summarize_guard_audit as audit


def write_file(path: Path, text: str) -> None:
    path.write_text(textwrap.dedent(text).strip() + "\n", encoding="utf-8")


class SummarizeGuardAuditTest(unittest.TestCase):
    def test_matches_audit_rows_to_entry_orders_and_groups_outcomes(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            position_path = base / "positions.csv"
            json_path = base / "summary.json"
            md_path = base / "summary.md"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                1,PROVE_USDT,4,kOpenShort,kSell,200,200,190,5002,6002,11,false,none,0,0,false,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status,cumulative_filled_quantity,position_id
                10,PROVE_USDT,4,entry,kOpenLong,6001,kCancelled,0,1
                11,PROVE_USDT,4,entry,kOpenShort,6002,kFilled,10,2
                """,
            )
            write_file(
                position_path,
                """
                symbol,symbol_id,position_id,status,gross_pnl,net_pnl
                PROVE_USDT,4,1,closed,-1.2,-1.3
                PROVE_USDT,4,2,closed,0.5,0.4
                """,
            )

            summary = audit.summarize_guard_audit(
                guard_path, order_path, position_path
            )
            audit.write_summary_json(summary, json_path)
            audit.write_summary_markdown(summary, md_path)

            saved = json.loads(json_path.read_text(encoding="utf-8"))

        self.assertEqual(saved["totals"]["open_signal_count"], 2)
        self.assertEqual(saved["totals"]["would_block_count"], 1)
        self.assertEqual(saved["groups"]["blocked"]["order_count"], 1)
        self.assertEqual(saved["groups"]["blocked"]["zero_fill_cancelled"], 1)
        self.assertEqual(saved["groups"]["allowed"]["filled"], 1)
        self.assertEqual(saved["groups"]["blocked"]["net_pnl"], "-1.3")
        self.assertEqual(saved["groups"]["allowed"]["net_pnl"], "0.4")

    def test_missing_position_file_still_summarizes_orders(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status,cumulative_filled_quantity,position_id
                10,PROVE_USDT,4,entry,kOpenLong,6001,kCancelled,0,1
                """,
            )

            summary = audit.summarize_guard_audit(guard_path, order_path, None)

        self.assertEqual(summary["totals"]["open_signal_count"], 1)
        self.assertEqual(summary["groups"]["blocked"]["order_count"], 1)
        self.assertEqual(summary["warnings"], [])

    def test_counts_unmatched_audit_rows(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status,cumulative_filled_quantity,position_id
                11,PROVE_USDT,4,entry,kOpenShort,6002,kFilled,10,2
                """,
            )

            summary = audit.summarize_guard_audit(guard_path, order_path, None)

        self.assertEqual(summary["totals"]["unmatched_audit_rows"], 1)
        self.assertEqual(summary["totals"]["unmatched_order_rows"], 1)

    def test_missing_required_fields_warns_and_does_not_match_empty_keys(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            write_file(
                guard_path,
                """
                would_block,symbol_id
                true,4
                """,
            )
            write_file(
                order_path,
                """
                order_role
                entry
                """,
            )

            summary = audit.summarize_guard_audit(guard_path, order_path, None)

        self.assertEqual(summary["totals"]["unmatched_audit_rows"], 1)
        self.assertEqual(summary["totals"]["unmatched_order_rows"], 1)
        self.assertIn(
            "guard_audit missing required fields: action, signal_lag_id, symbol",
            summary["warnings"],
        )
        self.assertIn(
            "order_detail missing required fields: action, cumulative_filled_quantity, position_id, signal_lag_id, status, symbol_id",
            summary["warnings"],
        )

    def test_reports_block_rates_ratios_and_position_status_breakdown(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            position_path = base / "positions.csv"
            md_path = base / "summary.md"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                1,PROVE_USDT,4,kOpenLong,kBuy,200,200,190,5002,6002,11,false,none,0,0,false,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status,cumulative_filled_quantity,position_id
                10,PROVE_USDT,4,entry,kOpenLong,6001,kPartiallyCancelled,2,1
                11,PROVE_USDT,4,entry,kOpenLong,6002,kCancelled,0,2
                """,
            )
            write_file(
                position_path,
                """
                symbol,symbol_id,position_id,status,gross_pnl,net_pnl
                PROVE_USDT,4,1,partial_closed,1.2,1.1
                PROVE_USDT,4,2,missing_entry,-0.5,-0.6
                """,
            )

            summary = audit.summarize_guard_audit(
                guard_path, order_path, position_path
            )
            audit.write_summary_markdown(summary, md_path)
            markdown = md_path.read_text(encoding="utf-8")

        self.assertEqual(summary["by_symbol"]["PROVE_USDT"]["block_rate"], "0.5")
        self.assertEqual(summary["by_action"]["kOpenLong"]["block_rate"], "0.5")
        self.assertEqual(summary["groups"]["blocked"]["partially_filled"], 1)
        self.assertEqual(summary["groups"]["allowed"]["zero_fill_cancel_rate"], "1")
        self.assertEqual(summary["groups"]["blocked"]["partial_closed_positions"], 1)
        self.assertEqual(summary["groups"]["allowed"]["missing_entry_positions"], 1)
        self.assertIn("Gross PnL", markdown)
        self.assertIn("Warnings", markdown)
        self.assertIn("PROVE_USDT", markdown)

    def test_missing_quantity_or_position_key_does_not_pollute_metrics(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            position_path = base / "positions.csv"
            write_file(
                guard_path,
                """
                open_signal_index,symbol,symbol_id,action,side,trigger_exchange_ns,lead_exchange_ns,lag_exchange_ns,signal_lead_id,signal_lag_id,raw_price,would_block,would_block_reason,lag_vol_jump_count,lag_vol_amplitude,lag_vol_hot,lag_vol_cooldown_active,lag_vol_cooldown_until_ns,jump_threshold,jump_count_threshold,jump_window_ns,amplitude_threshold,amplitude_window_ns,cooldown_ns,drift_instant,ratio_std,drift_mean,drift_guard_outcome
                0,PROVE_USDT,4,kOpenLong,kBuy,100,100,90,5001,6001,10,true,lag-vol-guard-trigger,3,0.031,true,false,900,0.005,3,300000000000,0.025,1000000000,900000000000,nan,nan,nan,not_evaluated
                """,
            )
            write_file(
                order_path,
                """
                local_order_id,symbol,symbol_id,order_role,action,signal_lag_id,status
                10,PROVE_USDT,4,entry,kOpenLong,6001,kCancelled
                """,
            )
            write_file(
                position_path,
                """
                symbol,symbol_id,status,gross_pnl,net_pnl
                PROVE_USDT,4,closed,12.0,11.7
                """,
            )

            summary = audit.summarize_guard_audit(
                guard_path, order_path, position_path
            )

        self.assertEqual(summary["groups"]["blocked"]["order_count"], 1)
        self.assertEqual(summary["groups"]["blocked"]["cancelled"], 1)
        self.assertEqual(summary["groups"]["blocked"]["zero_fill_cancelled"], 0)
        self.assertEqual(summary["groups"]["blocked"]["position_count"], 0)
        self.assertEqual(summary["groups"]["blocked"]["gross_pnl"], "0")
        self.assertEqual(summary["groups"]["blocked"]["net_pnl"], "0")
        self.assertIn(
            "order_detail missing required fields: cumulative_filled_quantity, position_id",
            summary["warnings"],
        )
        self.assertIn(
            "position missing required fields: position_id",
            summary["warnings"],
        )

    def test_header_only_missing_fields_still_warns(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            base = Path(temp_dir)
            guard_path = base / "guard.csv"
            order_path = base / "orders.csv"
            position_path = base / "positions.csv"
            write_file(guard_path, "would_block")
            write_file(order_path, "order_role")
            write_file(position_path, "status")

            summary = audit.summarize_guard_audit(
                guard_path, order_path, position_path
            )

        self.assertEqual(summary["totals"]["open_signal_count"], 0)
        self.assertIn(
            "guard_audit missing required fields: action, signal_lag_id, symbol, symbol_id",
            summary["warnings"],
        )
        self.assertIn(
            "order_detail missing required fields: action, cumulative_filled_quantity, position_id, signal_lag_id, status, symbol_id",
            summary["warnings"],
        )
        self.assertIn(
            "position missing required fields: gross_pnl, net_pnl, position_id, symbol_id",
            summary["warnings"],
        )


if __name__ == "__main__":
    unittest.main()
