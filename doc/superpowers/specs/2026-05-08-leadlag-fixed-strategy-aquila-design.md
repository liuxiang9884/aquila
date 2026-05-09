# LeadLag Fixed 策略到 Aquila 的分层设计

## 目标

本文把 `leadlag-fixed-strategy-reconstruction-guide.md` 中的 current fixed 策略拆成可审阅、可迁移的设计结构。每一层都先还原 fixed 语义，再说明它在 `aquila` 链路中的落点，最后给出后续验证口径。

2026-05-09 已把 `config/strategy.zip` 解压到 `third_party/strategy/`，并对照 fixed Go 源码补齐 recorder / queue / noise / spread 等细节。当前源码事实源是：

```text
third_party/strategy/wt-invariant-strategy-leadlag-must-fix/leadlag/algo/strategy.go
third_party/strategy/wt-invariant-strategy-leadlag-must-fix/leadlag/algo/analysis.go
third_party/strategy/wt-invariant-strategy-leadlag-must-fix/leadlag/algo/move.go
third_party/strategy/wt-invariant-strategy-leadlag-must-fix/leadlag/algo/cost_model.go
third_party/strategy/wt-invariant-strategy-leadlag-must-fix/leadlag/algo/execute_cache.go
third_party/strategy/wt-invariant-strategy-leadlag-must-fix/utils/queue/queue.go
third_party/strategy/wt-invariant-strategy-leadlag-must-fix/utils/queue/monotonic_queue.go
```

本文不实现代码，不承诺性能收益，不替代后续实现计划。它的作用是让后续实现先对齐 fixed 行为，再按 `aquila` 的低延迟结构落地。

## 设计原则

- 先复原 fixed 语义，再对齐 `aquila` 结构。
- 行情、统计、alignment、threshold、signal、order state 分层，不把所有逻辑压进一个大函数。
- 启动期消化字符串、配置、交易所差异和合约 metadata；热路径只使用稳定的 numeric config、`Exchange` enum 和 `symbol_id`。
- 订单生命周期事实只来自 private feedback，不用发送成功或 API ack 推进持仓状态。
- 第一版允许一个 strategy 管理多个 lead-lag pair，但每个 `symbol_id` 最多配置一个 pair；不支持同一 symbol 配置多组 lead/lag。

## 总链路

fixed 的 `OnRawBBO()` 在 `aquila` 中不应直接变成一个巨大的同名函数，而应拆成策略线程内的一条处理链：

```text
Gate/Binance DataSession
  -> BookTicker SHM
  -> Strategy DataReader
  -> leadlag::Strategy::OnBookTicker(BookTicker)
  -> pair lookup by BookTicker.symbol_id
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
OnBookTicker(ticker):
    find pair by ticker.symbol_id
    if no initialized pair:
        return
    update raw lead/lag state
    if both raw sides valid:
        update drift samples
    update alignment phase
    if phase != Active:
        return
    if just entered Active:
        reset and seed trading recorders
    if ticker.exchange is pair lead:
        update lead recorder / threshold
        close existing hold positions by lead signal
        maybe open new position
    if ticker.exchange is pair lag:
        update lag recorder / spread / noise
        update trailing, stoploss, lag-driven close
```

## Fixed Go 源码对照索引

