# LeadLag 实盘操作与 Report Runbook

本文是 LeadLag 真实订单启动、监控、停止、flat 检查和 report 生成的唯一操作入口。恢复语义见
`docs/lead_lag_reconcile_design.md`，字段 contract 见 `docs/lead_lag_live_report_csv_schema.md`。每次真实订单都需要用户对当次运行明确授权。

## 当前安全边界

- `tools/lead_lag/live_strategy.cpp` 默认 validate/signal-only；只有 `strategy.mode=live`、realtime data、feedback SHM、凭据和
  显式 `--execute` 同时满足时才进入 live-orders。
- 推荐外层入口是 `scripts/lead_lag/run_live_with_guard.py`，负责 REST preflight、runner 监控、final check 和异常 stop-and-flat。
- `kContinuityLost`、Gate/Bitget `UnknownResult`、global feedback loss 或 unresolved order 会终止本轮交易并进入 handoff；
  V1 不在同一轮恢复开仓。
- 真实账户事实最终由 REST open-orders/positions/final flat 证明；本地 position 不能替代账户检查。
- 默认只允许小额 guarded smoke。无人值守长跑、扩大 symbols/frequency/fanout 前必须重新检查当前安全边界和证据门。
- 临时配置、日志、snapshot 和输出写入 `/home/liuxiang/tmp/<run_id>/`；不把 scratch 文件写入仓库或 `/tmp`。
- Runtime CPU 遵守 `docs/runtime_cpu_allocation.md`：`0-15` 实盘，`16-31` 测试/diagnostics/benchmark。

## 启动 Pipeline

### 1. 确认事实源与本次授权

```bash
git status --short --branch
git log --oneline -8
```

复核本文、`docs/project_onboarding_guide.md`、`docs/lead_lag_reconcile_design.md` 和对应交易所 trading 文档。记录 duration、
symbols、backend、fanout、strategy config、instrument catalog、endpoint/TLS、commit 和 run id。用户未给 duration 或真实订单范围时先确认，
不能自行扩成长跑。

Run id 使用 `YYYYMMDD_HHMMSS_<label>`，创建隔离目录：

```text
/home/liuxiang/tmp/<run_id>/
  configs/
  logs/
  bin/
```

### 2. 使用新鲜 binary 与配置

代码或交易配置刚修改时，先完成对应 build/test/commit。Fusion live 使用 release binary、
`AQUILA_DATA_SESSION_DIAG_LEVEL=0` 和显式 metadata mode。只修改文档/report 不需要重编译。

Strategy config 必须是 live-orders 配置，不得把 signal-only config 与 `--execute` 混用。Instrument catalog 使用当前
`config/instruments/usdt_future_universe.csv`；准备 live run 时把该文件冻结到 run directory。历史 catalog 文件名不能作为
新 run 入口，也不能让 producer、consumer、strategy、gateway 或 report 混用不同 catalog。

### 3. 生成并核对 affinity overlay

推荐 profile：`config/runtime_affinity/lead_lag_requested_12symbols_node0.toml`。生成临时 TOML 不连接交易所、不读账户：

```bash
scripts/lead_lag/run_live_with_guard.py \
  --run-id <run_id> \
  --settle usdt \
  --contract <SYMBOL> \
  --affinity-profile config/runtime_affinity/lead_lag_requested_12symbols_node0.toml \
  --affinity-output-dir /home/liuxiang/tmp/<run_id>/configs \
  --affinity-gate-market-config config/data_sessions/gate_data_session_requested_20260521.toml \
  --affinity-binance-market-config config/data_sessions/binance_data_session_requested_20260521.toml \
  --affinity-order-feedback-config config/order_feedback/gate_order_feedback_session.toml \
  --prepare-affinity-only \
  --no-pretty \
  -- ./build/release/tools/lead_lag_strategy --config <strategy_config>
```

只有外部 data/feedback process 确实使用这些 overlay 时才传 `--affinity-external-configs-applied`，否则 report 必须记录
`affinity_split=false`。Fusion launch config 不会由单路 overlay 自动改写，实际 CPU 以 TOML、`ps` 和 log 为准。

### 4. 行情 preflight

确认 Gate/Binance/Bitget data process 与 canonical SHM 正常、symbol/feed 匹配、freshness 在阈值内。使用 fusion 时同时检查 N 路 source、
fusion process、canonical SHM、metadata 增长和 recorder `skipped/overruns`；strategy 只能指向 canonical channel。

