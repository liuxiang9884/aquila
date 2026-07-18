# Bitget 46-symbol Fusion 24 小时实盘计划

## 目标

- 使用 `config/strategies/lead_lag_bitget_combined_46symbols_highspeed_fanout4_20260718.toml`
  的 46 个 Binance-lead / Bitget-lag pair 和逐 symbol 参数。
- Bitget BBO 使用 `N=8` fastest-route fusion，固定 `4 HA + 4 HS`；Binance BBO 使用
  `N=4` fastest-route fusion。策略只读取两路 canonical SHM。
- 在 fresh-run isolation、guard、quiescence 和最终 REST flat 约束下，取得最长 24 小时的真实订单、
  行情、feedback、latency、fill 与 PnL 证据。

## 非目标

- 不改变订单状态机、feedback authoritative source、reconcile 或 stop-and-flat 语义。
- `fast-fill` 继续只写诊断日志，不参与 feedback 或交易状态。
- 不把公开 BBO fusion latency 外推为订单 fillability、PnL 或交易所撮合延迟。
- 不在本轮引入不受验证的 endpoint failover、strategy restart 或跨 run resume。

## 已锁定输入

- Instrument catalog 使用大 catalog `config/instruments/usdt_future_universe.csv`。
- Bitget public SBE endpoint 顺序为 source `0-3` HA
  `vip-ws-uta.bitget.com`、source `4-7` HS `vip-ws-uta-pub-a.bitget.com`。
- Binance public market data 使用四路当前 production endpoint contract；实际 host/IP/TLS 在冻结配置和
  run manifest 中记录。
- 运行 duration 上限为 `86400s`；任何证据门、freshness、账户、route 或进程检查失败都提前终止，不为凑满
  24 小时而重启。
- 使用 Release binaries、run-specific market/gateway/feedback SHM、runtime configs、manifest 和 log。

## Grill Me 已锁定决策

1. **订单 fanout**：本轮 runtime overlay 固定 `order_session_fanout=1`，不使用 source config
   中尚无真实订单证据的 `fanout=4`。行情 fanout 与订单 fanout 相互独立。
2. **资金与并发**：保持每 child `open_notional=10`、每 pair `parallel=1` 和全局
   `max_gross_notional=1000`。
3. **TradFi perpetual 范围**：10 个 Binance `TRADIFI_PERPETUAL` 必须在启动时逐
   symbol 通过交易时段、双边 BBO 和 freshness；不满足的 symbol 从本轮 strategy、两家行情订阅和
   REST allowlist 一并排除，其余 symbol 继续启动。最终实际 symbol 数以 fresh preflight 为准。
4. **BBO recorder**：为 Bitget/Binance canonical fusion 各启一个独立 recorder，以支持后续
   signal/order/BBO 离线匹配；recorder 不进入交易链路。
5. **账户级限速**：Bitget gateway 使用所有 route 共享的滚动 1 秒窗口，最多接受 5 个 trade
   command，并为 cancel / reduce-only 预留 2 个 slot。命令不排队；触发限速时发布明确 rejection，
   外部 supervisor 立即终止本轮并执行既有 strict stop-and-flat。
6. **停止条件**：沿用 Gate 式 fail-closed；关键进程退出、route not-ready、continuity/unknown、
   stale canonical BBO、账户限速或 gross exposure 证据失败都提前终止，不在同一 run 内重启。

## CPU 与资源边界

- 当前 32 个 physical core、单 NUMA node。使用 `CPU 2` strategy、`CPU 3` feedback、
  `CPU 4` gateway route、`CPU 5` Bitget fusion、`CPU 6-13` Bitget sources、
  `CPU 14` Binance fusion、`CPU 15-18` Binance sources。
- `CPU 20-27` 预留 ENA IRQ/RSS；`CPU 28-29` 分配两路 recorder，`CPU 30-31`
  分配 market-data logger，`CPU 0-1` 分配 guard、REST、低频 logger 和 housekeeping，
  `CPU 19` 保留 emergency/control。若 fresh IRQ/RSS 检查与该图不一致，在启动前调整 runtime
  CPU 图而不是让关键线程与 IRQ 重叠。
- 本轮整机禁止并发 benchmark、replay、build、RTT probe 和其他 fusion 测试。
- 关键线程必须逐线程验证 `/proc/<pid>/task/*/status` affinity；不能只信 TOML 或进程主线程。
- 启动前记录 ENA IRQ/RSS、CPU topology、当前负载、磁盘、`/dev/shm` 和网络 drop 计数；不在未 A/B 的情况下
  修改 IRQ/coalescing 系统配置。

## 分阶段执行

1. Grill Me 锁定上述决策并重新审查本计划。
2. fresh 查询 Bitget fee、46/46 双边合约状态、TradFi 交易时段与双边 BBO；任一配置费率不一致先更新并提交。
3. 生成冻结 runtime configs、二进制/catalog SHA-256、CPU 图和全量 symbol allowlist。
4. 运行 build/focused tests、fusion/gateway/strategy validate-only、行情 dry-run 与短时 signal-only preflight。
5. 按 `docs/lead_lag_live_operations.md` 做 fresh REST baseline、emergency dry-run、feedback/gateway
   readiness、manifest `mark-applied` 和 guard 验证。
6. 只按 `order_session_fanout=1` 启动；任何阶段失败都 quiesce + stop-and-flat，禁止同 run 重启。
7. 长跑期间至少每 10 分钟检查绑定 PID、route readiness、freshness、错误、订单生命周期、REST/SHM health 和
   recorder skipped/overruns。
8. 自然或异常结束后，只以绑定进程 quiescence、guard summary 和 fresh REST flat 收敛；随后生成 report。

## 验证与回滚

- 所有真实订单前置检查必须 fresh，不复用历史 flat、fee、合约状态或 endpoint 证据。
- 启动前任一 open order、非零 position、fee mismatch、stale/单边 BBO、route not-ready、manifest/config/PID
  mismatch 都 fail closed。
- `UnknownResult`、`ContinuityLost`、unresolved order 或关键进程身份变化立即停止新开仓，执行 strict
  stop-and-flat，成功后仍保持停机。
- 只有 `quiescence.ok=true` 后才允许 final REST 或 emergency mutation；无法证明 flat 时返回失败并人工 handoff。

## 已知风险

- source config 的 fanout=4 entry 是四张 full-size child，不是把单笔数量四等分；本轮使用
  fanout=1 runtime overlay，且在真实订单前完成 account-wide limiter。
- 10 个 TradFi perpetual 不是 24x7 稳定交易，catalog 状态不能替代当时 BBO/freshness。
- 主机没有 kernel full isolation，且 ENA IRQ 当前可能落在 live core；即使重新绑核仍有 hypervisor、NIC、L3 cache
  和内存带宽共享噪声。
- Outer guard 被 `SIGKILL`、主机失效或 REST 全不可用仍需要人工介入。
