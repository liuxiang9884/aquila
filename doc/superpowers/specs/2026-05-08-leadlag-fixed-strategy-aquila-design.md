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

截至 2026-05-10，本文 1-7 部分整体仍是 LeadLag 策略层设计，不表示完整策略 C++ 实现已经完成。当前仓库已有的是 `config/strategy/lead_lag.toml`、`strategy/lead_lag/config.h`、`strategy/lead_lag/config.cpp`、`strategy/lead_lag/types.h`、`strategy/lead_lag/raw_market_state.h`、`test/strategy/lead_lag_config_test.cpp`、`test/strategy/lead_lag_raw_market_state_test.cpp`，以及 `core/base/` 中可复用的通用底层结构；recorder 组合层、drift / alignment、threshold、signal、execution feedback state 和整体 `leadlag::Strategy` 尚未实现。

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
| 3. Recorder / queue / noise | `BBOVolRecorder`、`StreamRecorder`、`StreamStdEmaRecorder`、`MoveQueue`、`LagSpreadBuffer()` | BBO extrema 是时间窗口 monotonic queue；`MoveQueue` 是按 `stats_window` 时间边界切窗并 roll 后清空；Aquila 设计中的低延迟路径默认使用 fixed-bin histogram 近似 quantile，dual heap 保留为单 quantile exact / replay 对照口径；`StreamStdEmaRecorder` 名字带 EMA，但实现是 rolling normalized std 的 rolling mean；lag spread 是绝对 spread 的 `StreamRecorder(stats_window)` 均值。 |
| 4. Drift / alignment | `UpdateDrift()`、`alignmentWarmup()`、`alignmentReady()`、`enterActivePhase()` | drift 用 `(lag_ask + lag_bid) / (lead_ask + lead_bid)`；`drift_warmup > 0` 时用配置值，否则回退到 `stats_window`；进入 Active 后重置并播种交易侧 recorder。 |
| 5. Threshold | `UpdateMoveThreshold()`、`ProfitBuffer()` | roll 时先冻结 noise、用旧 move queue 计算 quantile / threshold，再清空 queue，最后 push 当前 lead tick 的 move；`profitBuffer = fee*2 + LeadNoise + LagNoise`。 |
| 6. Signal | `OnLeadBBO()`、`OnLagBBO()`、`OpenLong()`、`OpenShort()`、`CloseLong()`、`CloseShort()`、`ExecuteStoploss*()` | lead tick 顺序是更新 recorder / threshold、先平仓再开仓；lag tick 顺序是 spread / noise、trailing、stoploss、signal close；`drift_limit` 只限制新开仓。 |
| 7. Order / feedback state | `ExecuteCache`、`registerCacheOrder()`、`OnOrder()`、`GetOrderFilled()` | 发送订单只设置 pending；只有 terminal order feedback 推进 position / stage；position 归零删除 cache，非零进入 Hold。 |

## 1. 配置与 Symbol Metadata

### Aquila 第一版设计决定

第一版策略名固定为 `lead_lag`。runtime config 仍使用 `[strategy]`，其中：

```toml
[strategy]
name = "lead_lag"
config = "config/strategy/lead_lag.toml"
```

user strategy config 设计上固定放在 `config/strategy/lead_lag.toml`，root table 为 `[lead_lag]`，并显式写入 `version = "1.0"`。`version` 使用字符串保存，避免后续 `1.10` 一类版本被 TOML 数值语义改写。当前仓库已实现 `strategy/lead_lag/config.h` / `config.cpp` 解析该 TOML，并在启动期用 instrument catalog 校验 lead / lag metadata；`leadlag::Strategy` 构造路径尚未实现。

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
- 第 5 部分 threshold engine 落地时，`trigger.quantile` 需要在 `move` 之外补充 histogram range / precision；第一版建议显式配置 up / down 两侧范围和绝对精度，不从历史数据或 `trigger.lead` 隐式推导。

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
- Active 切换时需要 previous quote seed：如果当前变价 tick 触发 Active，recorder 用进入本 tick 前的 quote 播种，再让当前 tick 正常进入 active handler，避免第一笔 move 被吃掉。
- 如果 Active 是 lag tick 触发，fixed 会设置 `resumeLeadTick`；下一笔 lead tick 即使 same-price，也允许进入一次 lead handler，之后清掉该 flag。
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

MarketSideState
  latest_quote: QuoteSnapshot
  previous_quote: QuoteSnapshot
  has_quote
  has_previous_quote

PairMarketState
  lead: MarketSideState
  lag: MarketSideState
  last_event_ns

MarketSideUpdate
  tracked
  price_changed
  role
```

字段语义：

- `latest_quote` 是该 side 当前最新可用 BBO 快照，用于 drift、recorder 和 signal。
- `previous_quote` 是更新前上一笔同 side quote 的完整压缩快照，服务 Active 切换时的 recorder seed；它必须保留时间，不只是 bid/ask price。
- `has_quote` 表示该 side 是否收到过至少一笔有效 quote；`lead.has_quote && lag.has_quote` 后才允许更新 drift。
- `has_previous_quote` 表示该 side 是否存在可用于 Active seed 的上一笔 quote。
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

这一层的 Go / Aquila 差异需要显式保留：

- Go 用 exchange string 路由；Aquila 热路径用 `BookTicker.symbol_id` vector lookup 加 `Exchange` enum role 判断。
- Go 保存并传递完整 `utils.BBO`；Aquila raw state 只保存 `QuoteSnapshot`，不保存 volume、symbol string、metadata 或完整 `BookTicker`。
- Go 的 `OnLeadBBO()` 会原地修改传入 BBO 的 drifted price；Aquila 不修改 SHM 输入事件，drifted lead 作为单独 snapshot 写入 pair state。
- 当前 `DataReader` live config 使用 `read_mode = "latest"`，可能跳过中间行情；因此 same-price 推进 drift / alignment 只对策略实际收到的事件成立。严格 fixed replay 对账应使用专门 replay feed 或 drain/direct 输入口径，不在 live 热路径做跨 source 排序。

side 更新语义：

```text
UpdateMarketSide(side, ticker):
    if !side.has_quote:
        side.latest_quote = {ticker.local_ns, ticker.bid_price, ticker.ask_price}
        side.has_quote = true
        return {tracked = true, price_changed = true}

    if ticker.bid_price == side.latest_quote.bid_price &&
       ticker.ask_price == side.latest_quote.ask_price:
        return {tracked = true, price_changed = false}

    side.previous_quote = side.latest_quote
    side.has_previous_quote = true
    side.latest_quote = {ticker.local_ns, ticker.bid_price, ticker.ask_price}
    return {tracked = true, price_changed = true}
```

`price_changed` 只比较 bid / ask price，不比较 volume。重复价格 tick 不替换 quote 时间戳或数量，但仍会在两边 valid 后用旧 quote 产生 paired drift 样本并推进 alignment；Active 后如果 `price_changed=false` 且不是 Active 切换恢复 tick，则不推进 recorder / threshold / signal。

Active 切换 seed 语义按 fixed 复刻，但使用 `QuoteSnapshot`：

```text
lead_seed = lead.latest_quote
lag_seed = lag.latest_quote