| 层 | fixed Go 入口 | 关键结论 |
| --- | --- | --- |
| 1. 配置与 metadata | `StrategyConfig`、`SetDefault()`、`LagTakerFee()`、`ExecutionConfig.EntrySpreadLimit()`、`CalOpenQty()`、`FormatQty()` | `max_entry_spread < 0` 时回退到 `trailing_stop`；默认 lag taker fee 来自 hard-coded exchange map；数量按 `price`、`QuantityTick` 和 `ContractValue` 归整。 |
| 2. Raw market state | `OnRawBBO()`、`updateRawMarketState()` | same-price raw tick 不替换已保存 quote，但仍用旧 quote 推进 drift / alignment；未 Active 前不进入交易 recorder / signal。 |
| 3. Recorder / queue / noise | `BBOVolRecorder`、`StreamRecorder`、`StreamStdEmaRecorder`、`MoveQueue`、`LagSpreadBuffer()` | BBO extrema 是时间窗口 monotonic queue；`MoveQueue` 是按 `stats_window` 时间边界切窗并 roll 后清空；`StreamStdEmaRecorder` 名字带 EMA，但实现是 rolling normalized std 的 rolling mean；lag spread 是绝对 spread 的 `StreamRecorder(stats_window)` 均值。 |
| 4. Drift / alignment | `UpdateDrift()`、`alignmentWarmup()`、`alignmentReady()`、`enterActivePhase()` | drift 用 `(lag_ask + lag_bid) / (lead_ask + lead_bid)`；`drift_warmup > 0` 时用配置值，否则回退到 `stats_window`；进入 Active 后重置并播种交易侧 recorder。 |
| 5. Threshold | `UpdateMoveThreshold()`、`ProfitBuffer()` | roll 时先冻结 noise、用旧 move queue 计算 quantile / threshold，再清空 queue，最后 push 当前 lead tick 的 move；`profitBuffer = fee*2 + LeadNoise + LagNoise`。 |
| 6. Signal | `OnLeadBBO()`、`OnLagBBO()`、`OpenLong()`、`OpenShort()`、`CloseLong()`、`CloseShort()`、`ExecuteStoploss*()` | lead tick 顺序是更新 recorder / threshold、先平仓再开仓；lag tick 顺序是 spread / noise、trailing、stoploss、signal close；`drift_limit` 只限制新开仓。 |
| 7. Order / feedback state | `ExecuteCache`、`registerCacheOrder()`、`OnOrder()`、`GetOrderFilled()` | 发送订单只设置 pending；只有 terminal order feedback 推进 position / stage；position 归零删除 cache，非零进入 Hold。 |

## 1. 配置与 Symbol Metadata

### Aquila 第一版决定

第一版策略名固定为 `lead_lag`。runtime config 仍使用 `[strategy]`，其中：

```toml
[strategy]
name = "lead_lag"
config = "config/strategy/lead_lag.toml"
```

user strategy config 固定放在 `config/strategy/lead_lag.toml`，root table 为 `[lead_lag]`，并显式写入 `version = "1.0"`。`version` 使用字符串保存，避免后续 `1.10` 一类版本被 TOML 数值语义改写。

pair 配置使用数组：

```toml
[[lead_lag.pairs]]
symbol = "BTC_USDT"
symbol_id = 0
lead_exchange = "binance"
lag_exchange = "gate"
lag_taker_fee = 0.00016
```

`trigger`、`execute`、`bbo_record` 和 `capacity` 在第一版作为 pair 子表解析，启动期直接展开成每个 `PairSlot` 的 numeric config。这样每个 pair 在热路径只读自己的固定字段，不做全局默认值合并、不做字符串查表，也不依赖 Go 配置文件的原始形状。

### Fixed 语义还原

fixed 策略的单个 pair 是单 symbol、双交易所结构：

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
- `fee`、`quantity_tick`、`contract_value` 虽不完整体现在策略 JSON 中，但 fixed 逻辑依赖它们做成本和数量归整；`aquila` 中对应交易基础信息从 instrument catalog 解析，合约面值使用 `notional_multiplier` 表达。
- `drift_*` 是 alignment 激活条件，不是交易信号阈值本身。

Go 源码补充：

