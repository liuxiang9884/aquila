# LeadLag ContinuityLost 应急处理与后续恢复设计

**目标：** 明确 LeadLag 实盘运行中收到 `OrderFeedbackKind::kContinuityLost` 后的第一版处理方式。当前不做自动恢复和继续交易；先停止系统，再通过 Python Gate REST API 查询仓位、撤销挂单、用 reduce-only 市价单平掉风险敞口，并用 REST 复核账户已经回到安全状态。

**架构：** V1 采用 `stop-and-flat`。C++ live runner 负责识别 `ContinuityLost` 并停止自动交易；Python REST 应急工具负责账户侧处理：查询 position、查询 open orders、撤单、提交 reduce-only market close、轮询复核。更复杂的 read-only reconcile / 自动恢复只作为 V2 后续设计保留，不作为当前打开真实交易的前置目标。

**技术栈：** C++20 strategy / runtime、Gate WebSocket order feedback、Gate APIv4 REST Python helper、CTest、Python unit tests、`git diff --check`。

---

## 当前决策

- `ContinuityLost` 表示下行订单事实流连续性不可证明；WS reconnect 成功不等于订单事实已经恢复。
- V1 不尝试在同一轮运行中恢复 LeadLag 策略状态，也不恢复新开仓。
- V1 的目标是让系统快速进入安全停止状态：停止自动交易，清理挂单，平掉 in-scope position，然后停住等待人工复核。
- 如果 Gate 账户是 LeadLag 专用账户，应急工具可以扫描并处理账户下所有 futures position。
- 如果 Gate 账户可能混用其他策略或人工仓位，应急工具必须要求显式 contract allowlist，不能默认平掉全账户。
- 市价平仓必须使用 reduce-only，避免应急脚本因方向、数量或重复执行错误打开新仓。
- V1 不新增独立 `AccountPositionFeedbackSession`，也不把 account / position realtime feedback 放进恢复前置条件。
- 对齐 Sirius 当前边界：`third_party/sirius/exchange/gate/trade/TradeEngine` 只启用 `futures.orders` 反馈进入策略事件队列，策略持仓由订单 filled / partial filled / cancel / reject 回报推导；account / position REST 查询用于快照和复核，不作为策略实时状态主线。

## 当前事实

- `OrderFeedbackKind::kContinuityLost` 是 transport / session / delivery 层控制事件，不是 Gate `futures.orders` 普通生命周期事件。
- 当前会产生 `ContinuityLost` 的主要来源：
  - feedback WS active 后断开或进入 reconnect / closing / closed；
  - feedback SHM 某个 strategy lane 队列满；
  - global continuity lost fanout 时 lane 满，publisher 会保留 pending event 后续重试。
- 枚举中还保留了 `kReconnectUnknownWindow`、`kDecodeUnrecoverable`、`kProducerRestart`，这些应作为后续增强场景处理。
- `OrderManager` 和 LeadLag `ExecutionState` 仍是内存状态；进程退出后不能靠它们自动恢复交易。
- 当前已有 `scripts/gate/account/query_gate_account.py`、read-only `scripts/gate/trading/reconcile_futures_orders.py`、V1 可下单应急工具 `scripts/gate/trading/emergency_flatten_futures.py` 和外围 guard wrapper `scripts/lead_lag/run_live_with_guard.py`。

## V1 应急流程

1. 触发
   - live runner 收到 `OrderFeedbackKind::kContinuityLost`。
   - 或 supervisor / runbook 观察到 feedback session 断线、producer restart、SHM lane full 等等价风险信号。

2. 停止自动交易
   - C++ live runner 停止 data-to-order loop，不再提交任何策略订单。
   - 不继续执行 LeadLag 正常 close / stoploss 逻辑。
   - 不因为 feedback WS reconnect 成功而恢复交易。

3. 确定 REST 应急处理范围
   - 专用账户：允许查询并处理账户下所有 futures position。
   - 共享账户：必须传入 contract allowlist，只处理 allowlist 内合约。
   - 应急 summary 必须记录处理范围，便于事后审计。

