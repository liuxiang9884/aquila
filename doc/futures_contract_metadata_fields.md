# 期货合约元数据字段说明

## 目的

`scripts/gate/query_futures_contracts.py` 和
`scripts/binance/query_um_futures_contracts.py` 用于在启动前查询交易所合约基础信息，并输出同一组
`pandas.DataFrame` 字段。当前范围只覆盖下单前最少需要的交易约束字段，不覆盖完整风控、费率、杠杆和风险限额配置。

这组字段的目标是让策略和下单模块先基于统一 schema 做价格格式化、数量格式化、基础上下限检查和交易所适配。更细的交易所特有规则应在后续 adapter 中保留原始字段或单独扩展。

## 当前脚本

Gate USDT futures：

```bash
scripts/gate/query_futures_contracts.py BTC_USDT ETH_USDT --format csv
```

Binance USD-M futures：

```bash
scripts/binance/query_um_futures_contracts.py BTCUSDT ETHUSDT --format csv
```

两个脚本都支持：

```bash
--file symbols.txt
--format table|csv|json
--output-csv output.csv
```

`symbols.txt` 中每行一个 symbol，空行和以 `#` 开头的行会被忽略。

## 统一字段

| 字段 | 含义 | Gate 映射 | Binance 映射 |
| --- | --- | --- | --- |
| `exchange` | 交易所标识。 | 固定 `gate`。 | 固定 `binance`。 |
| `symbol_id` | 本次输出中的内部 ID，从 0 开始按输入 symbol 顺序递增。 | 脚本生成。 | 脚本生成。 |
| `exchange_symbol` | 交易所合约名。 | REST `name`，例如 `BTC_USDT`。 | `exchangeInfo.symbol`，例如 `BTCUSDT`。 |
| `base_asset` | 标的资产。 | 从 `BTC_USDT` 拆出 `BTC`。 | `baseAsset`。 |
| `quote_asset` | 报价资产。 | 从 `BTC_USDT` 拆出 `USDT`。 | `quoteAsset`。 |
| `settle_asset` | 结算或保证金资产。 | 当前固定 `USDT`。 | `marginAsset`。 |
| `status` | 交易状态，脚本统一转大写。 | REST `status`。 | `exchangeInfo.status`。 |
| `contract_type` | 合约类型。 | REST `type`，常见为 `direct`。 | `contractType`，例如 `PERPETUAL`。 |
| `price_tick` | 最小变价单位。 | `order_price_round`。 | `PRICE_FILTER.tickSize`。 |
| `price_decimal_places` | 价格格式化小数位，由 `price_tick` 推导。 | 由 `order_price_round` 推导。 | 由 `tickSize` 推导。 |
| `quantity_step` | 下单数量步长。 | `enable_decimal=false` 时为 `1.0`；`enable_decimal=true` 时暂为空。 | `LOT_SIZE.stepSize`。 |
| `quantity_decimal_places` | 数量格式化小数位。 | `enable_decimal=false` 时为 `0`；`enable_decimal=true` 时暂为空。 | 由 `LOT_SIZE.stepSize` 推导。 |
| `min_quantity` | 最小下单数量。 | `order_size_min`。 | `LOT_SIZE.minQty`。 |
| `max_quantity` | 最大限价单数量。 | `order_size_max`。 | `LOT_SIZE.maxQty`。 |
| `max_market_quantity` | 最大市价单数量。 | `market_order_size_max`。 | `MARKET_LOT_SIZE.maxQty`。 |
| `min_notional` | 最小名义金额。 | 当前为空，Gate contract endpoint 没有直接同义字段。 | `MIN_NOTIONAL.notional` 或 `NOTIONAL.minNotional`。 |
| `notional_multiplier` | 名义金额乘数，用于把交易所原生下单数量转换成名义金额；现货或 base-asset 数量合约通常为 `1.0`。 | `quanto_multiplier`。 | 固定 `1.0`。 |
| `price_limit_up` | 委托价允许向上偏离比例。 | `order_price_deviate`。 | `PERCENT_PRICE.multiplierUp - 1`。 |
| `price_limit_down` | 委托价允许向下偏离比例。 | `order_price_deviate`。 | `1 - PERCENT_PRICE.multiplierDown`。 |
| `market_price_bound` | 市价单可接受的价格偏离边界。 | `market_order_slip_ratio`。 | `marketTakeBound`。 |

## 关键差异

### 数量单位不同

Gate 的 quantity 相关字段表示合约张数；Binance USD-M 的 quantity 相关字段表示 base asset 数量。

因此统一字段可以服务基础校验，但策略层不应直接假设两个交易所的 quantity 单位相同。需要按交易所 adapter 做转换：

```text
Gate notional ~= price * quantity * notional_multiplier
Binance notional ~= price * quantity * 1.0
Spot notional ~= price * quantity * 1.0
```

### Gate decimal size 暂不猜测

Gate 的部分合约会返回 `enable_decimal=true`，例如此前实测 `ETH_USDT` 出现 `order_size_min=0`。当前脚本在这种情况下让：

```text
quantity_step = None
quantity_decimal_places = None
```

这样做是为了避免在未确认 Gate decimal size 规则前，把错误的数量步长写入交易主路径。后续如果确认 Gate 对 decimal contract size 的精确规则，应补测试后再填充这两个字段。

### `price_limit_*` 不是完全同一规则

Gate 的 `order_price_deviate` 是 REST contract 返回的价格偏离比例。Binance 的 `price_limit_up/down` 由 `PERCENT_PRICE` filter 推导。

这两个字段都可用于下单前基础价格保护，但它们不是同一个交易所规则的逐字等价实现。实际订单校验仍应保留交易所 adapter 中的细节。

### `min_notional` 目前只在 Binance 有直接值

Binance USD-M futures 在 `exchangeInfo` 中提供 `MIN_NOTIONAL` 或 `NOTIONAL` filter。Gate futures contract endpoint 当前没有直接同义字段，所以脚本输出为空。

Gate 后续如需最小名义金额约束，应基于交易所订单规则或错误回报进一步确认，不能用 Binance 的规则推断。

## 使用边界

- 这份 schema 只覆盖下单前最小交易约束。
- 不包含完整费率、风险限额、杠杆、持仓模式、账户级 order rate limit 或 user-data 权限信息。
- 不应用 `pricePrecision` / `quantityPrecision` 替代 Binance 的 `tickSize` / `stepSize`；Binance 官方文档明确说明它们不应作为 tick/step 使用。
- `symbol_id` 是脚本输出顺序 ID，不是交易所 ID；生产配置应保证同一 symbol 在系统内有稳定 ID。
