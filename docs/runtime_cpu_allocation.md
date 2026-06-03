# Runtime CPU 分配规范

## 目的

本文记录当前 32 个物理 core 机器上的实盘与测试 CPU 分区约定。后续启动实盘、行情测试、RTT probe、recorder、benchmark 或分析脚本时，必须先按本文检查 CPU 分配，避免测试任务抢占实盘 hot path。

当前机器为 32 个物理 core，CPU id `0-31`。默认分区：

```text
0-15   live / production reserved cores
16-31  test / diagnostics / benchmark cores
```

除非用户明确重新授权，不要把测试 data session、recorder、RTT probe、benchmark、build、replay 或 Python 分析放到 `0-15`。实盘运行时也不要把重 benchmark、全量 build、大规模 replay 或 pcap heavy capture 放到同机并发执行；即使 CPU id 分区不同，测试仍会共享 L3 cache、内存带宽、文件系统、NIC 和 kernel softirq。

## 实盘区 `0-15`

| Core | 默认用途 |
| ---: | --- |
| `0` | OS / shell / guard script / 轻量控制进程；默认不放 hot path。 |
| `1` | live log backend / REST preflight / final check / health guard。 |
| `2` | Gate market data session。 |
| `3` | Binance market data session。 |
| `4` | Strategy / `TradingRuntime` 主线程；当前 Gate order owner 也可按现有 profile 与 strategy 同核。 |
| `5` | live log backend。若未来把 Gate order owner 拆到 CPU5，必须同时迁移 log backend，避免同核竞争。 |
| `6` | Gate order feedback session。 |
| `7` | 备用 live core：account / position feedback、第二条 order session 或 failover。 |
| `8-11` | 备用 live core：future multi-exchange、live data shard 或 dedicated low-rate probe。 |
| `12-13` | live recorder / read-only monitor；只在需要录制时使用，并观察 `overruns` / `skipped`。 |
| `14-15` | 保留 core，用于临时 emergency、handoff 或 failover；不要常驻塞满。 |

当前 LeadLag live affinity profile `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml` 仍是实盘事实源之一，目标为 Gate MD CPU2、Binance MD CPU3、strategy / Gate order owner CPU4、Gate order feedback CPU6、log CPU5。该 profile 位于 `0-15` 实盘区内；如果后续改 profile，必须继续遵守本文分区，或在文档和 onboarding 中同步说明例外。

## 测试区 `16-31`

| Core | 默认用途 |
| ---: | --- |
| `16-19` | 测试 Gate data session / private endpoint 对比。 |
| `20-23` | 测试 recorder / `data_reader_probe` / replay dump。 |
| `24-25` | Gate `OrderSession` RTT probe / protocol probe。 |
| `26-27` | pcap / tcpdump / diagnostics collector。 |
| `28-29` | benchmark / replay / Python analysis。 |
| `30` | test log backend。 |
| `31` | build / `ctest` / scripts 控制进程。 |

4 路 Gate BTC_USDT private plain 行情对比测试推荐形态：

```text
data session: 16, 18, 20, 22
recorder:     17, 19, 21, 23
analysis:     28, 29
test logs:    30
```

`data_reader.execution_policy.bind_cpu_id` 当前只在 parser 中保留配置值，`data_reader_recorder` 本身不会因为该字段自动绑核。需要 recorder 严格跑在测试区时，必须用外层 `taskset`、脚本或后续实现的 runtime policy 显式约束；最终报告中也只能按实际 `ps` / `taskset` / scheduler 证据声明 recorder CPU。

## 操作规则

1. 启动任何实盘或测试前，先检查是否已有同类进程正在运行：

```bash
ps -eo pid,psr,comm,args | rg 'gate_data_session|binance_data_session|data_reader_recorder|lead_lag_strategy|gate_order_session|order_session_rtt_probe'
```

2. 实盘 hot path 默认使用 `0-15`，测试默认使用 `16-31`；测试任务不得占用 `0-15`，除非用户明确要求本轮例外。
3. `core 0` 不作为低延迟 hot path 首选；优先保留给 OS / shell / guard 这类轻量控制任务。
4. 每次 live report、latency report 或 handoff 中涉及性能 / 延迟结论时，应记录实际 CPU 分配、endpoint、TLS/plain、`connect_ip`、是否使用 affinity profile，以及是否存在同机测试负载。
5. 如果测试必须与实盘同机并发，只允许轻量观测类任务；重负载 benchmark、全量 build、大规模 replay、heavy pcap 默认等实盘结束后再跑。
6. 修改 `config/runtime_affinity/*`、data session 临时 TOML 或 live operations pipeline 后，同步检查本文和 `docs/project_onboarding_guide.md` 是否需要更新。