4. 撤销 in-scope open orders
   - 先查询 in-scope contract 的 open orders。
   - 对所有 in-scope open orders 发 cancel。
   - 撤单失败、REST 超时或返回不确定结果时继续进入复核循环；最终不能证明 open orders 为空则失败停住。

5. 平掉 in-scope position
   - 查询 REST positions。
   - 对每个非零 position，按反方向提交 reduce-only market close，数量为当前绝对 position size。
   - 不使用 LeadLag signal、threshold、order sizing 或 execution group 推导下单数量。
   - 不依赖 feedback WS 来确认平仓结果，只以 REST position / open orders 复核为准。

6. 二次撤单和复核
   - 平仓后再次查询 open orders，取消任何新出现或残留的 in-scope open orders。
   - 轮询 REST，直到 in-scope positions 全部为 0 且 open orders 为空。
   - 成功后系统仍保持停止，不自动重启。
   - 失败后系统保持停止，输出失败原因并进入人工处理。

## V1 必须满足的 guardrails

- 所有应急平仓单必须是 reduce-only market order。
- 应急脚本必须幂等：重复执行只会继续撤残留挂单、继续平残留 position，不会打开新仓。
- 默认不在共享账户上做 account-wide flatten。
- 每次执行必须输出结构化 summary：trigger、scope、orders_cancelled、close_orders_submitted、final_positions、final_open_orders、result。
- 不把 API secret、完整敏感账户原始响应或本地临时配置写入 committed docs。
- REST 失败、rate limit、返回结构异常、position 无法确认、open orders 无法确认时，都不能宣称恢复成功。

## Python REST 应急工具要求

建议新增独立脚本：

```text
scripts/gate/trading/emergency_flatten_futures.py
```

建议 CLI：

```bash
scripts/gate/trading/emergency_flatten_futures.py \
  --settle usdt \
  --scope dedicated-account \
  --confirm-dedicated-account \
  --max-position-count 8 \
  --poll-timeout-sec 30 \
  --no-pretty
```

共享账户模式必须显式传合约：

```bash
scripts/gate/trading/emergency_flatten_futures.py \
  --settle usdt \
  --scope allowlist \
  --contract BTC_USDT \
  --poll-timeout-sec 30 \
  --no-pretty
```

脚本行为：

- 复用 `query_gate_account.py` 中的签名、请求和查询能力。
- 新增 REST cancel order 和 market close request builder。
- 支持 dry-run / plan-only，用于 smoke 前确认会处理哪些仓位和挂单。
- 默认输出 JSON summary，便于 runner / supervisor 读取。
- 退出码建议：
  - `0`：成功确认 in-scope flat 且无 open orders；
  - `2`：执行完成但 REST 复核未达成 flat；
  - `3`：配置或 scope guard 拒绝执行；
  - `4`：REST 请求失败或响应无法解释。

## C++ runner 处理要求

- `RunLiveOrders()` 打开真实交易前，必须能在 strategy 收到 `ContinuityLost` 后停止 trading loop。
- 停止应优先保留日志和 summary，不应静默退出。
- runner 可以选择只退出并返回专用 exit code，由外层 supervisor 调 Python 应急脚本；也可以在冷路径中直接调用应急脚本。第一版更建议 supervisor 调用，避免 C++ 策略进程承担 REST 平仓职责。
- signal-only runner 不提交订单，可以继续把 `ContinuityLost` 作为 diagnostics 记录；真实交易 runner 必须触发 stop-and-flat。

建议 exit code：

- `0`：正常 duration / manual stop。
- `10`：检测到 `ContinuityLost`，live runner 已停止，等待外层执行 emergency flatten。
- `11`：检测到 `ContinuityLost`，runner 尝试执行 emergency flatten 但失败或未确认 flat。

## 外围 guard wrapper

`scripts/lead_lag/run_live_with_guard.py` 是 V1 推荐入口，用于把 preflight、live runner、final REST check 和 emergency flatten 串成一个外围安全壳。它不改变 `TradingRuntime` 热路径，也不让 C++ runner 直接执行 REST 平仓。

