# LeadLag 实盘操作 Pipeline

## 目的

本文定义 agent 在收到 LeadLag 实盘启动、巡检、报告生成和打包触发词时的标准执行流程。`AGENTS.md` 只保留触发词索引；本文是具体操作步骤的事实源。

本文不替代 `docs/lead_lag_live_runtime_plan.md` 和 `docs/lead_lag_reconcile_design.md`。真实订单启动前仍必须先确认当前 runbook 的阻断条件、测试顺序和安全边界。

## 实盘交易启动 Pipeline

当用户输入“启动实盘测试”、“启动 12 pair 跑一小时”、“开始 live smoke”、“启动交易端”、“跑一段实盘交易”或等价表达时，默认执行 LeadLag live-orders guarded run pipeline。该流程只负责交易端；已健康运行的 Gate / Binance data session 和 recorder 默认保持不变，不随每次交易测试重启或停止。

1. 先确认当前工作区和事实源：
   - 运行 `git status --short --branch`，记录当前分支、ahead/behind 和未提交状态；不要预设本地与 `origin/main` 同步。
   - 快速复核 `docs/project_onboarding_guide.md`、`docs/lead_lag_live_runtime_plan.md` 和 `docs/lead_lag_reconcile_design.md` 中的当前阻断条件、测试顺序和安全边界。
   - 如果文档仍要求“复核前不要启动无人值守真实订单长跑”，只能启动符合该边界的小额 guarded smoke；不能因为用户说“后台跑一小时”就绕过当前交接限制。
