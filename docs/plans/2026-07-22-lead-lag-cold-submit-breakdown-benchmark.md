# LeadLag cold submit 分段 benchmark 计划

## 目标

建立与 `20260722_052013_bitget_combined46_n6_hs_fanout1_24h` 更接近的
LeadLag submit benchmark，分别测量 warm submit 与经过 46-symbol 行情工作集扰动后的
cold submit，并把 `signal_decision_ns` 到首个 gateway command 的延迟归因到现有
submit stage。

基线固定为实盘使用的 commit `deba9c3`。结果只解释本机策略 submit 路径，不声明交易所
网络、成交率或 PnL 收益。

## 非目标

- 不修改生产策略、订单状态机、fixed risk slot、fanout 或真实交易配置的行为。
- 不启动真实订单，不修改本机 `kernel.perf_event_paranoid` 或其他系统设置。
- 不用 `sleep`、随机噪声或一次性 `clflush` 代替持续行情造成的工作集扰动。
- 本轮只建立可信测量面并定位成本，不直接做生产优化。

## 当前证据与缺口

- 实盘 `decision → strategy request_send` P50 为 `6.925 us`；其中 log landmark 显示
  `signal-triggered → order-intent` P50 为 `4.778 us`。
- 延迟随距上一条 triggered signal 的间隔增长：`≤1 s`、`1–10 s`、`>10 s` 的
  P50 分别为 `2.876 us`、`6.382 us`、`7.189 us`。
- 现有 `BM_LeadLagSubmitPathBreakdownOrderGatewayLiveLike30RiskOn` 的正式 timed submit
  紧跟在 setup submit 后；现有 stage P50 约 `1.02 us`，代表 warm 下限，不能解释实盘
  cold 路径。
- 生产实盘没有逐 stage timestamp；`perf` PMU/call graph 因系统策略不可用。

## 关键设计

### 测量 workload

建议用确定性的 46-symbol non-triggering `BookTicker` sweep 持续驱动同一个 Strategy runtime，
污染与实盘相同的行情处理、pair state 和分支工作集，然后触发一次 fanout=1 submit。保留立即
再次触发的 warm case 作为同进程对照。

已锁定：主要结论使用 deterministic 46-symbol churn。实盘 strategy thread 持续处理行情，
并非睡眠等待；artificial cache eviction / idle 只作为机制诊断，不作为主要 workload。

### 配置与边界

- 从本次实盘配置加载 46-symbol pair/instrument/risk 参数，并在 benchmark 内强制
  `order_session_fanout=1`。
- 使用内存中的 `OrderGatewayClient`/SHM queue；首个 route ready，不启动网络 session。
- logger 保持 `info`，file sink 写到 `/home/liuxiang/tmp`；同时提供 log-off 对照，区分 Quill
  frontend copy/enqueue 与业务计算。
- 只在 `AQUILA_LEAD_LAG_STRATEGY_ENABLE_TEST_HOOKS` 下记录 stage，不给生产 binary 增加字段。
- 计时端点同时报告：decision → signal log done、逐 submit stage、首个 gateway command
  timestamp、实际 `TryPush` 完成。

### 统计方法

- 固定诊断 CPU `16`，先保存环境与负载快照。
- 每个 case 至少 5 组独立进程，每组至少 1,024 个有效 sample；报告每组
  P50/P95/P99/max 和五组中位数，不把不同起止点的百分比相加。
- 对 churn sweep 数做最小校准，直到 cold case 的各段分布进入稳定平台；不得为了贴合
  `6.925 us` 人工选择参数。
- observer/timestamp 开销用 no-stage 对照量化；若测量开销改变结论，停止归因。

## 实现步骤

1. 加入一个会失败的 benchmark contract check，证明现有 case 的 timed submit 前存在刚执行的
   warm submit，且没有 cold stage sample。
2. 将 runtime 构造、non-triggering 46-symbol churn、单次 submit trigger 和 stage 采样拆成可复用
   benchmark fixture；不改变生产接口。
3. 增加 warm、46-symbol churn cold、log-off 诊断 case，并输出一致的 stage counters。
4. 先跑 focused build/test，再在固定 CPU 上做五组 fresh benchmark。
5. 对照实盘 landmark、benchmark stage 与 endpoint 语义，明确哪些结论已证明、哪些仍是推断。

## 验证

- 构建 `lead_lag_submit_breakdown_benchmark`。
- 运行新增 benchmark contract test/focused strategy tests。
- 运行 warm/cold/log-off case，检查每组有效样本数、stage 单调性、无 submit failure。
- `git diff --check`，并确认生产 target 不启用 test hooks。
- 最终独立 review benchmark 是否意外把 setup、logger backend flush、runtime 构造或多 route
  fanout 计入目标区间。

## 回滚

改动限定于 benchmark/test 和本计划文档。若 workload 无法稳定复现实盘量级或 observer 开销过大，
回滚新增 benchmark，不修改生产代码，也不据此提出优化。

## 未决风险

- 确定性 ticker churn 能复现工作集压力，但不能直接证明某一级硬件 cache miss；PMU 不可用时只
  能通过 cold/warm、log-on/off 和逐 stage 对照做机制推断。
- 实盘 46-symbol 输入频率与 symbol 分布不均匀；固定 sweep 是可重复近似，不是逐 tick replay。
- logger 的 bounded queue 初次分配和 backend 消费状态可能污染首批 sample，必须预热 logger 并
  丢弃初始化样本。
