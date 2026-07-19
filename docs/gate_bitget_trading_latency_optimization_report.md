# Gate / Bitget 交易链路延迟优化报告

更新时间：2026-07-19

## 1. 结论摘要

截至优化代码 checkpoint `cd0297f`，本阶段共接受 8 项生产代码优化：

| 提交 | 优化项 | 主要正式结果 | 结论 |
| --- | --- | --- | --- |
| `9b09d73` | 缓存已初始化的 LeadLag pair runtime，避免 global risk 全量扫描无效 slot | focused risk scan `p50/p99/p99.9` 分别改善 `15.58%/44.78%/66.04%`，均为 5/5 组同向 | 采用 |
| `c48b41b` | 用固定 bitset 索引 reserved-open risk slot | focused risk scan `p50/p99/p99.9` 分别改善 `89.77%/89.69%/94.51%`，均为 5/5 组同向 | 采用 |
| `f99dfc0` | terminal feedback 复用入口已找到的 `StrategyOrder*` | Strategy Gate `p50/p99` 改善 `11.31%/7.56%`；Strategy Bitget 改善 `13.30%/5.64%` | 采用 |
| `f1dc51c` | Gate request correlation 合并到单一 request log-fields map | Gate OrderSession submit `p50/p99` 改善 `2.34%/7.16%`，5/5 组同向；response 中性 | 采用 |
| `788b6c4` | Gate session active 时缓存 owner thread TID | Gate gateway submit `p50/p99` 改善 `14.25%/13.08%`，5/5 组同向 | 采用 |
| `c9aa980` | 对 Bitget login/place/cancel 固定 JSON format 使用 `FMT_COMPILE` | Bitget gateway submit `p50/p99/p99.9` 改善 `17.91%/17.09%/5.39%`，均为 5/5 组同向 | 采用 |
| `51bc4e7` | 删除 Bitget send log 中重复的 realtime timestamp 别名 | 每条 `bitget_order_send` 少一个重复 key/value；按约束只审计字段重复性和必要性，不声明数值收益 | 采用 |
| `d4102fc` | Gate SBE feedback 的未消费 var-string 只做长度/边界验证 | 完整 Gate parser → SHM → LeadLag runtime `p50/p99` 改善 `4.95%/4.32%`，均为 5/5 组同向 | 采用 |

### 1.1 所有已采用优化的链路级效果

不存在一个有意义的“全部优化总百分比”：risk scan、submit、ACK 和 terminal feedback 是不同
调用面，把它们的百分比直接相加会重复计算或混合不同分母。下表按可比链路给出累计效果。

| 链路与测量面 | 最早 baseline → 最新 candidate | 累计变化 | 证据性质 |
| --- | --- | --- | --- |
| LeadLag global/open risk focused scan | `p50 799 → 69 ns`；`p99 1283 → 73 ns`；`p99.9 5012.5 → 87 ns` | `-91.36%/-94.31%/-98.26%` | `9b09d73` 与 `c48b41b` 顺序执行的同一 focused benchmark；跨两轮汇总，不是单轮 paired 结果 |
| Gate gateway submit | `p50 1025 → 869.5 ns`；`p99 1403.5 → 1186 ns`；`p99.9 12611 → 12387.5 ns` | `-15.17%/-15.50%/-1.77%` | `f1dc51c` 最早 baseline 到 `788b6c4` 最新 candidate 的同一 benchmark 跨轮参考 |
| Bitget gateway submit | `p50 714.5 → 586.5 ns`；`p99 784 → 650 ns`；`p99.9 10246.5 → 9694 ns` | `-17.91%/-17.09%/-5.39%` | `c9aa980` 五组正式 A/B，5/5 组同向 |
| Gate terminal feedback parser → SHM → runtime | `p50 1421 → 1238 ns`；`p99 1596.5 → 1430 ns` | `-12.88%/-10.43%` | `f99dfc0` 最早 baseline 到 `d4102fc` 最新 candidate 的同一 benchmark 跨轮参考；`p99.9` 跨轮不稳定，不给累计收益 |
| Bitget terminal feedback parser → SHM → runtime | `p50 2214.5 → 2086.5 ns`；`p99 2423.5 → 2295 ns`；`p99.9 2636.5 → 2525 ns` | `-5.78%/-5.30%/-4.23%` | `f99dfc0` 五组正式 A/B；三项均为 4/5 组同向 |
| Bitget send log | 每条 send log 少一个重复 realtime timestamp 别名 | 定性减少一个字段 | 用户明确要求不为 backend log 字段删减运行额外 benchmark 或测试 |