- `LagTakerFee()` 优先使用 `config.cost.lag_taker_fee`，否则从 hard-coded `takerFees` map 读取 lag 交易所费率。
- `ExecutionConfig.EntrySpreadLimit()` 在 `max_entry_spread < 0` 时回退到 `trailing_stop`，保持旧配置兼容；配置了 `max_entry_spread` 后，开仓 spread gate 与 stoploss 回撤阈值解耦。
- `BBORecord.Size` 有默认值，但 fixed `SetContext()` 中 recorder / stream recorder 多处直接传 `100`；它更像 Go 版本的初始容量口径，不应在 `aquila` 中直接等价成热路径最大容量。
- `CalOpenQty()` 先用 `open_notional / price` 得到 raw quantity，再由 `FormatQty()` 按 `QuantityTick * ContractValue` 四舍五入归整。

### Aquila 链路对应

这一层属于启动期。一个 `LeadLagStrategy` 可以管理多个 pair，pair 用 `symbol_id` 直接索引，不在热路径遍历。建议拆成四类结构：

```text
LeadLagStrategyConfig
  name = "lead_lag"
  version = "1.0"
  strategy_id
  pairs: vector<LeadLagPairConfig>

LeadLagPairConfig
  symbol
  symbol_id
  lead_exchange
  lag_exchange
  open_notional
  lag_taker_fee
  trigger params
  execution params
  recorder params

LeadLagInstrumentMetadata
  symbol_id
  lag_exchange
  lag exchange symbol
  price_tick
  price_decimal_places
  quantity_step / contract quantity unit
  quantity_decimal_places
  notional_multiplier
  lag_taker_fee
  min/max quantity
  min_notional if needed
  price_limit flags/values if used

LeadLagRuntimeState
  pairs_by_symbol_id: vector<LeadLagPairSlot>

LeadLagPairSlot
  initialized
  pair config
  lag instrument metadata
  per-pair market / recorder / alignment / threshold / execution state
```

`pairs_by_symbol_id` 的 size 在启动期按配置中的最大 `symbol_id + 1` 分配。收到 `BookTicker` 后先按 `symbol_id` 过滤：如果 `symbol_id` 超出 vector 范围，或对应 slot 未初始化，直接返回。slot 初始化后，再用 `BookTicker.exchange` 判断它是该 pair 的 lead 还是 lag；如果既不是 lead 也不是 lag，也直接返回。

同一个 `symbol_id` 不支持配置多个 pair，启动期遇到重复 `symbol_id` 直接拒绝。这个约束让热路径保持：

```text
BookTicker.symbol_id -> O(1) PairSlot lookup
BookTicker.exchange  -> Lead / Lag role
```

pair config 同时写 `symbol` 和 `symbol_id`。`symbol` 让配置可读，`symbol_id` 服务热路径。启动期必须校验：

```text
lead_info = catalog.Find(lead_exchange, symbol)
lag_info  = catalog.Find(lag_exchange, symbol)

lead_info exists
lag_info exists
lead_info.symbol_id == configured symbol_id
lag_info.symbol_id == configured symbol_id
```

交易基础信息来自 `instrument_catalog`。当前 `InstrumentInfo` 已包含 `exchange_symbol`、`price_tick`、`price_decimal_places`、`quantity_step`、`quantity_decimal_places`、`min_quantity`、`max_quantity`、`max_market_quantity`、`min_notional` 和 `notional_multiplier`。启动期使用 lag 交易所的 `InstrumentInfo` 构建一份压缩后的 `LeadLagInstrumentMetadata`，策略运行期只持有这份 metadata，不保存或依赖 `const InstrumentInfo*`。

费用当前不是 `InstrumentInfo` 字段。`lag_taker_fee` 第一版从 pair config、fee config 或同一启动期 metadata loader 注入；后续可以扩展 catalog schema 或接入账户费率 metadata。无论来源如何，fee 必须在启动期成为 pair 的稳定数值，不让 signal 层临时查询。

对 Gate futures，`open_notional` 到交易所原生下单 quantity 的换算必须使用 `notional_multiplier`：

```text
notional ~= price * quantity * notional_multiplier
quantity ~= open_notional / (price * notional_multiplier)
```