if role == Lead && price_changed && lead.has_previous_quote:
    lead_seed = lead.previous_quote

if role != Lead && lead.has_previous_quote:
    lead_seed = lead.previous_quote

if role == Lag && price_changed && lag.has_previous_quote:
    lag_seed = lag.previous_quote

resume_lead_tick = role != Lead
SeedActiveRecorders(lead_seed, lag_seed, now)
```

Active handler gating：

```text
allow_resume_lead = role == Lead && resume_lead_tick

if !price_changed && prev_phase == Active && !allow_resume_lead:
    return

if allow_resume_lead:
    resume_lead_tick = false
```

这个规则把三个边界固定下来：

- same-price：Aligning 阶段不替换 raw quote，但仍可推进 drift / alignment。
- previous quote seed：Active 初始化 recorder 使用进入当前变价 tick 前的 quote，避免吞掉第一笔 active move。
- resume lead：lag tick 触发 Active 后，下一笔 lead tick 即使 same-price 也允许恢复一次 lead 交易链路。

### 验证口径

- lead tick 只更新 lead raw。
- lag tick 只更新 lag raw。
- 两边未同时 valid 前不产生 drift 样本。
- raw state 不保存完整 `BookTicker`，只保存 `QuoteSnapshot{local_ns, bid_price, ask_price}`、`previous_quote` 和对应 valid flag。
- 策略窗口时间使用 `BookTicker.local_ns`，不是 `exchange_ns`。
- 超出 `pairs_by_symbol_id` 范围的 `symbol_id` 直接返回。
- slot 未初始化的 `symbol_id` 直接返回。
- exchange 既不是该 pair lead 也不是 lag 时直接返回。
- 相同 bid/ask 的重复 tick 标记为 `price_changed=false`。
- 相同 bid/ask 的重复 tick 不替换 latest quote，但两边 valid 后仍增加 drift sample。
- 变价 tick 更新 `previous_quote` 和 `latest_quote`；same-price tick 不更新 `previous_quote`。
- Active 前不调用 threshold / signal。
- 当前变价 tick 触发 Active 时，seed 使用 previous quote，当前 tick 继续进入对应 active handler。
- lag tick 触发 Active 时，下一笔 lead tick 即使 same-price 也允许进入一次 lead handler，并清掉 `resume_lead_tick`。
- 已 Active 的普通 same-price tick 不进入 recorder / threshold / signal。

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

`queue.MonotonicQueue` 是 fixed Go 的本地自定义实现，不是外部库。它内部包含全量 FIFO queue、max 候选 queue 和 min 候选 queue：

```text
MonotonicQueue
  Queue
  Max
  Min

Put(value):
  Queue.Put(value)
  while Max.back < value:
      Max.PopTail()
  Max.Put(value)
  while Min.back > value:
      Min.PopTail()
  Min.Put(value)

Pop():
  value = Queue.Pop()
  if Max.front == value:
      Max.Pop()
  if Min.front == value:
      Min.Pop()
```

比较 price 时使用严格 `<` / `>`，相等价格不会在 push 时被挤掉；重复价格按 FIFO 顺序保留。正常 `OnBBO()` 是摊还 `O(1)`，`GetMin()` / `GetMax()` 是 `O(1)`；Go 容量不足时会动态扩容并 copy ring，当次扩容是 `O(n)`。

注意：fixed Go 这里使用 `bbo.ServerTime`，而非 `Context.Now()`。`aquila` 第一版如果统一使用 `BookTicker.local_ns` 驱动全部策略窗口，这是一个有意的工程取舍；严格 fixed replay 对账时需要单独验证该差异。

`aquila` 设计选择：

```text
BboExtremaWindow
  window_ns = bbo_record.window
  implementation = vector-backed monotonic deque
  initial capacity = extrema_window_capacity

  samples
  bid_min
  bid_max
  ask_min
  ask_max

RecorderStats
  extrema_capacity_grow_count
  ring_queue_capacity_grow_count
  move_quantile_capacity_grow_count
```

更新输入：

```text
OnLeadActiveTick(drifted_lead):
    lead_extrema.Update(drifted_lead.local_ns, drifted_bid, drifted_ask)

OnLagActiveTick(raw_lag):
    lag_extrema.Update(raw_lag.local_ns, lag_bid, lag_ask)
```

`lead_extrema` 的输入是 drifted lead quote，`lag_extrema` 的输入是 raw lag quote。进入 Active 时先 reset trading recorder，再用 seed quote 播种 extrema：

```text
lead_extrema.Reset()
lag_extrema.Reset()

lead_extrema.Update(lead_seed.local_ns, drifted_lead_seed_bid, drifted_lead_seed_ask)
lag_extrema.Update(lag_seed.local_ns, lag_seed_bid, lag_seed_ask)
```

Aquila 的 monotonic deque 同样保持：

```text
Update() amortized O(1)
GetMin()/GetMax() O(1)
space O(window sample count)
```

每个内部 storage 使用 `std::vector` 并在启动期 `reserve(extrema_window_capacity)`。vector 允许自动扩容，保证计算准确性；扩容不是错误，不丢样本、不 reset recorder、不暂停新开仓。每次 vector 实际扩容后：

```text
++stats.extrema_capacity_grow_count
log symbol_id, exchange, vector name, new vector capacity
```

`Stats` 只记录统计数字，不保存结构化扩容事件；具体哪个 vector、哪个 symbol / exchange、扩容后 capacity 只写入 log。扩容是后续调大初始 capacity 的依据。

容量统一放入运行期 capacity config，配置文件可选覆盖；未配置时使用默认值：

```text
RuntimeCapacityConfig
  extrema_window_capacity = 16 * 1024
  move_queue_capacity = 16 * 1024
  noise_window_capacity = 16 * 1024
  spread_window_capacity = 16 * 1024
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

`gonum/stat.Quantile(..., stat.Empirical, ..., nil)` 要求输入升序；`Empirical` 返回第一个使经验 CDF 达到 `p` 的样本值，不做线性插值。无权重、非空样本下等价于：

```text
k = max(1, ceil(p * n))
quantile = sorted[k - 1]
```

Go 的 `Qunatile()` 在某一侧样本为空时返回 `0`，因为只在 `len(slice) > 0` 时调用 `stat.Quantile()`。

Go 时间复杂度：

```text
normal active lead tick: append Up/Down, amortized O(1)
roll tick: sort Up + sort Down, O(n log n)
quantile after sort: O(1)
space: O(n)
```

其中 `n` 是当前 `stats_window` 切窗内 active lead tick 数。这个实现总成本可接受，但 roll tick 会集中承担排序开销，导致主路径 tail spike。

#### Aquila exact quantile 方案：dual heap

`DoubleHeap<T>` 可以维护单个 exact empirical quantile，避免在 roll tick 做 `O(n log n)` 排序。它保留 Go 的切窗语义和 empirical quantile 语义，但把计算成本摊到每个 active lead tick。