Gate submit 和 Gate feedback 的累计值用于说明优化前后量级。它们来自顺序相邻但不同时间的正式
A/B 轮次，不具有单轮交替 paired A/B 的抗漂移强度；每项正式采用结论仍以下文对应的单项 A/B
为准。

## 2. 已采用优化明细

百分比按 `(candidate / baseline - 1) × 100%` 计算；负值表示延迟降低。表内数值是各组
`median` 再取组间中位数。

### 2.1 `9b09d73`：缓存已初始化 pair runtime

原实现的 global risk 计算遍历按最大 `symbol_id` 展开的整个
`pair_runtime_by_symbol_id_`，每次判断 `initialized`。候选在初始化阶段构建紧凑的
`initialized_pair_runtimes_` 指针数组，热路径只遍历有效 pair。

| 指标 | baseline | candidate | 提升 | 同向组数 |
| --- | ---: | ---: | ---: | ---: |
| `real_time` | 843.198 ns | 686.656 ns | 18.57% | 5/5 |
| `p50` | 799 ns | 674.5 ns | 15.58% | 5/5 |
| `p99` | 1283 ns | 708.5 ns | 44.78% | 5/5 |
| `p99.9` | 5012.5 ns | 1702 ns | 66.04% | 5/5 |

采用原因：初始化后不变的信息被移出热路径；结果等价，focused 四项指标全部 5/5 同向，
相关 strategy tests 通过。

证据：`risk-scan-ab/formal-final`。

### 2.2 `c48b41b`：reserved-open risk slot bitset

原实现每次 risk check 都线性扫描全部 `order_price_texts_`。候选在 reserve/release 时维护
固定 bitset，risk check 使用 `std::countr_zero` 只访问存在 reserved-open risk 的 slot。

| focused risk scan | baseline | candidate | 提升 | 同向组数 |
| --- | ---: | ---: | ---: | ---: |
| `real_time` | 684.869 ns | 69.907 ns | 89.79% | 5/5 |
| `p50` | 674.5 ns | 69 ns | 89.77% | 5/5 |
| `p99` | 708 ns | 73 ns | 89.69% | 5/5 |
| `p99.9` | 1586 ns | 87 ns | 94.51% | 5/5 |

实际配置 submit paired benchmark：

- 总路径 `p50 8173 → 7088 ns`，改善 `13.28%`，5/5 组同向。
- 总路径 `p99 39155 → 37264 ns`，组中位数改善 `4.83%`，3/5 组同向。
- 总路径 `p99.9 49162 → 49871 ns`，回归 `1.44%`，不声明尾延迟收益。
- risk stage `p50/p99 1125/1880 → 336/664 ns`，改善 `70.13%/64.68%`。

采用原因：focused 路径收益大且稳定，实际配置总路径的 `p50` 明确改善，risk stage
改善与实现机制一致；维护位图的 reserve/release 等价性测试已覆盖，strategy tests 88/88
通过。`p99.9` 未作为采用收益。

证据：`risk-reservation-index-ab/formal`、`risk-reservation-index-ab/e2e-paired`。

### 2.3 `f99dfc0`：复用 terminal feedback 已查到的订单指针

`OnOrderResponse` / `OnOrderFeedback` 已通过 `context.FindOrder()` 得到
`StrategyOrder*` 用于 timing 和 log。原实现进入 `ApplyFinishedOrder()` 后按
`local_order_id` 再查一次；候选直接传递并复用已有指针，retire 前保存
`local_order_id`。

