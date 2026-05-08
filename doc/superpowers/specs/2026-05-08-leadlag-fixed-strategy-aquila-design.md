# LeadLag Fixed 策略到 Aquila 的分层设计

## 目标

本文把 `leadlag-fixed-strategy-reconstruction-guide.md` 中的 current fixed 策略拆成可审阅、可迁移的设计结构。每一层都先还原 fixed 语义，再说明它在 `aquila` 链路中的落点，最后给出后续验证口径。

本文不实现代码，不承诺性能收益，不替代后续实现计划。它的作用是让后续实现先对齐 fixed 行为，再按 `aquila` 的低延迟结构落地。

## 设计原则

- 先复原 fixed 语义，再对齐 `aquila` 结构。
- 行情、统计、alignment、threshold、signal、order state 分层，不把所有逻辑压进一个大函数。
- 启动期消化字符串、配置、交易所差异和合约 metadata；热路径只使用稳定的 numeric config、source id 和 symbol id。
- 订单生命周期事实只来自 private feedback，不用发送成功或 API ack 推进持仓状态。
- 第一版聚焦单 strategy、单 symbol、单 lead、单 lag；保留 `parallel` 概念，但不先扩展多 symbol。

## 总链路

fixed 的 `OnRawBBO()` 在 `aquila` 中不应直接变成一个巨大的同名函数，而应拆成策略线程内的一条处理链：

```text
Gate/Binance DataSession
  -> BookTicker SHM
  -> Strategy DataReader
  -> LeadLagStrategy::OnBookTicker(source_id, BookTicker)
  -> raw market state update
  -> drift / alignment
  -> if Active: recorder / threshold
  -> signal decision
  -> Strategy OrderPool / execution state
  -> Gate OrderSession place/cancel
  -> Gate OrderFeedbackSession
  -> feedback SHM
  -> Strategy feedback apply
```

策略线程内的主要事件流：

```text
OnBookTicker(source, ticker):
    update raw lead/lag state
    if both raw sides valid:
        update drift samples
    update alignment phase
    if phase != Active:
        return
    if just entered Active:
        reset and seed trading recorders
    if source is lead:
        update lead recorder / threshold
        close existing hold positions by lead signal
        maybe open new position
    if source is lag:
        update lag recorder / spread / noise
        update trailing, stoploss, lag-driven close
```

## 1. 配置与 Symbol Metadata

### Fixed 语义还原

fixed 策略是单 symbol、双交易所结构：

```text
symbol = eth_usdt
lead   = binance
lag    = gateio
```

配置分为三组：

- `trigger`：lead / close 初始阈值、`lag_part`、`quantile.move`、`target_profit_rate`、`drift_limit`、`drift_period`、`drift_min_samples`、`drift_warmup`。
- `execute`：`open` 名义金额、`trailing_stop`、`max_entry_spread`、`parallel`。
- `bbo_record`：`window`、`stats_window`、`size`。

关键语义：

- `lead` 是信号源，`lag` 是唯一交易场所。
- `execute.open` 是名义金额，开仓时才用价格和合约 metadata 换算数量。
- `parallel = 1` 表示同一时间只允许一个 active 交易组。
- `max_entry_spread` 只管开仓前 lag 盘口宽度；`trailing_stop` 只管持仓回撤。
- `fee`、`quantity_tick`、`contract_value` 虽不完整体现在策略 JSON 中，但 fixed 逻辑依赖它们做成本和数量归整。
- `drift_*` 是 alignment 激活条件，不是交易信号阈值本身。

### Aquila 链路对应

这一层属于启动期。建议拆成三类结构：

```text
LeadLagStrategyConfig
  strategy_id
  symbol_id
  lead_source
  lag_source
  trigger params
  execution params
  recorder params

LeadLagInstrumentMetadata
  symbol_id
  lag exchange symbol
  price_tick
  quantity_step / contract quantity unit
  contract_value / notional_multiplier
  taker_fee
  min/max quantity
  min_notional if needed

LeadLagRuntimeBindings
  lead DataReader source index
  lag DataReader source index
  order session route
  feedback lane / strategy_id
```

