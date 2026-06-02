# Scripts 目录重组设计

## 目标

整理 `scripts/` 下 Gate / Binance 相关脚本目录，让 market data、account、trading 和 diagnostics 职责更清晰，并同步更新测试路径和文档命令。

## 目录规则

Gate 脚本按职责拆分：

```text
scripts/gate/market_data/
scripts/gate/account/
scripts/gate/trading/
scripts/gate/diagnostics/
```

Binance 脚本按职责拆分：

```text
scripts/binance/market_data/
```

测试目录镜像生产脚本目录：

```text
scripts/test/gate/market_data/
scripts/test/gate/account/
scripts/test/gate/trading/
scripts/test/gate/diagnostics/
scripts/test/binance/market_data/
```

跨交易所或通用脚本暂不移动：

```text
scripts/market_data/
scripts/instruments/
scripts/lead_lag/
scripts/tardis/
```

## 移动映射

Gate market data：

- `scripts/gate/query_futures_contracts.py` -> `scripts/gate/market_data/query_futures_contracts.py`

Gate account：

- `scripts/gate/query_gate_account.py` -> `scripts/gate/account/query_gate_account.py`

Gate trading：

- `scripts/gate/place_futures_order.py` -> `scripts/gate/trading/place_futures_order.py`
- `scripts/gate/emergency_flatten_futures.py` -> `scripts/gate/trading/emergency_flatten_futures.py`
- `scripts/gate/reconcile_futures_orders.py` -> `scripts/gate/trading/reconcile_futures_orders.py`
- `scripts/gate/run_futures_order_smoke.py` -> `scripts/gate/trading/run_futures_order_smoke.py`

Gate diagnostics：

- `scripts/gate/analyze_order_session_rtt_pcap.py` -> `scripts/gate/diagnostics/analyze_order_session_rtt_pcap.py`
- `scripts/gate/discover_gate_ws_ips.py` -> `scripts/gate/diagnostics/discover_gate_ws_ips.py`
- `scripts/gate/probe_gate_ws_connect_ip.py` -> `scripts/gate/diagnostics/probe_gate_ws_connect_ip.py`

Binance market data：

- `scripts/binance/query_um_futures_contracts.py` -> `scripts/binance/market_data/query_um_futures_contracts.py`

## 文档更新

同步更新 `README.md`、专题文档、superpowers spec / plan 中的旧路径。`scripts/market_data/query_common_usdt_perp_klines.py` 和 `scripts/instruments/generate_common_usdt_perp_catalog.py` 会继续作为跨交易所工具保留原位置，但其 import 路径要指向新的 Gate / Binance market data 目录。

## 验证

运行移动影响到的 Python 测试：

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
