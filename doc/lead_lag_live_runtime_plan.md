# LeadLag 实盘运行 Runbook

## 目的

本文是 LeadLag 实盘运行和长时间测试的当前入口。它只保留当前边界、运行顺序、验证入口和仍未完成项；旧的逐任务实现计划、历史命令流水账和单次运行原始报告已清理。

## 当前边界

- `tools/lead_lag/live_strategy.cpp` 是 LeadLag live runner 入口。
- 默认 validate-only / signal-only，不提交真实订单。
- 只有同时满足显式 `--execute`、`strategy.mode=live`、API 凭据、feedback SHM 和 realtime data reader 时，才进入真实 live-orders runtime。
- 缺少凭据时 `RunLiveOrders()` 返回 exit code `2`；真实订单模式收到 `OrderFeedbackKind::kContinuityLost` 后停止 trading loop，并返回 handoff exit code `10`。
- V1 对齐 Sirius 边界：策略持仓由订单回报推导；停机后用 REST final check / emergency flatten 校验真实账户；不新增独立 `AccountPositionFeedbackSession`。
- `scripts/lead_lag/run_live_with_guard.py` 是真实订单测试推荐外层入口，负责 REST preflight、runner 退出监控、final REST check 和异常 stop-and-flat。
- 真实订单模式不写 per-signal CSV；信号与下单意图通过日志中的 `trigger_ticker_id`、`lead_lag_signal_triggered` 和 `lead_lag_order_intent` / `lead_lag_order_intent_rejected` 对齐；成功提交后的订单主事实源是 `lead_lag_order_submitted`，其中包含 `local_order_id`、最终 `position_id`、`position_event`、`position_direction`、`entry_local_order_id`、`signal_role`、`order_role`、`quantity_text` 和 `price_text`；终态日志 `lead_lag_order_finished` 同步输出 `position_id`、`position_direction`、`order_role`、`entry_local_order_id` 和 `order_finished_local_ns`，用于后续生成 `position.csv`。
- replay / signal-only live 只有显式 `--signals-output` 才写 signal CSV。

## 关键配置

| 文件 | 用途 |
| --- | --- |
| `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` | requested 12-symbol live strategy runtime；文件名保留历史 `11symbols`，内容已追加 `ETH_USDT`。 |
| `config/strategies/lead_lag_requested_11symbols_20260522.toml` | requested 12-symbol LeadLag pair / execute / risk 配置。 |
| `config/data_sessions/gate_data_session_requested_20260521.toml` | Gate requested symbols data session。 |
| `config/data_sessions/binance_data_session_requested_20260521.toml` | Binance requested symbols data session。 |
| `config/data_readers/strategy_data_reader_requested_20260521.toml` | LeadLag requested symbols realtime reader。 |
| `config/order_feedback/gate_order_feedback_session.toml` | Gate private order feedback session。 |

requested 配置当前覆盖：

```text
PROVE_USDT, RAVE_USDT, ZEC_USDT, SIREN_USDT, ETC_USDT, DASH_USDT,
RIVER_USDT, SUI_USDT, INJ_USDT, ENA_USDT, BRETT_USDT, ETH_USDT
```

`RAVE_USDT`、`SIREN_USDT`、`RIVER_USDT` 是 Gate decimal-size 合约；instrument catalog 由 `order_size_min=0.1` 推导 `quantity_step=0.1` 和 `quantity_decimal_places=1`。C++ order / feedback / Gate encoder / LeadLag sizing 已使用 `double quantity` + `quantity_text`，Gate WS 下单带 `X-Gate-Size-Decimal: 1` 时把 JSON `size` 编码为 string。

12-symbol 策略当前启用：

- `lead_lag.risk.max_gross_notional = 2000.0`：限制 strategy 全局持仓和 pending open reservation 的总 notional；只拒绝新开仓，不阻止 reduce-only close。
- `execute.open_slippage = 3`、`execute.close_slippage = 3`：12 个 symbol 均按 `price_tick` 调整 IOC limit 价格；slippage 只影响实际下单价，不改变 signal 触发条件。

## 已完成证据摘要

