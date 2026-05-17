# LeadLag fixed Go / Aquila C++ 静态语义审计

## 背景

本审计只做静态源码对照，不运行 fixed Go。目标是把 fixed Go LeadLag 的关键语义与 Aquila C++ 当前实现逐项对齐，列出差异、影响和建议，供后续与策略研发讨论是否修改。

范围：

- fixed Go：`third_party/strategy/wt-invariant-strategy-leadlag-must-fix/leadlag/algo/`
- Aquila C++：`strategy/lead_lag/`
- 当前重点：行情 tick 入口、时间口径、alignment、recorder / threshold、signal、execution cache。

不在本审计中判断线上收益或延迟；性能结论仍必须以 benchmark / profile / replay 实测为准。

## 总体结论

当前 C++ replay 主信号链路在主要开平仓公式、same-price tick、active seed、drift、noise、spread、threshold 更新顺序、stoploss 和 close / open 顺序上基本按 fixed Go 静态语义实现。

需要重点讨论的差异有三类：

1. **时间口径差异，高影响。** fixed Go 大部分窗口用 `Context.Now()`，BBO extrema 用 `bbo.ServerTime`；Aquila 统一用 `BookTicker.exchange_ns`，缺失时才回退 `local_ns`。这是 Aquila 之前明确过的设计选择，但如果目标是 fixed Go 逐 tick 精确对齐，它会影响 extrema、roll、noise、spread、drift warmup 和同时间排序。
2. **move quantile 实现差异，中高影响。** fixed Go 在 roll 时 sort slice 并用 `gonum/stat.Quantile(..., stat.Empirical)`；Aquila 默认使用 fixed-bin histogram 近似 quantile。低延迟路径可接受，但精确对账需要 exact 模式或离线对照。
3. **订单生命周期差异，高影响但不影响“只看信号” replay。** fixed Go 通过 order / feedback 把 `StageOpen -> StageHold -> StageClose` 串起来；Aquila replay 目前使用 synthetic accounting，open signal 直接形成 hold position。它适合检查信号触发，不等价于真实回测成交链路。

## 语义对照表

