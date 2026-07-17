# aquila 新对话导读

本文是接手入口，只保留当前事实、领域索引、代码入口、验证命令和可执行下一步。完整 contract、runbook 和证据位于专题文档；
当前 branch/ahead/dirty 只信 `git status --short --branch`。

## 启动顺序

在 `/home/liuxiang/dev/aquila` 执行：

```bash
git status --short --branch
git log --oneline -8
```

然后读取 `AGENTS.md`、`README.md`、本文和 `docs/evaluation_support.md`。涉及编码或技术开发时，先使用
`docs/skills/adaptive-development/SKILL.md` 判断并输出 `Level: L0-L3`，再直接执行对应质量门；本仓库默认不使用
Superpowers 工作流。进入设计/架构/实现计划或关键交易链路取舍前，按 `AGENTS.md` 询问是否启用 Grill Me；订单、行情、
风控、恢复、并发和低延迟主路径建议 `grill-me-enhanced`。

## 领域索引

| 方向 | 事实源 |
| --- | --- |
| 开发工作流 | `AGENTS.md`、`docs/skills/adaptive-development/SKILL.md` |
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
- 开发流程统一使用 `adaptive-development` 的 `L0-L3` 分级：`L1` 无计划，`L2` 只写会话内简短计划，`L3` 必须使用
  专用 branch/worktree、markdown 计划、完整风险验证和 PR。aquila 的协议、订单状态、风控、恢复、并发、实盘安全和性能关键路径
  默认升级为 `L3`；详细规则只在 `AGENTS.md` 和 skill 事实源维护。
- Gate/Binance/Bitget `BookTicker` 与 `Trade` 可发布到同一个 typed SHM object。`BookTicker` payload 72 bytes，
  `Trade` 64 bytes；historical/recorder 只接受 typed binary format v1，旧 raw/ABI artifact 需重录。
- Fastest-route fusion 按 `(exchange,symbol_id,id)` 单调 first-processed-wins，输出一条 canonical stream；行情证据不能外推为
  fillability/PnL。当前架构见 fusion 文档。
- Gate 单路 trading、private feedback、OrderGateway SHM V2 和 LeadLag gateway backend 已实现；多路 gateway 尚无真实订单证据。
  Ack/direct response 不是 terminal，unknown/continuity 进入 reconcile。
- Bitget `OrderSession`、`OrderFeedbackSession`、RTT probe、OrderGateway 与 LeadLag lag metadata 已实现。HA/高速 endpoint probe
  已有 passive IOC Ack+terminal+REST flat 双证据；fanout=1 gateway smoke 也已有 Ack+terminal+quiescence+REST flat 证据。
  `bitget_lead_lag_top20_highspeed_20260715T154837Z` 已完成 20-symbol、fanout=1、10 小时真实订单运行：644 个 signal、
  211 个 submitted order、21 个 closed position，quiescence/final flat 通过；实际净 PnL `-0.03536520 USDT`。证据和
  边界见对应 report 与 `docs/bitget_trading.md`。
- Bitget V1 已选择 strict stop-and-flat，不修改跨进程 `local_order_id/clientOid`：不恢复交易、不允许 strategy-only restart；
  LeadLag 每轮使用 manifest v2，gateway smoke 使用专用 manifest v1；两者都使用 run-specific SHM 并绑定 PID/start-time/config/account，
  gateway smoke 额外绑定 data session、拒绝已存在的 run directory，并校验 runner CSV/summary。交易 runner 退出后先停止所有绑定
  producer，再通过完整分页的 REST 双订单 snapshot、范围撤单和 reduce-only 平仓证明 flat。Helper/guard/isolation 已有自动测试；
  `BTCUSDT` read-only baseline、emergency dry-run、flat-account helper、修复后的 tiny-position stop-and-flat 和 fanout=1 gateway
  passive IOC 均已有 2026-07-14 当次 live 证据。LeadLag manifest 现只放行 route count `1/4`；四路要求所有 Bitget pair
  `order_session_fanout=4`，并逐 route 固化/复核 OrderSession contract。20-symbol 四路策略配置入口为
  `config/strategies/lead_lag_bitget_top20_highspeed_fanout4_20260716.toml`：每个 pair 使用 lead/lag freshness `3ms/500ms`、
  `open_notional=10`，entry 计算量低于
  instrument `min_quantity` 时直接使用最小量，高于最小量时保留计算结果。Gateway 配置入口为
  `config/order_gateways/bitget_order_gateway_4routes.toml`；当前只有代码、自动测试和 CLI validate-only 证据，尚无四路 live 证据。
- 用户指定的 Binance-lead/Bitget-lag 30-symbol 准备配置入口为
  `config/strategies/lead_lag_bitget_requested_top30_highspeed_fanout4_20260716.toml`。2026-07-16 官方快照中 30/30 双边均存在；
  11 个原先缺失的 symbol 已补入大 universe catalog。`SKHY/SNDK/SKHYNIX/SOXL/MU/KORU/SAMSUNG/DRAM/MRVL/EWY`
  是 Binance `TRADIFI_PERPETUAL`，真实启动前必须重新确认交易时段和双边 BBO；当前只有 catalog/config、只读行情连接和
  manifest v2 prepare/validate-only 证据，没有该 30-symbol 组合的真实订单证据。
- 30-symbol fanout=1 14 小时 BBO recorder live run
  `bitget_lead_lag_requested_top30_fanout1_14h_bbo_20260716T113602Z` 仍在运行；2026-07-16 15:20:36Z 的只读快照显示 7/7
  绑定进程存活、293 个 signal、115 个 submitted/Ack/terminal、7 个完整 cycle、14 fills、0 unresolved，REST allowlist
  snapshot 为 flat，recorder 仍在增长。Strategy 预计 2026-07-17 01:43:50Z 自然结束，随后 gateway/feedback 和 guard 继续完成
  quiescence 与 stop-and-flat；该时间点后的最终状态必须重新读取进程、guard summary 和 REST，不能从本快照推断。