guard REST 凭据默认从 strategy command 的 `--config` 读取：先解析 strategy TOML 的
`[strategy.order_session].config`，再读取 order session TOML 的
`[order_session.credentials] api_key_env` / `api_secret_env`。`--api-key` / `--api-secret`
只作为显式覆盖入口，必须成对传入，并且在可解析 order session config 时必须和其 env 名称一致；
否则 wrapper 以 `config_error` 拒绝启动，避免 preflight / final check 查错账户。

典型 allowlist 用法：

```bash
scripts/lead_lag/run_live_with_guard.py \
  --settle usdt \
  --contract BTC_USDT \
  --poll-timeout-sec 30 \
  --no-pretty \
  -- \
  ./build/debug/tools/lead_lag_strategy \
    --config config/strategies/lead_lag_btc_strategy.toml \
    --connect-data \
    --execute \
    --duration-sec 60
```

wrapper 行为：

- 启动前查询 allowlist contracts，要求 open orders 为空、position `size=0`、`pending_orders=0`，且 `value` / `margin` residual 为 0；不满足时拒绝启动，不自动清理共享账户残留。
- 子进程 exit code 为 `0` 时仍执行 final REST check；final check flat 才返回 `0`。
- 子进程 exit code 非 `0`、子进程异常、final REST check 失败或 final check 非 flat 时，调用 `emergency_flatten_futures.py` 的同模块逻辑执行 stop-and-flat。
- emergency flatten 成功时 wrapper 返回 `10`，表示系统已经执行应急平仓但仍保持停机，等待人工复核。
- emergency flatten 失败或无法确认 flat 时 wrapper 返回 `11`，不得自动重启交易。
`emergency_flatten_futures.py` 会带 `X-Gate-Size-Decimal: 1` 做 REST 查询 / 下单，position
`size` 使用 `Decimal` 解析；`size=0` 的 REST position 必须携带 `value` / `margin` 才能证明
flat，字段缺失会按 REST 响应无法解释处理；decimal size 会以 raw JSON number 提交 reduce-only
close。

## V1 验证

最小验证顺序：

1. Python unit tests

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/account/query_gate_account_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

2. dry-run / plan-only

```bash
scripts/gate/trading/emergency_flatten_futures.py --settle usdt --scope allowlist --contract BTC_USDT --dry-run --no-pretty
```

3. 空账户或 flat account smoke

```bash
scripts/gate/trading/emergency_flatten_futures.py --settle usdt --scope allowlist --contract BTC_USDT --no-pretty
scripts/gate/account/query_gate_account.py orders --contract BTC_USDT --status open --no-pretty
scripts/gate/account/query_gate_account.py positions --contract BTC_USDT --no-pretty
```

期望：无下单需求或仅执行必要撤单；最终 open orders 为空、position size 为 0。

4. 小额真实仓位 smoke

先用受控脚本建立最小仓位，再运行 emergency flatten。期望：应急脚本提交 reduce-only market close，REST 复核 position size 回到 0，open orders 为空。

5. `ContinuityLost` runner smoke

强制 feedback session 断线或注入 `kContinuityLost` event。期望：真实交易 runner 停止自动交易并返回应急 exit code；supervisor / runbook 执行 Python emergency flatten；最终 REST 复核 flat。

## V2 Read-Only Reconcile / Resume 设计

V2 的目标是未来在不强制平仓的情况下恢复可信状态并恢复交易。它不是当前 V1 的交付目标。

V2 可保留以下原则：

- 收到 `ContinuityLost` 后先暂停新开仓。
- snapshot 本地 `OrderManager` 和 LeadLag execution groups。
- 查询 Gate REST open orders、finished orders、positions 和 account summary。
- 只用明确身份映射恢复订单事实：
  - primary：Gate text `t-<local_order_id>`；
  - secondary：唯一 `exchange_order_id`；
  - time-window + contract / side / size / price 只能作为人工 candidate，不能自动 apply。
- REST position 必须与重建后的 LeadLag position 一致。
- 任意缺失、冲突、歧义或 REST 失败都进入 `ManualIntervention`。
- 只有所有 identity、open orders、terminal facts 和 position 都一致时，才允许清除 `needs_reconcile` 并恢复交易。

