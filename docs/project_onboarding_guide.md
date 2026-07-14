# aquila 新对话导读

本文是接手入口，只保留当前事实、领域索引、代码入口、验证命令和可执行下一步。完整 contract、runbook 和证据位于专题文档；
当前 branch/ahead/dirty 只信 `git status --short --branch`。

## 启动顺序

在 `/home/liuxiang/dev/aquila` 执行：

```bash
git status --short --branch
git log --oneline -8
```

然后读取 `AGENTS.md`、`README.md`、本文和 `docs/evaluation_support.md`。进入设计/架构/实现计划或关键交易链路取舍前，
按 `AGENTS.md` 询问是否启用 Grill Me；订单、行情、风控、恢复、并发和低延迟主路径建议 `grill-me-enhanced`。

## 领域索引

| 方向 | 事实源 |
| --- | --- |
| WebSocket | `docs/websocket_client.md` |
| DataReader / data session / SHM | `docs/data_reader_config.md`、`docs/data_session_config.md`、`docs/data_session_shm_communication_design.md` |
| Fusion | `docs/market_data_fusion.md`、`docs/gate_fastest_route_fusion_shadow_results.md`、`docs/fusion_tardis_bbo_comparison.md` |
| Binance market data | `docs/agent-handoff-binance-market-data.md` |
| Bitget market data | `docs/agent-handoff-bitget-market-data.md` |
| Bitget trading | `docs/bitget_trading.md` |
| Gate trading / gateway | `docs/gate_trading.md`、`docs/gate_order_session_rtt_probe_design.md` |
| Trading component model | `docs/strategy_order_component_model.md` |
| LeadLag strategy | `strategy/lead_lag/README.md` |
| LeadLag live / report | `docs/lead_lag_live_operations.md`、`docs/lead_lag_live_report_csv_schema.md` |
| LeadLag recovery | `docs/lead_lag_reconcile_design.md` |
| LeadLag latency | `docs/lead_lag_latency_analysis.md`、`docs/diagnostic_fields.md` |
| LeadLag replay / evidence | `docs/lead_lag_live_replay_testing.md`、`docs/lead_lag_ordi_tardis_hdf_signal_pnl_comparison.md` |
| Fillability | `docs/exchange_matching_fillability_notes.md` |
| FixedOrderedSlotPool / multi-group | `docs/lead_lag_fixed_ordered_slot_pool_parallel.md` |
| Gate OBU / OrderBook | `docs/gate_obu_order_book_notes.md` |
| Instrument catalog | `docs/futures_contract_metadata_fields.md` |
| Runtime CPU / IRQ | `docs/runtime_cpu_allocation.md` |
| TUI | `docs/tui.md` |

## 当前事实

- 项目使用 C++20/CMake/vcpkg。临时 log、scratch config、live snapshot、benchmark 和 build temp 默认在
  `/home/liuxiang/tmp`。
- Gate/Binance/Bitget `BookTicker` 与 `Trade` 可发布到同一个 typed SHM object。`BookTicker` payload 72 bytes，
  `Trade` 64 bytes；historical/recorder 只接受 typed binary format v1，旧 raw/ABI artifact 需重录。
- Fastest-route fusion 按 `(exchange,symbol_id,id)` 单调 first-processed-wins，输出一条 canonical stream；行情证据不能外推为
  fillability/PnL。当前架构见 fusion 文档。
- Gate 单路 trading、private feedback、OrderGateway SHM V2 和 LeadLag gateway backend 已实现；多路 gateway 尚无真实订单证据。
  Ack/direct response 不是 terminal，unknown/continuity 进入 reconcile。
- Bitget `OrderSession`、`OrderFeedbackSession`、RTT probe、OrderGateway 与 LeadLag lag metadata 已实现。HA/高速 endpoint probe
  已有 passive IOC Ack+terminal+REST flat 双证据；gateway/LeadLag 未发真实订单。
- Bitget V1 已选择 strict stop-and-flat，不修改跨进程 `local_order_id/clientOid`：不恢复交易、不允许 strategy-only restart；
  每轮使用 run-specific gateway/feedback SHM 与 manifest v2，绑定 PID/start-time/config/account；strategy 退出后先停止 gateway/feedback，
  再通过完整分页的 REST 双订单 snapshot、范围撤单和 reduce-only 平仓证明 flat。Helper/guard/isolation 已有自动测试，尚无 Bitget
  gateway 或 LeadLag live 证据；`BTCUSDT` read-only baseline、emergency dry-run、flat-account helper 和修复后的 tiny-position
  stop-and-flat 已有 2026-07-14 当次 live 证据。首次 tiny-position 尝试暴露的 cancel code `25204` 跳过 close 问题已安全收口、
  修复并通过重跑，细节见 Bitget trading 文档。
- LeadLag live 统一使用 guarded runbook；`ContinuityLost/UnknownResult` 后终止本轮并 stop-and-flat，不在同一轮恢复开仓。Report CSV contract、reconcile 和 latency
  分别有独立专题文档。
