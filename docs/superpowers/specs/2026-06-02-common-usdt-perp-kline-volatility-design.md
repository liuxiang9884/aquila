# Gate / Binance 共同 USDT 永续合约 K 线波动率设计

## 目标

基于 Gate / Binance 共同 USDT 永续合约 universe，抓取两个交易所的 1 分钟 K 线，并用 close-to-close log return 计算过去 `N` 分钟 realized volatility。第一版默认输出 30 分钟和 60 分钟波动率，用于后续 LeadLag 候选品种初筛。

## 输入

- 默认 instrument catalog：`config/instruments/usdt_futures_common_gate_binance_20260602.csv`。
- catalog 中每个内部 `symbol` 应有 Gate 和 Binance 两行。
- Gate 使用 `exchange_symbol`，例如 `BTC_USDT`。
- Binance 使用 `exchange_symbol`，例如 `BTCUSDT`。

## 数据源

- Gate futures 1m K 线：`GET /futures/{settle}/candlesticks`，第一版使用 `settle=usdt`、`interval=1m`。
- Binance USD-M futures 1m K 线：`GET /fapi/v1/klines`，第一版使用 `interval=1m`。
- 脚本串行请求并支持 `--request-sleep-sec`，避免为全量 494 个 symbol 做高并发 REST 压力。

## 输出

所有运行产物写入 `/home/liuxiang/tmp` 下的 run 目录，默认格式：

```text
/home/liuxiang/tmp/common_usdt_perp_klines_<YYYYMMDD_HHMMSS>/
```

目录内包含：

```text
gate_1m_klines.csv
binance_1m_klines.csv
volatility_summary.csv
run_metadata.json
```

项目仓库不提交这些临时 CSV。

### K 线 CSV 字段

每个交易所一个 CSV，字段统一为：

```text
exchange,symbol,exchange_symbol,open_time_ms,close_time_ms,open,high,low,close,volume,quote_volume,closed
```

`closed=false` 的未完成最新分钟不参与波动率计算。

### Summary CSV 字段

```text
symbol,
gate_vol_30m_bps,gate_vol_60m_bps,gate_valid_30m,gate_valid_60m,gate_close_count,
binance_vol_30m_bps,binance_vol_60m_bps,binance_valid_30m,binance_valid_60m,binance_close_count,
max_vol_60m_bps,min_vol_60m_bps
```

## 波动率口径

使用 close-to-close log return：

```text
r_i = log(close_i / close_{i-1})
vol_Nm = sqrt(sum(r_i^2))
vol_Nm_bps = vol_Nm * 10000
```

计算 `N` 分钟波动率至少需要 `N + 1` 个已完成 close。若不足，则对应 `valid=false` 且 vol 留空。

不做 annualize。输出是过去窗口内的 realized volatility bps。

## 实现入口

新增脚本：

```text
scripts/market_data/query_common_usdt_perp_klines.py
```

新增测试：

```text
scripts/test/market_data/query_common_usdt_perp_klines_test.py
```

脚本解释器使用仓库现有 `/home/liuxiang/dev/pyenv/lx/bin/python`。

## 验证

单元测试覆盖：

- catalog 读取并按 symbol 合并 Gate/Binance exchange symbol。
- Binance kline array 解析。
- Gate kline dict / array 解析。
- closed kline 过滤。
- 30m / 60m realized volatility 计算。
- 原始 kline CSV 和 summary CSV 字段输出。

最小验证命令：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/market_data/query_common_usdt_perp_klines_test.py
git diff --check
```

live smoke 只抓少量 symbol，例如：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/market_data/query_common_usdt_perp_klines.py \
  --symbols BTC_USDT,ETH_USDT \
  --lookback-minutes 70 \
  --request-sleep-sec 0.05
```