热路径不做 string exchange 比较、string symbol 比较、TOML/JSON 查表、REST metadata 查询或交易所差异判断。热路径可直接使用的预计算字段包括：

```text
lead_source_id
lag_source_id
symbol_id
open_notional
lag_taker_fee
trailing_stop
max_entry_spread
lag_part
drift_limit
parallel
price_tick / quantity_step / contract_value
window_ns / stats_window_ns / drift_period_ns / drift_warmup_ns
drift_min_samples
quantile_move
target_profit_rate
```

### 验证口径

- 配置默认值与 fixed `SetDefault()` 一致。
- fixed JSON 字段能映射到对应 config 字段。
- lag metadata 必须包含 fee、quantity step、contract value 等下单必要信息。
- lead source 和 lag source 不允许相同。
- `strategy_id < 8`，与 feedback SHM lane 路由一致。
- `parallel=0`、`drift_min_samples=0`、`stats_window < window`、缺少 lag metadata 等配置在启动期拒绝。

## 2. 行情输入与 Raw Market State

### Fixed 语义还原

fixed 的行情入口是 `OnRawBBO(bbo)`。所有 raw lead / raw lag BBO 都先进入这里。更新顺序是：

```text
1. 判断 bbo.exchange 是 lead 还是 lag
2. 保存进入本 tick 前的 prevLead / prevLag
3. updateRawMarketState()
4. 如果 lead 和 lag raw BBO 都有效，UpdateDrift()
5. updateAlignmentPhase()
6. 如果未 Active，直接返回
7. 如果刚进入 Active，enterActivePhase()
8. 根据当前 tick 是 lead 还是 lag，进入 OnLeadBBO / OnLagBBO
```

关键语义：

- raw lead / raw lag 状态必须先更新。
- drift 样本只要求两边 raw BBO 都有效，不要求 Active。
- Active 前不允许交易侧 recorder / signal 被污染。
- 重复价格 tick 可以更新 raw state，但通常不推进交易链路，Active 切换恢复 tick 除外。
- lead tick 主要驱动 threshold、开仓和 lead-driven close。
- lag tick 主要驱动 spread/noise、trailing stop 和 lag-driven close。

### Aquila 链路对应

这一层位于 Strategy 线程：

```text
DataReader.Poll()
  -> LeadLagStrategy::OnBookTicker(source_id, BookTicker)
```

建议状态：

```text
LeadLagMarketState
  lead:
    current BookTicker
    previous BookTicker
    valid bool
  lag:
    current BookTicker
    previous BookTicker
    valid bool
  last_event_ns

MarketTickUpdate
  side: Lead / Lag / Ignored
  price_changed: bool
  both_sides_valid: bool
  event_ns
```

fixed 的 `bbo.exchange` 判断在 `aquila` 中应变成 source binding 判断：

```text
if source_id == lead_source_id:
    update lead raw state
elif source_id == lag_source_id:
    update lag raw state
else:
    ignore or debug assert
```

时间口径第一版建议使用策略线程本地时间推进窗口和 warmup；测试和 replay 可以通过注入 clock 保持可对账。

### 验证口径

- lead tick 只更新 lead raw。
- lag tick 只更新 lag raw。
- 两边未同时 valid 前不产生 drift 样本。
- untracked source 不进入交易链路。
- 相同 bid/ask 的重复 tick 标记为 `price_changed=false`。
- Active 前不调用 threshold / signal。
- Active 切换 tick 的 resume 语义单独测试。

## 3. Recorder / Queue / Noise 统计

### Fixed 语义还原

统计层包括：

```text
lead recorder: rolling bid/ask min/max
lag recorder: rolling bid/ask min/max
lead volema: rolling normalized lead mid volatility
lag volema: rolling normalized lag mid volatility
lag spread: rolling mean lag spread
move queue: lead up/down move 分布
```

关键顺序：

- Active 前 raw/drift 可以更新，但交易侧 recorder 不更新。
- 第一次进入 Active 时，`enterActivePhase()` 重置并播种交易 recorder。
- lead tick 进入 `OnLeadBBO()` 后，先更新 lead recorder / lead noise，再调用 `UpdateMoveThreshold()`。
- lag tick 进入 `OnLagBBO()` 后，先更新 lag recorder / spread / noise，再进行 trailing stop / close。