| 测量面 | `p50` | `p99` | `p99.9` | `real_time` |
| --- | --- | --- | --- | --- |
| Gate Strategy | `973 → 863 ns`，`-11.31%`，5/5 | `1097.5 → 1014.5 ns`，`-7.56%`，5/5 | `1281.5 → 1108 ns`，`-13.54%`，4/5 | `-10.51%`，5/5 |
| Bitget Strategy | `970 → 841 ns`，`-13.30%`，4/5 | `1082.5 → 1021.5 ns`，`-5.64%`，4/5 | `1285 → 1199.5 ns`，`-6.65%`，3/5 | `-12.91%`，5/5 |
| Gate parser → SHM → runtime | `1421 → 1381.5 ns`，`-2.78%`，4/5 | `1596.5 → 1579 ns`，`-1.10%`，3/5 | `4389 → 2443 ns`，`-44.34%`，4/5 | `-2.46%`，3/5 |
| Bitget parser → SHM → runtime | `2214.5 → 2086.5 ns`，`-5.78%`，4/5 | `2423.5 → 2295 ns`，`-5.30%`，4/5 | `2636.5 → 2525 ns`，`-4.23%`，4/5 | `-5.44%`，4/5 |

采用原因：修改只消除同一事件内的重复 lookup，不改变订单所有权和 retire 顺序；Gate/Bitget
Strategy 与两条完整 parser → SHM → runtime 均未出现组中位数回归，replay、测试和 review
通过。Gate 完整链 `p99` 的同向组数只有 3/5，因此不把其 `1.10%` 小收益外推为强稳定收益。

证据：`reuse-order-pointer-formal-gate`、`reuse-order-pointer-formal-bitget`、
`reuse-order-pointer-formal-parser-runtime`、`reuse-order-pointer-replay`。

### 2.4 `f1dc51c`：合并 Gate request tracking

原实现同时维护 `request_id_to_local_order_id_` 和 `request_id_to_log_fields_`。后者本来已经
包含 `local_order_id`，候选删除前一张重复 map，并在 response 侧复用单次 lookup 的
iterator。

| 测量面 | `p50` | `p99` | `p99.9` | `real_time` |
| --- | --- | --- | --- | --- |
| OrderSession place | `963.5 → 941 ns`，`-2.34%`，5/5 | `1348 → 1251.5 ns`，`-7.16%`，5/5 | `9583.5 → 9391 ns`，`-2.01%`，2/5 | `-4.17%`，5/5 |
| PlaceStrategyOrder | `934.5 → 931.5 ns`，`-0.32%`，5/5 | `1265 → 1234 ns`，`-2.45%`，5/5 | `9200.5 → 9001.5 ns`，`-2.16%`，4/5 | `-0.87%`，5/5 |
| GatewayWorker | `1025 → 1011.5 ns`，`-1.32%`，5/5 | `1403.5 → 1359.5 ns`，`-3.14%`，5/5 | `12611 → 12619 ns`，`+0.06%`，2/5 | `-1.63%`，5/5 |

response `HandlePlaceAck` / `HandlePlaceResult` 的 `real_time` 分别为
`+0.03%/+0.02%`，属于中性。

采用原因：删除重复状态和重复 hash lookup，unknown/mismatch/erase 语义保持不变；submit
中心与 `p99` 全部同向，response 无实质回归。完整 gateway `p99.9` 不声明收益。

证据：`gate-session-map-formal`、`gate-session-map-response-formal`。

### 2.5 `788b6c4`：缓存 Gate owner thread TID

`OrderSession` 的 I/O callback 与 runtime hook 受 owner-thread confinement 约束。候选在
session active 时缓存 TID，在每次 `ArmAckLatencyDiagnostic()` 时复用，避免热路径重复
执行 TID 查询；连接离开 active 时清空缓存。