```text
MoveQuantileWindow
  roll_at_ns
  window_ns = bbo_record.stats_window
  up:   OnlineEmpiricalQuantile(p = quantile.move)
  down: OnlineEmpiricalQuantile(p = 1 - quantile.move)
```

每个 `OnlineEmpiricalQuantile` 维护两个 heap：

```text
OnlineEmpiricalQuantile
  p
  count
  lower: max-heap  // 最小的 max(1, ceil(p * count)) 个样本
  upper: min-heap  // 剩余样本
```

不变量：

```text
lower.size == max(1, ceil(p * count)) when count > 0
all(lower) <= all(upper)
Value() = lower.top()
```

`Value()` 在没有样本时返回 `0`，对齐 Go 的空 slice 返回值。`Reset()` 只清 logical size，保留 vector capacity。

底层不用 `std::priority_queue`，而使用：

```text
std::vector<double> + std::push_heap / std::pop_heap
```

原因是 `aquila` 需要直接控制 `reserve()`、capacity grow 统计、扩容日志和 reset 语义。初始化时按 `move_queue_capacity` 对 4 个 heap vector 预留容量：

```text
up.lower
up.upper
down.lower
down.upper
```

为避免单个 heap 在偏高或偏低 quantile 下超过预留，第一版可让每个 heap 都 `reserve(move_queue_capacity)`；这样只有当前切窗样本数超过该容量时才可能在热路径扩容。内存代价是每个 pair 约：

```text
4 * move_queue_capacity * sizeof(double)
```

以 `move_queue_capacity = 16 * 1024` 计算约 512 KiB。

更新过程：

```text
if now > roll_at_ns:
    up = up_quantile.Value()
    down = down_quantile.Value()
    update thresholds with old window quantiles
    Reset(now)  // clear heaps, keep capacity

up_quantile.Add(lead_bid / bid_min - 1)
down_quantile.Add(lead_ask / ask_max - 1)
```

当前 tick 仍然不参与刚 roll 出来的 threshold；roll 顺序与 fixed Go 一致。

复杂度：

```text
active lead tick: O(log n) worst/amortized
roll tick: O(1)
quantile read: O(1)
reset: O(1)
space: O(n)
```

这个方案不降低每个窗口的总复杂度阶数；它把 Go 的 roll 排序 spike 拆散到每个 active lead tick，使主路径延迟更稳定。

但 `DoubleHeap<T>` 只服务一个 quantile。若同一批 move 样本需要同时计算多组 quantile，例如 p50 / p60 / p75 / p90，当前只能为每个 quantile 各维护一组 `DoubleHeap<T>`：

```text
k quantiles -> k * DoubleHeap
update cost -> O(k * log n) per sample
memory      -> O(k * n)
```

因此 dual heap 适合单 quantile exact 场景，不适合作为 LeadLag 后续多 quantile threshold engine 的默认方案。

扩容记录按 3-2 / 3-3 的规则：

```text
++stats.move_quantile_capacity_grow_count
log symbol_id, exchange, vector name, new vector capacity
```

`RecorderStats` 只保存次数，不保存结构化扩容事件。具体是 `up.lower`、`up.upper`、`down.lower` 还是 `down.upper` 扩容，以及扩容后的 capacity，只写 log。扩容不丢样本、不暂停计算。

#### Aquila 默认选择：Histogram quantile

当前第 5 部分 threshold engine 只需要两个 quantile：上行 move 的 `p = quantile.move` 和下行 move 的 `p = 1 - quantile.move`。每个 histogram 只服务一个 quantile，不需要 `ValueMany()`。`aquila` 仍默认选择 `HistogramQuantile<T>` 作为 move quantile 的实现方向：它牺牲 exact empirical quantile，换取固定 range / bins 下的稳定低延迟查询和 reset。

Histogram 用固定 bins 近似 quantile。`range = [min_value, max_value]` 是固定数值坐标系，不是当前窗口真实价格上下界；当前窗口真实样本只会让其中一段 bins 的 counter 非零：

```text
HistogramQuantile
  min_value
  max_value
  bin_width = (max_value - min_value) / bins
  counts[bins]
  count
  underflow_count
  overflow_count
  touched_min_bin
  touched_max_bin
```

单 quantile 更新与查询复杂度：

```text
active lead tick: O(1)
roll quantile scan: O(touched_bins)
reset: O(touched_bins)
space: O(bins)
```

其中：

```text
touched_bins = touched_max_bin - touched_min_bin + 1
```

`Add(value)` 时直接通过 `(value - min_value) / bin_width` 算 bin index，更新 counter，并维护当前窗口触达的最小 / 最大 bin。`Value()` 只在 touched bin 区间内按 rank 扫描；`Reset()` 只对 touched bin 区间执行清零，并重置 touched 边界。因此当 configured range 较大、当前窗口实际发生区间较窄时，查询和 reset 都不会再扫完整 range。

查询 rank 仍保持 empirical quantile 语义：

```text
target = max(1, ceil(p * count))
```

当 `p <= 0.5` 时从 touched low 端向右累计；当 `p > 0.5` 时从 touched high 端向左累计，并使用：

```text
reverse_target = count - target + 1
```

两种方向返回的 bin 与 full forward scan 等价。当前 `core/base/histogram_quantile.h` 提供 `ValueScalar()`、`ValueAvx2()`、`ValueAvx512()`；本机 microbenchmark 显示 AVX2 是 10000 bins 场景下更稳的默认 SIMD 路径，AVX512 没有胜过 AVX2。

若后续策略确实需要从同一批样本读取多组 quantile，Histogram 仍能在同一个 `counts` 上完成。若 quantile 列表已排序，可以一遍扫描 bins 输出全部目标 quantile：

```text
add all samples: O(n)
read k quantiles: O(touched_bins + k)
space: O(bins)
```

这比维护 `k` 组 dual heap 更适合多 quantile 的低延迟路径。

在样本不发生 underflow / overflow 时，误差由 `bin_width` 控制。若返回 conservative edge：

```text
up quantile:   bin upper edge
down quantile: bin lower edge
```

则阈值方向更保守，绝对误差上界约为 `bin_width`。例如范围 `[0, 0.01]`、`4096` bins：

```text
bin_width = 0.01 / 4096 ~= 0.0000024414 = 0.0244 bp
```

Histogram 初始化后固定 range / bins 时不会在热路径分配内存；但它不是 exact empirical quantile，overflow / underflow 时误差不再受 `bin_width` 约束。因此 fixed replay 精确对齐仍应保留 exact 对照路径，设计中的低延迟路径默认使用 histogram，并通过 range / bins / overflow 统计验证误差边界。

已存在的通用 `core/base` 结论：

这些结论只说明底层可复用结构已经存在，不表示 `BboExtremaWindow`、`NoiseState`、`SpreadState` 或 `MoveQuantileWindow` 已经作为 LeadLag 策略层实现。