随后 quantity 再按 `quantity_step` / `quantity_decimal_places` 归整，并检查 `min_quantity` / `max_quantity` / `min_notional` 等约束。

热路径不做 string exchange 比较、string symbol 比较、TOML/JSON 查表、REST metadata 查询或交易所差异判断。热路径可直接使用的预计算字段包括：

```text
lead_exchange
lag_exchange
symbol_id
open_notional
lag_taker_fee
trailing_stop
max_entry_spread
lag_part
drift_limit
parallel
price_tick / quantity_step / notional_multiplier
window_ns / stats_window_ns / drift_period_ns / drift_warmup_ns
drift_min_samples
quantile_move
target_profit_rate
```

### 验证口径

- 配置默认值与 fixed `SetDefault()` 一致。
- fixed JSON 字段能映射到对应 config 字段。
- pair config 中的 `symbol` 与 `symbol_id` 必须和 lead / lag 两边 catalog 记录一致。
- `open` 在 `aquila` config 中命名为 `open_notional`，继续表示每次开仓目标名义金额。
- `open_notional` 到下单 quantity 的转换必须使用 `notional_multiplier`，不能直接用 `open_notional / price` 作为 Gate quantity。
- 策略运行期持有压缩后的 `LeadLagInstrumentMetadata`，不保存完整 `InstrumentInfo*`。
- lag metadata 必须从 instrument catalog 解析出 quantity step、notional multiplier、price tick 等下单必要信息；fee 如进入成本模型，也必须从启动期 metadata 获得。
- lead exchange 和 lag exchange 不允许相同。
- 同一个 `symbol_id` 不允许重复配置 pair。
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

- raw lead / raw lag price 变化时必须先更新；same-price tick 不替换已保存 raw quote。
- drift 样本只要求两边 raw BBO 都有效，不要求 Active。
- Active 前不允许交易侧 recorder / signal 被污染。
- 重复价格 tick 仍会用已保存的 raw quote 推进 drift / alignment；Active 后通常不推进交易链路，Active 切换恢复 tick 除外。
- lead tick 主要驱动 threshold、开仓和 lead-driven close。
- lag tick 主要驱动 spread/noise、trailing stop 和 lag-driven close。

### Aquila 链路对应

这一层位于 Strategy 线程：

```text
DataReader.Poll()
  -> leadlag::Strategy::OnBookTicker(BookTicker)
  -> pairs_by_symbol_id[ticker.symbol_id]
  -> leadlag::Pair::OnBookTicker(BookTicker)
```

`leadlag` 作为 namespace 名使用；本 namespace 内类型不再加 `LeadLag` 前缀。raw market state 不保存完整 `BookTicker`，只保存 fixed 策略计算需要的压缩 quote。建议状态：

```text
namespace aquila::strategy::leadlag

QuoteSnapshot
  local_ns
  bid_price
  ask_price

PriceSnapshot
  bid_price
  ask_price

MarketSideState
  latest_quote: QuoteSnapshot
  prev_price: PriceSnapshot
  has_quote

PairMarketState
  lead: MarketSideState
  lag: MarketSideState
  last_event_ns

MarketSideUpdate
  price_changed
```

字段语义：

- `latest_quote` 是该 side 当前最新可用 BBO 快照，用于 drift、recorder 和 signal。
- `prev_price` 是更新前上一笔同 side quote 的 bid/ask，只服务 `price_changed` 判断和诊断。
- `has_quote` 表示该 side 是否收到过至少一笔有效 quote；`lead.has_quote && lag.has_quote` 后才允许更新 drift。
- `last_event_ns` 使用当前 `BookTicker.local_ns`。

fixed 的 `bbo.exchange` 判断在 `aquila` 中直接使用 `BookTicker.exchange`。`DataReader` 配置负责 attach 哪些 SHM channel；策略热路径不依赖 `DataReader` 的 source index。

