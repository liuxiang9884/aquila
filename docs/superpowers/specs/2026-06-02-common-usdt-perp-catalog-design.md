# Gate / Binance 共同 USDT 永续合约 Instrument Catalog 设计

## 目标

新增脚本生成 Gate 与 Binance 都可交易的 USDT 永续合约 instrument catalog。该 catalog 用于后续按波动率、流动性筛选 LeadLag 候选品种，不覆盖现有 `config/instruments/usdt_futures.csv`。

## 范围

- Binance 输入来自 USD-M futures `exchangeInfo`。
- Gate 输入来自 USDT futures contracts endpoint。
- 只保留两边都处于可交易状态的 USDT 永续合约。
- 生成新的 `aquila.instrument.v1` CSV，放在 `config/instruments/`。
- 用户指定 `~/pyenv/lx/bin/python`；当前机器该路径不存在，实际使用仓库既有可执行解释器
  `/home/liuxiang/dev/pyenv/lx/bin/python`。

## 过滤规则

Binance 保留：

- `quoteAsset = "USDT"`。
- `marginAsset = "USDT"`。
- `contractType = "PERPETUAL"`。
- `status = "TRADING"`。

Gate 保留：

- contract name 形如 `*_USDT`。
- `status` 大小写归一后为 `TRADING`。
- USDT settle futures endpoint 返回的合约。

## Symbol 规范

- 内部 symbol 统一使用 `BASE_USDT`，例如 `BTC_USDT`。
- Binance `BTCUSDT` 转换成 `BTC_USDT`。
- Gate `BTC_USDT` 直接使用。
- 交集按内部 symbol 计算。

## 输出规则

脚本默认输出到：

```text
config/instruments/usdt_futures_common_gate_binance_<YYYYMMDD>.csv
```

如果目标文件已存在，默认拒绝覆盖；需要显式 `--overwrite`。

CSV 使用现有 catalog schema：

```text
symbol_id,symbol,exchange,exchange_symbol,base_asset,quote_asset,settle_asset,product_type,status,contract_type,price_tick,price_decimal_places,quantity_step,quantity_decimal_places,min_quantity,max_quantity,max_market_quantity,min_notional,notional_multiplier,price_limit_up,price_limit_down,market_price_bound
```

每个交集 symbol 输出两行：Gate 一行，Binance 一行。两行共享同一个 `symbol_id`。`product_type` 固定为 `linear_perpetual`。

## 实现入口

新增脚本：

```text
scripts/instruments/generate_common_usdt_perp_catalog.py
```

脚本复用现有 metadata 映射：

- `scripts/gate/query_futures_contracts.py`
- `scripts/binance/query_um_futures_contracts.py`

若现有脚本缺少全量列表 helper，只在本次需要的范围内补充小型函数，不改变现有 CLI 行为。

## 验证

新增 Python unit test 覆盖：

- Binance 全量 payload 过滤。
- Gate 全量 payload 过滤。
- Binance / Gate symbol 归一化。
- 交集计算。
- 输出 catalog 列顺序、`symbol_id` 分配和每个 symbol 两行输出。
- 默认不覆盖已有输出文件。

最小验证命令：

```bash
/home/liuxiang/pyenv/lx/bin/python scripts/test/instruments/generate_common_usdt_perp_catalog_test.py
git diff --check
```