2. 明确本次 run 参数：
   - `duration_sec` 来自用户要求；未给出时先确认，不默认长跑真实订单。
   - `contracts` 优先使用用户指定列表；用户只说“12 pair”时使用当前 requested 12-symbol 列表：`PROVE_USDT`、`RAVE_USDT`、`ZEC_USDT`、`SIREN_USDT`、`ETC_USDT`、`DASH_USDT`、`RIVER_USDT`、`SUI_USDT`、`INJ_USDT`、`ENA_USDT`、`BRETT_USDT`、`ETH_USDT`。
   - 真实订单默认 strategy config 使用 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`；不要误用 signal-only config 搭配 `--execute`。
   - `run_id` 默认用启动时间和标签生成，例如 `YYYYMMDD_HHMMSS_12pair_live`；临时运行目录写入 `/home/liuxiang/tmp/<run_id>/`。
   - 运行 CPU 分区必须遵守 `docs/runtime_cpu_allocation.md`：当前 `0-15` 是实盘保留区，`16-31` 是测试 / diagnostics / benchmark 区；启动或重启任何测试任务时不要占用实盘 hot path core。
   - 本轮 Ack RTT 复核默认使用 affinity profile `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml`；核心链路目标绑核为 Gate MD CPU2、Binance MD CPU3、strategy / Gate order owner CPU4、Gate order feedback CPU6、log backend CPU5，均位于实盘保留区。
   - 如果本轮明确使用 Gate / Binance fastest-route fusion 行情，默认使用 31-symbol no-TON 配置族：Gate launch config `config/market_data_fusion/gate_data_fusion_31symbols_no_ton_book_ticker_4sources.toml`，Binance launch config `config/market_data_fusion/binance_data_fusion_31symbols_no_ton_book_ticker_4sources.toml`，strategy config `config/strategies/lead_lag_31symbols_no_ton_fusion_live_strategy_20260616.toml`。`TON_USDT` 不得加入本轮订阅或交易集合；2026-06-16 Gate private plain 已返回 `unknown currency pair TON_USDT`，整批订阅会因此失败。
   - Fusion 实盘前必须使用 release binary，构建参数为 `AQUILA_DATA_SESSION_DIAG_LEVEL=0` 和 `AQUILA_FUSION_METADATA_MODE=file`。Gate / Binance `data_fusion` 必须先于 strategy 启动，并确认 canonical SHM 有数据、metadata 文件增长、recorder / probe 无异常 `skipped` / `overruns` 后，才启动 guarded strategy。
   - `run_live_with_guard.py` 当前的 `--affinity-*` overlay 面向单路 data session / order feedback config，不会自动重写 `gate_data_fusion` / `binance_data_fusion` launch config。使用 fusion 行情时，report 中只能按实际启动的 fusion TOML 和 `ps` / log 证据记录 CPU 分配；除非确实应用了对应外部配置，不要声明旧的 data-session affinity split。
   - 如果用户明确要求“全部重新开”、“不与现存的放在一起”或等价表达，本轮必须使用隔离 SHM name 和 `/home/liuxiang/tmp/<run_id>/configs/` 下的临时配置重新启动 Gate data、Binance data、Gate order feedback 和策略；不要复用正在服务其它 run 的 data / feedback SHM。若用户同时指定 Gate private non-TLS，则 Gate data、Gate order session 和 Gate order feedback 都使用 `fxws-private.gateapi.io:80`、`enable_tls=false`；Binance data session 仍按 Binance public TLS 配置。
3. 如果本轮刚修改过代码或交易配置，先完成相应 build / test / commit，再启动 live run；不要用过期 binary 或未验证配置下真实订单。只修改报告或文档时不需要重编译。
   - 当前 LeadLag 策略配置必须使用 `[lead_lag.pairs.trigger.drift_guard]`；旧 `trigger.drift_limit` 和 `trigger.drift_guard.mode` 会被 parser 拒绝。`drift_guard` 是 signal 后、订单 intent 前执行的 open-only emergency sanity guard，close / stoploss 不受影响。live 配置应保留 `capacity.drift_guard_window_capacity = 131072` 或按实测 tick rate 上调，用于降低 1 分钟 drift guard 窗口在 hot path 扩容的风险。
4. 生成本轮 affinity overlay：
   - 在启动或重启核心链路组件前，先生成 `/home/liuxiang/tmp/<run_id>/configs/` 下的临时 TOML；该步骤不连接交易所、不读账户、不启动策略。
   - 后续如果确实用这些临时 TOML 启动 Gate / Binance data session 和 Gate order feedback session，guard 启动时才可以带 `--affinity-external-configs-applied`，report 中的 `affinity_split` 才可视为 true。

```bash
scripts/lead_lag/run_live_with_guard.py \
  --run-id <run_id> \
  --settle usdt \
  --contract <SYMBOL_1> \
  --contract <SYMBOL_2> \
  --affinity-profile config/runtime_affinity/lead_lag_requested_12symbols_node0.toml \
  --affinity-output-dir /home/liuxiang/tmp/<run_id>/configs \
  --affinity-gate-market-config config/data_sessions/gate_data_session_requested_20260521.toml \
  --affinity-binance-market-config config/data_sessions/binance_data_session_requested_20260521.toml \
  --affinity-order-feedback-config config/order_feedback/gate_order_feedback_session.toml \
  --prepare-affinity-only \
  --no-pretty \
  -- \
  ./build/release/tools/lead_lag_strategy \
    --config config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml
```

5. 检查行情端：
   - 通过 `ps`、日志或 SHM probe 确认 `gate_data_session` 和 `binance_data_session` 正在运行并写入预期 SHM。
   - 如果使用 fusion 行情，检查对象改为 `gate_data_fusion` / `binance_data_fusion`、各自 4 路 source SHM、canonical fusion SHM 和 metadata 文件；strategy data reader 必须指向 canonical fusion SHM，而不是 source SHM 或旧单路 data session SHM。
   - 确认 live data session 实际 owner CPU 位于 `0-15`；如果同机有测试 data session / recorder，确认它们位于 `16-31`，或在最终回复中明确本轮例外。
   - 如果行情端缺失，优先使用上一步生成的 `gate_market_data` / `binance_market_data` 临时 TOML；否则按 `config/data_sessions/gate_data_session_requested_20260521.toml` 和 `config/data_sessions/binance_data_session_requested_20260521.toml` 启动并在最终回复中说明未应用 affinity split。启动 stdout / stderr 写入本次 `/home/liuxiang/tmp/<run_id>/`。
   - 如果 recorder 已按用户要求长期运行，保持运行；如需新 recorder，按 recorder runbook 单独启动，不把 recorder 和交易端生命周期绑死。
6. 启动 Gate order feedback session：
   - 必须使用上一步生成的 `gate_order_feedback` 临时 TOML，并把 `file_sink_name` 和 stdout / stderr 指向 `/home/liuxiang/tmp/<run_id>/`；如果未使用 affinity 临时 TOML，最终回复和 report 中不能声称 full affinity split 已应用。
   - `duration-sec` 至少为策略 `duration_sec + 300`，保证策略退出和 guard final check 有回报缓冲时间。
   - 默认不清理正在使用的 feedback SHM；只有确认没有 live strategy 正在消费、且需要清理 stale state 时，才可在临时副本中设置 `remove_existing = true`。
   - 单 symbol 小额 smoke 或刚经历过 `ContinuityLost` / emergency flatten 后的重试，必须先确认没有任何 live strategy 正在消费 `aquila_gate_order_feedback`，然后把本轮 feedback 临时 TOML 的 `remove_existing` 改为 `true` 再启动 feedback session；否则旧 SHM 中残留的控制事件可能导致策略第一轮 feedback poll 立即停止。若已有健康 feedback session 正在服务其它策略，不要清理它，改用新的隔离 SHM 或先停止相关策略。
   - feedback session 必须先进入 ready / subscribed 状态，再启动真实订单策略。
7. 用 guard wrapper 启动真实订单策略，后台运行必须使用可脱离当前 shell 的方式，例如 `setsid ... > <run_dir>/guarded_live.stdout 2>&1 < /dev/null &`。推荐命令形态：

```bash
scripts/lead_lag/run_live_with_guard.py \
  --run-id <run_id> \
  --settle usdt \
  --contract <SYMBOL_1> \
  --contract <SYMBOL_2> \
  --poll-timeout-sec 30 \
  --affinity-profile config/runtime_affinity/lead_lag_requested_12symbols_node0.toml \
  --affinity-output-dir /home/liuxiang/tmp/<run_id>/configs \
  --affinity-gate-market-config config/data_sessions/gate_data_session_requested_20260521.toml \
  --affinity-binance-market-config config/data_sessions/binance_data_session_requested_20260521.toml \
  --affinity-order-feedback-config config/order_feedback/gate_order_feedback_session.toml \
  --affinity-external-configs-applied \
  --no-pretty \
  -- \
  ./build/release/tools/lead_lag_strategy \
    --config config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml \
    --connect-data \
    --execute \
    --duration-sec <duration_sec>
