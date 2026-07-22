# Bitget Fusion mixed HA/HS N 扩展两小时对比计划

## 目标

- 同时运行 `N=2/4/6/8` 四组 Bitget fastest-route fusion，每组 source 数量严格按一半 HA、一半 high speed（HS）分配。
- 每组用一个独立 `data_reader_recorder` 保存 canonical BBO bin 和 fusion metadata，运行两小时后比较 source 数扩展对 latency、tail latency、fastest-source 分布与数据完整性的影响。
- 固定 symbols、feed、catalog、二进制、运行时长和主机环境，使四组差异仅来自 fusion source 数量。

## 非目标

- 不启动策略、OrderSession 或真实订单。
- 不修改 fusion、data session 或 recorder 的生产代码和持久化格式。
- 不把本机两小时观测外推为长期 endpoint 稳定性、fillability 或 PnL 结论。

## 关键决策与边界

- 四组分别为 `mixed_n2`、`mixed_n4`、`mixed_n6`、`mixed_n8`；每组前半 source 使用 HA
  `vip-ws-uta.bitget.com`，后半使用 HS `vip-ws-uta-pub-a.bitget.com`，均连接
  `/v3/ws/public/sbe`。
- 固定订阅 `BTCUSDT`、`ETHUSDT`、`SOLUSDT` 的 `book_ticker`，统一使用大 catalog
  `config/instruments/usdt_future_universe.csv`。
- `max_events_per_source=1`，每组只录制 fusion canonical output，并同时保存 metadata；不录制各 source 原始流。
- 四组并行运行 `7,200,000 ms`。冻结 Release 二进制、catalog、生成配置和 SHA-256 到独立 run 目录。
- 24 个关键线程独占 CPU `8-31`：
  - `mixed_n2`：fusion `8`，sources `9-10`；
  - `mixed_n4`：fusion `11`，sources `12-15`；
  - `mixed_n6`：fusion `16`，sources `17-22`；
  - `mixed_n8`：fusion `23`，sources `24-31`。
- recorder main 分别绑定 CPU `0/2/4/6`；每组 fusion 主线程与冷路径 logger 共用 CPU
  `1/3/5/7`。不同组的关键线程不共享 CPU。

## 执行步骤

1. 确认没有遗留 Aquila fusion、recorder 或交易进程，并检查 `/dev/shm`、磁盘、内存余量。
2. 生成四组冻结配置和 supervisor；对所有 fusion config 执行 `--dry-run`，并运行相关 focused tests。
3. 启动四个 fusion，稳定 12 秒后启动四个 recorder；校验八个进程存活、20 条 WebSocket
   连接、线程绑核和 bin/metadata 持续增长。
4. 两小时到期后只由绑定 PID 的 supervisor 收敛 recorder；确认进程 quiescence、正常退出码和产物完整。
5. 使用统一脚本分析四组的 ingress latency、fusion hop、source winner、时间桶和异常计数，形成可追溯报告。

## 验证策略

- 静态验证 TOML 可解析、source 数为 `2/4/6/8`、每组 HA/HS 各半、关键 CPU 全局唯一。
- fresh dry-run 覆盖四组；fresh CTest 覆盖 Bitget fusion/data session 与 recorder config。
- 启动后读取 `status.json`、`process_manifest.json`、`ss` 和 `/proc/<pid>/task/*/status`，
  以实际 PID、连接和 affinity 为准。
- 分别间隔读取 canonical bin 与 metadata 大小，确认四组都在增长；检查 decode error、
  disconnect、reconnect、overrun 和 supervisor error。
- 最终结果只以本次冻结 run 目录中的配置、日志、bin、metadata、状态和分析文件为证据。

## 回滚与停止

- 启动检查失败时 supervisor fail closed，只终止 manifest 中记录且 start ticks 匹配的本轮进程。
- 运行中任一 fusion 或 recorder 意外退出，终止本轮其余进程并标记失败，不自动重启或切换 endpoint。
- 人工停止时只向本轮 supervisor 发送 `SIGTERM`，由其收敛子进程；不得使用模糊进程名批量终止。

## 当前执行状态

- `2026-07-18T05:00:37Z` 的首次启动检查在建连前 fail closed：
  `/home/liuxiang/tmp/bitget_bbo_mixed_ha_hs_n2_n4_n6_n8_2h_20260718T045936Z`。
  四组均因初始 `taskset` 只允许 aux CPU，无法通过 source CPU availability 校验而以
  `cpu_binding_error` 退出；未启动 recorder，无遗留 Aquila 进程。修正为启动时允许本组完整
  CPU 集，在 fusion/source/logger 线程创建后再把主线程收窄到 aux CPU。
- 正式 run：
  `/home/liuxiang/tmp/bitget_bbo_mixed_ha_hs_n2_n4_n6_n8_2h_20260718T050235Z`。
  supervisor PID `383924`；fusion PID 分别为 `383926/383927/383928/383929`，recorder PID
  分别为 `384066/384067/384068/384069`。
- 四个 fusion 于 `2026-07-18T05:03:27Z` 启动，四个 recorder 于
  `2026-07-18T05:03:39Z` 启动；fusion 预计在 `2026-07-18T07:03:27Z` 自然到期。
- 启动验证：
  - 四组 fresh dry-run 均为 `result=ok`，source count 分别为 `2/4/6/8`；
  - focused Release CTest `4/4` 通过；
  - 八个进程均存活且 PID start ticks 匹配，实际 established TLS socket 分别为
    `2/4/6/8`，合计 `20`；
  - `/proc` affinity 确认 fusion/source 关键线程独占 CPU `8-31`，recorder main 使用
    `0/2/4/6`，主线程与 logger 使用 `1/3/5/7`；
  - 20 秒窗口中四组 canonical bin 均从 `443248` 增至 `467872` bytes，metadata 均从
    `369360` 增至 `385776` bytes；启动日志未发现 decode error、disconnect、reconnect、
    非零 overrun/dropped、fatal 或 unexpected。
- 冻结二进制 SHA-256：`bitget_data_fusion`
  `5f2ffc7df3b2c89f731f4c97d53cf64003cafe7b11fe782be0619294815511c4`，
  `data_reader_recorder`
  `33b5d9e8eb06b3e0ea9a87b4efdbbda950c63510d210eccef0cd25750352cd51`。

## 风险

- 四组共享主机、NIC 和远端 Bitget 服务，组间仍可能存在资源与网络路径干扰。
- HA/HS endpoint 的远端负载和路由会随时间变化；同步运行减少但不能消除该混杂因素。
- source 数增加会改变 winner 采样与重复事件竞争，结果应按 symbol、时间桶和 source endpoint
  分层解释，不能只比较全局中位数。
