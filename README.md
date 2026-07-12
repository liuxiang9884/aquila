# aquila

`aquila` 是面向 crypto 高频交易的 C++20/CMake 仓库，覆盖交易所行情与交易接入、typed SHM、DataReader、
fastest-route fusion、订单状态与反馈、LeadLag 策略、benchmark 和只读监控。默认同时关注正确性、确定性、最低延迟、
尾延迟、可恢复性和可观测性。

## Onboarding

新对话或新接手先执行：

```bash
git status --short --branch
git log --oneline -8
```

然后按顺序读取：

```text
AGENTS.md
README.md
docs/project_onboarding_guide.md
docs/evaluation_support.md
```

`docs/project_onboarding_guide.md` 是当前状态、领域事实源、代码入口、验证命令和下一步的集中索引。
当前 branch/ahead/dirty 只信 `git status`。

## 构建

本机默认使用 `$HOME/vcpkg`，toolchain 位于：

```text
$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake
```

当前主要依赖包括 fmt、quill、tomlplusplus、CLI11、GTest、magic-enum、yyjson、simdjson、fast-float、benchmark、
Abseil、FTXUI、OpenSSL 和 Drogon。新增依赖前优先沿用现有 vcpkg/CMake 结构。

```bash
./build.sh debug
./build.sh release
./build.sh --jobs 16 release
```

Agent 创建的 build/temp/log/scratch/benchmark 输出默认放在 `/home/liuxiang/tmp`；需要 compiler/tool 临时目录时设置：

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh debug
```

## 测试与验证

```bash
ctest --test-dir build/debug --output-on-failure
git diff --check
```

Focused：

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
ctest --test-dir build/debug -R '(gate_order|order_gateway|order_feedback|trading_runtime)' --output-on-failure
ctest --test-dir build/debug -R '^bitget_(order|operation)' --output-on-failure
ctest --test-dir build/debug -R '(market_data|data_session|data_reader|fusion)' --output-on-failure
ctest --test-dir build/debug -R lead_lag --output-on-failure
ctest --test-dir build/debug -R monitor_ --output-on-failure
```

Evaluation 边界检查：

```bash
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

期望无命中。

## 文档地图

| 领域 | 当前事实源 |
| --- | --- |
| WebSocket | `docs/websocket_client.md` |
| Data session / SHM / reader | `docs/data_session_config.md`、`docs/data_session_shm_communication_design.md`、`docs/data_reader_config.md` |
| Fusion | `docs/market_data_fusion.md` |
| Binance market data | `docs/agent-handoff-binance-market-data.md` |
| Bitget market data | `docs/agent-handoff-bitget-market-data.md` |
| Bitget trading | `docs/bitget_trading.md` |
| Gate trading | `docs/gate_trading.md` |
| Trading component model | `docs/strategy_order_component_model.md` |
| LeadLag strategy | `strategy/lead_lag/README.md` |
| LeadLag live/report | `docs/lead_lag_live_operations.md` |
| LeadLag recovery | `docs/lead_lag_reconcile_design.md` |
| LeadLag latency | `docs/lead_lag_latency_analysis.md` |
| Fillability | `docs/exchange_matching_fillability_notes.md` |
| Diagnostic/CSV fields | `docs/diagnostic_fields.md`、`docs/lead_lag_live_report_csv_schema.md` |
| CPU/IRQ | `docs/runtime_cpu_allocation.md` |
| TUI | `docs/tui.md` |

## 主要运行入口

| 方向 | Binary/script |
| --- | --- |
| Gate/Binance/Bitget data session | `tools/*_data_session` |
| Gate/Binance/Bitget fusion | `tools/*_data_fusion` |
| DataReader probe/recorder | `tools/data_reader_probe`、`tools/data_reader_recorder` |
| Gate/Bitget order session probe | `tools/*_order_session_probe`、`tools/*_order_session_rtt_probe` |
| Gate/Bitget gateway | `tools/gate_order_gateway`、`tools/bitget_order_gateway` |
| LeadLag | `tools/lead_lag_replay`、`tools/lead_lag_strategy` |
| TUI | `monitor/gate_account_tui` |
| REST read-only | `scripts/gate/account/`、`scripts/bitget/account/` |

具体 CLI、配置、协议和安全边界以对应领域文档为准。README 不复制交易所操作 runbook。

## Live 安全默认值

- 所有真实订单必须由用户对当次 run 明确授权；dry-run、login、subscription、validate-only 不构成下单授权。
- 启动前做当次 REST account/open-order/position baseline，结束后证明 flat；历史 flat、余额、白名单或 endpoint 状态不能复用。
- Direct operation/Ack 不等于 accepted、filled 或 terminal；订单生命周期只信 private feedback 与 reconcile。
- `UnknownResult`/`ContinuityLost` 后暂停新开仓，不伪造 reject/cancel，不自动重发。
- 真实 LeadLag 统一按 `docs/lead_lag_live_operations.md`；交易所专属 P0/P1 阻断见 `docs/gate_trading.md`、
  `docs/bitget_trading.md`。
- `0-15` 是 live reserved CPU，`16-31` 是 test/diagnostic/benchmark CPU；实际规则见 CPU 文档。

性能、吞吐、稳定性、fillability 和 PnL 结论必须绑定具体 command/config/CPU/run/benchmark/profile 证据。