```text
if ticker.symbol_id is outside pairs_by_symbol_id range:
    return
if pairs_by_symbol_id[ticker.symbol_id] is not initialized:
    return

pair = pairs_by_symbol_id[ticker.symbol_id]
if ticker.exchange == pair.lead_exchange:
    update lead raw state
elif ticker.exchange == pair.lag_exchange:
    update lag raw state
else:
    return
```

时间口径第一版已定为使用策略线程本地时间推进窗口和 warmup；测试和 replay 可以通过注入 clock 保持可对账。

Go 源码中 `Context.Now()` 驱动 drift / alignment warmup、`MoveQueue` roll、noise、spread 和 order 日志时间；`BBOVolRecorder.OnBBO()` 例外，它用 `bbo.ServerTime` 做 extrema 窗口淘汰。具体到现有 `BookTicker`，`aquila` 第一版 live 和 replay 默认都用 `ticker.local_ns` 作为策略时间 `now`；`exchange_ns` 暂时只作为诊断字段。若后续要做 byte-for-byte fixed replay 对账，应单独评估是否给 BBO extrema 注入 fixed-compatible server timestamp。

side 更新语义：

```text
UpdateMarketSide(side, ticker):
    if !side.has_quote:
        side.latest_quote = {ticker.local_ns, ticker.bid_price, ticker.ask_price}
        side.prev_price = {ticker.bid_price, ticker.ask_price}
        side.has_quote = true
        return {price_changed = true}

    if ticker.bid_price == side.latest_quote.bid_price &&
       ticker.ask_price == side.latest_quote.ask_price:
        return {price_changed = false}

    side.prev_price = {
        side.latest_quote.bid_price,
        side.latest_quote.ask_price
    }
    side.latest_quote = {ticker.local_ns, ticker.bid_price, ticker.ask_price}
    return {price_changed = true}
```

`price_changed` 只比较 bid / ask price，不比较 volume。重复价格 tick 不替换 quote 时间戳或数量，但仍会在两边 valid 后用旧 quote 产生 paired drift 样本并推进 alignment；Active 后如果 `price_changed=false` 且不是 Active 切换恢复 tick，则不推进 recorder / threshold / signal。

### 验证口径

- lead tick 只更新 lead raw。
- lag tick 只更新 lag raw。
- 两边未同时 valid 前不产生 drift 样本。
- raw state 不保存完整 `BookTicker`，只保存 `QuoteSnapshot{local_ns, bid_price, ask_price}` 和 `prev_price`。
- 策略窗口时间使用 `BookTicker.local_ns`，不是 `exchange_ns`。
- 超出 `pairs_by_symbol_id` 范围的 `symbol_id` 直接返回。
- slot 未初始化的 `symbol_id` 直接返回。
- exchange 既不是该 pair lead 也不是 lag 时直接返回。
- 相同 bid/ask 的重复 tick 标记为 `price_changed=false`。
- 相同 bid/ask 的重复 tick 不替换 latest quote，但两边 valid 后仍增加 drift sample。
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

`leadlag` namespace 内类型不加 `LeadLag` 前缀。统计层已对照 fixed Go 源码确认真实窗口语义；`aquila` 实现不需要逐字复制 Go 容器，但输出必须能和 fixed replay 对齐。

建议状态：

```text
RecorderState
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

#### Go 源码确认：BBO extrema

`BBOVolRecorder` 的语义是 rolling 最近 `bbo_record.window` 内的 bid / ask 极值。current fixed 常用配置中：

```text
bbo_record.window = 1s
```

每个 pair 持有两个 extrema window：

```text
lead_extrema -> lead side bid_min / bid_max / ask_min / ask_max
lag_extrema  -> lag side bid_min / bid_max / ask_min / ask_max
```

fixed Go 更新位置：

```text
Active lead tick -> leadContext.recorder.OnBBO(bbo.ServerTime, ask, bid)
Active lag tick  -> lagContext.recorder.OnBBO(bbo.ServerTime, ask, bid)
```

fixed Go 内部结构：

```text
BBOVolRecorder
  Ask: MonotonicQueue<PricePoint>
  Bid: MonotonicQueue<PricePoint>
  Times: Queue<int64>
  Window: milliseconds