- BTC_USDT flat-account emergency smoke、tiny-position emergency smoke 和隔离 `ContinuityLost` stop-and-flat smoke 已完成。
- ZEC_USDT `--smoke-open-close` 小额 filled open / close 和 `--smoke-unfilled-cancel` 小额挂单撤单 smoke 已完成；最终 REST 复核 open orders 为空、position `size=0`。
- 本地端到端 benchmark 已覆盖 signal-to-submit 路径和 feedback 回报路径。
- 2026-05-22 release 11-symbol live-orders guarded run 不是通过项：只完成 1 组 RIVER_USDT strategy open / close；RAVE_USDT IOC partial fill 在 REST 上可见，但当时 private feedback / strategy terminal feedback 缺失，guard 停机后平仓。
- 2026-05-23/24 已修复 decimal quantity、Gate decimal-size WS 编码、Gate `futures.orders` 高精度 fill price parser、REST final check / emergency flatten decimal residual 判断；这些修复仍需完整 strategy 小额 live smoke 复核。
- `--smoke-submit-reject` 和 `gate_order_session_failure_probe` 已有诊断入口和测试，但 ZEC_USDT 安全 IOC、BTC zero-size submit、nonexistent cancel live 探测均未收到最终 failure response，不能计入已完成 smoke。

## 推荐测试顺序

1. 启动 Gate / Binance data session 和 Gate order feedback session，确认 SHM 和 feedback 都在预期路径。
2. 如需 live / replay 信号对比，执行 `doc/lead_lag_live_replay_testing.md` 中的 `lead_lag_live_replay_signal_parity <duration>`，不使用 `--execute`。
3. 真实订单复核前，先跑 targeted tests：

```bash
ctest --test-dir build/debug -R '(lead_lag|signal_csv_writer|gate_order_feedback|gate_submit_response_parser|order_latency)' --output-on-failure
```

4. 用小额 live smoke 复核 decimal-size / IOC partial-fill 修复：
   - 选择 allowlist symbol，优先覆盖 `RAVE_USDT` 或同类 decimal-size 合约。
   - 使用 `scripts/lead_lag/run_live_with_guard.py` 包住 `lead_lag_strategy --execute`。
   - 所有临时 log、stdout、REST summary 和运行产物写入 `/home/liuxiang/tmp/<run_id>`。
   - 结束后检查 strategy terminal feedback、feedback session summary、REST open orders、position `size`、`value` 和 `margin` residual。
5. 小额复核通过后，再做 12-symbol guarded live smoke。
6. 12-symbol smoke 通过后，才继续 30 分钟、2-4 小时或更长时间真实订单 guarded run。

## 常用命令形态

signal-only：

```bash
./build/release/tools/lead_lag_strategy \
  --config config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml \
  --connect-data \
  --duration-sec <duration_sec> \
  --signals-output /home/liuxiang/tmp/<run_id>/live_signals.csv
```

真实订单 guarded run：

```bash
scripts/lead_lag/run_live_with_guard.py \
  --settle usdt \
  --contract <SYMBOL> \
  --poll-timeout-sec 30 \
  --no-pretty \
  -- \
  ./build/release/tools/lead_lag_strategy \
    --config config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml \
    --connect-data \
    --execute \
    --duration-sec <duration_sec>
```

账户复核：

```bash
scripts/gate/query_gate_account.py orders --contract <SYMBOL> --status open --no-pretty
scripts/gate/query_gate_account.py positions --contract <SYMBOL> --no-pretty
```

## 通过判定

- live runner 按预期 exit code 退出；非零退出必须由 guard summary 解释。
- final REST check 证明 in-scope open orders 为空。
- position `size=0`，且 `value` / `margin` residual 符合当前 decimal residual 判断。
- strategy terminal feedback 与 REST finished order 能对齐；IOC partial fill 必须有 terminal feedback 或被明确标记为未通过。
- feedback session 没有未解释断线、decode unrecoverable、SHM queue full 或 continuity lost。
- 对性能或延迟的结论必须附本轮 benchmark / live log 证据。

## 下一步

- 优先做 decimal-size / IOC partial-fill 修复后的完整 strategy 小额 live smoke。
- 继续 failure response 探测前，先确认 Gate 可返回最终 error 的安全请求形态。
- 小额复核通过后，再安排 12-symbol guarded live smoke；不要在复核前启动无人值守真实订单长跑。
- account / position realtime feedback 是 V2 可选能力，不是当前 V1 长跑前置项。
