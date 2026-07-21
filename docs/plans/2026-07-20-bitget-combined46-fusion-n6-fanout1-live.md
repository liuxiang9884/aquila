# Bitget 46-symbol Fusion N=6 Fanout=1 实盘计划

## 目标

- 以 numeric order request 优化提交 `cd1163c` 为代码基线。
- 沿用
  `config/strategies/lead_lag_bitget_combined_46symbols_highspeed_fanout4_20260718.toml`
  的 46-symbol 参数，运行时固定 `order_session_fanout=1`。
- Bitget BBO 使用 `N=6` fastest-route fusion，固定 `3 HS + 3 HA`；Binance 继续使用
  上一轮的 `N=4` canonical fusion。
- 在 fresh-run isolation、guard、quiescence 和最终 REST flat 约束下取得新的真实订单、
  行情、feedback、latency、fill 和 PnL 证据。

## 非目标

- 不修改订单状态机、feedback authoritative source、reconcile 或 stop-and-flat 语义。
- 不启用 order fanout=4，不改变每个 pair 的资金、阈值、slippage 或并发参数。
- 不加入上一轮代理自行增加的 Bitget 账户级 command limiter。
- `fast-fill` 继续只作诊断，不参与订单状态。
- 不把公开 BBO fusion latency 外推为订单 fillability、PnL 或交易所撮合延迟。
- 不在同一 run 内重启 strategy、gateway 或 feedback。

## 已锁定决策

- 代码从 `cd1163c` 建立独立 live branch/worktree，不组合 `f8ccfcf` 或其他未包含在
  优化 HEAD 中的交易功能。
- Source strategy config 有 46 个 pair；10 个 Binance `TRADIFI_PERPETUAL` 只有在当次
  交易时段、双边 BBO 和 freshness 全部通过时才能进入实际 live scope，否则从 strategy、
  两家行情订阅和 REST allowlist 一并排除。
- Bitget market source 固定六路，HA 和 HS 各三路；source 顺序、endpoint、CPU 和实际
  connected endpoint 写入 run artifact。
- Order gateway 固定单 route、`order_session_fanout=1`，不启用账户级 command
  limiter。
- 沿用每 child `open_notional=10`、每 pair `parallel=1` 和全局
  `max_gross_notional=1000`。
- 实际 duration 必须在真实订单启动前由用户明确；未锁定 duration 时只执行无交易副作用的
  准备、构建、测试和只读检查。

## 影响边界

- 生产代码保持 numeric order request 优化 HEAD，不新增新的交易语义。
- 运行期写入仅限新的 `/home/liuxiang/tmp/<run_id>/`，包含 frozen binary、config、
  manifest、log、recorder 和 REST 证据。
- 真实订单范围只包含 fresh preflight 后的实际 active symbols；账户清理使用同一 allowlist。

## 执行步骤

1. 核对优化 HEAD 和 runtime gateway config 均不包含账户级 command limiter。
2. 构建 Release，运行 Bitget gateway、OrderSession、LeadLag、manifest 和 guard focused tests。
3. 生成 fresh run id 和隔离目录，冻结 binary、catalog、strategy、gateway、feedback、
   fusion 和 recorder 配置及 SHA-256。
4. Fresh 查询 account fee、合约状态、TradFi 交易时段和双边 BBO；按证据生成实际 symbol
   scope，配置 fee 不一致时阻断启动。
5. 启动 3 HA + 3 HS Bitget source、Bitget fusion、4 路 Binance fusion 和两路 recorder；
   运行 freshness 与短时 signal-only preflight。
6. 执行无冲突进程检查、REST flat baseline、emergency dry-run、feedback/gateway ready、
   manifest `mark-applied` 和 guard validation。
7. 以用户确认的 duration 启动 guarded live run；立即核对 PID、CPU、route、endpoint、
   config、错误状态和 REST preflight。
8. 每 10 分钟检查关键进程、route ready、freshness、订单生命周期、continuity/unknown、
   REST/SHM health 和 recorder overruns。
9. 正常或异常结束后先证明 gateway/feedback quiescence，再以 fresh REST snapshot 证明 flat，
   最后生成 report。

## 验证策略

- `git diff --check`
- Bitget OrderGateway/OrderSession focused CTest
- LeadLag strategy/runtime focused CTest
- Bitget prepare/guard Python tests
- Release config validate-only
- Fresh fee comparison、行情 freshness/signal-only preflight、REST baseline 与 emergency dry-run
- Live Ack/terminal/reduce-only、quiescence 和 final REST flat

## 回滚与停止

- 组合代码、测试或配置验证失败时不启动真实订单。
- Open order、非零 position、fee mismatch、单边/stale BBO、route not-ready、
  manifest/config/PID mismatch 均 fail closed。
- `UnknownResult`、`ContinuityLost`、unresolved order 或关键进程退出时立即结束
  本轮，执行 strict stop-and-flat；成功后仍保持停机。
- 无法证明 quiescence 或 flat 时进入人工 handoff，不启动下一轮。

## 未决风险

- 本次 duration 尚未由用户明确。
- 10 个 `TRADIFI_PERPETUAL` 是否进入实际 scope 取决于启动时 fresh 交易时段和 BBO 证据。
- Numeric request 优化版本首次进入本组 live 配置，需要 fresh build/test 和 live preflight
  证明。
- Outer guard 被 `SIGKILL`、主机失效或 REST 全不可用时仍需要人工介入。
