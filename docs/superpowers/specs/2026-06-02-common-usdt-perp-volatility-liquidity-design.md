# Common USDT Perp Volatility / Liquidity 设计

## 目标

扩展 `scripts/market_data/query_common_usdt_perp_klines.py`，在拉取 Gate / Binance 交集 USDT perpetual 最新 1m K 线后，除已有 raw kline CSV 和合并 summary 外，额外为每个交易所生成一个独立结果 CSV，便于直接按波动率和成交额筛选 lead lag 候选品种。

## 输出

每次运行仍写入 `/home/liuxiang/tmp/<run_id>/`，保留现有文件：

```text
gate_1m_klines.csv
binance_1m_klines.csv
volatility_summary.csv
run_metadata.json
```

新增两个结果文件：

```text
gate_volatility_liquidity.csv
binance_volatility_liquidity.csv
```

## 结果 CSV 字段

每个交易所一个 CSV，每行一个内部 symbol：

```text
exchange,symbol,exchange_symbol,
vol_30m_bps,vol_60m_bps,
quote_volume_30m,quote_volume_60m,
volume_30m,volume_60m,
valid_30m,valid_60m,
close_count,latest_closed_open_time_ms,reference_price
```

- `vol_*m_bps`：最近 N 分钟 close-to-close realized volatility，单位 bps；N 分钟需要最近 `N + 1` 根已完成 1m close。
- `quote_volume_*m`：最近 N 根已完成 1m K 线的 `quote_volume` 求和。Binance 为 USDT quote asset volume；Gate 为 `sum`，USDT futures 下作为 USDT 成交额使用。
- `volume_*m`：最近 N 根已完成 1m K 线的原始 `volume` 求和，仅作为交易所内参考，不作为跨交易所主流动性指标。
- `valid_*m`：对应窗口是否同时有足够 close 和 K 线数据。
- `reference_price`：参考价，取该交易所最近一根已完成 1m K 线的 close。
- `latest_closed_open_time_ms`：该 symbol 最近一根已完成 1m K 线的 open time。

## 计算边界

- 只使用 `closed=true` 的 K 线；未完成最新分钟不参与波动率或成交额。
- 结果 CSV 的窗口取当前运行的 `--vol-window` 参数，默认仍为 `30` 和 `60`。
- raw kline CSV 不改 schema，已有 `volatility_summary.csv` 保持兼容。
- 若某个 symbol 请求失败，结果 CSV 保留该 symbol 行但指标为空、`valid_*m=false`；失败继续写入 `run_metadata.json.failures`。

## 验证

- 单元测试覆盖结果行构建、成交额窗口求和、CSV 字段输出。
- 使用现有 `/home/liuxiang/dev/pyenv/lx/bin/python` 运行测试。
- live 拉取最新数据后检查新增两个 CSV 文件存在，且行数与成功 symbol 数量一致。