- LeadLag live 统一使用 guarded runbook；`ContinuityLost/UnknownResult` 后终止本轮并 stop-and-flat，不在同一轮恢复开仓。Report CSV contract、reconcile 和 latency
  分别有独立专题文档。
- Instrument catalog 的新运行统一入口是 `config/instruments/usdt_future_universe.csv`。旧小型 catalog
  已删除；新 run 必须统一使用该文件或 run-specific 冻结副本，不能混用不同 `symbol_id`/metadata。
  带日期的旧 catalog 只保留给对应历史配置复现，历史 run 仍只使用其归档 catalog。
- Gate OBU/OrderBook 只完成讨论、quick probe，以及未被 producer/consumer 使用的 `Orderbook<Level>` 类型草案；尚未实现
  decoder/local book/depth typed channel，该草案也不是已批准的 published ABI 或 persistent format。继续实现前仍需决定命名、
  count 类型、`symbol_id`/`exchange` 和存储布局。
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
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/prepare_gateway_smoke_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/run_gateway_smoke_with_guard_test.py
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

1. Bitget trading：fanout=1 gateway 与 signal-conditioned LeadLag 已有 live 证据；下一门是按当次授权执行 fanout=4 staged
   LeadLag，先验证四 route ready、每 child 最小量、Ack/terminal 归组、reduce-only 收敛、quiescence 和 final flat。每轮必须
   fresh run；account limiter、failover、fast-fill 和 resume/persistent ID 仍未完成。
2. Gate trading：下一步是 guarded gateway smoke，量化 route skew、Ack RTT、terminal feedback 与 fillability；先复核 account budget、
   reconcile 和 liveness。
3. LeadLag live：任何真实 run 按 `docs/lead_lag_live_operations.md`，使用新鲜 release/config、freshness/slippage preflight、
   REST baseline/final flat 和隔离 run dir。若使用 30-symbol 准备配置，还要先排除 `TRADIFI_PERPETUAL` 非交易时段或停牌造成的
   单边 stale BBO。
4. Fillability：普通 BTC touch probe 的 99% 不能外推到 signal-conditioned LeadLag；按 fillability 文档的 row/group、BBO stage 和
   lifecycle 口径复查。
5. Gate OBU：实现前先批准 published `OrderBook` ABI，再以 decoder/local-book TDD 覆盖 group count、empty/delete、gap/resubscribe。
6. 性能/CPU：先按 latency/CPU 文档记录环境并做 A/B；无 benchmark/profile/live 证据不宣称收益。

## 给下一个对话的提示

先运行 `git status --short --branch` 和 `git log --oneline -8`，再按 onboarding 顺序读取入口文档；branch/ahead/dirty 只信
`git status`。涉及编码或技术开发先读取并执行 `docs/skills/adaptive-development/SKILL.md`，输出 Level 后直接推进；默认不要启用
Superpowers。设计、计划或关键交易链路取舍先询问 Grill Me。

Bitget 下一步先读 `docs/bitget_trading.md`：V1 已采用 strict stop-and-flat + fresh-run isolation，代码和自动测试已完成，
LeadLag manifest v2、gateway smoke manifest v1、REST 保守 snapshot、runner evidence 校验和进程 quiescence 均已落地；
`BTCUSDT` baseline、emergency dry-run、flat-account helper、
修复后的 tiny-position stop-and-flat 和 fanout=1 gateway passive IOC 均已有 2026-07-14 live 证据。Gateway run
`bitget_gateway_smoke_20260714T061702Z` 取得 direct Ack、独立 cancelled terminal、三进程 quiescence 和 final flat，成交为 0；
`bitget_lead_lag_top20_highspeed_20260715T154837Z` 又取得 20-symbol、fanout=1、10 小时 signal-conditioned LeadLag
证据，21 个成交 entry 均完整平仓，quiescence/final flat 通过。四路 gateway/fanout contract 和 entry quantity clamp 已完成代码、测试和
validate-only，配置入口为 `config/order_gateways/bitget_order_gateway_4routes.toml`，但尚无四路 live 证据。下一门是
fanout=4 staged LeadLag。用户指定的 30-symbol 组合已完成双边合约核对、catalog 更新和配置准备，入口为
`config/strategies/lead_lag_bitget_requested_top30_highspeed_fanout4_20260716.toml`；其中 10 个 Binance
`TRADIFI_PERPETUAL` 在真实启动前必须复核交易时段和双边 BBO，当前没有该组合的真实订单证据。任何真实订单必须按 runbook
的分阶段证据门取得当次授权；不要把 fresh-run 解释为可 resume，
也不要在同一 run 重启 strategy。当前 30-symbol fanout=1 14 小时 run
`bitget_lead_lag_requested_top30_fanout1_14h_bbo_20260716T113602Z` 预计 2026-07-17 01:43:50Z 后进入最终收敛；新对话应先恢复
15 分钟只读监控，最终只信绑定进程 quiescence、guard summary 和新鲜 REST flat。Bitget 后续时间戳证据 TODO 已写入
`docs/bitget_trading.md`：保存 place `cTime`/`ts`、order push lifecycle 时间和 fill `execTime`，但零成交 IOC 仍没有文档明确的
order-ingress/match-attempt 时间戳。
Gate、LeadLag、fusion、TUI 和 OBU 等方向按上方领域索引进入，不从已删除的完成态 plan/spec 接手。