- `counts` 使用 `std::vector<std::uint32_t>` 预分配；`count`、underflow、overflow 使用 `std::uint64_t`。
- bins 数不强制 `bit_ceil`，按 range / precision 或 reference / bp error 精确计算；默认 bins 为 `4096`。
- 提供 `InitWithReferenceError()` 和 `InitWithRangePrecision()`，分别从 reference bp 误差、range 绝对精度推导 bins。
- `Reset()` 使用 touched bin 区间清零；当前代码用 `std::memset` 表达连续内存清零语义，但 benchmark 未显示相对 `std::fill(..., 0)` 有稳定收益。
- 不采用 epoch reset：明确知道窗口 roll 点时，直接 reset 更简单；epoch 会把额外 epoch 读写转移到更高频的 `Add()` / `Value()` 路径。
- 暂不实现 `ValueMany()`；第 5 部分 threshold engine 中每个 histogram 只查询一个 quantile，up histogram 查询 `quantile.move`，down histogram 查询 `1 - quantile.move`。

2026-05-10 本机 release microbenchmark，`10000 bins`、`p=0.6`、configured range `[900,1100]`、窗口样本 `[980,1015]`：

```text
ValueOnly, ns/query:
Scalar:  1549 -> 237   touched-bin scan ~6.5x
AVX2:     825 -> 125   touched-bin scan ~6.6x
AVX512:   878 -> 129   touched-bin scan ~6.8x

Value+Reset, ns:
Scalar:  2147 -> 566   touched-bin reset ~3.8x
AVX2:    1410 -> 454   touched-bin reset ~3.1x
AVX512:  1474 -> 465   touched-bin reset ~3.2x
```

这些数字是 `core_base_structures_benchmark` 的局部 microbenchmark，只用于指导 `HistogramQuantile<T>` 设计，不外推为完整 LeadLag 策略链路时延。

#### Go 源码确认：3-2 lead_noise / lag_noise

`lead_noise` / `lag_noise` 不使用单调队列。fixed Go 使用 `StreamStdEmaRecorder`，底层是两个 `StreamRecorder`：

```text
StreamStdEmaRecorder
  items: StreamRecorder(window = bbo_record.window)
  ema:   StreamRecorder(period = bbo_record.stats_window)
```

每个 `StreamRecorder` 是按时间淘汰的 rolling recorder，不是固定条数窗口，也不是切窗 roll：

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

Go 的 `StreamRecorder` 里面有两个本地自定义 FIFO queue：

```text
data queue.Queue[float64]
time queue.Queue[time.Time]
```

因此 `lead_noise` / `lag_noise` 一共使用：

```text
2 sides * 2 StreamRecorder per side * 2 queue per StreamRecorder = 8 Go queues
```

这些 queue 是 slice-backed ring，正常 push / pop 不分配；满了会动态扩容并 copy，当次扩容是 `O(n)`。

`StreamStdEmaRecorder` 名字带 `Ema`，但源码不是指数 EMA。它是两层 rolling mean：

```text
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

更新输入：

```text
Active lead tick:
  lead_mid = (drifted_lead_ask + drifted_lead_bid) / 2
  lead_noise.OnData(now, lead_mid)

Active lag tick:
  lag_mid = (lag_ask + lag_bid) / 2
  lag_noise.OnData(now, lag_mid)
```

Go 这里的 `now` 来自 `Context.Now()`，不是 `bbo.ServerTime`。淘汰条件是：

```text
now - oldest_time > period
```

等于窗口边界时保留。复杂度：

```text
OnData() amortized O(1)
GetMean() O(1)
GetStd() O(1)
space O(samples inside windows)
grow O(n)
```

`aquila` 设计选择：

```text
RingQueue<T>
  storage: std::vector<T>
  capacity: power of two
  mask = capacity - 1
  head
  size

TimedValue
  local_ns
  value

MeanStdWindow
  samples: RingQueue<TimedValue>
  sum
  sum_sq

MeanWindow
  samples: RingQueue<TimedValue>
  sum

NoiseState
  mid_window: MeanStdWindow     // period = bbo_record.window
  ratio_window: MeanWindow      // period = bbo_record.stats_window
```

`RingQueue<T>` 是纯底层数据结构，支持 struct 元素，不知道 mean / std / noise 语义。时间和值在 Aquila 合成一个 `TimedValue`，等价于 Go 的 `time queue + data queue` 同步 push/pop。`MeanStdWindow` / `MeanWindow` 负责时间窗口淘汰和 running sum；`NoiseState` 负责组合两个窗口。

每个 side 使用两个逻辑 ring queue：

```text
lead_noise.mid_window
lead_noise.ratio_window
lag_noise.mid_window
lag_noise.ratio_window
```

也就是 4 个 `RingQueue<TimedValue>`，等价覆盖 Go 的 8 个物理 queue。计算逻辑单独放在 recorder 层：

```text
UpdateNoise(state, now, mid):
    state.mid_window.OnData(now, mid)
    mean = state.mid_window.Mean()
    std = state.mid_window.Std()
    if mean != 0:
        state.ratio_window.OnData(now, std / mean)

NoiseLive(state):
    return state.ratio_window.Mean()
```

底层 `RingQueue<T>` 的 capacity 必须是 2 的次幂，索引用 `& mask`：

```text
index = (head + offset) & mask
```

初始化时把配置 capacity 归整到 next power of two，并 `resize(capacity)`。容量不足时由 `RingQueue` 控制扩容到 `capacity * 2`，重新按 FIFO 顺序 copy，`head = 0`，更新 `mask`。扩容后继续计算，保证准确性。

扩容记录按 3-1 extrema 的方式处理：

```text
++stats.ring_queue_capacity_grow_count
log symbol_id, exchange, queue name, new vector capacity
```

`Stats` 只记录次数，不保存结构化扩容事件；具体哪个 queue 扩容只写 log。

#### Go 源码确认：3-3 lag_spread / LagSpreadBuffer

`lagContext.spread` 是 `StreamRecorder(stats_window)`，输入是 lag 侧绝对 spread。它只在 active lag tick 更新，不在 lead tick 更新。

```text
OnLagBBO:
  spread.OnData(now, lag_ask - lag_bid)

LagSpreadBuffer(lag_bbo):
  current = lag_ask - lag_bid
  history = spread.GetMean()
  return max(current - history, 0)
```

Go 初始化：

```text
enterActivePhase:
  lagContext.spread.Init(statsWindow, 100)
```

所以 Go 使用：

```text
1 StreamRecorder
2 queue.Queue:
  data queue.Queue[float64]
  time queue.Queue[time.Time]
```

计算过程：

```text
OnLagBBO(now, lag_bid, lag_ask):
  spread = lag_ask - lag_bid
  count++
  total += spread
  sqTotal += spread * spread
  data.Put(spread)
  time.Put(now)
  while now - oldest_time > stats_window:
      old = data.Pop()
      time.Pop()
      count--
      total -= old
      sqTotal -= old * old
  mean = total / count