`MoveQueue` 的 roll 顺序：

```text
if ShouldRoll(now):
    freeze LeadNoise / LagNoise
    compute quantile and thresholds from old queue
    MoveQueue.Roll(now)

push current lead move into the new/current queue
```

### Aquila 链路对应

建议状态：

```text
LeadLagRecorderState
  lead_extrema
  lag_extrema
  lead_noise
  lag_noise
  lag_spread
  move_queue
```

热路径接口：

```text
SeedActive(lead_seed_drifted, lag_seed, now)

OnLeadActiveTick(drifted_lead, now)
  -> update lead extrema
  -> update lead noise
  -> feed threshold engine

OnLagActiveTick(lag_bbo, now)
  -> update lag extrema
  -> update lag spread
  -> update lag noise
```

后续 signal 层应读取 snapshot，而不是直接依赖 recorder 内部实现：

```text
RecorderSnapshot
  lead_bid_min
  lead_ask_min
  lead_bid_max
  lead_ask_max
  lag_bid_min
  lag_ask_min
  lag_bid_max
  lag_ask_max
  lead_noise
  lag_noise
  lag_spread_mean
```

第一版优先保持 fixed 的窗口语义和更新顺序一致，数据结构可以先用清晰固定容量 ring；后续再根据 profile 决定是否改为 monotonic queue 或其他低延迟结构。

### 验证口径

- Active 前输入 BBO 不改变交易 recorder。
- `SeedActive()` 后 recorder 初始极值等于 seed。
- lead tick 只更新 lead recorder / lead noise / move queue。
- lag tick 只更新 lag recorder / lag spread / lag noise。
- roll 时先用旧 queue 算阈值，再 roll，再 push 当前 tick。
- `LagSpreadBuffer = max(current_spread - mean_spread, 0)`。
- 固定 replay 下 extrema、noise、move quantile 与 fixed 对齐到可接受误差。

## 4. Drift 与 Alignment Phase

### Fixed 语义还原

drift 公式：

```text
drift = (lag_ask + lag_bid) / (lead_ask + lead_bid)
```

它把 lead 价格缩放到 lag 坐标系：

```text
lead_drifted_bid = lead_bid * drift_mean
lead_drifted_ask = lead_ask * drift_mean
```

`UpdateDrift()` 负责：

- 在 lead / lag raw BBO 都有效时积累 paired drift 样本。
- 更新 rolling drift mean / std。
- 增加 `driftSamples`。
- 首次 paired sample 记录 `firstPairedDriftTs`。
- 更新 `DriftStdEma`，供 threshold exit 使用。
- 设置 `driftReady`。

alignment phase：

```text
Bootstrap -> Aligning -> Active
```

`alignmentReady()` 至少要求：

```text
both raw sides valid
driftSamples >= drift_min_samples
now - firstPairedDriftTs >= drift_warmup
```

进入 Active 时：

- 重置交易 recorder / noise / spread / move queue。
- 如果 `driftReady`，先用 drift mean 缩放 lead seed。
- 用当前 raw lead / raw lag 快照播种交易 recorder。
- 设置 `resumeLeadTick`，避免 Active 切换 tick 被错误跳过或重复处理。

`drift_limit` 不是 alignment 条件。它只在 lead tick 开仓前限制新开仓；不限制统计更新、平仓或止损。

### Aquila 链路对应

建议状态：

```text
LeadLagAlignmentState
  phase
  drift_ready
  drift_samples
  first_paired_drift_ns
  resume_lead_tick
  logged_active
  drift_recorder
  drift_std_ema
```

接口：

```text
OnPairedRawBbo(now, raw_lead, raw_lag)
  -> update drift mean/std/sample counters

UpdatePhase(now, market_state)
  -> Bootstrap / Aligning / Active

EnterActive(now, raw_lead, raw_lag, recorder_state)
  -> reset trading recorder state
  -> seed with drifted lead and raw lag
```

输出 snapshot：

```text
AlignmentSnapshot
  phase
  drift_ready
  drift_mean
  drift_std_ema
  drift_deviation = abs(drift_mean - 1)
```

