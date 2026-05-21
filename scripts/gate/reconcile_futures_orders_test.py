#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest

import reconcile_futures_orders as reconcile


class FakeRequester:
    def __init__(
        self,
        *,
        open_orders=None,
        finished_orders=None,
        position=None,
        fail_labels=None,
    ):
        self.open_orders = [] if open_orders is None else open_orders
        self.finished_orders = [] if finished_orders is None else finished_orders
        self.position = {"contract": "BTC_USDT", "size": "0"} if position is None else position
        self.fail_labels = set() if fail_labels is None else set(fail_labels)
        self.calls = []

    def __call__(self, api_request):
        self.calls.append((api_request.label, api_request.endpoint_path, api_request.query_string))
        if api_request.label in self.fail_labels:
            raise RuntimeError(f"{api_request.label} unavailable")
        if api_request.label == "open_orders":
            return self.open_orders
        if api_request.label == "finished_orders":
            return self.finished_orders
        if api_request.label == "rest_position":
            return self.position
        raise AssertionError(f"unexpected request label: {api_request.label}")


def local_order_id(strategy_id=4, sequence=123):
    return (strategy_id << 56) | sequence


class ReconcileFuturesOrdersTest(unittest.TestCase):
    def test_flat_local_state_with_no_open_orders_and_flat_position_recovers(self):
        requester = FakeRequester()

        summary = reconcile.reconcile_futures_orders(
            requester=requester,
            local_state={"orders": [], "execution_groups": [], "position": {"size": "0"}},
        )

        self.assertEqual(summary["state"], "Recovered")
        self.assertEqual(summary["manual_intervention_reason"], "")
        self.assertEqual(summary["mapped_orders"], [])
        self.assertEqual(summary["unmapped_local_orders"], [])
        self.assertEqual(summary["unmapped_remote_orders"], [])
        self.assertEqual(summary["rest_position"], {"contract": "BTC_USDT", "size": "0"})
        self.assertTrue(summary["position_match"])
        self.assertTrue(summary["queries"]["ok"])
        self.assertEqual(summary["queries"]["errors"], {})
        self.assertEqual(
            [call[0] for call in requester.calls],
            ["open_orders", "finished_orders", "rest_position"],
        )

    def test_unmatched_rest_open_order_requires_manual_intervention(self):
        requester = FakeRequester(open_orders=[{"id": "gate-1", "contract": "BTC_USDT", "size": "1"}])

        summary = reconcile.reconcile_futures_orders(
            requester=requester,
            local_state={"orders": [], "execution_groups": [], "position": {"size": "0"}},
        )

        self.assertEqual(summary["state"], "ManualIntervention")
        self.assertIn("unmapped remote open order", summary["manual_intervention_reason"])
        self.assertEqual(summary["unmapped_remote_orders"], [{"id": "gate-1", "contract": "BTC_USDT", "size": "1"}])
        self.assertTrue(summary["position_match"])

    def test_residual_rest_position_requires_manual_intervention(self):
        requester = FakeRequester(position={"contract": "BTC_USDT", "size": "2"})

        summary = reconcile.reconcile_futures_orders(
            requester=requester,
            local_state={"orders": [], "execution_groups": [], "position": {"size": "0"}},
        )

        self.assertEqual(summary["state"], "ManualIntervention")
        self.assertIn("position mismatch", summary["manual_intervention_reason"])
        self.assertFalse(summary["position_match"])
        self.assertEqual(summary["rest_position"], {"contract": "BTC_USDT", "size": "2"})

    def test_invalid_rest_position_size_requires_manual_intervention(self):
        requester = FakeRequester(position={"contract": "BTC_USDT", "size": "bad"})

        summary = reconcile.reconcile_futures_orders(
            requester=requester,
            local_state={"orders": [], "execution_groups": [], "position": {"size": "0"}},
        )

        self.assertEqual(summary["state"], "ManualIntervention")
        self.assertIn("invalid REST position", summary["manual_intervention_reason"])
        self.assertFalse(summary["position_match"])

    def test_invalid_rest_open_orders_shape_requires_manual_intervention(self):
        requester = FakeRequester(open_orders={"unexpected": "shape"})

        summary = reconcile.reconcile_futures_orders(
            requester=requester,
            local_state={"orders": [], "execution_groups": [], "position": {"size": "0"}},
        )

        self.assertEqual(summary["state"], "ManualIntervention")
        self.assertIn("invalid REST open_orders", summary["manual_intervention_reason"])

    def test_conflicting_local_position_sources_require_manual_intervention(self):
        summary = reconcile.reconcile_futures_orders(
            requester=FakeRequester(),
            local_state={
                "orders": [],
                "position": {"size": "0"},
                "execution_groups": [{"group_id": 1, "signed_position_quantity": "1"}],
            },
        )

        self.assertEqual(summary["state"], "ManualIntervention")
        self.assertIn("conflicting local position", summary["manual_intervention_reason"])
        self.assertEqual(summary["local_position_size"], "0")

    def test_local_pending_order_maps_to_remote_text(self):
        order_id = local_order_id()
        requester = FakeRequester(
            open_orders=[
                {
                    "id": "gate-2",
                    "text": f"t-{order_id}",
                    "contract": "BTC_USDT",
                    "size": "1",
                }
            ]
        )

        summary = reconcile.reconcile_futures_orders(
            requester=requester,
            local_state={
                "orders": [
                    {
                        "local_order_id": order_id,
                        "exchange_order_id": "",
                        "contract": "BTC_USDT",
                        "status": "accepted",
                        "is_finished": False,
                    }
                ],
                "execution_groups": [],
                "position": {"size": "0"},
            },
        )

        self.assertEqual(summary["state"], "ManualIntervention")
        self.assertEqual(summary["unmapped_local_orders"], [])
        self.assertEqual(summary["unmapped_remote_orders"], [])
        self.assertEqual(len(summary["mapped_orders"]), 1)
        self.assertEqual(summary["mapped_orders"][0]["local_order_id"], order_id)
        self.assertEqual(summary["mapped_orders"][0]["matched_by"], "text")
        self.assertEqual(summary["mapped_orders"][0]["remote_order"]["id"], "gate-2")

    def test_partial_rest_failure_blocks_recovery_and_reports_errors(self):
        local_state = {"orders": [], "execution_groups": [], "position": {"size": "0"}}

        strict_summary = reconcile.reconcile_futures_orders(
            requester=FakeRequester(fail_labels={"finished_orders"}),
            local_state=local_state,
            allow_partial=False,
        )

        self.assertEqual(strict_summary["state"], "ManualIntervention")
        self.assertIn("REST query failed", strict_summary["manual_intervention_reason"])
        self.assertFalse(strict_summary["queries"]["ok"])
        self.assertFalse(strict_summary["queries"]["allow_partial"])
        self.assertNotEqual(strict_summary["queries"]["errors"], {})

        partial_summary = reconcile.reconcile_futures_orders(
            requester=FakeRequester(fail_labels={"finished_orders"}),
            local_state=local_state,
            allow_partial=True,
        )

        self.assertEqual(partial_summary["state"], "ManualIntervention")
        self.assertIn("REST query failed", partial_summary["manual_intervention_reason"])
        self.assertFalse(partial_summary["queries"]["ok"])
        self.assertTrue(partial_summary["queries"]["allow_partial"])
        self.assertIn("open_orders", partial_summary["queries"]["results"])
        self.assertEqual(
            partial_summary["queries"]["errors"],
            {"finished_orders": "RuntimeError: finished_orders unavailable"},
        )


if __name__ == "__main__":
    unittest.main()
