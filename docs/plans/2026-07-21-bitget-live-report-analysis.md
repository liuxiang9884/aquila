# Bitget LeadLag live report 一等支持计划

## 目标

把当前以 Gate 为中心的 LeadLag live report 扩展为对 Bitget 一等支持，并让本轮
`20260720_162559_bitget_combined46_n6_fanout1_12h` 可以由标准 pipeline 生成单一事实源：

- 正确关联 Bitget instrument、symbol、gateway Ack、order feedback 和 late Ack；
- 输出 entry signal/order/fill 漏斗、实际成交率、close retry 和最终一致性；
- 用一行一个 `execId` 的 execution CSV 对账 fast-fill、authoritative order 与可选 REST fills；
- 用 recorder BookTicker binary 对 IOC 在 Bitget `cTime` 到 `execTime`/cancel `updatedTime`
  窗口做保守的 marketability 分类；
- 在标准 `report.md` 中输出 Bitget 专属 latency、实际 fee/PnL 和证据边界，不再依赖手工
  `analysis_report.md` 修正错误结果。

## 非目标

- 不修改 strategy、gateway、feedback、market-data 或真实交易行为。
- 不把 fast-fill 升级为订单状态源；authoritative 状态仍来自 `order` channel。
- 不把跨本机/Bitget 时钟差解释为单程网络时延。
- 不从 public BBO 推断交易所漏撮合；同毫秒 mixed/missing 保持不确定。
- 不用本轮与历史不同 topology/config 的 run 声明因果性能提升。
- 不在正在运行的实盘目录中改写日志、binary 或 config。

## 关键决策

1. 保持现有 `signal.csv`、`order_detail.csv`、`position.csv`、`latency.csv` 向后兼容；新增：
   - `execution_detail.csv`：每个 Bitget fast-fill/REST execution 一行；
   - `order_fillability.csv`：每个 submitted Bitget IOC 一行。
2. CLI 新增显式 `--exchange`、可重复 `--additional-log`、可选
   `--book-ticker-manifest` 和 `--rest-fills-json`；旧 Gate 单 `--log` 调用继续工作。
3. instrument catalog 按目标 exchange 选择，并同时支持策略 `ALLO_USDT`、交易日志
   `ALLOUSDT` 和 catalog `exchange_symbol` 的 alias。
4. Bitget gateway/feedback 原始日志按 `local_order_id`/`clientOid` 合并；日志文件顺序不影响
   terminal-before-Ack 的最终结果。
5. `execution_detail.csv` 以 fast-fill 为实时执行事实，REST fills 用于补充实际 fee、
   `execPnl` 和最终 execution 对账；同一 `execId` 只保留一行并显式记录来源覆盖。
6. BBO 分类只在同一 Bitget exchange 毫秒域中比较：
   `all_cross`、`no_cross`、`mixed`、`missing`。Report 层映射为保守的
   `marketable_observed`、`not_marketable_observed`、`indeterminate`。
7. Bitget latency 不复用 Gate `x_in/x_out`：输出本地 monotonic/realtime 可比 duration、
   `cTime -> exec/cancel` 同交易所时钟 duration，以及 fast-fill/order push 到达差。
8. 跨 symbol 滑点总览以 notional-weighted bps 为主，ticks 只保留 per-symbol/诊断用途。

## 影响边界

- `scripts/lead_lag/analyze_order_detail.py`
- 新增 Bitget execution/fillability 分析模块（位于 `scripts/lead_lag/`）
- `scripts/lead_lag/generate_live_report.py`
- 对应 `scripts/test/lead_lag/` 测试与 fixture
- `docs/lead_lag_live_report_csv_schema.md`
- `docs/lead_lag_live_operations.md`
- 若新增/修改诊断字段含义，仅更新 report schema；本任务不修改生产 log key，因此不修改
  `docs/diagnostic_fields.md`。

## 实施步骤

1. 写失败测试证明当前 parser 忽略 Bitget gateway/feedback、catalog 只读取 Gate、late Ack
   无法恢复。
2. 实现 exchange-aware instrument alias 和多日志合并，补齐 Bitget order/latency 字段。
3. 写失败测试并实现 fast-fill execution parser、REST fill enrichment、quantity/coverage 对账。
4. 写小型 typed BookTicker fixture，证明已成交 `execTime` 和零成交 IOC 窗口分类。
5. 扩展 report 漏斗、fillability、fast-fill、Bitget latency、fee/PnL、安全/flat 摘要。
6. 同步 CSV schema 和 Report Pipeline 命令。
7. 用合成 fixture 运行单元测试，再对已结束后的本轮真实日志生成候选 report 到
   `/home/liuxiang/tmp`，核对计数、fast-fill coverage、BBO分类和 final flat。

## 验证策略

- 先运行新增测试并确认因目标能力缺失而失败。
- Focused：
  - `scripts/test/lead_lag/analyze_order_detail_test.py`
  - 新增 Bitget execution/fillability test
  - `scripts/test/lead_lag/generate_live_report_test.py`
- Regression：运行 `scripts/test/lead_lag/` 下所有 `*_test.py`。
- 真实 run：生成候选 report，核对 submitted/finished、order fill/fast-fill、REST fill、
  BBO coverage 和最终 flat；不改 run 原始目录。
- 文档与边界：`git diff --check`。

## 回滚方案

- 新功能由 `--exchange bitget` 和可选输入启用；旧 Gate 默认路径保持不变。
- 每个阶段形成独立原子 commit，可按 commit 逐项 revert。
- 如真实 run 对账失败，不发布 report schema 变更，回滚到上一个通过 fixture 与 Gate
  regression 的 commit。

## 未解决风险

- Bitget `cTime`、order `updatedTime`、fast-fill `execTime` 与 public BBO timestamp 虽经成交
  样本校准，仍只有毫秒精度且未承诺属于同一内部服务时钟。
- Public BBO 不是 matching-engine 内部 order book；fillability 只能给观察分类。
- REST fills 需要 run 时间范围和账户权限；缺失时 report 必须明确实际 fee/PnL 不可用，
  不能回退为看似精确的零值。
- 大型 recorder segments 需要 mmap/局部时间窗口读取，不能全量载入内存。