### 验证口径

- 单边行情不会产生 drift 样本。
- lead / lag 都 valid 后按 fixed 语义产生 paired drift 样本。
- `first_paired_drift_ns` 只在第一笔 paired sample 设置。
- 样本数不足或 warmup 不足时保持 Aligning。
- readiness 满足后只切一次 Active。
- Active 切换时重置并播种交易 recorder。
- `drift_limit` 超限只禁止开仓。
- drifted lead 等于 raw lead 乘 drift mean。

## 5. Threshold Engine

### Fixed 语义还原

Threshold engine 生成：

```text
UpEntry
DownEntry
UpExit
DownExit
LeadNoise
LagNoise
DriftStdEma
```

它只在 lead active tick 上更新：

```text
UpdateMoveThreshold(now, drifted_lead_bbo):
    if MoveQueue.ShouldRoll(now):
        LeadNoise = lead_volema.GetStdEma()
        LagNoise  = lag_volema.GetStdEma()

        up   = quantile(Up, quantile.move)
        down = quantile(Down, 1 - quantile.move)
        exit = DriftStdEma.GetMean()

        profitBuffer = fee*2 + LeadNoise + LagNoise

        if up - exit > profitBuffer:
            UpEntry = up
            UpExit = exit
        else:
            UpEntry = exit + profitBuffer
            UpExit = exit

        if -down - exit > profitBuffer:
            DownEntry = down
            DownExit = -exit
        else:
            DownEntry = -exit - profitBuffer
            DownExit = -exit

        MoveQueue.Roll(now)

    MoveQueue.Push(
        lead_bid / bid_min - 1,
        lead_ask / ask_max - 1
    )
```

关键语义：

- `UpEntry` / `DownEntry` 是动态开仓阈值。
- `UpExit` / `DownExit` 来自 drift std EMA。
- `profitBuffer = fee*2 + LeadNoise + LagNoise`。
- `target_profit_rate` 不进入 threshold roll，而是在开仓成本门槛中叠加。
- 调用顺序必须是 lead recorder 先吃当前 tick，再 roll/push threshold，再 signal。

### Aquila 链路对应

建议状态：

```text
LeadLagThresholdState
  up_entry
  down_entry
  up_exit
  down_exit
  lead_noise
  lag_noise
  drift_std_ema
  initialized
```

接口：

```text
OnLeadActiveTick(now, drifted_lead, recorder_snapshot, alignment_snapshot)
  -> maybe roll thresholds
  -> push current move
  -> return ThresholdSnapshot
```

该层不下单，不读写 position，不判断 lag 当前 spread 是否可交易。它只把统计状态转成 threshold snapshot。

### 验证口径

- 初始 threshold seed 与 fixed 一致。
- 未到 roll 时间时只 push move。
- roll 时按旧 queue 计算 quantile。
- 上行分支和兜底分支分别覆盖。
- 下行分支符号单独测试。
- fee 使用 `fee*2`。
- `target_profit_rate` 不进入 threshold roll。
- 固定 replay 下四个 threshold 与 fixed 对齐。

## 6. Signal：Open / Close / Stoploss

### Fixed 语义还原

lead tick 顺序：

```text
OnLeadBBO(drifted_lead):
    update lead recorder / noise / threshold

    close existing Hold positions by signal

    if len(caches) >= parallel:
        return
    if drift_ready and abs(drift - 1) > drift_limit:
        return

    if OpenLong():
        return
    OpenShort()
```

lag tick 顺序：

```text
OnLagBBO(lag):
    update lag recorder / spread / noise

    for Hold cache:
        update trailing
        if stoploss triggered:
            continue
        signal close
```

优先级：

```text
已有持仓管理 > 新开仓
stoploss > signal close
OpenLong 尝试 > OpenShort 尝试
pending order 存在时不重复发 close / stoploss
```

`OpenLong` 门禁：

```text
leadMove = lead_bid / lead_bid_min - 1
askDiff  = lead_ask / lead_ask_min - 1
prcDiff  = lead_bid / lag_ask - 1

prcDiff > 0
leadMove >= UpEntry
askDiff >= UpEntry
moveSpace / leadMove > lag_part
targetSpace >= requiredEdge
spreadPct(lag) <= max_entry_spread
```

