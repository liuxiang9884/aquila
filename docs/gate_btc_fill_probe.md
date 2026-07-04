# Gate BTC Fill Probe

本文记录 `fill_probe_strategy` 的用途、配置、观测字段和实盘前检查边界。该工具用于在 Gate
`BTC_USDT` 合约上测量最小下单量在 fusion BBO 对手价附近的 GTC / IOC 成交行为；它不是常驻策略，
也不承担自动风控或 emergency flatten。

## 安全边界

- 仅面向 `BTC_USDT`，当前专用配置固定 `symbol_id=93`、`exchange_symbol=BTC_USDT`。
- 凭据只使用 `TEST_KEY` / `TEST_SECRET` 环境变量，不在配置或日志中写入 secret。
- 单个 entry order 的名义金额由 instrument catalog 的 `notional_multiplier`、当前 entry price 和
  `min_quantity` 计算，配置上限为 `max_entry_notional_usdt=10`。
- `max_nodes` 表示最多提交多少个开仓 node。每个开仓 node 固定同时提交 1 个 GTC entry 和 1 个 IOC
  entry，分别走不同 order session；close order、close retry、cancel command 不计入该上限。
- 没有读到可用 BBO 或 freshness gate 未通过时，不创建开仓 node，也不消耗 `max_nodes` 配额。
- 一个 node 会同时发 route 0 GTC 和 route 1 IOC。若两路都成交，临时 exposure 可能接近单笔上限的
  2 倍。
- 当前 Gate encoder 只支持 limit order；close 使用 reduce-only IOC aggressive limit，不是 native
  market order。
- 运行中不做周期性 REST 对账。若 node 在 `unresolved_timeout_ms` 内无法回到 flat，工具写出
  `node_unresolved` 并退出，不会自动 emergency flatten。
- 未经主 agent / 用户明确授权，不允许启动真实 order gateway / feedback / probe 组合进行真实交易。

## 专用配置

- Probe config：`config/fill_probe/gate_btc_fill_probe_20260703.toml`
- Order gateway config：`config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml`
- Order session config：`config/order_sessions/gate_order_session_btc_fill_probe_private_plain_20260703.toml`
- Order feedback config：`config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml`
- 行情输入 SHM：`aquila_gfusion_20260701_102201_30s_ogw24h`
- 行情 channel：`book_ticker_channel`
- Order gateway SHM：`aquila_ogw_btc_fill_probe_20260703`
- Order feedback SHM：`aquila_ofb_btc_fill_probe_20260703`

## Binance Trigger / Gate Quote 模式

`trigger_mode = "binance_trigger_gate_quote"` 用 Binance fusion `BTC_USDT` BBO 触发 node，并使用触发时本机
已可见的最新 Gate fusion `BTC_USDT` BBO 作为下单 quote。entry 严格使用 Gate 对手价：buy 用 Gate ask，
sell 用 Gate bid，不增加 slippage。GTC route 0 + IOC route 1、close retry、unresolved 停止条件和名义金额
上限沿用 Gate-direct probe。

运行前置条件：

- Binance fusion canonical SHM 可读：`aquila_bfusion_20260701_102201_30s_ogw24h` / `book_ticker_channel`
- Gate fusion canonical SHM 可读：`aquila_gfusion_20260701_102201_30s_ogw24h` / `book_ticker_channel`
- `binance_freshness_ns < 2_000_000`
- `gate_freshness_ns < 50_000_000`
- `BTC_USDT` min quantity notional `<= 10 USDT`

该模式的主指标是 entry fillability。Latency 字段用于解释 fill / no-fill，不作为本轮实验的主成败指标。

### 30 分钟 / 300 node 实验命令

下面命令是授权后的 runbook。实现验证阶段只允许 build、unit test 和 `--validate-config`，不要自动启动真实
feedback、gateway 或 probe 进程。

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh release

./build/release/tools/gate_order_feedback_session \
  --config config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml

./build/release/tools/gate_order_gateway \
  --config config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml

./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml \
  --preflight-only

./build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_binance_trigger_gate_quote_probe_20260703.toml
```

### Cross-exchange 归因口径

- `binance_freshness_ns = decision_ns - binance_local_ns`
- `gate_freshness_ns = decision_ns - gate_local_ns`
- `gate_exchange_delta_ns = gate_exchange_ns - binance_exchange_ns`
- `gate_local_delta_ns = gate_local_ns - binance_local_ns`
- `trigger_to_send_ns = submit_ns - decision_ns`

`skip_reason = stale_binance_trigger` 表示 Binance trigger 过期；`skip_reason = stale_gate_quote` 表示 Gate quote
过期；`skip_reason = missing_gate_quote` 表示 Binance 触发时本机还没有可用 Gate BTC quote。这些 skipped
row 不计入 `max_nodes`。

## 可安全执行的校验

下面命令只做构建、配置解析或 SHM 预检，不会发单：

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh release

build/release/tools/gate_order_gateway \
  --config config/order_gateways/gate_order_gateway_btc_fill_probe_20260703.toml \
  --validate-only

build/release/tools/gate_order_feedback_session \
  --config config/order_feedback/gate_order_feedback_session_btc_fill_probe_20260703.toml \
  --validate-only

build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_fill_probe_20260703.toml \
  --validate-config
```