长期 recorder 不跟随每次 strategy 重启。用户要求完全隔离时，所有 data/feedback/gateway SHM 和 log 使用本 run 的唯一名称，
不能复用正在服务其他策略的对象。

### 5. Feedback 与 gateway preflight

Feedback session 必须先 login/subscribed ready。其 duration 至少为 strategy duration + 300s。只有确认无 live consumer 时才允许
`remove_existing=true`；否则使用新 SHM，避免旧 `ContinuityLost` 让 strategy 启动后立即停止。

使用 order gateway 时先 validate config/SHM、route count/readiness 和 account consistency。Bitget 历史首个 live 使用
`fanout=1`；四路 LeadLag 使用 `config/order_gateways/bitget_order_gateway_4routes.toml`，并要求每个 Bitget pair
`order_session_fanout=4`。20-symbol 策略配置使用
`config/strategies/lead_lag_bitget_top20_highspeed_fanout4_20260716.toml`，每个 pair 使用 lead/lag freshness `3ms/500ms`、
`open_notional=10`；entry 计算量
低于 instrument `min_quantity` 时直接使用最小量，高于最小量时保留计算结果。不允许 strategy-only restart，也不允许复用旧
gateway/feedback SHM。Bitget V1 以 strict stop-and-flat + fresh-run isolation 替代跨进程唯一 ID 后继续恢复交易；
persistent ID 只在未来需要 resume/overlap 时重新成为前置条件。

Bitget 每轮先生成 config 与 manifest；该命令会把交易关键的嵌套 config 引用固化为绝对路径，但不联网、不读取账户、不创建 SHM：

```bash
scripts/lead_lag/prepare_bitget_live_run.py prepare \
  --run-id <run_id> \
  --strategy-config <bitget_strategy_source> \
  --gateway-config config/order_gateways/bitget_order_gateway_4routes.toml \
  --feedback-config config/order_feedback/bitget_order_feedback_session.toml
```

随后用生成的 `bitget_feedback__*.toml` 启动 `bitget_order_feedback_session --connect`，用
`bitget_gateway__*.toml` 启动 `bitget_order_gateway --connect`。确认 feedback subscribed ready、manifest route count 与
gateway 一致；四路 run 必须看到 route `0..3` 全部 ready，再执行：

```bash
scripts/lead_lag/prepare_bitget_live_run.py mark-applied \
  --runtime-manifest /home/liuxiang/tmp/<run_id>/configs/bitget_live_manifest.json \
  --gateway-pid <gateway_pid> \
  --feedback-pid <feedback_pid>
```

`mark-applied` 会验证两个 PID 当前存活且分别是预期 gateway/feedback binary，argv 包含 `--connect` 并以绝对路径精确使用生成配置；
manifest v2 记录 `/proc/<pid>/stat` start time 防止 PID reuse。三个 TOML 的路径、SHM、route count、逐 route 交易 contract、
credential env 和两个进程中的实际 credential 值也必须一致；四路还会复核 LeadLag fanout contract。
Credential 值不会写入 artifact。Ready 仍需按 log 单独确认。旧 run 必须先停止完整交易栈并获得 REST flat 证据，才能创建下一轮。

### 6. REST baseline 与 guard

Guard 必须确认 credentials env 名与 order session config 一致、目标账户可访问、open orders 为空、目标 symbols positions flat、
freshness preflight 和 slippage/taker-buffer preflight 通过。IP 白名单、余额和 flat 只信当次 REST 输出。

凭据从 `[order_session.credentials] api_key_env/api_secret_env` 等配置读取；Bitget 还要求 `api_passphrase_env`。
命令行传的是环境变量名，不输出 secret/passphrase。Bitget `--execute` 会在读取这些环境变量和访问 REST 前验证 runtime manifest。

### 7. 启动真实订单

示例：

```bash
setsid scripts/lead_lag/run_live_with_guard.py \
  --run-id <run_id> \
  --settle usdt \
  --contract <SYMBOL> \
  --poll-timeout-sec 30 \
  --affinity-profile config/runtime_affinity/lead_lag_requested_12symbols_node0.toml \
  --affinity-output-dir /home/liuxiang/tmp/<run_id>/configs \
  --no-pretty \
  -- ./build/release/tools/lead_lag_strategy \
       --config <strategy_config> \
       --connect-data \
       --execute \
       --duration-sec <duration_sec> \
  > /home/liuxiang/tmp/<run_id>/guarded_live.stdout 2>&1 < /dev/null &
```