`OpenShort` 是镜像门禁，注意符号方向：

```text
leadMove = lead_ask / lead_ask_max - 1
bidDiff  = lead_bid / lead_bid_max - 1
prcDiff  = lead_ask / lag_bid - 1

prcDiff < 0
leadMove <= DownEntry
bidDiff <= DownEntry
moveSpace / leadMove >= lag_part
-targetSpace >= requiredEdge
spreadPct(lag) <= max_entry_spread
```

signal close：

```text
CloseLong:
    if pending order: return
    if lead_bid / lag_bid - 1 < UpExit:
        IOC sell close at lag bid

CloseShort:
    if pending order: return
    if lead_ask / lag_ask - 1 > DownExit:
        IOC buy close at lag ask
```

stoploss：

```text
StoplossLong:
    trailing_high = max(trailing_high, lag_bid)
    fallback = lag_bid / trailing_high - 1
    if fallback <= -trailing_stop:
        aggressive IOC sell close at lag_bid * 0.995

StoplossShort:
    trailing_low = min(trailing_low, lag_ask)
    fallback = -(lag_ask / trailing_low - 1)
    if fallback <= -trailing_stop:
        aggressive IOC buy close at lag_ask * 1.005
```

### Aquila 链路对应

Signal 层输出订单意图，不直接编码 Gate JSON：

```text
LeadLagOrderIntent
  action: OpenLong / OpenShort / CloseLong / CloseShort
  reason: Signal / Stoploss
  side
  price
  quantity or notional
  time_in_force = IOC
  reduce_only
  target execution cache
```

链路：

```text
signal produces intent
  -> Strategy allocates/updates order state
  -> OrderPool stores StrategyOrder
  -> Gate OrderSession sends place/cancel from order struct
```

数量边界：

```text
open quantity = format(open_notional / price)
close quantity = format(current position quantity)
```

格式化依赖启动期缓存的 metadata。Signal 公式不混入 Gate wire encoding。

### 验证口径

- `OpenLong` 每个 reject gate 单独覆盖。
- `OpenShort` 镜像逻辑和符号方向单独覆盖。
- `parallel` 达上限时不新开仓。
- `drift_limit` 超限时不新开仓，但 close / stoploss 仍可触发。
- lead tick 中 close 先于 open。
- lag tick 中 stoploss 先于 signal close。
- pending order 时不重复 close / stoploss。
- stoploss long 使用 `lag_bid * 0.995`；short 使用 `lag_ask * 1.005`。
- 固定 replay 下 open / close 触发点与 fixed 对齐。

## 7. Order / Position / Feedback 状态机

### Fixed 语义还原

fixed 的 `ExecuteCache` 管理一个交易组：

```text
ExecuteCache
  stage: Idle / Open / Hold / Close
  order: current pending order
  position
  trailing
  groupTag
  grouping
```

这里 `Idle` 表示该 execution slot 没有 pending order，也没有 position，可以被下一次开仓信号复用。在 `aquila` 中不一定需要长期保存 Idle cache；更自然的实现是 active cache 被删除。

状态转移：

```text
Idle
  -> OpenLong/OpenShort send open order
  -> Open

Open
  -> order terminal and position != 0
  -> Hold

Open
  -> order terminal and position == 0
  -> Idle / delete cache

Hold
  -> signal close / stoploss send close order
  -> Close

Close
  -> order terminal and position == 0
  -> Idle / delete cache

Close
  -> order terminal and position != 0
  -> Hold
```

`OnOrder(msg)` fixed 语义：

```text
find cache by ClientID
update order filled qty / status / avg fill price

if order finished:
    append order to grouping
    filled = GetOrderFilled(order)
    if side == Long:
        position += filled
    else:
        position -= filled

    if position == 0:
        Stage = Idle
        delete cache
    else:
        if Stage == Open and avg_fill_price > 0:
            trailing = avg_fill_price
        Stage = Hold

    clear pending order
```

关键语义：