| 测量面 | `p50` | `p99` | `p99.9` | `real_time` |
| --- | --- | --- | --- | --- |
| OrderSession place | `942.5 → 811.5 ns`，`-13.90%`，5/5 | `1250.5 → 1097 ns`，`-12.28%`，5/5 | `8976.5 → 9663 ns`，`+7.65%`，2/5 | `-13.51%`，5/5 |
| PlaceStrategyOrder | `932 → 790.5 ns`，`-15.18%`，5/5 | `1241.5 → 1078.5 ns`，`-13.13%`，5/5 | `8637.5 → 7125.5 ns`，`-17.51%`，5/5 | `-15.07%`，5/5 |
| GatewayWorker | `1014 → 869.5 ns`，`-14.25%`，5/5 | `1364.5 → 1186 ns`，`-13.08%`，5/5 | `12540 → 12387.5 ns`，`-1.22%`，3/5 | `-13.73%`，5/5 |

采用原因：缓存值的生命周期与既有 owner-thread contract 一致，并新增测试观察缓存值；
三层 submit 的 `p50/p99/real_time` 全部 5/5 同向。OrderSession place 的 `p99.9` 回归，
因此只声明稳定的中心和 `p99` 收益。

证据：`gate-session-tid-formal`。

### 2.6 `c9aa980`：预编译 Bitget 固定 JSON format

login/place/cancel 的 JSON schema 是编译期固定字面量。候选使用 `FMT_COMPILE`，保留
`fmt::format_to_n`、固定 buffer、截断检查和原 payload，只消除运行时 format 解析。

| 测量面 | baseline | candidate | 提升 | 同向组数 |
| --- | ---: | ---: | ---: | ---: |
| EncodePlace `real_time` | 247.108 ns | 122.253 ns | 50.53% | 5/5 |
| EncodeCancel `real_time` | 132.444 ns | 82.781 ns | 37.50% | 5/5 |
| OrderSession `p50/p99/p99.9` | 635.5/684/4082 ns | 498/549.5/1752.5 ns | 21.64%/19.66%/57.07% | 5/5、5/5、5/5 |
| StrategyContext `p50/p99/p99.9` | 667.5/2636/6869.5 ns | 526/2458/5190.5 ns | 21.20%/6.75%/24.44% | 5/5、5/5、4/5 |
| GatewayWorker `p50/p99/p99.9` | 714.5/784/10246.5 ns | 586.5/650/9694 ns | 17.91%/17.09%/5.39% | 5/5、5/5、5/5 |

采用原因：component 到 gateway 外层均同向，gateway 三个分位全部 5/5 改善；payload
等价性和既有 encoder/session 测试通过。

证据：`bitget-fmt-compile-screen`。目录名保留历史 `screen`，其中实际保存了完整 5 组
baseline/candidate JSON，本报告按这 5 组重新汇总。

### 2.7 `51bc4e7`：删除 Bitget send log 重复 timestamp

`bitget_order_send` 同时输出同一 realtime 时点的 `request_send_local_ns` 和
`request_send_realtime_ns`。候选删除 send log 中的后一个重复别名；response 仍保留
`request_send_realtime_ns`，`request_send_monotonic_ns`、encode-done 和 write-complete
字段均不变，日志继续使用既有 `NOVA_*` wrapper。

采用原因：字段语义可由同一 log record 的既有字段完整表达，删除后不损失诊断能力。
日志格式化与输出位于 backend thread；按用户约束，本项不运行额外 log latency benchmark
或测试，也不声明具体纳秒收益。

### 2.8 `d4102fc`：Gate 未消费 var-string 不再写出 `string_view`

`ReadVarString8()` 允许 `out == nullptr`。对协议中不被后续逻辑消费的字段，仍读取长度并
验证 payload 边界，但不再反复覆盖临时 `string_view`；需要使用的 `role`、`text`、
`finish_as` 保持原解析路径。

| 测量面 | baseline | candidate | 提升 | 同向组数 |
| --- | ---: | ---: | ---: | ---: |
| parser component `real_time` | 79.666 ns | 60.642 ns | 23.88% | 3/3 |
| session component `real_time` | 113.734 ns | 108.184 ns | 4.88% | 3/3 |
| parser → SHM component `real_time` | 123.273 ns | 117.923 ns | 4.34% | 3/3 |
| 完整 parser → SHM → runtime `p50` | 1302.5 ns | 1238 ns | 4.95% | 5/5 |
| 完整 parser → SHM → runtime `p99` | 1494.5 ns | 1430 ns | 4.32% | 5/5 |
| 完整 parser → SHM → runtime `p99.9` | 8247.5 ns | 7402 ns | 10.25% | 3/5 |