如果本次确实应用外部 affinity overlay，再增加 `--affinity-external-configs-applied`。启动后立即确认 guard/strategy/feedback/gateway PID、
`lead_lag_live_orders_runtime_started`、REST preflight、实际 config、CPU、endpoint 和日志中无 `ERROR/FATAL/ContinuityLost/
needs_reconcile/manual_intervention`。

Bitget 命令还必须显式选择 exchange 和 runtime manifest，且 strategy `--config` 必须等于 manifest 中的 run-specific config：
strategy command 必须直接执行 `lead_lag_strategy`，不通过 `bash -c`、`env` 或 `taskset` wrapper 隐藏参数。
Bitget 真实 `--execute` 不允许覆盖为非生产 REST base URL。

```bash
setsid scripts/lead_lag/run_live_with_guard.py \
  --exchange bitget \
  --run-id <run_id> \
  --runtime-manifest /home/liuxiang/tmp/<run_id>/configs/bitget_live_manifest.json \
  --settle usdt \
  --contract <SYMBOL> \
  --poll-timeout-sec 30 \
  --no-pretty \
  -- ./build/release/tools/lead_lag_strategy \
       --config /home/liuxiang/tmp/<run_id>/configs/strategy__<source>.toml \
       --connect-data \
       --execute \
       --duration-sec <duration_sec>
```

该示例只定义形状，不构成真实订单授权。证据门顺序仍是 read-only baseline → emergency dry-run → 单独授权 flat-account helper →
单独授权 tiny-position stop-and-flat → 单独授权 fanout=1 gateway passive IOC → 单独授权 fanout=1 signal-conditioned LeadLag →
单独授权 fanout=4 staged LeadLag。截至 2026-07-16，fanout=1 signal-conditioned LeadLag 及其前置证据门已有对应证据，
四路只有代码、自动测试和 validate-only 证据；单路结果不能替代四路 child quantity、Ack/terminal 归组、reduce-only
收敛或 latency 证据。
详细 run id 和边界见 `docs/bitget_trading.md`。

## 运行期监控

持续超过 10 分钟的真实订单默认每 10 分钟检查：

- 所有 PID、CPU affinity、最新 log timestamp 和 canonical market-data freshness。
- Gateway 每条 route 的 ready/not-ready 状态；四路 run 中任一路失效都要单独记录，不能只看 aggregate ready count。
- `lead_lag_signal_triggered/submitted/finished` 数量。
- Filled 只按 `cumulative_filled_quantity > 0` 或 terminal filled feedback 统计。
- `ERROR`、`FATAL`、`ContinuityLost`、`UnknownResult`、`needs_reconcile`、`manual_intervention`。
- Guard/strategy/feedback/gateway stdout/log 尾部和 REST/SHM health。

异常立即报告；不能等 run 结束。收到 continuity/unknown 后不自动重启或重发。Bitget guard 在 strategy 退出后先证明
gateway/feedback 已停止，再执行对应交易所的幂等 emergency helper；REST 使用完整分页的
`open orders → positions → open orders` snapshot 证明 flat。无法停止绑定进程或无法证明 flat 时禁止启动下一轮。

## 停止、恢复与 flat

正常 duration/信号停止后继续 drain response/feedback，再由 guard 执行 final REST check。核对：

- Guard 最后一条 JSON 的 `ok/result/exit_code/quiescence/final_check.flat/open_orders/positions`；Bitget final REST 或 flatten
  之前必须有 `quiescence.ok=true`。
- Strategy summary 的 responses/feedbacks/continuity-lost/needs-reconcile/manual-intervention。
- Submitted/finished/filled/cancelled/partial 数量和所有 unresolved local/exchange order。
- Feedback session 的 continuity lost 是否发生在 strategy 运行期间；策略退出后的 session shutdown 必须明确区分。

非 flat、unknown、部分成交、safety-close 失败或 guard 异常时，进入 `docs/lead_lag_reconcile_design.md`，不得伪造 terminal 或直接恢复开仓。
Emergency cleanup 的 REST 输出、人工动作和最终 flat 都写入 run directory。

Bitget guard 正常退出且 quiescence + final flat 返回 `0`；异常 stop-and-flat 成功返回 `10` 并保持停机；进程无法停止、cleanup
失败或无法证明 flat 返回 `11`。
Outer guard 被 `SIGKILL`、主机失效、网络隔离或 REST 全不可用仍是人工 handoff 边界，首次 smoke 必须有人值守。

最终运行回复至少给出 run dir、实际命令/config/commit、PIDs、退出原因、REST flat、signal/order/fill 统计，以及 Ack RTT、
send-to-finish 和 exchange lifecycle 的分离摘要。

## Report Pipeline