OnBBO(milliTs, ask, bid):
  Times.Put(milliTs)
  Ask.Put({milliTs, ask})
  Bid.Put({milliTs, bid})
  while milliTs - Times.Tail() > Window:
      Times.Pop()
      Ask.Pop()
      Bid.Pop()
```

注意：fixed Go 这里使用 `bbo.ServerTime`，而非 `Context.Now()`。`aquila` 第一版如果统一使用 `BookTicker.local_ns` 驱动全部策略窗口，这是一个有意的工程取舍；严格 fixed replay 对账时需要单独验证该差异。

`aquila` 实现选择：

```text
BboExtremaWindow
  window_ns = bbo_record.window
  implementation = fixed-capacity monotonic deque
  storage = preallocated vector
  default capacity = 16 * 1024 samples
```

容量统一放入运行期 capacity config，配置文件可选覆盖；未配置时使用默认值：

```text
RuntimeCapacityConfig
  extrema_window_capacity = 16 * 1024
  move_queue_capacity = 16 * 1024
  noise_window_capacity = 16 * 1024
  spread_window_capacity = 16 * 1024
```

容量不足时不在热路径扩容，也不静默覆盖。它只影响新开仓信号完整性，不阻断已有持仓处理：

```text
on extrema capacity overrun:
  record diagnostics
  disable new opens for this pair
  continue raw / drift / alignment
  continue close / stoploss / feedback
  reset and rewarm affected recorder
  after a full bbo_record.window is rebuilt, allow new opens again
```

#### Go 源码确认：MoveQueue

`MoveQueue` 是切窗，不是严格 rolling 最近 `stats_window`。它持有当前窗口内的 `Up` / `Down` 两个 slice：

```text
MoveQueue
  RollAt
  Window = stats_window
  Up: []float64
  Down: []float64

Init(t, window):
  Window = window
  Up = []
  Down = []
  Roll(t)

ShouldRoll(t):
  return t.After(RollAt)

Roll(t):
  Up = Up[:0]
  Down = Down[:0]
  RollAt = t.Truncate(Window).Add(Window)
```

`UpdateMoveThreshold()` 的源码顺序是：

```text
if MoveQueue.ShouldRoll(now):
    LeadNoise = lead_volema.GetStdEma()
    LagNoise = lag_volema.GetStdEma()
    MoveQueue.Sort()
    up, down = MoveQueue.Quantile(quantile.move)
    exit = DriftStdEma.GetMean()
    update UpEntry / UpExit / DownEntry / DownExit
    MoveQueue.Roll(now)

MoveQueue.Push(
    lead_bid / bidMin - 1,
    lead_ask / askMax - 1
)
```

确认行为：

- Move sample 只在 lead active tick 上产生，不在 lag tick 上产生。
- 每个 lead active tick 产生两个值：`up_move = lead_bid / lead_bid_min - 1`，`down_move = lead_ask / lead_ask_max - 1`。
- `lead_bid_min` / `lead_ask_max` 来自已经更新过的 `BBOVolRecorder`。
- roll 判断先于当前 tick 的 push，因此当前 tick 的 move 不参与刚刚 roll 出来的 threshold。
- roll 使用 `t.After(RollAt)`，等于边界时不 roll，严格晚于边界才 roll。
- roll 后 `Up` / `Down` 清空，下一行再 push 当前 tick，进入新窗口。
- quantile 使用 `gonum/stat.Quantile` 的 `stat.Empirical` 模式；`up = Quantile(p, Up)`，`down = Quantile(1-p, Down)`。

`aquila` 第一版建议用 fixed-capacity exact切窗 queue + sort reference 复刻输出语义。后续如为了热路径稳定性改成 fixed-bin histogram，必须保留 replay 对照，明确记录 quantile 近似误差。

#### Go 源码确认：StreamRecorder / noise / spread

`StreamRecorder` 是按时间淘汰的 rolling recorder，不是切窗 roll：

```text
StreamRecorder
  period
  count
  total
  sqTotal
  data queue
  time queue

