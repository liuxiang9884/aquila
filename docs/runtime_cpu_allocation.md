# Runtime CPU 分配规范

## 目的

本文记录当前 32 个物理 core 机器上的实盘与测试 CPU 分区约定。后续启动实盘、行情测试、RTT probe、recorder、benchmark 或分析脚本时，必须先按本文检查 CPU 分配，避免测试任务抢占实盘 hot path。

本文只记录 CPU/IRQ 运行约束和候选系统优化；具体交易、fusion 或 benchmark 流程链接对应领域事实源。

当前机器为 32 个物理 core，CPU id `0-31`。默认分区：

```text
0-15   live / production reserved cores
16-31  test / diagnostics / benchmark cores
```

除非用户明确重新授权，不要把测试 data session、recorder、RTT probe、benchmark、build、replay 或 Python 分析放到 `0-15`。实盘运行时也不要把重 benchmark、全量 build、大规模 replay 或 pcap heavy capture 放到同机并发执行；即使 CPU id 分区不同，测试仍会共享 L3 cache、内存带宽、文件系统、NIC 和 kernel softirq。

## 硬件 / 系统优化基线

本节记录 2026-07-02 对当前机器的只读检查和后续优化建议。这里的 CPU / IRQ profile 是候选运行方案，不代表已经完成内核级隔离，也不代表已经证明 latency 或 fillability 收益；任何性能结论都必须重新用 live smoke、RTT probe、data session latency 或 benchmark 证据确认。

当前机器基线：

- `lscpu` 显示 32 个 vCPU，CPU id `0-31`，单 NUMA node，`Thread(s) per core = 1`，运行在 `KVM / AWS` 环境。
- 当前 kernel command line 没有 `isolcpus`、`nohz_full`、`rcu_nocbs` 或 `irqaffinity`；`/sys/devices/system/cpu/isolated` 为空，`/sys/devices/system/cpu/nohz_full` 为 `(null)`。
- 当前默认 cpuset / affinity 允许 `0-31`，`irqbalance` active，`/proc/irq/default_smp_affinity = ffffffff`。
- `enp55s0` 使用 AWS ENA driver；当前 `ethtool -l enp55s0` 显示 `Combined: 8`，`ethtool -x enp55s0` 的 RSS indirection table 均匀映射到 queue `0-7`。
- RPS / XPS 当前关闭：`/sys/class/net/enp55s0/queues/rx-*/rps_cpus = 00000000`，`tx-*/xps_cpus = 00000000`。
- ENA coalescing 当前为 `Adaptive RX: on`、`rx-usecs: 20`、`tx-usecs: 0`；ring 当前为 `RX=1024`、`TX=1024`，RX max `8192`，TX max `1024`。
- 当前 ENA IRQ 有一部分落在 live / 临时 live core 上：`Tx-Rx-0 -> CPU11`、`Tx-Rx-2 -> CPU7`、`Tx-Rx-5 -> CPU5`、`Tx-Rx-6 -> CPU9`、`Tx-Rx-7 -> CPU16`，management IRQ 在 `CPU18`。

参考资料：

- Linux networking scaling: `https://docs.kernel.org/networking/scaling.html`
- Linux IRQ affinity: `https://docs.kernel.org/core-api/irq/irq-affinity.html`
- Linux ENA driver: `https://docs.kernel.org/networking/device_drivers/ethernet/amazon/ena.html`
- AWS ENA latency tuning: `https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ena-improve-network-latency-linux.html`
- AWS ENA best practices: `https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/ena/ENA_Linux_Best_Practices.rst`
- Ubuntu CPU / IRQ isolation guidance: `https://documentation.ubuntu.com/real-time/latest/how-to/cpu-boot-configs/`

### 候选隔离 profile

如果继续当前 30-symbol fusion order-gateway 形态，`16-19` 已被 4 条 order route worker 临时视为 live core。此时不应简单沿用“`2-15` isolated live、`16-31` test”的硬隔离 profile；候选方案应改为：

```text
CPU0-1    housekeeping / OS / shell / guard / REST / journald
CPU2-19   live isolated cores，包括 fusion、data sessions、strategy、feedback、4 route workers
CPU20-27  ENA Tx/Rx queue IRQ pool
CPU28-31  test / diagnostics / build / Python analysis
```

对应 boot 参数方向：