触发“总结/生成/打包本次或上一次实盘 report”时：

1. 运行 `git status --short --branch`，定位明确的 run id、strategy log、guard stdout、feedback/gateway log 和实际 config。
2. “上一次”若有多个候选必须询问，不能按文件名猜对应关系。
3. Gate 需要合并日志时，在 `/home/liuxiang/tmp/<run_id>/` 生成 merged input；不改原始 log。Bitget 默认保留 strategy、gateway、feedback 分离日志，分别传给生成器。
4. 使用本 run 实际 instrument catalog、config、guard stdout 和 run definition。Bitget 优先使用 run 归档的 `inputs/usdt_future_universe.csv`，缺省时生成器使用 checked-in 的 `config/instruments/usdt_future_universe.csv`。
5. Bitget fillability 只使用已落盘且 manifest 已封口的 BookTicker segment；运行中的 `.tmp` segment 不在 manifest 内，不把缺失窗口归因于市场不可成交。
6. 如需账户实际 PnL，归档本 run 时间范围内 `/api/v3/trade/fills` 的原始 JSON，并传 `--rest-fills`。生成器只接受能关联本 run authoritative order 的 fill；未提供 REST 时实际手续费/PnL 必须标为 unavailable。
7. 若 `reports/<run_id>/` 已存在，先确认复用、换 run id 或覆盖。

生成：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/generate_live_report.py \
  --run-id <run_id> \
  --log <merged_or_strategy_log> \
  --config <strategy_config> \
  --instrument-catalog <instrument_catalog> \
  --guard-stdout <guard_stdout> \
  --output-root reports
```

Bitget 分离日志示例：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/generate_live_report.py \
  --run-id <run_id> \
  --exchange bitget \
  --log <strategy_log> \
  --additional-log <bitget_gateway_log> \
  --additional-log <bitget_feedback_log> \
  --feedback-log <bitget_feedback_log> \
  --config <strategy_config> \
  --instrument-catalog <run_dir>/inputs/usdt_future_universe.csv \
  --book-ticker-manifest <run_dir>/records/bitget_book_ticker_manifest.jsonl \
  --run-definition <run_dir>/inputs/run_definition.json \
  --rest-fills <run_scoped_bitget_fills.json> \
  --guard-stdout <guard_stdout> \
  --output-root reports
```

`--rest-fills` 可省略；省略时仍生成 fast-fill/BBO 分析，但 `report.md` 不给出“实际 PnL”。

输出：

```text
reports/<run_id>/report.md
reports/<run_id>/signal.csv
reports/<run_id>/order_detail.csv
reports/<run_id>/position.csv
reports/<run_id>/latency.csv
reports/<run_id>/execution_detail.csv       # Bitget
reports/<run_id>/order_fillability.csv      # Bitget
reports/<run_id>/run_definition.json        # 传入时
reports/<run_id>/lead_lag_live_report_csv_schema.md
reports/<run_id>/runtime_configs/  # 有可归档 overlay 时
```

最小验证：核心四个 CSV 存在且有 header；Bitget 另外验证 `execution_detail.csv`、`order_fillability.csv` 有 header；schema snapshot 与 `docs/lead_lag_live_report_csv_schema.md` 一致；runtime profile、run definition 与归档配置一致。Gate report 检查 Ack 上行、Gate `x_in->x_out`、Ack 下行和 exchange lifecycle。Bitget report 检查 signal→entry submit→any-fill、cancel reason、fast-fill 对账、REST coverage、IOC BBO fillability、notional-weighted slippage bps、send→write、write→Ack、creation→terminal、close retry、持仓时间、guard final flat 和 Strategy 终态审计；后者至少核对 `needs_reconcile`、`manual_intervention`、`unknown_local_order_feedbacks`、`terminal_feedbacks_ignored` 和 `feedback_continuity_lost_events`。只有传入并完整匹配 REST fills 时，才检查实际 fee/PnL。

打包默认生成 `reports/<run_id>.zip`：

```bash
cd reports
/home/liuxiang/dev/pyenv/lx/bin/python -m zipfile -c <run_id>.zip <run_id>
/home/liuxiang/dev/pyenv/lx/bin/python -m zipfile -l <run_id>.zip
```

Report folder 与 zip 作为独立原子提交；只有用户明确要求 push 才 push。

## 验证入口

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_order_detail_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/analyze_bitget_execution_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/generate_live_report_test.py
git diff --check
```

任何 live 操作都必须使用当次新鲜 preflight/final evidence；历史 run 的 flat、余额、endpoint 或白名单不能复用。