```

guard REST preflight / final check / emergency flatten 的凭据默认从 strategy config 的
`[strategy.order_session].config` 继续读取 order session TOML，并使用其中
`[order_session.credentials] api_key_env` / `api_secret_env`。通常不要手工传
`--api-key` / `--api-secret`；如果显式传入，必须和 order session config 的 env 名称一致，
否则 guard 会以 `config_error` 拒绝启动。guard stdout 的 `credentials` 只记录 env 名称和来源，
不会输出 secret 值。

如果 Gate / Binance data session 或 Gate order feedback session 没有使用本轮生成的临时 TOML，去掉 `--affinity-external-configs-applied`；此时 guard summary 会保留 generated config 路径，但 `affinity_split=false`。

8. 启动后必须做一次即时检查：
   - guard / strategy / feedback PIDs 是否存在。
   - strategy log 是否出现 `lead_lag_live_orders_runtime_started`。
   - guard preflight 是否通过，账户初始 open orders 为空、目标 contracts flat。
   - guard stdout 中的 `credentials.api_key_env` / `credentials.api_secret_env` 是否和 strategy order session 使用的 env 名称一致。
   - guard stdout 中的 `affinity.generated_configs` 是否存在，并确认实际 strategy command 使用 `/home/liuxiang/tmp/<run_id>/configs/` 下的临时 strategy TOML。
   - 是否出现 `ERROR`、`FATAL`、`ContinuityLost`、`feedback_global_continuity_lost` 或 `needs_reconcile=true`。
9. 对持续时间超过 10 分钟的真实订单 run，默认每 10 分钟做一次健康检查，除非用户明确说“不需要监控”。每次检查至少统计：
   - strategy / guard / feedback PIDs。
   - `lead_lag_signal_triggered`、`lead_lag_order_submitted`、`lead_lag_order_finished` 数量。
   - `ERROR`、`FATAL`、`ContinuityLost`、`feedback_global_continuity_lost`、`needs_reconcile`、`manual_intervention` 命中。
   - 最新 guard stdout、strategy log 和 feedback stdout 尾部摘要。
   - 对明显异常立即汇报；不要等到整个 run 结束才说明。
10. guard / strategy 退出后立即做结束检查：
   - 读取 guard 最后一行 JSON summary，确认 `ok`、`result`、`exit_code`、`final_check.flat`、`open_orders` 和目标 contracts position。
   - 核对 strategy summary 中 `order_responses`、`order_feedbacks`、`feedback_continuity_lost_events`、`needs_reconcile`、`manual_intervention`。
   - 统计 submitted / finished / filled / cancelled 数量；fill 数量必须以 `cumulative_filled_quantity > 0` 或 terminal filled feedback 为准。
   - 如果 feedback session 在策略结束后因自身 `duration-sec` 到期发布 `feedback_global_continuity_lost`，最终回复要明确它发生在策略退出之后；如果发生在策略运行期间，则按异常处理。
11. 最终回复必须给出本次 run 的路径和核心证据：
    - run directory、strategy log、guard stdout、feedback stdout。
    - 启动参数：duration、contracts、strategy config、当前 commit / ahead 状态。
    - 退出状态：normal flat、guard flatten、needs reconcile 或失败原因。
    - signal/order/fill 简要统计和任何延迟 / feedback 异常摘要；延迟摘要至少区分 Ack RTT、send-to-finish 本地闭环和 exchange Ack-to-finish 交易所侧 lifecycle。
    - 如果用户随后要求“生成 report”，继续执行本文的“实盘交易 report pipeline”。

## 实盘交易 Report Pipeline

当用户输入“总结上一次实盘交易”、“生成上一次实盘 report”、“生成本次实盘 report”或等价表达时，默认自动执行 LeadLag live report pipeline：

1. 先确认当前工作区状态：运行 `git status --short --branch`，避免把无关改动混进 report 提交。
2. 定位本次实盘输入：
   - 优先使用用户明确给出的 `run_id`、strategy log、guard stdout 和 config。
   - 如果用户只说“上一次”，从 `/home/liuxiang/log/` 中按 mtime 查找最近的 `lead_lag_strategy*live*.log`，并从 `/home/liuxiang/tmp/` 中查找同轮 `guarded_live*.stdout`；如果存在多个候选或无法判断对应关系，先向用户确认，不要猜。
   - 策略配置优先使用 guard / runner 启动命令里的 config；无法从上下文确定时，使用当前 12-symbol live 默认配置 `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml`，并在最终回复中说明该假设。
   - 如果策略 stdout / guard stdout 与 `gate_order_feedback_session.stdout` 分离，先在 `/home/liuxiang/tmp/<run_id>/` 生成一个 merged log，顺序拼接 strategy / guard stdout 和 feedback stdout；report 的 `--log` 使用 merged log，`--guard-stdout` 仍使用原始 guard stdout。这样 `feedback_event`、guard summary 和 runtime affinity 都能被同一份报告解析。
   - 如果 guard stdout 包含 `affinity` summary，report 会在 `report.md` 中记录 profile、split、core_path 和 generated config 路径；`latency.csv` 会包含 `gate_order_ack_latency_diagnostic` outlier 字段。
   - 如果本次 run 使用的合约集合不是默认 12-symbol catalog，必须显式传 `--instrument-catalog`；例如 30-symbol run 使用 `config/instruments/usdt_futures_common_gate_binance_20260602.csv`，否则 report 可能缺少 `contract_multiplier` 并导致 PnL 不完整。
   - `run_id` 优先使用用户给定值；否则从日志文件名或启动时间推导为 `YYYYMMDD_HHMMSS_<label>`，推导不唯一时先确认。
3. 使用固定脚本生成报告目录：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/generate_live_report.py \
  --run-id <run_id> \
  --log <lead_lag_strategy.log> \
  --config <strategy_config.toml> \
  --instrument-catalog <instrument_catalog.csv> \
  --guard-stdout <guard_stdout> \
  --output-root reports
```