```

`lag_spread_mean = lagContext.spread.GetMean()`。`sqTotal` 在 Go 的 `StreamRecorder` 中也维护，但 3-3 只读取 mean，不读取 std。时间来源是 `Context.Now()`，不是 `bbo.ServerTime`。淘汰条件仍是严格 `>`，等于 `stats_window` 边界时保留。

复杂度：

```text
OnData() amortized O(1)
GetMean() O(1)
LagSpreadBuffer() O(1)
space O(lag active ticks inside stats_window)
grow O(n)
```

`aquila` 设计选择：

```text
SpreadState
  spread_window: MeanWindow  // period = bbo_record.stats_window
```

底层复用 3-2 的 `RingQueue<TimedValue>`：

```text
MeanWindow
  samples: RingQueue<TimedValue>
  sum
```

计算逻辑：

```text
UpdateLagSpread(state, now, lag_bid, lag_ask):
    spread = lag_ask - lag_bid
    state.spread_window.OnData(now, spread)

LagSpreadMean(state):
    return state.spread_window.Mean()

LagSpreadBuffer(state, lag_bid, lag_ask):
    current = lag_ask - lag_bid
    history = state.spread_window.Mean()
    return max(current - history, 0)
```

3-3 复用 3-2 的底层类型和扩容策略，但不和 noise 合并运行时 queue。运行期状态独立：

```text
lag_noise.ratio_window   // std(mid)/mean(mid) over stats_window
lag_spread.spread_window // absolute spread over stats_window
```

也就是复用类型，不合并实例。扩容仍由 `RingQueue<T>` 控制，capacity 必须是 2 的次幂，索引用 `& mask`，扩容到 `capacity * 2` 后继续计算。Stats 沿用：

```text
++stats.ring_queue_capacity_grow_count
log symbol_id, exchange, queue name, new vector capacity
```

`Stats` 只记录次数；具体扩容的是 `lag_spread.spread_window` 还是其他 ring queue，只写 log。

`BuildEntryCostBreakdown()` 只把 `LagSpreadBuffer / triggerPrice` 记录为 embedded friction；`RequiredEdge()` 不包含 spread 或 lag spread buffer，避免与 `targetSpace` 中已经扣掉的价格摩擦重复计算。

### 验证口径

- Active 前输入 BBO 不改变交易 recorder。
- `SeedActive()` 后 recorder 初始极值等于 seed。
- lead tick 只更新 lead recorder / lead noise / move quantile window。
- lag tick 只更新 lag recorder / lag spread / lag noise。
- BBO extrema 输出 rolling `bbo_record.window` 内的 bid / ask min/max。
- BBO extrema vector 扩容后继续计算，不丢样本；`RecorderStats.extrema_capacity_grow_count` 只记录次数，扩容细节写 log。
- MoveQueue 按 `stats_window` 切窗，roll 后清空；不是严格 rolling 最近 `stats_window`。
- MoveQueue 在边界时间 `t == RollAt` 不 roll，`t > RollAt` 才 roll。
- roll 时用旧 queue 计算 quantile，当前 lead tick 的 move 进入新窗口。
- Aquila 设计中的低延迟路径默认使用 fixed-bin histogram 近似 move quantile；dual heap exact empirical quantile 只作为单 quantile exact / replay 对照口径。
- Histogram 必须记录 range / bins / bin_width / underflow / overflow，并接受 `bin_width` 级别近似误差；触发 underflow / overflow 后误差不再只由 `bin_width` 约束。
- `LeadNoise` / `LagNoise` 是 rolling normalized std 的 rolling mean，不是指数 EMA。
- `lag_spread_mean` 是 absolute spread 的 `StreamRecorder(stats_window)` mean。
- `LagSpreadBuffer = max(current_spread - mean_spread, 0)`。
- 固定 replay 下 extrema、noise 与 fixed 对齐；move quantile 若使用 histogram，则按配置误差、underflow / overflow 和 exact 对照路径单独验收。

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

- 重置交易 recorder / noise / spread / move quantile window。
- 如果 `driftReady`，先用 drift mean 缩放 lead seed。
- 用当前 raw lead / raw lag 快照播种交易 recorder。
- 设置 `resumeLeadTick`，避免 Active 切换 tick 被错误跳过或重复处理。

`drift_limit` 不是 alignment 条件。它只在 lead tick 开仓前限制新开仓；不限制统计更新、平仓或止损。

### Aquila 链路对应

第 4 部分可以独立落地为 drift / alignment 状态模块，先不耦合 threshold、signal 或 order state。它只依赖第 2 部分 raw market state 的 `QuoteSnapshot` 和第 3 部分 ring-window 统计能力，输出 Active 切换事件和 `AlignmentSnapshot`，供 recorder seed 和第 5 部分 threshold 使用。

建议文件边界：

```text
strategy/lead_lag/types.h
  QuoteSnapshot
  Side / PairRole
  DriftedQuote helper types shared by raw state, alignment and recorder

strategy/lead_lag/window_stats.h
  MeanWindow<TimedValue>
  MeanStdWindow<TimedValue>
  thin wrappers over core/base/RingQueue<T>

strategy/lead_lag/alignment.h
  AlignmentPhase
  AlignmentConfig
  AlignmentSnapshot
  ActiveTransition
  AlignmentState

test/strategy/lead_lag_alignment_test.cpp
  drift sample, warmup, phase transition and seed semantics