## 当前实现状态

- `scripts/gate/trading/emergency_flatten_futures.py` 已实现 allowlist / dedicated-account scope、dry-run、open order cancel、reduce-only market close、poll verify 和失败 exit code。
- `scripts/lead_lag/run_live_with_guard.py` 已实现启动前 REST preflight、runner 退出后 final REST check、异常退出 flatten、final 非 flat flatten 和 flatten failure 映射。
- `lead_lag_strategy --execute` 已接入真实 live-orders runtime；`ContinuityLost` 在 live orders 模式下会请求 runtime stop，并返回 exit code `10`。缺少凭据时返回 exit code `2`，不会进入 runtime create。
- signal-only 模式不提交订单；`ContinuityLost` 只作为 diagnostics / degraded 状态记录。
- V1 应急成功后系统仍保持停止，不自动恢复交易。

## 2026-05-22 V1 Emergency Smoke Evidence

- Commit under test: `45fcf96 Stop lead lag live runner on continuity loss`.
- Scope: `allowlist` / `BTC_USDT`; 不使用 dedicated-account 全账户平仓。
- Flat-account smoke: initial open orders 为空，initial position `size=0` / `pending_orders=0`; dry-run 计划无撤单、无 close order；实际 helper 返回 `result=verified_flat`，`close_orders_submitted=[]`，最终 `size=0` / open orders 为空。
- Tiny-position smoke: 先用 REST IOC market buy 建立 BTC_USDT `size=1`；helper 计划并提交 reduce-only IOC market close，payload 关键字段为 `size=-1`、`price=0`、`tif=ioc`、`reduce_only=true`；返回 `result=verified_flat`，最终 `size=0` / `pending_orders=0` / open orders 为空。
- ContinuityLost smoke: 使用 `/tmp/aquila_v1_continuity_20260522_011533` 隔离 market-data SHM 和 feedback SHM；data sessions 只创建空 SHM、不连接行情 websocket；feedback session 发布 `global_continuity_lost_events_published=1` / `shm_published=8`；`lead_lag_strategy --execute` 返回 exit code `10`，summary 显示 `emergency_handoff=true`、`order_feedbacks=1`、`book_tickers=0`、`data_reader_events=0`、`recovery_state=degraded_needs_reconcile`、`needs_reconcile=true`、`new_entries_paused=true`。
- ContinuityLost handoff 后执行 emergency helper，返回 `result=verified_flat`；REST 复核 BTC_USDT open orders 为空、position `size=0` / `pending_orders=0`。

## 验证矩阵

| 阶段 | 命令 | 期望 |
| --- | --- | --- |
| 文档 | `git diff --check` | 无 whitespace error |
| Python helper | `/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/emergency_flatten_futures_test.py` | scope、dry-run、cancel、market close、verify、失败 exit code 覆盖 |
| Read-only REST | `scripts/gate/account/query_gate_account.py positions --contract BTC_USDT --no-pretty` | 返回当前 position，不产生交易副作用 |
| Emergency dry-run | `scripts/gate/trading/emergency_flatten_futures.py --scope allowlist --contract BTC_USDT --dry-run --no-pretty` | 输出 plan，不提交订单 |
| Emergency live smoke | `scripts/gate/trading/emergency_flatten_futures.py --scope allowlist --contract BTC_USDT --no-pretty` | 最终 open orders 为空、position size 为 0 |
| Runner smoke | `ctest --test-dir build/debug -R lead_lag_live_strategy --output-on-failure` | `ContinuityLost` live 模式停止，signal-only 模式不提交订单 |

## 后续边界

- V2 read-only reconcile / resume 是后续优化，不是当前应急方案。
- 已有 `scripts/gate/trading/reconcile_futures_orders.py`、LeadLag recovery state API 和 runner recovery diagnostics 可作为 V2 基础。
- V2 不应阻塞 V1 stop-and-flat，也不应在任意 REST 缺失、冲突、歧义或失败时自动恢复交易。
