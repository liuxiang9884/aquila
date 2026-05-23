#!/home/liuxiang/dev/pyenv/lx/bin/python

import unittest
from decimal import Decimal

import run_live_with_guard as guard


class FakeStateReader:
    def __init__(self, states):
        self.states = list(states)
        self.calls = []

    def __call__(self, requester, settle, contracts):
        self.calls.append((settle, tuple(contracts)))
        if not self.states:
            raise AssertionError("unexpected state read")
        return self.states.pop(0)


class FakeProcessRunner:
    def __init__(self, result):
        self.result = result
        self.commands = []

    def __call__(self, command):
        self.commands.append(list(command))
        return self.result


class FakeFlattenRunner:
    def __init__(self, result):
        self.result = result
        self.calls = []

    def __call__(self, config, requester, clock):
        self.calls.append(config)
        return self.result


def flat_state(contract="BTC_USDT"):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(contract=contract, size=0, pending_orders=0)
        ],
        open_orders=[],
    )


def open_order_state(contract="BTC_USDT"):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(contract=contract, size=0, pending_orders=0)
        ],
        open_orders=[guard.OpenOrder(contract=contract, order_id="12345")],
    )


def position_state(contract="BTC_USDT", size=1):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(contract=contract, size=size, pending_orders=0)
        ],
        open_orders=[],
    )


def residual_value_state(contract="RAVE_USDT"):
    return guard.GuardState(
        positions=[
            guard.PositionSnapshot(
                contract=contract,
                size=Decimal("0"),
                pending_orders=0,
                value=Decimal("0.11248"),
                margin=Decimal("0.022496"),
            )
        ],
        open_orders=[],
    )


def config(command=None):
    return guard.GuardConfig(
        settle="usdt",
        contracts=["BTC_USDT"],
        strategy_command=command
        or ["./build/debug/tools/lead_lag_strategy", "--execute"],
        poll_timeout_sec=3.0,
        poll_interval_sec=0.5,
    )


class RunLiveWithGuardTest(unittest.TestCase):
    def test_query_guard_state_treats_position_not_found_as_flat_contract(self):
        calls = []

        def fake_request(api_request):
            calls.append(api_request)
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/positions/ZEC_USDT"):
                raise RuntimeError('HTTP 400: {"label":"POSITION_NOT_FOUND"}')
            if api_request.method == "GET" and api_request.endpoint_path.endswith("/orders"):
                return []
            raise AssertionError(f"unexpected request: {api_request}")

        state = guard.query_guard_state(fake_request, "usdt", ["ZEC_USDT"])

        self.assertTrue(state.flat())
        self.assertEqual(state.positions[0].contract, "ZEC_USDT")
        self.assertEqual(state.positions[0].size, 0)
        self.assertEqual(state.positions[0].pending_orders, 0)
        self.assertEqual(state.open_orders, [])
        self.assertEqual(len(calls), 2)

    def test_preflight_not_flat_refuses_to_start_or_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([open_order_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_PREFLIGHT_FAILED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "preflight_not_flat")
        self.assertEqual(process.commands, [])
        self.assertEqual(flatten.calls, [])
        self.assertEqual(summary["preflight"]["open_orders"][0]["order_id"], "12345")

    def test_preflight_residual_value_refuses_to_start_or_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([residual_value_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_PREFLIGHT_FAILED)
        self.assertEqual(summary["result"], "preflight_not_flat")
        self.assertEqual(process.commands, [])
        self.assertEqual(flatten.calls, [])
        self.assertFalse(summary["preflight"]["flat"])
        self.assertEqual(summary["preflight"]["positions"][0]["value"], "0.11248")

    def test_normal_exit_and_flat_final_state_returns_ok(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state(), flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_OK)
        self.assertTrue(summary["ok"])
        self.assertEqual(summary["result"], "normal_exit_flat")
        self.assertEqual(process.commands, [config().strategy_command])
        self.assertEqual(flatten.calls, [])

    def test_nonzero_strategy_exit_runs_flatten_and_reports_handoff(self):
        process = FakeProcessRunner(
            guard.ProcessResult(exit_code=guard.STRATEGY_EXIT_CONTINUITY_LOST)
        )
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "strategy_exit_flattened")
        self.assertEqual(summary["strategy"]["exit_code"], guard.STRATEGY_EXIT_CONTINUITY_LOST)
        self.assertEqual(len(flatten.calls), 1)
        self.assertEqual(flatten.calls[0].contracts, ["BTC_USDT"])
        self.assertFalse(flatten.calls[0].dry_run)

    def test_normal_exit_with_nonflat_final_state_runs_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state(), position_state(size=2)]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "final_check_flattened")
        self.assertEqual(summary["final_check"]["positions"][0]["size"], 2)
        self.assertEqual(len(flatten.calls), 1)

    def test_normal_exit_with_residual_value_final_state_runs_flatten(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=0))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_OK,
                {"ok": True, "result": "verified_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state("RAVE_USDT"), residual_value_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "final_check_flattened")
        self.assertFalse(summary["final_check"]["flat"])
        self.assertEqual(summary["final_check"]["positions"][0]["value"], "0.11248")
        self.assertEqual(len(flatten.calls), 1)

    def test_flatten_failure_returns_emergency_failed(self):
        process = FakeProcessRunner(guard.ProcessResult(exit_code=1))
        flatten = FakeFlattenRunner(
            (
                guard.FLATTEN_EXIT_NOT_FLAT,
                {"ok": False, "result": "not_flat"},
            )
        )

        exit_code, summary = guard.run_guarded_live(
            config=config(),
            requester=lambda request: {},
            process_runner=process,
            flatten_runner=flatten,
            state_reader=FakeStateReader([flat_state()]),
        )

        self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FAILED)
        self.assertFalse(summary["ok"])
        self.assertEqual(summary["result"], "strategy_exit_flatten_failed")
        self.assertEqual(summary["flatten"]["result"], "not_flat")

    def test_parse_args_uses_separator_for_strategy_command(self):
        parsed = guard.parse_args(
            [
                "--contract",
                "btc_usdt",
                "--poll-timeout-sec",
                "5",
                "--",
                "./build/debug/tools/lead_lag_strategy",
                "--execute",
                "--duration-sec",
                "10",
            ]
        )
        config = guard.config_from_args(parsed)

        self.assertEqual(config.contracts, ["BTC_USDT"])
        self.assertEqual(config.poll_timeout_sec, 5.0)
        self.assertEqual(
            config.strategy_command,
            [
                "./build/debug/tools/lead_lag_strategy",
                "--execute",
                "--duration-sec",
                "10",
            ],
        )


if __name__ == "__main__":
    unittest.main()
