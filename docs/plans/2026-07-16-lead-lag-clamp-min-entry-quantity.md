# LeadLag 最小开仓量 clamp 实施计划

## 目标

将 20 个 Bitget symbol 的 `open_notional` 统一设置为 `10 USDT`。LeadLag 开仓数量按 `open_notional` 和当前订单价格
计算后低于 instrument `min_quantity` 时，把数量提升为 `min_quantity`；高于最小量时保留计算结果。四路 fanout 的
每个 entry child 继续使用相同数量。

## 非目标

- 不在策略热路径查询 REST 或动态改写 instrument metadata。
- 不改变 normal close、stoploss、retry 的 full-position `reduce-only` 数量语义。
- 不删除既有信号生成、freshness、`parallel` 或订单状态机语义；本次“无其他拦截”限定为开仓数量计算路径。
- 不启动真实订单，也不把自动测试结果表述为四路 live 证据。

## 关键决定与边界

- 删除此前未经用户明确同意新增的 `require_min_entry_quantity` 配置、strict equality gate、reject reason、prepare
  校验及其测试/文档；不保留兼容入口。
- 开仓数量统一使用 `max(calculated_quantity, min_quantity)`；高于最小量时不因 quantity 被拒绝。
- clamp 目标是 runtime instrument catalog 中的 `min_quantity`；live 配置仍负责把 Bitget `minOrderAmount` 转换为当次有效
  最小可成交数量并写入 catalog。
- clamp 必须在 route selection、risk reservation 和第一张 child 提交前完成；risk 按实际 fanout 数乘以 clamp 后数量。
- `min_quantity` 缺失或非法时不猜测数量，保留 `zero_quantity` fail-closed 行为。
- 新增 20-symbol Bitget 四路 LeadLag source config；所有 pair 使用 `open_notional=10`、`order_session_fanout=4`。

## 实施步骤

1. 增加失败测试：四路配置在计算值低于最小量时应向四路各提交最小量，高于最小量时保留计算数量。
2. 删除 strict-min gate 全部代码，并把整数数量计算改为最小量 clamp。
3. 增加 20-symbol 四路 source config，自动检查 pair 数、symbol 集合、全部 `open_notional=10` 和 fanout=4。
4. 更新 LeadLag、Bitget trading、诊断字段与 onboarding 中的当前 contract。
5. 完成 focused build/test、Python live prepare/guard 回归、Release 全量 build/ctest、diff/evaluation 边界检查。
6. 原子提交实现与文档，迁移长期事实后删除本计划，push feature 并按用户要求 fast-forward 到 `main`。

## 回滚

回滚实现提交即可恢复“低于最小量返回 `zero_quantity`”和 strict-min equality gate；新配置独立删除即可回滚。

## 剩余风险

- runtime catalog 的 `min_quantity` 是当次快照；若它未吸收 Bitget `minOrderAmount`，clamp 到 API 原始 `minOrderQty`
  仍可能被交易所拒绝。
- 固定有效最小量不能覆盖价格下跌导致的 `minOrderAmount` 变化；runtime catalog 必须在每轮 fresh run 前按当次
  Bitget contract/ticker 重新生成。