OnData(now, data):
  push data and now
  while now - oldest_time > period:
      pop oldest data/time
  mean = total / count

GetStd():
  sqrt(E[x^2] - E[x]^2)
```

fixed Go 的底层 queue 满了会动态扩容；`aquila` 不应这样做，应使用启动期 capacity 并在 overrun 时显式降级或拒绝新开仓。

`StreamStdEmaRecorder` 名字带 `Ema`，但源码不是指数 EMA。它是两层 rolling mean：

```text
items = StreamRecorder(window = bbo_record.window)
ema   = StreamRecorder(period = bbo_record.stats_window)

OnData(t, mid):
  items.OnData(t, mid)
  std = items.GetStd()
  mean = items.GetMean()
  if std is not NaN and mean != 0:
      ema.OnData(t, std / mean)

GetStdEma():
  return ema.GetMean()
```

因此：

- `LeadNoise` 是 lead active tick 中 lead mid 的 `rolling_mean(std(mid over window) / mean(mid over window))`。
- `LagNoise` 是 lag active tick 中 lag mid 的同一公式。
- `LeadNoise` / `LagNoise` 只在 `MoveQueue` roll 时冻结到 threshold state；非 roll tick 只继续更新 recorder。

`lagContext.spread` 是 `StreamRecorder(stats_window)`，输入是绝对 spread：

```text
OnLagBBO:
  spread.OnData(now, lag_ask - lag_bid)

LagSpreadBuffer(lag_bbo):
  current = lag_ask - lag_bid
  history = spread.GetMean()
  return max(current - history, 0)
```

`BuildEntryCostBreakdown()` 只把 `LagSpreadBuffer / triggerPrice` 记录为 embedded friction；`RequiredEdge()` 不包含 spread 或 lag spread buffer，避免与 `targetSpace` 中已经扣掉的价格摩擦重复计算。

### 验证口径

- Active 前输入 BBO 不改变交易 recorder。
- `SeedActive()` 后 recorder 初始极值等于 seed。
- lead tick 只更新 lead recorder / lead noise / move queue。
- lag tick 只更新 lag recorder / lag spread / lag noise。
- BBO extrema 输出 rolling `bbo_record.window` 内的 bid / ask min/max。
- BBO extrema capacity overrun 只禁用新开仓，不阻断 close / stoploss / feedback。
- MoveQueue 按 `stats_window` 切窗，roll 后清空；不是严格 rolling 最近 `stats_window`。
- MoveQueue 在边界时间 `t == RollAt` 不 roll，`t > RollAt` 才 roll。
- roll 时用旧 queue 计算 quantile，当前 lead tick 的 move 进入新窗口。
- `LeadNoise` / `LagNoise` 是 rolling normalized std 的 rolling mean，不是指数 EMA。
- `lag_spread_mean` 是 absolute spread 的 `StreamRecorder(stats_window)` mean。
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
now - firstPairedDriftTs >= alignment_warmup
```

其中 `alignment_warmup` 的 fixed Go 规则是：

```text
if drift_warmup > 0:
    alignment_warmup = drift_warmup
else:
    alignment_warmup = bbo_record.stats_window
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
- `drift_warmup=0` 时使用 `stats_window` 作为 alignment warmup。
- same-price raw tick 不替换 raw quote，但仍可推进 drift sample 和 alignment phase。

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
- `MoveQueue.ShouldRoll(now)` 使用 `now > RollAt`，不是 `now >= RollAt`。
- `MoveQueue.Roll(now)` 把 `RollAt` 设置为 `now.Truncate(stats_window) + stats_window`，并清空当前 move samples。
- `LeadNoise` / `LagNoise` 是从 recorder 读出的 frozen snapshot；非 roll tick 不改变 threshold 中保存的 noise 值。

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
- roll 边界使用严格大于语义。
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

源码中的成本模型边界：

```text
EntryCostBreakdown.RequiredEdge()
  = Fee + LeadNoise + LagNoise