```text
isolcpus=managed_irq,domain,2-19 nohz_full=2-19 rcu_nocbs=2-19 irqaffinity=0-1,20-31
```

如果后续不使用 30-symbol order-gateway 的 `16-19` worker 临时例外，可以退回更保守的候选隔离：

```text
CPU0-1    housekeeping
CPU2-15   live isolated cores
CPU16-19  test / diagnostics 或低优先级 live spare，按当轮 profile 决定
CPU20-27  ENA Tx/Rx queue IRQ pool
CPU28-31  test / build / analysis
```

### ENA IRQ / queue 建议

第一版优化建议优先把 ENA IRQ 从 live hot core 搬走，而不是把每个网卡 queue 精确放到对应交易线程同核。原因：

- 当前 ENA 只有 8 个 combined queue，RSS 按 4-tuple hash 把 TCP flow 分配到 queue；`ntuple-filters` 当前为 `off [fixed]`，不能稳定地把某个 Gate / Binance session 指定到某个 RX queue。
- 把 IRQ 放到 hot core 可能改善 cache locality 和 p50，但会让 hardirq / NAPI / softirq 抢占 active-spin owner thread，容易扩大 p99 / p99.9 / max。
- 当前架构中 Gate / Binance source data sessions、fusion thread、strategy / `TradingRuntime`、Gate order gateway worker 和 feedback session 都是低延迟 owner thread，默认应优先保护这些线程不被网卡 IRQ 抢占。

候选 ENA IRQ affinity：

```text
ena-mgmt IRQ     -> CPU1
Tx-Rx queue0 IRQ -> CPU20
Tx-Rx queue1 IRQ -> CPU21
Tx-Rx queue2 IRQ -> CPU22
Tx-Rx queue3 IRQ -> CPU23
Tx-Rx queue4 IRQ -> CPU24
Tx-Rx queue5 IRQ -> CPU25
Tx-Rx queue6 IRQ -> CPU26
Tx-Rx queue7 IRQ -> CPU27
```

实际 IRQ id 会随 reboot / device reset 变化，不能硬编码数字；每次启动前应按名称重新枚举：

```bash
grep -E 'ena|enp55s0|Tx-Rx' /proc/interrupts
for irq in $(grep -E 'enp55s0|ena|Tx-Rx' /proc/interrupts | cut -d: -f1 | tr -d ' '); do
  echo -n "$irq "
  cat /proc/irq/$irq/smp_affinity_list
done
```

如果使用 `irqbalance`，需要配置 banned CPU 或在 live 窗口内用受控脚本重复检查，避免 irqbalance 覆盖手工 affinity。AWS ENA best practices 默认不建议直接关闭 irqbalance；如果为了严格隔离而停用或限制 irqbalance，必须在 run report / handoff 中记录。

### RPS / XPS / coalescing 建议

- RPS：第一版保持关闭。当前 ENA 已有 8 个硬件 RSS queue，RPS 会引入 IPI 和 remote backlog，在当前低延迟主路径中可能增加 jitter；只有当某个 RX queue CPU 饱和或 ENA stats 出现 RX drop / overrun 时，再单独设计 RPS A/B。
- XPS：作为第二阶段优化测试。可以尝试为 order gateway worker 所在 CPU 配置 TX queue 映射，降低 TX queue lock contention；但 XPS 不决定 ACK / feedback 回来的 RX queue，因此不能把它当成 RX 定向方案。
- RSS queue 数：保留 `Combined=8` 作为第一版。减少 queue 数可节省 IRQ core，但会增加多个行情 / order flow 撞同一 RX queue 的概率。
- Interrupt moderation：当前 `Adaptive RX: on`、`rx-usecs=20`。第一轮先把 IRQ 从 live core 搬走并保持现状；第二轮再测试 `adaptive-rx off rx-usecs 0 tx-usecs 0`。关闭 moderation 可能降低单包网络延迟，也会提高 interrupt overhead；是否采用必须看 p99 / p99.9 / max 和 ENA stats。

### 验证要求

硬件 / 系统优化的验证不能只看配置是否写入成功，必须至少记录：