- 发送订单不等于成交。
- 只有 order terminal feedback 能推进 position 和 stage。
- 开仓 IOC 完全未成交：`Open -> Idle`，cache 删除。
- 开仓部分成交：`Open -> Hold`，position 为成交量。
- 开仓完全成交：`Open -> Hold`，trailing 初始化为 avg fill price。
- 平仓完全成交：`Close -> Idle`，cache 删除。
- 平仓部分成交：`Close -> Hold`，保留剩余 position。
- rejected open：无 position 时删除 cache。
- rejected close：已有 position 时回到 `Hold`。
- filled quantity 按 tick / contract value 归整。

### Aquila 链路对应

fixed 的 `OnOrder(msg)` 在 `aquila` 中应对应：

```text
Strategy::OnOrderFeedback(OrderFeedbackEvent)
```

而不是 `OrderSession` ack callback。职责边界：

```text
Strategy
  order object
  OrderPool
  position / execution cache
  signal apply
  feedback apply

Gate OrderSession
  place/cancel encode and send
  request_sequence -> local_order_id correlation
  light API response diagnostics

Gate OrderFeedbackSession
  private futures.orders login / subscribe / parse
  publish OrderFeedbackEvent to SHM

Feedback SHM
  strategy_id lane routing
  Strategy thread drain
```

建议状态：

```text
LeadLagExecutionCache
  stage
  pending_order_id
  local_open_order_id
  local_close_order_id
  exchange_order_id
  position_quantity
  avg_entry_price
  trailing_price
  group_id
  side: Long / Short
  open_reason / close_reason

LeadLagExecutionState
  max_parallel
  active_caches
  local_order_id -> cache index
```

feedback event 推进：

```text
Accepted:
  record exchange_order_id
  notify OrderSession cancel cache

PartialFilled:
  update cumulative filled and position
  if opening and position != 0: stage can become Hold
  keep pending order unless event is terminal

Filled:
  terminal
  update position
  clear pending order
  position == 0 -> delete cache
  position != 0 -> Hold

Cancelled / Finished:
  terminal
  apply filled quantity
  clear pending order
  position == 0 -> delete cache
  position != 0 -> Hold

Rejected:
  terminal
  clear pending order
  no position -> delete cache
  existing position -> Hold
```

如果 feedback SHM 报 lane/global gap，LeadLag 应进入 degraded / needs reconcile 状态，暂停新开仓，等待后续 REST reconcile 设计补齐状态。

### 验证口径

- 开仓 IOC 完全未成交：cache 删除。
- 开仓部分成交：进入 Hold，position 为成交量。
- 开仓完全成交：进入 Hold，trailing 初始化。
- 平仓完全成交：cache 删除。
- 平仓部分成交：回到 Hold，剩余 position 正确。
- rejected open：无 position 时删除 cache。
- rejected close：已有 position 时回到 Hold。
- filled quantity 按 tick / contract value 归整。
- pending order 存在时 signal 不重复发 close。
- accepted event 更新 exchange order id，并通知 OrderSession cancel cache。
- terminal event 清理 OrderSession cancel cache。
- feedback gap 后暂停新开仓。

## 阶段性实现边界

第一版目标是行为可对齐，不追求一次性完成全部生产能力：

- 支持单 strategy、单 symbol、单 lead、单 lag。
- 支持 fixed 的 alignment、threshold、open/close/stoploss 语义。
- 支持通过 fake feedback 推进状态机。
- 对接真实 Gate feedback 前，不把 OrderSession ack 当成成交事实。
- 不在第一版实现 REST reconcile、multi-symbol portfolio、跨交易所下单、batch/amend/cancel-all。

## 后续分析顺序

建议按本文 7 层逐段审阅：

1. 配置与 symbol metadata 是否完整。
2. raw market state 的时间和 source binding 是否符合 `aquila` 当前 DataReader。
3. recorder / queue 是否需要完全复刻 fixed 数据结构，还是只复刻输出语义。
4. alignment readiness 是否需要引入 `stats_window` 条件。
5. threshold 符号和 roll 顺序是否确认。
6. signal 的成本模型、spread buffer、stoploss 价格是否确认。
7. feedback 事件到 execution cache 的状态推进是否确认。