生成目录必须是 `reports/<run_id>/`，包含：

```text
report.md
signal.csv
order_detail.csv
position.csv
latency.csv
lead_lag_live_report_csv_schema.md
runtime_configs/  # 如果 guard stdout 中有 affinity.generated_configs 且源文件仍存在
```

4. 如果 `reports/<run_id>/` 已存在，默认不要覆盖；先检查已有内容并询问用户是复用、另取 run id，还是明确重新生成。
5. 生成后做最小校验：
   - `signal.csv`、`order_detail.csv`、`position.csv`、`latency.csv` 都存在且有 header。
   - `lead_lag_live_report_csv_schema.md` 与 `docs/lead_lag_live_report_csv_schema.md` 一致。
   - 如果 report 包含 `Runtime Profile`，确认 `runtime_configs/` 中已归档存在的 generated TOML / profile。
   - `report.md` 必须包含 Ack RTT 三段拆解：上行 `request_send_local_ns -> ack_exchange_request_ingress_ns`、Gate `x_in -> x_out`、下行 `ack_exchange_response_egress_ns -> ack_local_receive_ns`，并列出 `>5ms` Ack tail 的主导阶段。
   - `report.md` 必须包含滑点分析、raw PnL 分析，以及 actual / raw 两套胜率。Raw PnL 使用 entry / exit `raw_price` 计算，fee 仍使用报告 CSV 中的配置费率估算值；如果本轮另行查询到交易所实际合约费率，应在报告中补充实际费率复算说明。
   - 用 report schema 检查四个 CSV 的所有 header 字段都有说明。
   - 运行 `git diff --check`。
6. “打包”默认表示在 `reports/` 目录中，把本次 `reports/<run_id>/` report folder 压缩成 zip 文件，输出为 `reports/<run_id>.zip`。推荐命令：

```bash
cd reports
/home/liuxiang/dev/pyenv/lx/bin/python -m zipfile -c <run_id>.zip <run_id>
```

zip 文件中应包含 `<run_id>/report.md`、四个 CSV 和 `lead_lag_live_report_csv_schema.md`；生成后用 `python -m zipfile -l reports/<run_id>.zip` 或等价命令确认内容。除非用户明确要求其他格式，不生成 tar/zst。
7. 最终回复必须概述 report 目录、zip 路径、signal/order/position/latency 行数、关键 PnL / latency 摘要和退出原因；latency 摘要至少区分 Ack RTT、Ack 上行、Gate `x_in -> x_out`、Ack 下行、send-to-finish 本地闭环和 exchange Ack-to-finish 交易所侧 lifecycle；交易摘要必须说明滑点、raw PnL 和 actual / raw 胜率，不要只说“已生成”。
8. 将 `reports/<run_id>/` 和 `reports/<run_id>.zip` 作为一个原子 git commit；commit message 使用英文，例如 `Add LeadLag live report <run_id>`。
9. 只有用户明确说“push”、“上传到 git”或等价表达时，才在 commit 后执行 `git push`；否则只提交并说明当前 ahead 状态。
