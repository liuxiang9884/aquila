# Scripts Directory Reorganization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move Gate and Binance scripts into clearer role-based subdirectories and update tests, imports, and documentation references.

**Architecture:** Use `git mv` to preserve file history. Keep cross-exchange scripts in `scripts/market_data/` and `scripts/instruments/`, but update their import path setup to point at the new Gate / Binance market data directories.

**Tech Stack:** Python script path refactor, `unittest`, Markdown docs, `rg`, `git mv`.

---

## Task 1: Move Scripts And Tests

**Files:**

- Move Gate scripts into `market_data/`, `account/`, `trading/`, `diagnostics/`.
- Move Binance metadata script into `scripts/binance/market_data/`.
- Move tests into matching `scripts/test/...` directories.

Steps:

- [ ] Create target directories with `mkdir -p`.
- [ ] Use `git mv` for every mapped file.
- [ ] Remove untracked `__pycache__` directories with `rm -rf`.

## Task 2: Fix Python Imports

**Files:**

- `scripts/gate/trading/*.py`
- `scripts/gate/diagnostics/discover_gate_ws_ips.py`
- `scripts/lead_lag/run_live_with_guard.py`
- `scripts/instruments/generate_common_usdt_perp_catalog.py`
- moved tests under `scripts/test/gate/**` and `scripts/test/binance/**`

Steps:

- [ ] Add local `sys.path` setup for moved Gate trading scripts so `account` and sibling trading modules resolve when executed directly.
- [ ] Add local `sys.path` setup for `discover_gate_ws_ips.py` so it can import `probe_gate_ws_connect_ip.py`.
- [ ] Update `run_live_with_guard.py` to import Gate trading/account modules from new directories.
- [ ] Update `generate_common_usdt_perp_catalog.py` to import metadata mappers from `scripts/gate/market_data` and `scripts/binance/market_data`.
- [ ] Update all moved test `SCRIPT_DIR` calculations to point at new source directories.

## Task 3: Update Documentation Paths

**Files:**

- `README.md`
- `docs/futures_contract_metadata_fields.md`
- `docs/data_session_config.md`
- `docs/data_reader_config.md`
- `docs/project_onboarding_guide.md`
- `docs/gate_order_session_rtt_probe_design.md`
- `docs/lead_lag_ack_latency_outlier_analysis.md`
- `docs/lead_lag_live_runtime_plan.md`
- `docs/lead_lag_reconcile_design.md`
- `docs/agent-handoff-binance-market-data.md`
- `docs/tui_onboarding_guide.md`
- `docs/diagnostic_fields.md`
- existing `docs/superpowers/specs/*` and `docs/superpowers/plans/*` references

Steps:

- [ ] Replace old script paths with new paths.
- [ ] Keep paths to cross-exchange scripts unchanged.
- [ ] Run `rg 'scripts/(gate|binance)/[^/]+\\.py|scripts/test/(gate|binance)/[^/]+\\.py' docs README.md scripts` and review remaining root-level Gate/Binance script paths.

## Task 4: Verification

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/market_data/query_futures_contracts_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/binance/market_data/query_um_futures_contracts_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/account/query_gate_account_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/reconcile_futures_orders_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/run_futures_order_smoke_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/diagnostics/analyze_order_session_rtt_pcap_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/diagnostics/discover_gate_ws_ips_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/diagnostics/probe_gate_ws_connect_ip_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/instruments/generate_common_usdt_perp_catalog_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/query_common_usdt_perp_klines_test.py
git diff --check
```

Expected: all tests pass, no whitespace errors.

## Task 5: Commit

Run:

```bash
git status --short --branch
git add .
git commit -m "Reorganize exchange scripts by role"
```

Expected: commit contains file moves, import fixes, and documentation path updates.
