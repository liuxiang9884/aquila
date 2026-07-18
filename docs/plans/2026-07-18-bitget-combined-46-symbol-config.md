# Bitget 46-symbol 合并配置计划

## 目标

- 将现有 Bitget 30-symbol 配置与历史 20-symbol 配置按 symbol 去重合并，生成一份新的 46-symbol strategy config。
- 所有 pair 保持 `max_lag_freshness_ms = 500`。
- 使用本次 Bitget UTA 账户 `GET /api/v3/account/all-fee-rate?category=USDT-FUTURES`
  返回的逐 symbol `takerFeeRate` 写入 `lag_taker_fee`。

## 非目标

- 不修改现有 20-symbol 或 30-symbol 配置。
- 不启动 strategy、gateway、feedback 或任何真实订单。
- 不修改 trigger、slippage、risk、fanout、parallel、open notional 或订单状态语义。
- 不把 maker fee 加入当前只消费 taker fee 的策略 contract。

## 关键决定

- 新配置命名为
  `config/strategies/lead_lag_bitget_combined_46symbols_highspeed_fanout4_20260718.toml`。
- 保留当前 30-symbol 配置的原始顺序和完整 pair block；再按历史 20-symbol 配置顺序追加其中独有的 16 个 symbol。
- 重叠的 `HYPE_USDT`、`ZEC_USDT`、`WLD_USDT`、`ENA_USDT` 采用当前 30-symbol
  配置，避免旧 20-symbol slippage 与旧 fee 覆盖当前配置。
- 新配置继续使用现有 `fanout=4` checked-in contract；后续 fanout=1 live 必须由 fresh-run
  overlay 显式生成，不能把本配置当作真实启动授权。
- REST fee snapshot 只作为 2026-07-18 当次账户事实；每次新 live 仍必须重新查询并对账。

## 影响边界

- 新增一份 strategy TOML。
- 扩展 `lead_lag_config_test`，固定 46-symbol 顺序、500ms lag freshness 和逐 symbol taker fee。
- 更新 `docs/bitget_trading.md` 中的配置入口与 fee snapshot 摘要。

## 实施步骤

1. 先增加新配置 contract 测试，并确认因目标文件不存在而失败。
2. 从当前 30-symbol 配置复制全部 pair，再追加 20-symbol 独有 pair。
3. 查询账户 all-fee-rate，将 46 个 symbol 的 `takerFeeRate` 写入新配置。
4. 核对 46/46 symbol 在统一 instrument catalog 中同时存在 Binance lead 与 Bitget lag metadata。
5. 运行 focused config test、strategy validate-only、TOML/REST 对账和 diff 检查。
6. 将长期有效的入口和 fee 边界迁移到 `docs/bitget_trading.md`，完成后删除本计划。

## 验证策略

- 失败证据：新增测试在新 TOML 尚不存在时失败。
- `lead_lag_config_test`：验证 pair count、顺序、symbol id、exchange、500ms freshness、
  fanout、parallel 和逐 symbol fee。
- TOML 独立脚本：验证无重复 symbol，集合等于 20/30 配置并集。
- Catalog 对账：每个 symbol 均能加载 Binance/Bitget USDT futures metadata。
- Fresh REST 对账：46 个 `lag_taker_fee` 与当次 `takerFeeRate` 完全一致。
- Strategy `--validate-only`：使用统一大 catalog 加载新 config。
- `git diff --check`。

## 回滚

- 新配置为新增文件；回滚 commit 即可移除，不影响现有 20/30-symbol 配置和已归档 run。

## 未解决风险

- Fee、symbol 在线状态和交易时段会变化；checked-in 值不是永久 contract。
- 10 个 TradFi perpetual 及其他稀疏 symbol 不能从 catalog online 状态推断 24x7 可交易。
- 合并配置扩大 symbol 范围，但不构成 fanout=4 live safety、账户 limiter 或 fillability 证据。