EntryCostBreakdown.EmbeddedPriceFriction()
  = Spread + LagSpreadBuffer
```

`targetPrice` 已经扣除或加入 `LagSpreadBuffer`，`targetSpace` 也已经含有当前 bid/ask 价差影响，因此开仓门槛只比较 `targetSpace` 与 `RequiredEdgeWithTargetProfit(target_profit_rate)`。`Spread` 和 `LagSpreadBuffer` 会进入 attribution / diagnostics，但不重复进入 `RequiredEdge()`。

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
- fixed Go 只有 order terminal feedback 能推进 position 和 stage；非 terminal update 只更新 pending order 的 filled/status 字段。
- 开仓 IOC 完全未成交：`Open -> Idle`，cache 删除。
- 开仓部分成交：`Open -> Hold`，position 为成交量。
- 开仓完全成交：`Open -> Hold`，trailing 初始化为 avg fill price。
- 平仓完全成交：`Close -> Idle`，cache 删除。
- 平仓部分成交：`Close -> Hold`，保留剩余 position。
- rejected open：无 position 时删除 cache。
- rejected close：已有 position 时回到 `Hold`。
- filled quantity 按 quantity step / notional multiplier 归整。

### Aquila 链路对应

fixed 的 `OnOrder(msg)` 在 `aquila` 中应对应：

```text
OrderManager::OnOrderFeedback(OrderFeedbackEvent)
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
  update cumulative filled diagnostics / pending order fill
  fixed-compatible execution cache position waits for terminal event
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
- 非 terminal partial feedback 不提前切 stage，除非后续明确选择偏离 fixed 以提升实盘风险可见性。
- 平仓完全成交：cache 删除。
- 平仓部分成交：回到 Hold，剩余 position 正确。
- rejected open：无 position 时删除 cache。
- rejected close：已有 position 时回到 Hold。
- filled quantity 按 quantity step / notional multiplier 归整。
- pending order 存在时 signal 不重复发 close。
- accepted event 更新 exchange order id，并通知 OrderSession cancel cache。
- terminal event 清理 OrderSession cancel cache。
- feedback gap 后暂停新开仓。

## 阶段性实现边界

第一版目标是行为可对齐，不追求一次性完成全部生产能力：

- 支持单 strategy 管理多个 lead-lag pair；每个 `symbol_id` 最多一个 pair。
- 支持 fixed 的 alignment、threshold、open/close/stoploss 语义。
- 支持通过 fake feedback 推进状态机。
- 对接真实 Gate feedback 前，不把 OrderSession ack 当成成交事实。
- 不在第一版实现 REST reconcile、multi-symbol portfolio、跨交易所下单、batch/amend/cancel-all。

## 后续分析顺序

建议按本文 7 层逐段审阅：

1. 配置与 instrument catalog metadata 已定为 `lead_lag` / `version="1.0"` / `config/strategy/lead_lag.toml` / pair array；后续实现需按本节验证口径落 parser。
2. raw market state 的时间、`symbol_id` vector lookup 和 `BookTicker.exchange` role 判断是否符合 `aquila` 当前 DataReader。
3. recorder / queue 输出语义按 fixed Go 对齐；实现可不同，但必须覆盖切窗 MoveQueue、rolling normalized std mean 和 absolute spread mean。
4. alignment readiness 使用 `drift_warmup`，未配置时回退到 `stats_window`。
5. threshold 符号和 roll 顺序是否确认。
6. signal 的成本模型、spread buffer、stoploss 价格是否确认。
7. feedback 事件到 execution cache 的状态推进是否确认。