采用原因：保留所有长度和越界验证，component 与完整 runtime 的中心和 `p99` 都稳定改善。
`p99.9` 只有 3/5 同向，只记录观测值，不作为稳定收益主张。

证据：`gate-null-var-string-screen`、`gate-null-var-string-runtime-formal`。

## 3. 所有未采用候选

以下候选均已撤销，当前生产代码不包含这些修改。screening 数字只用于决定是否进入正式
A/B，不作为最终性能主张。

### 3.1 LeadLag risk / terminal feedback

| 尝试 | 结果摘要 | 是否采用 | 原因 |
| --- | --- | --- | --- |
| order price text active-bitset / single-index / heap / combined-layout | focused sparse-last-slot `p50` 最好约 `1195 → 41.5 ns`，改善 `96.5%`；最终完整 Gate feedback `p50/p99` 回归 `5.5%/2.2%`，Bitget `p99/p99.9` 回归 `2.0%/92.4%` | 否 | focused scan 收益没有传递到完整链，且交易反馈尾延迟明显回归；候选 diff 仅保存在 result 目录 |
| 删除 `lead_lag_order_finished` 重复/低价值字段 | 历史两组中 log helper `p50` 约节省 5 ns，但 Gate/Bitget Strategy terminal `p50` 分别回归约 `23.1%/18.8%` | 否 | 完整热路径回归，字段删除已完全撤销；此后按用户约束不再为 backend log 字段删减做额外 benchmark |
| execution known-group 快路径 | Gate 完整链 `p50/p99` 改善 `5.30%/5.64%`；Bitget `p50` 回归 `1.51%`、`p99.9` 回归 `9.90%`，Strategy `p50` 仅 2/5 同向 | 否 | Gate/Bitget 不一致，Bitget 和 Strategy 尾部不满足接受门 |
| retire known-order 快路径 | 两组正式结果中 Gate/Bitget Strategy `p50` 回归 `15.37%/14.30%`，`p99.9` 回归 `43.35%/36.99%` | 否 | 明确回归，提前停止正式 A/B |
| known pending-match 快路径 | Runtime Gate/Bitget `p50` 改善 `1.41%/3.92%`，但 Strategy Gate/Bitget `p99.9` 回归 `9.54%/13.58%`，Execution apply `p99.9` 回归 `5.11%` | 否 | 中心的小收益不能覆盖 Strategy/Execution 尾部回归 |

证据目录：`order-price-text-erase-scan`、`log-field-retirement-formal`、
`execution-known-group-formal`、`retire-known-order-formal`、
`known-pending-match-formal`。

### 3.2 Gate submit / common write diagnostics

