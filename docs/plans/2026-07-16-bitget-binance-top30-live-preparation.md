# Binance/Bitget Top30 LeadLag 实盘准备计划

## 目标

核对用户给出的 30 个 `USDT` 合约在 Binance USD-M 与 Bitget UTA USDT Futures 的当前可交易状态，更新
`config/instruments/usdt_future_universe.csv` 中对应的 Binance/Bitget 元数据，并生成可进入 Bitget LeadLag
fresh-run pipeline 的 30-symbol `fanout=4` 策略配置。

## 非目标

- 本轮不启动真实订单，不创建或复用 live `run_id`、gateway/feedback SHM 或 manifest。
- 不修改策略信号、订单状态机、reconcile、stop-and-flat、限流或账户所有权语义。
- 不修改当前运行中的四小时实盘；该 run 使用冻结配置、catalog 与二进制。

## 关键决定与边界

- 合约存在性与元数据只信本轮 Binance `fapi/v1/exchangeInfo` 和 Bitget UTA
  `api/v3/market/instruments?category=USDT-FUTURES` 官方响应。
- Binance 接受 `TRADING` 的 `PERPETUAL` 与 `TRADIFI_PERPETUAL`；Bitget 只接受 `online` 的
  `USDT-FUTURES/perpetual`。只有双边均满足的 symbol 进入策略配置。
- 已有 symbol ID 保持不变；新增 symbol 从 catalog 当前最大 ID 之后分配，同一 symbol 的 Binance/Bitget 行共享 ID。
- 策略沿用当前 Bitget live 基线：Binance lead、Bitget lag、High Speed 行情、freshness `3ms/500ms`、
  `open_notional=10`、`parallel=1`、`order_session_fanout=4`。价格滑点沿用 Gate 配置的 2bps entry/close、
  5bps stoploss 口径，并按本轮 Bitget ticker 与 `price_tick` 固化为 ticks。

## 实施步骤

1. 保存两家官方合约快照、哈希与查询时间，生成 30-symbol 双边状态矩阵。
2. 先增加配置/catalog 约束测试并证明当前缺失元数据或配置时失败。
3. 刷新 30 symbols 已有的 Binance/Bitget catalog 行，并补齐缺失行；保持 Gate 行和既有 ID 不变。
4. 生成 30-symbol Bitget LeadLag `fanout=4` 策略配置，记录本轮 ticker 派生的滑点 ticks。
5. 运行 catalog 一致性检查、Python focused tests、C++ config parser、strategy validate-only 与 Bitget
   fresh-run `prepare` dry-run；完成 diff review 和回归验证。
6. 将长期有效入口写入 Bitget trading/onboarding 文档，删除完成态计划并原子提交、push。

## 验证策略

- 30/30 双边存在、状态与 contract type 符合上述筛选条件。
- catalog 无 `(symbol, exchange)` 重复、无 symbol ID 冲突；30 symbols 均有 Binance/Bitget 行，字段与官方快照一致。
- 策略严格包含 30 个目标 symbol，全部为 `3ms/500ms`、`open_notional=10`、`fanout=4`，symbol ID 与 catalog 一致。
- 每个 Bitget pair 的 `price_tick`、`quantity_step`、`min_quantity` 和 `min_notional` 可用于最小量及下单文本构造。
- `prepare_bitget_live_run.py prepare` 能生成 manifest v2 overlay，但本轮不执行 `mark-applied` 或 guard `--execute`。

## 回滚与剩余风险

- catalog/config 提交可整体回滚；外部官方快照保留在 `/home/liuxiang/tmp`，不进入生产路径。
- `TRADIFI_PERPETUAL` 可能具有不同交易时段和停牌语义；`TRADING/online` 只证明查询时可用，不证明 24x7 行情连续。
- 固化的 slippage ticks 会随价格变化而偏离 2bps/5bps；真实启动前仍需重新拉取 contract/ticker、运行 freshness preflight
  和最小金额计划，并取得当次真实订单授权。