```

当前不执行实现；上述文件是后续落地时的目标边界。`strategy/CMakeLists.txt` 目前是 interface target，`strategy/lead_lag/*.h` 可以先按 header-only 方式接入；测试通过 `test/strategy/CMakeLists.txt` 新增 `lead_lag_alignment_test` target。

状态：

```text
AlignmentState
  phase
  drift_ready
  drift_samples
  first_paired_drift_ns
  resume_lead_tick
  logged_active
  drift_window        // StreamRecorder(drift_period)
  drift_std_window    // StreamRecorder(stats_window), stores drift_window.Std()
```

`drift_window` 对齐 fixed Go 的 `lagContext.drift`：按 `drift_period` 时间窗口记录 `drift = (lag_ask + lag_bid) / (lead_ask + lead_bid)`，提供 rolling mean 和 rolling std。`drift_std_window` 对齐 fixed Go 的 `threshold.DriftStdEma`：名字带 EMA，但实际是按 `stats_window` 记录 drift rolling std 的 rolling mean，不是指数 EMA。

接口：

```text
Init(config)
Reset()

OnPairedRawBbo(now_ns, raw_lead, raw_lag)
  -> drift_samples += 1
  -> first_paired_drift_ns set only for first sample
  -> drift_window.Add(now_ns, drift)
  -> drift_ready = true
  -> drift_std_window.Add(now_ns, drift_window.Std())

UpdatePhase(now_ns, lead_valid, lag_valid)
  -> Bootstrap / Aligning / Active

DriftLead(raw_lead)
  -> raw_lead scaled by drift_window.Mean() when drift_ready

EnterActive(now_ns, lead_seed, lag_seed, trigger_side)
  -> returns ActiveTransition
  -> lead_seed_drifted = DriftLead(lead_seed)
  -> lag_seed = raw lag seed
  -> resume_lead_tick = trigger_side == lag
```

输出 snapshot：

```text
AlignmentSnapshot
  phase
  drift_ready
  drift_samples
  first_paired_drift_ns
  drift_mean
  drift_std
  drift_std_ema
  drift_deviation = abs(drift_mean - 1)
  resume_lead_tick
```

`AlignmentConfig`：

```text
drift_period_ns
stats_window_ns
drift_warmup_ns
drift_min_samples
initial_capacity
```

`alignment_warmup_ns` 不是单独存储配置项，而是派生值：

```text
effective_warmup_ns = drift_warmup_ns > 0 ? drift_warmup_ns : stats_window_ns
```

Active seed 由第 2 部分 raw market state 提供：若当前变价 tick 触发 Active 且存在 previous quote，seed 使用进入当前 tick 前的 previous quote；否则使用 latest quote。`AlignmentState::EnterActive()` 只负责对 lead seed 做 drift scaling，并产生 `resume_lead_tick` 事件；真正 reset / seed BBO extrema、noise、spread 和 move quantile window 由 recorder 组合层消费 `ActiveTransition` 完成。

主循环落地顺序：

```text
OnBookTicker(ticker):
  now = ticker.local_ns
  price_changed, role = raw_market_state.Update(ticker)

  if raw lead and raw lag valid:
      alignment.OnPairedRawBbo(now, raw.lead.latest, raw.lag.latest)

  prev_phase = alignment.phase()
  phase = alignment.UpdatePhase(now, raw.lead.valid, raw.lag.valid)

  if prev_phase != Active && phase == Active:
      transition = alignment.EnterActive(now, lead_seed, lag_seed, role)
      recorder_state.ResetAndSeed(transition)

  if phase != Active:
      return

  if !price_changed && prev_phase == Active && !alignment.ConsumeResumeLeadTick(role):
      return

  continue active lead / lag handler
```

`ConsumeResumeLeadTick(role)` 只允许 lag tick 触发 Active 后的下一笔 lead tick 穿过 same-price gating；穿过后清掉 `resume_lead_tick`。

### 验证口径

- 单边行情不会产生 drift 样本。
- lead / lag 都 valid 后按 fixed 语义产生 paired drift 样本。
- `first_paired_drift_ns` 只在第一笔 paired sample 设置。
- drift rolling mean 等于 `drift_window` 当前 mean。
- `drift_std_ema` 等于 `drift_std_window` 当前 mean，输入样本是每次 paired drift 后的 `drift_window.Std()`。
- 样本数不足或 warmup 不足时保持 Aligning。
- readiness 满足后只切一次 Active。
- Active 切换时重置并播种交易 recorder。
- lag tick 触发 Active 时设置 `resume_lead_tick`，下一笔 lead same-price tick 允许进入一次 active handler，然后清掉 flag。
- lead tick 触发 Active 时当前 lead tick 继续进入 active handler，不设置下一笔 lead resume。
- `drift_limit` 超限只禁止开仓。
- drifted lead 等于 raw lead 乘 drift mean。
- `drift_warmup=0` 时使用 `stats_window` 作为 alignment warmup。
- same-price raw tick 不替换 raw quote，但仍可推进 drift sample 和 alignment phase。

后续实现任务拆分建议：

1. 写 `window_stats.h` 的 `MeanWindow` / `MeanStdWindow` 单测和实现，覆盖时间淘汰、mean/std、capacity grow 后结果不变。
2. 写 `alignment.h` 的 drift 单测和实现，覆盖 paired sample、first timestamp、drift mean/std、drift std rolling mean。
3. 写 phase readiness 单测和实现，覆盖 Bootstrap / Aligning / Active、`drift_warmup=0` fallback 和只切一次 Active。
4. 写 Active transition 单测和实现，覆盖 drifted lead seed、raw lag seed、lag-triggered resume lead、lead-triggered no resume。
5. 把 `AlignmentSnapshot` 接到第 5 部分 threshold 设计，暂不接 order / signal。

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
  last_up_quantile
  last_down_quantile
  last_profit_buffer
  roll_count
  initialized

MoveQuantileWindow
  roll_at_ns
  window_ns
  up_histogram
  down_histogram
```

接口：

```text
OnLeadActiveTick(now, drifted_lead, recorder_snapshot, alignment_snapshot)
  -> maybe roll thresholds
  -> push current move
  -> return ThresholdSnapshot
```

该层不下单，不读写 position，不判断 lag 当前 spread 是否可交易。它只把统计状态转成 threshold snapshot。

### Aquila histogram 设计

`MoveQuantileWindow` 用两个 `HistogramQuantile<double>` 对齐 fixed Go 的 `MoveQueue.Up` / `MoveQueue.Down`：

```text
up_histogram:
  sample   = lead_bid / bid_min - 1
  quantile = trigger.quantile.move
  edge     = upper

down_histogram:
  sample   = lead_ask / ask_max - 1
  quantile = 1 - trigger.quantile.move
  edge     = lower
```

因此每次 roll 只做两次查询：一个 up quantile、一个 down quantile。上行返回 bin upper edge、下行返回 bin lower edge，使近似阈值相对保守。某一侧窗口为空时返回 `0`，对齐 fixed Go 中空 slice 不调用 `stat.Quantile()` 的行为。

配置建议显式给出 range 和精度：

```toml
[lead_lag.pairs.trigger.quantile]
move = 0.75
up_min = 0.0
up_max = 0.02
down_min = -0.02
down_max = 0.0
precision = 0.000001
```

启动期用 `HistogramQuantile<double>::InitWithRangePrecision()` 预分配 bins：

```text
up_bins   = ceil((up_max - up_min) / precision)
down_bins = ceil((down_max - down_min) / precision)
```

bins 数不做 `bit_ceil`，因为当前查询和 reset 都不依赖 2 的次幂位运算。若未显式配置 range / precision，则只能退回 `4096` 默认 bins；生产策略建议每个 pair 明确配置，避免 underflow / overflow 让误差脱离 `bin_width` 约束。

roll 顺序保持 fixed Go：

```text
lead recorder and lead noise consume current tick

if now > roll_at_ns:
    freeze lead_noise / lag_noise
    up = up_histogram.ValueAvx2()
    down = down_histogram.ValueAvx2()
    exit = alignment_snapshot.drift_std_ema
    update UpEntry / DownEntry / UpExit / DownExit
    up_histogram.Reset()
    down_histogram.Reset()
    roll_at_ns = truncate(now, stats_window) + stats_window

push current up_move / down_move into the fresh or current window
```

当前默认查询路径建议优先用 AVX2；`ValueScalar()` 保留为通用 fallback，AVX512 当前 benchmark 没有优于 AVX2，不作为第一版默认路径。

复杂度：

```text
normal lead active tick:
  add up/down sample: O(1)

roll tick:
  quantile query: O(touched_up_bins + touched_down_bins)
  reset:          O(touched_up_bins + touched_down_bins)

space:
  O(up_bins + down_bins)
```

相对 fixed Go roll 时 sort 的 `O(n log n)`，histogram 把 roll 成本绑定在 touched bins 数量上；相对 `DoubleHeap<T>` exact quantile，它不需要为同一窗口的 up/down 分别维护排序样本，但结果是近似值，fixed replay 精确对账仍应保留 exact 对照口径。

### 验证口径

- 初始 threshold seed 与 fixed 一致。
- 未到 roll 时间时只 push move。
- roll 时按旧 queue 计算 quantile。
- roll 边界使用严格大于语义。
- 每个 histogram 只查询一个 quantile：up 使用 `quantile.move`，down 使用 `1 - quantile.move`。
- up histogram 使用 upper edge，down histogram 使用 lower edge。
- 空窗口 quantile 返回 `0`。
- lead noise 在当前 lead tick 更新后再 freeze；lag noise 使用最新 lag recorder snapshot。
- 上行分支和兜底分支分别覆盖。
- 下行分支符号单独测试。
- fee 使用 `fee*2`。
- `target_profit_rate` 不进入 threshold roll。
- histogram range / precision 配置能推导 bins，underflow / overflow 统计能被测试观察。
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

第 6 部分没有重计算，主要是把第 3-5 部分的统计结果转成开平仓订单意图，并维护当前 pair 的 execution group。建议拆成三个局部模块：

```text
strategy/lead_lag/cost_model.h
  EntryCostBreakdown

strategy/lead_lag/execution_state.h
  ExecutionStage
  ExecutionGroup
  ExecutionState

strategy/lead_lag/signal.h
  LeadLagSignalEngine
```

`EntryCostBreakdown` 对齐 fixed Go：

```text
RequiredEdge()
  = fee * 2 + lead_noise + lag_noise

RequiredEdgeWithTargetProfit(target_profit_rate)
  = RequiredEdge() + target_profit_rate

EmbeddedPriceFriction()
  = spread + lag_spread_buffer
```

`spread` 和 `lag_spread_buffer` 只进入 attribution / diagnostics，不重复加入 `RequiredEdge()`。原因是 `targetSpace` 已用 bid / ask 触发价并显式扣减或加入 `LagSpreadBuffer()`，否则会重复计算 entry-side price friction。

`ExecutionGroup` 对齐 fixed Go 的 `ExecuteCache`，但使用 `aquila` 已有 order manager 的 local order id：

```text
ExecutionGroup
  stage: Idle / Open / Hold / Close
  local_order_id
  signed_position_quantity
  trailing_price
  group_id
```

第 6 部分只负责创建 / 更新 group 与发出订单；订单回报如何把 `Open -> Hold`、`Close -> Idle` 推进，放到第 7 部分 order / position / feedback 状态机。

Signal 层直接调用 `StrategyContext::PlaceLimitOrder()`，不编码 Gate JSON：

```text
OpenLong:
  side = Buy
  price = lag_ask
  time_in_force = IOC
  reduce_only = false

OpenShort:
  side = Sell
  price = lag_bid
  time_in_force = IOC
  reduce_only = false

CloseLong / StoplossLong:
  side = Sell
  time_in_force = IOC
  reduce_only = true

CloseShort / StoplossShort:
  side = Buy
  time_in_force = IOC
  reduce_only = true
```

数量边界：

```text
open quantity = format(open_notional / price)
close quantity = format(current position quantity)
```

格式化依赖启动期缓存的 instrument metadata：`price_tick` / `price_decimal_places` 负责价格文本，`quantity_step` / `notional_multiplier` 负责 lag 原生数量。Signal 公式不混入 Gate wire encoding，也不在热路径查 instrument catalog。

`ExecutionConfig::EntrySpreadLimit()` 保留 fixed Go 兼容语义：

```text
if max_entry_spread < 0:
    return trailing_stop
return max_entry_spread
```

但生产配置建议显式给出正的 `max_entry_spread`，不要依赖 fallback。

### Aquila 处理顺序

lead active tick：

```text
OnLeadActiveTick:
  update lead recorder / lead noise / threshold

  for each Hold group:
      try signal close with latest lag quote

  if active group count >= parallel:
      return
  if drift_ready and abs(drift - 1) > drift_limit:
      return

  if TryOpenLong():
      return
  TryOpenShort()
```

lag active tick：

```text
OnLagActiveTick:
  update lag recorder / spread / lag noise / latest lag quote

  for each Hold group:
      update trailing
      if TryStoploss():
          continue
      try signal close with latest drifted lead quote
```

这里的 `active group count` 对齐 fixed Go 的 `len(caches)`，包括 Open / Hold / Close 阶段；只要 group 还没被第 7 部分状态机清掉，就占用 `parallel`。

### 复杂度

设 `P = execute.parallel`，第一版按 group 数线性扫描，不为 `parallel = 1` 做特殊分支：

```text
lead active tick:
  manage existing Hold groups: O(P)
  open long / short checks:    O(1)

lag active tick:
  stoploss / close checks:     O(P)

single open / close / stoploss decision:
  O(1)

space per pair:
  O(P)
```

第 6 部分没有需要 benchmark 的独立数值结构；后续性能验证重点是完整 strategy loop 中 signal branch 的主路径延迟，以及订单提交路径的端到端耗时。

### 验证口径

- `OpenLong` 每个 reject gate 单独覆盖。
- `OpenShort` 镜像逻辑和符号方向单独覆盖。
- `EntryCostBreakdown::RequiredEdge()` 不包含 spread / lag spread buffer；`RequiredEdgeWithTargetProfit()` 只额外加 `target_profit_rate`。
- `max_entry_spread < 0` fallback 到 `trailing_stop`，显式配置正值时使用 `max_entry_spread`。
- `parallel` 达上限时不新开仓。
- `drift_limit` 超限时不新开仓，但 close / stoploss 仍可触发。
- lead tick 中 close 先于 open。
- lag tick 中 stoploss 先于 signal close。
- pending order 时不重复 close / stoploss。
- open order 使用 IOC limit、`reduce_only=false`；close / stoploss order 使用 IOC limit、`reduce_only=true`。
- stoploss long 使用 `lag_bid * 0.995`；short 使用 `lag_ask * 1.005`。
- 第 6 部分只创建 / 更新 execution group 和 local order id；`Open -> Hold` / `Close -> Idle` 由第 7 部分 feedback 状态机验证。
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

fixed 的 `OnOrder(msg)` 在 `aquila` 中分成两层：

```text
OrderManager::OnOrderFeedback(OrderFeedbackEvent)
  -> 更新通用 StrategyOrder

LeadLagStrategy::OnOrderFeedback(event, context)
  -> 读取已更新 StrategyOrder
  -> 更新 LeadLag execution group / position / trailing
```

`StrategyRuntime::OnOrderFeedback()` 的调用顺序已经是先 `order_manager_->OnOrderFeedback(event)`，再回调 user strategy。因此 LeadLag 在回调中通过 `context.FindOrder(event.local_order_id)` 读到的是已经应用 feedback 后的 `StrategyOrder`。

通用订单状态仍由 `OrderManager` 负责，不在 LeadLag 内复制：

```text
OrderManager
  local_order_id allocation
  StrategyOrder storage
  Sent / Accepted / PartialFilled / Filled / Cancelled / Rejected status
  exchange_order_id cache / forget notification
  stale / duplicate / unknown feedback stats
  feedback gap stats

LeadLagExecutionState
  group stage
  signed position quantity
  pending local_order_id
  trailing price
  parallel occupancy
  degraded / needs_reconcile flag
```

`OrderSession` ack / submit response 不等价于 fixed Go 的 order message。ack 只说明请求发送 / 被接受处理；真正的 position 和 group stage 仍以 private order feedback 为准。唯一例外是 submit response rejected：如果 `OrderResponseKind::kRejected` 已让 `OrderManager` 把 order 标记为 finished，LeadLag 必须清掉对应 pending group，否则会卡在 `Open` / `Close`。

职责边界：

```text
StrategyRuntime
  drain feedback SHM
  call OrderManager before user strategy

OrderManager
  order object / OrderPool
  generic order status
  exchange_order_id cache / cleanup

LeadLagStrategy
  signal apply
  position / execution group apply
  degraded state on feedback gap

GateOrderSession
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

建议状态放在第 6 部分已定义的 `strategy/lead_lag/execution_state.h` 中：

```text
LeadLagExecutionGroup
  stage
  pending_local_order_id
  signed_position_quantity
  avg_entry_price
  trailing_price
  group_id
  side: Long / Short
  open_reason / close_reason

LeadLagExecutionState
  max_parallel
  active_groups
  local_order_id -> group index
  degraded
  needs_reconcile
```

`local_order_id -> group index` 第一版使用 `absl::flat_hash_map`，启动期按 `max_parallel * 2` reserve；由于 `max_parallel` 很小，后续若要进一步降低热路径不确定性，可改成 fixed vector 线性查找。

feedback event 推进：

```text
Accepted:
  OrderManager has recorded exchange_order_id
  LeadLag group stage unchanged

PartialFilled:
  OrderManager has updated cumulative filled diagnostics
  LeadLag position waits for terminal event
  group stage unchanged

Filled:
  terminal
  apply StrategyOrder.cumulative_filled_quantity by side
  clear pending order
  position == 0 -> delete cache
  position != 0 -> Hold

Cancelled / PartiallyCancelled:
  terminal
  apply StrategyOrder.cumulative_filled_quantity by side
  clear pending order
  position == 0 -> delete cache
  position != 0 -> Hold

Rejected:
  terminal
  clear pending order
  no position -> delete cache
  existing position -> Hold
```

position 更新使用 fixed-compatible terminal-only 语义。也就是说，`PartialFilled` 不提前更新 LeadLag position；等 `Filled` / `Cancelled` / `PartiallyCancelled` terminal event 到达后，使用 `StrategyOrder.cumulative_filled_quantity` 一次性应用到 group。为了和 fixed Go 的 `GetOrderFilled()` 对齐，应用前按 lag instrument 的 `quantity_step * notional_multiplier` 做四舍五入归整。

如果 terminal feedback 是开仓订单且最终 position 非零：

```text
stage = Hold
if AverageFillPrice() > 0:
    trailing_price = AverageFillPrice()
```

如果 terminal feedback 是平仓订单且剩余 position 非零，则回到 `Hold`，保留原 trailing，后续 lag tick 继续按第 6 部分 stoploss / signal close 管理。

如果 `OrderResponseKind::kRejected` 在 private feedback 前到达：

```text
find group by local_order_id
clear pending_local_order_id
if signed_position_quantity == 0:
    delete group
else:
    stage = Hold
```

这样 submit rejection 不会让 group 永久占用 `parallel`。

如果 feedback SHM 报 lane/global gap，LeadLag 应进入 degraded / needs reconcile 状态，暂停新开仓，等待后续 REST reconcile 设计补齐状态。

### OrderPool recycle

当前 `OrderManager` 会把 terminal order 标记为 `is_finished`，但没有公开释放 `OrderPool` slot 的 API。LeadLag 长时间实盘运行会持续产生 open / close IOC order，如果 finished order 永远保留在 pool 中，`order_capacity` 会变成运行时长上限。

第 7 部分实现时建议给 `OrderManager` 增加显式 retire API：

```text
OrderManager::RetireFinishedOrder(local_order_id) -> bool
```

语义：

```text
if order not found:
    return false
if !order.is_finished:
    return false
erase from OrderPool
return true
```

LeadLag 在 terminal feedback / submit rejection 已消费、execution group 已更新后调用 retire。这样 `OrderManager` 仍负责通用订单状态，LeadLag 只决定何时已经不再需要查询该 order。

### 复杂度

设 `P = execute.parallel`：

```text
normal feedback:
  OrderManager FindOrder: average O(1)
  local_order_id -> group lookup: average O(1)
  group state transition: O(1)

gap feedback:
  O(1)

space:
  LeadLag execution groups: O(P)
  local_order_id index:     O(P)
  OrderManager / OrderPool: O(order_capacity)
```

第 7 部分不需要独立 benchmark；验证重点是状态转换正确性、feedback gap 后停止新开仓，以及完整链路中的 order feedback 到策略状态更新延迟。

### 验证口径

- 开仓 IOC 完全未成交：cache 删除。
- 开仓部分成交：进入 Hold，position 为成交量。
- 开仓完全成交：进入 Hold，trailing 初始化。
- 非 terminal partial feedback 不提前切 stage，除非后续明确选择偏离 fixed 以提升实盘风险可见性。
- 平仓完全成交：cache 删除。
- 平仓部分成交：回到 Hold，剩余 position 正确。
- rejected open：无 position 时删除 cache。
- rejected close：已有 position 时回到 Hold。
- submit response rejected：无 position 时删除 cache，有 position 时回到 Hold。
- filled quantity 按 quantity step / notional multiplier 归整。
- pending order 存在时 signal 不重复发 close。
- accepted event 更新 exchange order id，并通知 OrderSession cancel cache。
- terminal event 清理 OrderSession cancel cache。
- terminal event 被 LeadLag 消费后 retire finished order，释放 `OrderPool` slot。
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

1. 配置与 instrument catalog metadata 的设计形状已定为 `lead_lag` / `version="1.0"` / `config/strategy/lead_lag.toml` / pair array；后续实现需按本节验证口径落 parser。
2. raw market state 的时间、`symbol_id` vector lookup 和 `BookTicker.exchange` role 判断是否符合 `aquila` 当前 DataReader。
3. recorder / queue 输出语义按 fixed Go 对齐；实现可不同，但必须覆盖切窗 MoveQueue、histogram move quantile、rolling normalized std mean 和 absolute spread mean。
4. alignment readiness 使用 `drift_warmup`，未配置时回退到 `stats_window`。
5. threshold 符号和 roll 顺序是否确认。
6. signal 的成本模型、spread buffer、stoploss 价格是否确认。
7. feedback 事件到 execution cache 的状态推进是否确认。