| 项 | Go 源码 | C++ 源码 | 结论 | 影响 | 建议 |
| --- | --- | --- | --- | --- | --- |
| 配置形态 | `strategy.go:30` 定义单 symbol、lead / lag 字符串和默认值；`strategy.go:106` 用默认 taker fee map。 | `config.h:79` / `config.cpp:120` 使用 pair array、`symbol_id`、`Exchange` enum、instrument catalog 和显式 `lag_taker_fee`。 | 设计差异。 | 中：如果 Go 生产参数没有完整复制到 TOML，信号会不同；参数一致时不影响热路径语义。 | 保留 C++ 设计；后续和研发校对时先固定一份参数快照。 |
| 未使用字段 | Go `TriggerConfig.Open` / `LeadPart` 定义在 `strategy.go:32`、`strategy.go:36`，当前信号路径未引用。 | C++ 未保留这两个字段。 | 可接受差异。 | 低：当前 Go 源码未使用，不影响现有信号。 | 不改；如果研发确认历史版本使用过，再补兼容字段。 |
| 行情路由 | Go 在 `OnRawBBO()` 用 exchange string 判断 lead / lag，见 `strategy.go:281`、`strategy.go:326`。 | C++ 用 `symbol_id` vector + `Exchange` enum，见 `raw_market_state.h:141`。 | 设计差异。 | 低：避免热路径字符串比较。 | 保留 C++ 设计。 |
| Quote 保存内容 | Go 保存完整 `utils.BBO`，`updateRawMarketState()` 见 `strategy.go:360`。 | C++ 只保存 `QuoteSnapshot{event_ns,bid_price,ask_price}`，见 `raw_market_state.h:34`。 | 设计差异。 | 低到中：信号公式只依赖 bid / ask；但 Go 的日志和 liquidity attribution 会用 qty / symbol。 | 信号路径不改；若要完整 debug 对账，可在 CSV 或 debug event 中补 qty。 |
| 全局时间 | Go `OnRawBBO()` 使用 `s.Context.Now()` 推动 drift / alignment，见 `strategy.go:288`、`strategy.go:346`；noise / spread / move roll 也使用 `Context.Now()`，见 `strategy.go:548`、`strategy.go:598`。 | C++ `BookTickerEventTimeNs()` 优先用 `exchange_ns`，见 `raw_market_state.h:14`；`Strategy::OnBookTicker()` 用它作为 `now_ns`，见 `strategy.h:83`。 | 语义差异。 | 高：会影响 warmup、roll 边界、窗口淘汰和同一毫秒 / 纳秒排序。 | 先讨论是否坚持“交易所撮合时间为全局时间”。若要 fixed Go 精确对齐，增加 replay mode 支持 Go 式 `context_now_ns` 和 extrema 单独时间。 |
| BBO extrema 时间 | Go `BBOVolRecorder.OnBBO(milliTs, ...)` 用 `bbo.ServerTime`，见 `analysis.go:67` 和 `strategy.go:543`。 | C++ `BboExtremaWindow::Update()` 用 `QuoteSnapshot.event_ns`，见 `recorders.h:103`。 | 语义差异，来源同时间口径。 | 高：extrema 是 open long / short 的 base，窗口边界差异会直接改变 lead move。 | 同上；可考虑 `QuoteSnapshot` 增加 `extrema_ns`，但只有在研发要求 fixed exact 时再改。 |
| same-price tick | Go 只比较 bid / ask，same-price 不更新 quote，见 `strategy.go:360`；Active 后 same-price 默认不进 handler，见 `strategy.go:319`。 | C++ `MarketSideState::Update()` 同样只比较 bid / ask，见 `raw_market_state.h:40`；Active 后 same-price 过滤见 `strategy.h:112`。 | 基本一致。 | 低。 | 不改。 |
| Active seed / resume lead | Go Active 切换时按当前 tick 和 previous quote 选择 seed，并在 lag 触发 Active 后允许下一笔 lead resume，见 `strategy.go:301`、`strategy.go:313`、`strategy.go:318`。 | C++ `SelectActiveSeed()` / `EnterActive()` / `ConsumeResumeLeadTick()` 对应实现，见 `raw_market_state.h:90`、`alignment.h:99`、`alignment.h:130`。 | 基本一致。 | 低。 | 不改；已有测试应继续覆盖 lag-trigger active 后的 lead resume。 |
| Drift rolling mean / std | Go drift 为 `(lag ask+bid)/(lead ask+bid)`，用 `StreamRecorder(drift_period)`，见 `strategy.go:346`、`analysis.go:109`。 | C++ drift 公式相同，`MeanStdWindow(drift_period)`，见 `alignment.h:70`。 | 时间一致时语义一致。 | 中：受全局时间口径影响。 | 不单独改；跟随时间口径决定。 |
| Drift std ema | Go `StreamStdEmaRecorder` 名字带 EMA，但实际是 normalized std 的 rolling mean，见 `analysis.go:177`。 | C++ `NoiseState` / `AlignmentState` 使用 rolling std mean，见 `alignment.h:81`、`recorders.h:176`。 | 语义一致。 | 低。 | 不改。 |
| Recorder 扩容 | Go queue 满时扩容，见 `queue.go:52`；MonotonicQueue 用内部 Queue 扩容，见 `monotonic_queue.go:30`。 | C++ `RingQueue` / `MonotonicDeque` 也允许扩容并记录 grow count，见 `recorders.h:161`、`recorders.h:305`。 | 语义一致，观测方式不同。 | 低：扩容影响性能，不应改变结果。 | 保留当前 stats 规则。 |
| MoveQueue roll 顺序 | Go `OnLeadBBO()` 先更新 lead extrema，再 `UpdateMoveThreshold()`，roll 后再 push 当前样本，见 `strategy.go:543`、`strategy.go:454`、`strategy.go:530`。 | C++ `OnLeadActiveTick()` 先更新 extrema，再 `MoveQuantileWindow::Update()`，roll before add，见 `recorders.h:343`、`recorders.h:251`。 | 顺序一致。 | 低。 | 不改。 |
| Move quantile 计算 | Go sort slice 后用 empirical quantile，见 `move.go:37`、`move.go:47`。 | C++ 默认 histogram，`up` 返回 upper edge，`down` 返回 lower edge，见 `recorders.h:232`。 | 实现差异。 | 中到高：阈值会有 bin 级误差；overflow / underflow 时误差不可只按 bin width 约束。 | 生产低延迟保留 histogram；如需 exact 对账，增加 replay-only exact empirical mode。 |
| Threshold 更新公式 | Go `up-entry/down-entry/up-exit/down-exit` 公式见 `strategy.go:478`。 | C++ 公式见 `threshold.h:41`。 | quantile 输入一致时公式一致。 | 中：主要受 quantile 和时间口径影响。 | 不单独改。 |
| Open long / short 条件 | Go long 见 `strategy.go:652`，short 见 `strategy.go:754`。 | C++ long 见 `signal.h:69`，short 见 `signal.h:120`。 | 公式基本一致，包括 price diff、lead move、lag part、entry cost、entry spread。 | 低到中：差异主要来自上游 extrema / threshold。 | 不改。 |
| Close / stoploss 顺序 | Go lead tick 先 close 再 open，见 `strategy.go:551`；lag tick 先 stoploss 再 close，见 `strategy.go:602`。 | C++ `OnLeadTick()` / `OnLagTick()` 顺序相同，见 `signal.h:172`、`signal.h:204`。 | 一致。 | 低。 | 不改。 |
| Close / stoploss 价格 | Go close / stoploss 见 `strategy.go:876`、`strategy.go:981`。 | C++ close / stoploss 见 `signal.h:245`、`signal.h:277`。 | 一致：close 用 lag bid/ask，stoploss 用 `0.995` / `1.005`。 | 低。 | 不改。 |
| 多仓遍历顺序 | Go `s.caches` 是 map，遍历顺序不稳定，见 `strategy.go:551`、`strategy.go:602`。 | C++ 用 `vector<ExecutionGroup>` 顺序扫描，见 `execution_state.h:264`、`signal.h:176`。 | 设计差异。 | 中：`execute.parallel > 1` 时同一 tick 多个 group 可触发不同 close 顺序；当前 ORDI 配置 `parallel=1` 无影响。 | 保留 C++ 确定性；如研发要求模拟 Go map 顺序，不建议照搬。 |
| replay position accounting | Go open 后进入 pending order，finished order 才更新 position，见 `strategy.go:736`、`strategy.go:1267`。 | C++ replay `PositionAccountingMode::kSyntheticSignals` 下 open signal 直接 AddHoldGroup，见 `strategy.h:318`。 | 语义差异。 | 高：不等价于真实回测成交，只适合检查信号触发。 | 保留 signal-only replay；文档和 CSV 继续明确这是 synthetic。真实回测需模拟 order / fill。 |
| 生产 feedback | Go `OnOrder()` 用 order message 推进 stage / position / trailing，见 `strategy.go:1267`。 | C++ `ExecutionState` 有 `ApplyTerminalOrder()`，见 `execution_state.h:110`；但 `Strategy::OnOrderResponse()` 空，`OnOrderFeedback()` 当前只处理 gap，见 `strategy.h:129`。 | 尚未完成。 | 高：生产闭环必须补。 | 等信号语义确认后，优先补 `OnOrderResponse()` / `OnOrderFeedback()` 到 execution state。 |
| open quantity / metadata | Go `CalOpenQty()` 用 open notional、quantity tick 和 contract value，见 `strategy.go:1233`。 | C++ signal 当前只输出 intent price / side；metadata 已在 `PairConfig::lag_instrument`，见 `config.h:65`。 | 生产实现差异。 | 中：不影响 signal CSV；影响真实下单数量。 | 生产下单接入时按 instrument catalog 实现数量换算和 tick rounding。 |

