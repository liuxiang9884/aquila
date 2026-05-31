# LeadLag Fixed 策略语义索引

## 定位

本文只保留 fixed 版 LeadLag 策略迁移到 Aquila 后仍有用的语义索引。当前实现事实源以 `strategy/lead_lag/README.md` 和 `strategy/lead_lag/*` 代码为准；本文不再保留早期 Go 回测工程路径、完整伪代码和逐变量历史推导。

适合在两种场景下阅读：

- 需要快速理解 fixed 策略的核心信号链路。
- 需要把历史 Go 策略语义映射到当前 C++ 模块。

## 当前 C++ 入口

| 文件 | 职责 |
| --- | --- |
| `strategy/lead_lag/config.*` | 解析 pair、trigger、execute、risk、metadata 和 reader/runtime config。 |
| `strategy/lead_lag/raw_market_state.h` | 按 pair / exchange 保存 latest / previous lead-lag BBO。 |
| `strategy/lead_lag/alignment.h` | 维护 drift rolling mean / std，推进 Bootstrap / Aligning / Active。 |
| `strategy/lead_lag/recorders.h` | BBO 极值、noise、spread、move quantile 统计。 |
| `strategy/lead_lag/threshold.h` | 根据 move、noise、drift、fee 更新开平仓阈值。 |
| `strategy/lead_lag/cost_model.h` | 开仓成本、spread buffer、目标收益和手续费模型。 |
| `strategy/lead_lag/signal.h` | open / close / stoploss 信号判断。 |
| `strategy/lead_lag/execution_state.h` | position / pending order / feedback continuity 状态。 |
| `strategy/lead_lag/strategy.h` | `OnBookTicker()` 主入口，串接全部模块。 |

## 策略一句话

LeadLag 是一个只在 lag 交易所下单的收敛策略：

1. 同时接收 lead / lag BBO。
2. 用 `lag_mid / lead_mid` 建立 drift 对齐。
3. 对齐进入 Active 后，用 lead 的先行动作判断 lag 是否尚未跟随。
4. 只有理论利润空间、lag spread、noise、risk 和 execution state 都允许时，才在 lag 上开仓。
5. 持仓后用 signal close 或 trailing stop 平仓。

当前常用实盘形态是 Binance 作为 lead、Gate 作为 lag，Gate 下 IOC limit order。

## 主流程

`Strategy::OnBookTicker()`：

```text
BookTicker
  -> RawMarketState route by symbol_id + exchange
  -> update latest / previous lead or lag quote
  -> if lead and lag both available:
       update AlignmentState
  -> if alignment active:
       lead tick -> update drifted lead, recorders, threshold, close/open signal
       lag tick  -> update lag recorder, stoploss / close signal
  -> replay mode: synthetic accounting
  -> live mode: OrderIntent -> StrategyContext -> OrderManager
```

时间口径：

- 优先使用 `BookTicker.exchange_ns`。
- 如果 `exchange_ns == 0`，回退到 `BookTicker.local_ns`。
- replay / 对账要求输入已按目标顺序预处理；`RealtimeDataReader` 的多 source round-robin 不做全局时间排序。

## 状态分层

| 层 | 当前 C++ 模块 | 核心状态 |
| --- | --- | --- |
| Raw market | `raw_market_state.h` | lead / lag latest quote、previous quote、pair role。 |
| Alignment | `alignment.h` | phase、drift samples、drift mean / std、seed quote。 |
| Recorders | `recorders.h` | lead / lag BBO 极值、noise、spread、move distribution。 |
| Threshold | `threshold.h` | open / close thresholds、lead noise、lag noise、drift std。 |
| Signal | `signal.h` | open long / short、close long / short、stoploss decision。 |
| Execution | `execution_state.h` | pending open、holding、pending close、continuity lost pause。 |

## 关键语义

### Alignment

- Bootstrap / Aligning 阶段只积累 paired lead / lag quote。
- 满足 warmup 时间和最小样本数后进入 Active。
- Active 入口会生成 seed quote，并重新播种交易侧统计器。
- drift 的经济含义是 lag 与 lead 的价格比例，用于把 lead quote 转换到 lag 价格域。