```bash
cat /proc/cmdline
cat /sys/devices/system/cpu/isolated
cat /sys/devices/system/cpu/nohz_full
cat /proc/irq/default_smp_affinity
grep -E 'ena|enp55s0|Tx-Rx|nvme' /proc/interrupts
for irq in $(grep -E 'enp55s0|ena|Tx-Rx' /proc/interrupts | cut -d: -f1 | tr -d ' '); do
  echo -n "$irq "
  cat /proc/irq/$irq/smp_affinity_list
done
ethtool -l enp55s0
ethtool -x enp55s0
ethtool -c enp55s0
ethtool -S enp55s0
```

性能侧至少比较：

- Gate / Binance data session 或 fusion 的 `exchange_ns -> local_ns`、SHM publish / reader gap、`skipped` / `overruns`。
- LeadLag strategy loop gap、`request_send_local_ns` fanout spread、Ack RTT p50 / p99 / p99.9 / max。
- Gate order Ack `x_in -> x_out`、上行 / 下行拆解和 ENA queue / IRQ CPU 关联。
- ENA `rx_*drop*`、`rx_overruns`、`queue_*_tx_queue_stop`、`pps_allowance_exceeded`、`bw_*_allowance_exceeded` 是否异常。

在 AWS/KVM 上，即使完成 CPU / IRQ 隔离，也不能排除 hypervisor / host noisy neighbor；最终报告必须说明运行环境和是否开启 full isolation。

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

31-symbol no-TON fusion live smoke 配置族使用独立 `gate_data_fusion` / `binance_data_fusion` launch config，不由 `config/runtime_affinity/lead_lag_requested_12symbols_node0.toml` 自动覆盖。已纳入仓库的配置当前分配如下：

| 组件 | Core |
| --- | --- |
| Gate fusion thread | `2` |
| Gate source data session threads | `8-11` |
| Gate fusion log backend | `1` |
| Binance fusion thread | `3` |
| Binance source data session threads | `12-15` |
| Binance fusion log backend | `7` |
| LeadLag strategy / Gate order owner | `4` |
| LeadLag log backend | `5` |
| Gate order feedback session | `6` |

该 profile 会占用实盘备用 core `8-15`，不应与另一套 live market-data profile 或重型测试任务并发运行，除非本轮明确记录这是有意例外。使用 fusion 行情生成 report 时，按实际启动的 fusion TOML、`ps` / log 证据记录 CPU 分配；不要把单路 data session affinity split 套用到 fusion bundle。

多路 fusion order-gateway smoke / validate 配置
`config/order_gateways/gate_order_gateway_30symbols_private_plain_20260627.toml` 和
`config/order_gateways/gate_order_gateway_30symbols_private_plain_20260701.toml` 额外使用 `16-19` 作为 4 条 Gate
order route worker CPU；`config/order_gateways/bitget_order_gateway_4routes.toml` 同样把四条 Bitget private
OrderSession worker 固定在 `16-19`。Gateway log backend 使用 `0`。这是 fusion 配置已经占用 `2/3/8-15`，strategy /
feedback / log 已占用 `4/5/6` 后的临时例外；若按这些配置启动真实订单，必须把 `16-19` 视为本轮 live 占用 core，
不得同时运行测试 data session、RTT probe、benchmark、replay 或 Python 分析，并在 report / handoff 中记录实际
`ps` / affinity 证据。正式宣称 latency 或 fillability 收益前，需要重新审视 CPU profile，避免把测试区例外误当作
稳定生产分配。

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
ps -eo pid,psr,comm,args | rg 'gate_data_session|binance_data_session|gate_data_fusion|binance_data_fusion|data_reader_recorder|lead_lag_strategy|gate_order_session|order_session_rtt_probe'
```

2. 实盘 hot path 默认使用 `0-15`，测试默认使用 `16-31`；测试任务不得占用 `0-15`，除非用户明确要求本轮例外。
3. `core 0` 不作为低延迟 hot path 首选；优先保留给 OS / shell / guard 这类轻量控制任务。
4. 每次 live report、latency report 或 handoff 中涉及性能 / 延迟结论时，应记录实际 CPU 分配、endpoint、TLS/plain、`connect_ip`、是否使用 affinity profile，以及是否存在同机测试负载。
5. 如果测试必须与实盘同机并发，只允许轻量观测类任务；重负载 benchmark、全量 build、大规模 replay、heavy pcap 默认等实盘结束后再跑。
6. 修改 `config/runtime_affinity/*`、data session 临时 TOML 或 live operations pipeline 后，同步检查本文和 `docs/project_onboarding_guide.md` 是否需要更新。