## 高影响差异评估

### 1. 时间口径

这是最需要先讨论的点。Aquila 之前的目标是“以交易所撮合时间戳为全局时间”，因此 `BookTickerEventTimeNs()` 统一使用 `exchange_ns`。这能让多交易所 replay 在一个确定时钟下排序，也避免本地接收时间对信号产生机器相关性。

fixed Go 静态源码则不是单一时间口径：

- `Context.Now()` 驱动 drift、alignment warmup、move roll、noise、spread。
- `bbo.ServerTime` 驱动 BBO extrema。

如果 Go 的 backtest `Context.Now()` 正好等于我们使用的 `exchange_ns`，则大部分窗口可对齐；但 BBO extrema 仍要确认 `ServerTime` 是否等于同一个字段。如果不等，最直接的信号差异会来自 extrema base price 和 move quantile roll 边界。

初步建议：

- 若策略研发确认 Aquila 回放就是要以 tx time 为准，保留当前实现，不为兼容 Go 引入多时间口径。
- 若目标是 fixed Go exact replay 对账，增加一个 replay-only time policy：全局时间、extrema 时间分别可配置，并把所选时间写进 debug CSV。

### 2. Move quantile

当前 C++ histogram 是低延迟设计，不是 Go empirical quantile 的逐样本 exact 实现。只要 histogram range 覆盖样本，误差可按 bin width 估计；但 underflow / overflow 会让误差不可控。

初步建议：

- 生产路径保留 histogram。
- 为静态语义校验 / 小窗口 replay 增加 exact empirical mode，直接保存样本并在 roll 时 sort，模拟 Go `MoveQueue`。该模式只用于对账，不进入生产热路径。

### 3. Synthetic replay

`PositionAccountingMode::kSyntheticSignals` 是为了“检查信号是否正确”，不是完整回测。它假设 open / close signal 当场形成 position 状态，因此会跳过 Go 的 pending order、filled quantity、avg fill price、partial fill 等状态。

初步建议：

- signal 校对继续用 synthetic replay。
- 文档和 CSV 中保持明确标识：signal CSV 是策略触发结果，不是 order/fill replay。
- 生产闭环应在信号语义确认后单独补 `OnOrderResponse()` / `OnOrderFeedback()`。

## 建议讨论顺序

1. **先确认时间口径。** 是否坚持 tx time 作为 Aquila replay 的唯一策略时间？如果是，Go exact parity 不应作为必须目标，只做公式/条件审计。
2. **再确认是否需要 exact quantile replay mode。** 如果研发需要逐条解释 C++ 与 Go 的阈值差异，建议加 replay-only exact mode。
3. **确认 signal-only replay 边界。** 目前 signal CSV 可以用来校对触发条件和 PnL 假设，但不能代表真实成交链路。
4. **最后补生产 feedback。** 当 1-3 确认后，再接 `OnOrderResponse()` / `OnOrderFeedback()`，避免在信号语义未定时扩大状态机复杂度。

## 当前不建议修改的点

- 不建议把 C++ 热路径改回字符串 exchange / symbol 路由。
- 不建议为了模拟 Go map 遍历顺序而放弃 `ExecutionGroup` vector 的确定性。
- 不建议让生产路径默认使用 sort-based exact quantile；如需要，做 replay-only mode。
- 不建议在 signal-only replay 中模拟完整 OrderSession；若要真实回测，应单独设计 order/fill simulator。