- Instrument catalog 当前入口：小型 `config/instruments/usdt_futures.csv`，大 universe
  `config/instruments/usdt_future_universe.csv`。旧 catalog 文件名不应用于新 run。
- Gate OBU/OrderBook 只完成讨论和 quick probe；尚未实现 decoder/local book/depth typed channel。若主工作树
  `core/market_data/types.h` dirty，先读 diff；用户 `Orderbook` 草案不是已完成 ABI。
- `FixedOrderedSlotPool` 已提供通用容器，但生产 LeadLag multi-group metadata 迁移仍按专题文档和独立分支事实确认，不能假设完成。
- 当前机器默认 `0-15` live reserved、`16-31` test/diagnostics/benchmark；kernel isolation/IRQ 调优仍是候选方案。

## 代码入口

| 模块 | 入口 |
| --- | --- |
| WebSocket | `core/websocket/` |
| Trading core | `core/trading/order_types.h`、`order_manager.h`、`trading_runtime.h`、`order_feedback_shm.h`、`order_gateway_*` |
| Gate trading | `exchange/gate/trading/`、`tools/gate/gate_order_gateway.cpp` |
| Bitget trading | `exchange/bitget/trading/`、`tools/bitget/`、`scripts/bitget/trading/`、`scripts/lead_lag/{prepare_bitget_live_run,run_live_with_guard}.py` |
| Market data | `core/market_data/`、`exchange/{gate,binance,bitget}/market_data/` |
| Fusion | `core/market_data/fusion/`、`tools/{gate,binance,bitget}/*_data_fusion.cpp` |
| DataReader/recorder | `core/market_data/*data_reader.h`、`tools/market_data/data_reader_*` |
| LeadLag | `strategy/lead_lag/`、`tools/lead_lag/`、`scripts/lead_lag/` |
| TUI | `monitor/` |

## 常用验证

```bash
./build.sh debug
ctest --test-dir build/debug --output-on-failure
git diff --check
```

Focused：

```bash
ctest --test-dir build/debug -R '(gate_order|order_gateway|order_feedback|trading_runtime|lead_lag)' --output-on-failure
ctest --test-dir build/debug -R '^bitget_(order|operation)' --output-on-failure
ctest --test-dir build/debug -R '(market_data|data_session|data_reader|fusion)' --output-on-failure
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/guard_exchange_adapter_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

Evaluation 边界修改后：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望无命中。

## 下一步建议

1. Bitget trading：下一证据门是 fanout=1 guarded gateway passive IOC，完成 Ack、terminal feedback 和 REST final flat 对账后，
   最后才是 signal-conditioned LeadLag。每轮必须 fresh run；
   多 route、account limiter、failover、fast-fill 和 resume/persistent ID 仍后置。
2. Gate trading：下一步是 guarded gateway smoke，量化 route skew、Ack RTT、terminal feedback 与 fillability；先复核 account budget、
   reconcile 和 liveness。
3. LeadLag live：任何真实 run 按 `docs/lead_lag_live_operations.md`，使用新鲜 release/config、freshness/slippage preflight、
   REST baseline/final flat 和隔离 run dir。
4. Fillability：普通 BTC touch probe 的 99% 不能外推到 signal-conditioned LeadLag；按 fillability 文档的 row/group、BBO stage 和
   lifecycle 口径复查。
5. Gate OBU：实现前先批准 published `OrderBook` ABI，再以 decoder/local-book TDD 覆盖 group count、empty/delete、gap/resubscribe。
6. 性能/CPU：先按 latency/CPU 文档记录环境并做 A/B；无 benchmark/profile/live 证据不宣称收益。

## 给下一个对话的提示

先运行 `git status --short --branch` 和 `git log --oneline -8`，再按 onboarding 顺序读取入口文档；branch/ahead/dirty 只信
`git status`。派发 subagent 必须按 `AGENTS.md` 选择项目级 `aquila_xhigh_worker`。设计、计划或关键交易链路取舍先询问 Grill Me。

Bitget 下一步先读 `docs/bitget_trading.md`：V1 已采用 strict stop-and-flat + fresh-run isolation，代码和自动测试已完成，
manifest v2、REST 保守 snapshot 和进程 quiescence 也已落地；`BTCUSDT` baseline、emergency dry-run 和 flat-account helper 已有
2026-07-14 live 证据；首次 tiny-position 尝试已安全收口并修复 code `25204` 边界，修复后的重跑已取得自动 close、terminal
order-info 和 final flat 证据。下一门是 fanout=1 gateway passive IOC，尚无 gateway/LeadLag live 证据。任何真实订单必须按
runbook 的分阶段证据门取得当次授权；不要把 fresh-run 解释为可 resume，也不要在同一 run 重启 strategy。
Gate、LeadLag、fusion、TUI 和 OBU 等方向按上方领域索引进入，不从已删除的完成态 plan/spec 接手。