| 尝试 | 结果摘要 | 是否采用 | 原因 |
| --- | --- | --- | --- |
| 调整 Gate ACK diagnostic arm/存储路径 | Gateway submit `p50/p99` 改善 `9.02%/5.61%`，但 response `HandlePlaceAck` 5/5 回归，`HandlePlaceResult` 5/5 回归且组中位数 `+5.02%` | 否 | 成本从 submit 转移到 ACK handler，不是整链净收益 |
| 调整 common write-complete clock 采集路径 | Gate Gateway submit `p50/p99` 改善 `5.05%/4.58%`，Bitget Gateway 改善 `2.98%/2.67%`；Gate `HandlePlaceResult` 5/5 回归 `2.39%` | 否 | submit 收益以 response 回归为代价，共同路径不满足完整链门槛 |
| Gate inline prepared place writer | Gateway `p50/p99` 改善 `15.59%/12.38%`；`HandlePlaceResult` 组中位数回归 `1.87%`，仅 1/5 同向 | 否 | 明显 code-layout/response 回归 |
| Gate out-of-line place writer | 单组 submit 中心改善约 `16%`；三组 response `HandlePlaceAck/Result` 分别回归 `1.49%/4.34%`，均 0/3 同向 | 否 | 把 writer 移出行内仍未消除 response 回归 |
| Gate 全 fixed JSON `FMT_COMPILE` | Encoder place/cancel 改善 `48.88%/43.57%`，Gateway `p50/p99` 改善 `14.84%/11.58%`；Gateway `p99.9` 回归 `1.54%`，仅 1/5 同向 | 否 | 本项目优先最低和尾延迟，外层 `p99.9` 未通过；这也是没有照搬 Bitget 已采用修改的原因 |
| Gate place-only `FMT_COMPILE` | screening Gateway `p50/p99` 改善约 `13.8%/10.4%`，`p99.9` 仅 2/3 同向；正式 paired 只完成 1 组 | 否 | 正式证据不完整，screening 尾部也不稳定 |
| Gate compiled place helper | place encoder 改善 `46.62%`，但 cancel encoder 回归 `19.53%`；Gateway `p99.9` 回归 `3.51%`，0/2 同向 | 否 | 为 place 获取收益却伤害 cancel 和 gateway tail |

证据目录：`gate-ack-arm-formal`、`gate-ack-arm-response-formal`、
`write-complete-clock-formal`、`write-complete-clock-response-formal`、
`gate-place-writer-screen`、`gate-place-writer-response-formal`、
`gate-place-writer-out-of-line-screen`、`gate-place-writer-out-of-line-response-screen`、
`gate-fmt-compile-formal`、`gate-place-only-fmt-compile-formal`、
`gate-place-compiled-helper-screen`。

### 3.3 Bitget submit

| 尝试 | 结果摘要 | 是否采用 | 原因 |
| --- | --- | --- | --- |
| 单独预编译 `clientOid` format | place/cancel encoder 分别回归 `0.83%/0.29%`；OrderSession `p50/p99.9` 回归 `0.50%/7.49%` | 否 | 单组 screening 已整体回归 |
| `clientOid` 改用 `to_chars` | cancel encoder 改善 `17.75%`，但 place encoder 回归 `1.79%`、OrderSession `p50` 回归 `1.30%` | 否 | place 是主要发送路径，收益方向不一致 |
| Bitget prepared place writer | 两组中 Gateway `p50/p99` 改善 `1.20%/2.64%`，但 encoder 回归 `1.80%`、OrderSession `p99.9` 不稳定 | 否 | 收益小、证据仅 screening，且 component/tail 不一致 |
| Bitget submit buffer 改为未初始化分配 | 三组中 OrderSession `p50/p99` 改善 `4.44%/4.21%`，但 `p99.9` 0/3 同向并回归 `20.61%` | 否 | Gate 既有未初始化 buffer 不能证明 Bitget 也应采用；Bitget 实测 tail 明确失败 |

证据目录：`bitget-client-oid-fmt-compile-screen`、
`bitget-client-oid-to-chars-screen`、`bitget-place-writer-screen`、
`bitget-uninitialized-buffer-screen`。

### 3.4 Gate feedback parser

| 尝试 | 结果摘要 | 是否采用 | 原因 |
| --- | --- | --- | --- |
| 新增专用 `SkipVarString8` helper | component parser/session/parser→SHM 改善 `22.23%/10.90%/9.90%`；完整 runtime `p50/p99` 回归 `3.00%/2.39%`，仅 1/4 同向 | 否 | component 胜出但完整链失败；随后改成更小的 nullable-output 方案，即 `d4102fc` |
| fixed-block unchecked read | component parser/session/parser→SHM 改善约 `3.01%/11.09%/9.10%`；完整 runtime `p99/p99.9` 均 0/2 同向并回归 | 否 | 删除局部边界检查没有形成完整链收益，还扩大 parser 安全风险 |
| hot literal compare | component 改善约 `13%–16%`；五组完整 runtime `p50/p99` 仅 2/5 同向，组中位数分别回归 `0.73%/0.71%` | 否 | code layout 抵消 component 收益 |