### Threshold

开平仓阈值不是固定常数，而是由这些因素共同决定：

- lead move quantile。
- lead / lag noise。
- drift std。
- lag spread。
- fee / target profit。

具体公式以 `threshold.h` 和 `cost_model.h` 为准。修改阈值逻辑时优先补 replay / unit test，不把历史本文档当作代码事实源。

### Open

开仓需要同时满足：

- alignment active。
- 当前没有冲突的 pending / holding 状态。
- lead move 超过动态 entry threshold。
- lag 尚未跟随，仍有收敛空间。
- lag spread 不超过配置上限。
- cost model 后仍满足目标收益要求。
- risk limits 允许新开仓。

### Close / Stoploss

平仓来源：

- signal close：lead / lag 价差已回到 close threshold 内。
- trailing stop：持仓后的不利移动触发 stoploss。
- continuity lost / guardrail：live 模式下停止开仓，由外部 guard 负责 stop-and-flat。

### Execution

- replay 可用 synthetic accounting。
- live 真实订单必须通过 `StrategyContext` / `OrderManager`。
- `OrderManager` 和 private feedback 是订单生命周期事实源。
- `ack` 不代表成交或持仓变化；成交和 terminal 以 feedback / REST guard 为准。

## 配置入口

常用文件：

| 文件 | 用途 |
| --- | --- |
| `config/strategies/lead_lag_requested_11symbols_strategy_20260522.toml` | requested symbols signal-only。 |
| `config/strategies/lead_lag_requested_11symbols_live_strategy_20260522.toml` | requested symbols live-orders runtime，必须显式 `--execute`。 |
| `config/strategies/lead_lag_ordi_replay.toml` | ORDI replay runtime。 |
| `config/strategies/lead_lag_ordi.toml` | ORDI 策略参数。 |
| `config/instruments/usdt_futures.csv` | symbol id、tick size、quantity step、decimal places 等 metadata。 |

更多运行入口见：

- `strategy/lead_lag/README.md`
- `docs/lead_lag_live_runtime_plan.md`
- `docs/lead_lag_live_operations_pipeline.md`
- `docs/lead_lag_live_replay_testing.md`
- `docs/lead_lag_reconcile_design.md`

## 回放和报告

回放：

```bash
./build/debug/tools/lead_lag_replay \
  --config config/strategies/lead_lag_ordi_replay.toml \
  --signals-output /home/liuxiang/tmp/lead_lag_signal.csv
```

真实订单模式不写 per-signal CSV；信号和订单通过 log 字段关联：

- `lead_lag_signal_triggered`
- `lead_lag_order_intent`
- `lead_lag_order_submitted`
- `lead_lag_order_response`
- `lead_lag_order_finished`

报告脚本：

```bash
scripts/lead_lag/analyze_order_detail.py
```

输出：

- `order_detail.csv`
- `position.csv`
- `latency.csv`

字段说明见 `docs/lead_lag_live_report_csv_schema.md` 和 `docs/diagnostic_fields.md`。

## 当前边界

- fixed 语义已经迁入 C++ 模块；本文不是完整历史复刻手册。
- 多 source realtime reader 不做 exchange timestamp merge；严格顺序 replay 需要离线预处理成单一 binary source。
- live V1 遇到 feedback `ContinuityLost` 后停止自动交易并返回 handoff exit code，由 guard 做 REST cancel / reduce-only flatten / final flat check。
- IOC partial-fill / decimal filled close 当前不再作为 active blocker；若 live run 再出现 terminal feedback、filled close 或 REST residual 异常，再做 targeted smoke。
- account / position realtime feedback 是 V2 可选能力，不是当前 V1 前置项。

## 验证命令

```bash
ctest --test-dir build/debug -R lead_lag --output-on-failure
./build/release/benchmark/strategy/lead_lag_strategy_benchmark
git diff --check
```