如果专用 order gateway 和 feedback SHM 已由授权流程启动，可额外执行 preflight。该命令会 attach
market / gateway / feedback SHM 并 claim feedback lane，但不会 submit order：

```bash
build/release/tools/fill_probe_strategy \
  --config config/fill_probe/gate_btc_fill_probe_20260703.toml \
  --preflight-only
```

## 授权后启动顺序

真实 probe 前后必须由主 agent / 用户明确授权，并按顺序执行：

1. 重新构建 release，并确认 `gate_fill_probe_test` 通过。
2. 执行 REST read-only 检查，确认 `BTC_USDT` 无 open order、无非零 position。
3. 检查 fusion SHM 正在提供 `BTC_USDT` 新鲜 BBO。
4. 启动专用 `gate_order_feedback_session`。
5. 启动专用 `gate_order_gateway`。
6. 运行 `fill_probe_strategy --preflight-only`。
7. 运行真实 `fill_probe_strategy --config ...`。
8. 结束后再次执行 REST read-only 检查，确认无 open order、position flat。

第 4、5、7、8 步涉及 live / REST 交易边界，本次实现验证阶段只保留命令说明，不自动执行。

## CSV 输出

`output.run_dir` 下写出 3 个 CSV 文件，所有文件按行 flush：

- `node.csv`：node 级决策和结果。核心字段包括 `run_id`、`node_id`、`side`、`bbo_id`、
  `bbo_exchange_ns`、`bbo_local_ns`、`decision_ns`、`submit_ns`、`finish_ns`、
  `local_freshness_ns`、`exchange_freshness_ns`、`bid_price`、`ask_price`、
  `entry_quantity`、`entry_notional_usdt`、`status`、`skip_reason`、`unresolved_reason`。Cross-exchange
  模式额外写出 `trigger_mode`、`binance_bbo_id`、`binance_exchange_ns`、`binance_local_ns`、
  `gate_bbo_id`、`gate_exchange_ns`、`gate_local_ns`、`binance_freshness_ns`、`gate_freshness_ns`、
  `gate_exchange_delta_ns`、`gate_local_delta_ns` 和 `trigger_to_send_ns`。
- `lifecycle.csv`：每个 node 的 GTC / IOC entry 与 close 生命周期。核心字段包括
  `lifecycle_kind`、`entry_local_order_id`、`entry_route_id`、`entry_tif`、`entry_price`、
  `entry_quantity`、`entry_submit_ns`、`entry_finish_ns`、`entry_result`、
  `entry_filled_qty`、`entry_avg_fill_price`、`close_route_id`、
  `close_attempts`、`close_filled_qty`、`close_avg_fill_price`、
  `close_attribution`、`pnl_usdt`、`fee_usdt`。
- `order_event.csv`：order gateway response 和 private feedback 明细。核心字段包括
  `local_order_id`、`parent_id`、`route_id`、`event_kind`、`response_kind`、
  `feedback_kind`、`exchange_order_id`、`exchange_ns`、`local_ns`、`price`、`quantity`、
  `cumulative_filled_quantity`、`left_quantity`、`finish_reason`、`reject_reason`。

`close_attribution=closed_by_net_flat` 表示该 node 的净仓位已由 close path 回到 flat；`none` 表示未发生
可归因 close 或 node 未完成 flat。当前 `pnl_usdt` / `fee_usdt` 预留为对账字段，未做 REST fee 归因。

## Log keys

- `fill_probe_config_ok`：`--validate-config` 成功。
- `fill_probe_start`：已 attach market / gateway / feedback SHM，并生成 `run_id`。
- `fill_probe_preflight_ok`：BBO、下单量和名义金额预检通过。
- `fill_probe_node_start`：一个 node 开始决策。
- `fill_probe_order_submitted`：entry、close 或 cancel command 已提交给 order gateway SHM。
- `fill_probe_order_event`：order gateway response 或 feedback event 已写入 `order_event.csv`。
- `fill_probe_node_done`：node 结束并写入 `node.csv` / `lifecycle.csv`。
- `fill_probe_node_unresolved`：node 超时且未回到 flat，工具将退出。
- `fill_probe_stop`：probe loop 正常退出。

任何成交率、延迟、fillability 或 Gate 行为结论都必须基于真实授权运行产生的 CSV、日志和最终 REST
read-only 检查，不应由配置解析或 dry-run 结果推断。