证据目录：`gate-skip-var-string-formal`、`gate-skip-var-string-runtime-formal`、
`gate-unchecked-fixed-read-screen`、`gate-unchecked-fixed-read-runtime-screen`、
`gate-hot-literal-compare-runtime-formal`。

### 3.5 Bitget ACK / feedback parser

| 尝试 | 结果摘要 | 是否采用 | 原因 |
| --- | --- | --- | --- |
| numeric parser 快路径 | parser 常见场景改善约 `1%–5%`；完整 Bitget runtime `p99` 回归 `8.43%`，`p99.9` 回归 `147.75%` 且 0/5 同向 | 否 | component 收益未传递到完整链，tail 明确失败 |
| fixed JSON literal compare | parser 常见场景改善约 `4%–12%`；same-build 完整 Bitget runtime `p99/p99.9` 回归 `80.19%/167.22%` | 否 | code layout / tail 回归远大于 parser 收益 |
| `code == 0` 时跳过 success `msg` | parser 只改善 `0.25%`；完整 ACK handler `p50` 回归 `1.15%`、`p99.9` 回归 `6.65%` | 否 | 局部收益太小，ACK handler 0/5 稳定改善 |
| `clientOid` 只调用一次 `get_string()` | component 改善约 `1.5%–2.5%`；两组完整 runtime `p50/p99/p99.9` 全部回归约 `6.8%/8.3%/6.7%` | 否 | 完整链 0/2 同向 |
| order cursor reset | component 改善约 `2.7%–4.3%`；五组完整 runtime 的组中位数 `p50/p99/p99.9` 分别回归 `2.15%/2.37%/1.45%`，`p99.9` 仅 1/5 同向 | 否 | 正式完整链未通过 |

证据目录：`bitget-numeric-parser-formal`、`bitget-numeric-parser-chain-formal`、
`bitget-literal-compare-formal`、`bitget-literal-compare-same-build-chain-screen`、
`bitget-operation-skip-success-msg-formal`、`bitget-direct-client-oid-string-screen`、
`bitget-direct-client-oid-string-runtime-screen`、`bitget-order-cursor-reset-runtime-formal`。

## 4. Profile、测试与结果边界

### 4.1 Profile 结论

- baseline、submit、ACK、feedback parser、parser → SHM → runtime、Strategy、
  ExecutionState 和 order lifecycle log 均已分层 profile。
- 最新 Gate / Bitget feedback parser gprof 分别位于
  `gate-feedback-parser-gprof-after-d4102fc` 和
  `bitget-feedback-parser-gprof-after-d4102fc`。
- `3156ce7` 新增 Bitget order ACK parser → correlation → handler 完整 benchmark，
  它是测量支撑提交，不是生产优化，也不单独声明延迟收益。
- 本机 `kernel.perf_event_paranoid=4`，无法取得 `perf` PMU/call graph；未修改系统设置。
  函数级排序使用 gprof/分层 benchmark，并由未插桩 Release A/B 确认。
- 剩余非拓扑热点均至少尝试两个最小候选；连续完整 A/B 没有找到新的可接受非拓扑优化。
  线程/进程拓扑属于下一阶段，必须在独立 branch/worktree 中进行。

### 4.2 环境与适用范围

- 环境快照：
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-results/baseline_environment.md`。
- CPU：Intel Xeon Platinum 8488C；benchmark 固定 CPU 29。
- 编译：GCC 13.3.0，Release，`-O3 -DNDEBUG`。
- 原始结果根目录：
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-results`。
- build 根目录：
  `/home/liuxiang/tmp/aquila-gate-bitget-trading-latency-builds`。
- 本报告只声明本机 parser、SHM、runtime、strategy 和 local submit path 延迟变化。
- 本轮没有发送真实订单，不声明公网、交易所 ingress、撮合、fillability、成交率、PnL 或
  实盘端到端收益。
- 所有采用项均要求行为等价、对应 tests、正式 A/B 和 diff review 通过；无法满足的候选均
  最小撤销。日志字段删减遵守用户的单独约束，只检查字段重复性和必要性。
