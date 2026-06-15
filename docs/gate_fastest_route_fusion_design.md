# Gate / Binance 多路最快行情融合设计讨论

本文记录 2026-06-13 起对 Gate / Binance 多路行情融合的当前讨论结论。目标不是 primary / standby，
也不是 primary stale 后切换 standby，而是让 N 路行情同时参与，fusion 层为 canonical `BookTicker`
stream 选择最快一路。

## 目标

对同一个 exchange symbol，同一个 exchange orderbook update 可能从 N 个 source 到达：

```text
source A
source B
source C
...
-> fastest-route fusion
-> canonical exchange BookTicker SHM
-> strategy
```

最快定义为：

```text
同一个 (symbol_id, BookTicker.id) 中，哪一路最先到达 fusion，哪一路就是该 update 的 winner。
```

winner 对应的完整 `BookTicker` 立即输出到 canonical SHM；其他 source 后续到达同一个
`(symbol_id, id)` 时直接丢弃，不比较、不计数、不再输出。

第一版 hot path 里的“到达 fusion”指 fusion 进程实际读到并处理该 `BookTicker` 的顺序 / 时间。独立 fusion process 通过 SHM 轮询多 source 时，这个顺序可能受 fusion reader 调度影响；因此 sidecar metadata 记录 winner 的 `source_local_ns` 和 `fusion_publish_ns`，用于离线拆分 source ingress latency 与 fusion hop latency。如果后续目标改为“最早到达本机 data session 的 source 获胜”，就需要改用 `source_local_ns` 或更靠近 socket 的 RX timestamp，但这可能要求等待或改变输出规则。

2026-06-14 更新：第一版进一步收敛为 **不等待、不回看、不补洞、不比较 payload、不在 hot path 统计 duplicate / conflict**。fusion 只按 `(symbol_id, BookTicker.id)` 做 per-symbol 单调推进；latency 和 winner attribution 通过 canonical `BookTicker` bin、各 source `BookTicker` bin 和 sidecar metadata bin 离线分析。

2026-06-14 后续更新：同一套 fusion runner 已扩展到 Binance，新增 `binance_book_ticker_fusion` 入口；
30-symbol / N=4 / 30 分钟 L4 shadow 已分别跑过 Gate private plain 和 Binance public TLS。最新结果见
`docs/gate_fastest_route_fusion_shadow_results.md`。

2026-06-15 后续方向：计划增加同进程多线程 bundle 作为多进程 V1 的 A/B 对照。bundle 内运行 N 个
data session thread、1 个 fusion thread 和 1 个统一 log backend thread；V1 bundle 仍使用 source
SHM 连接 data session 和 fusion，保留 recorder / `DataReader` 监控边界。direct in-process SPSC
ring 只作为 V2 建议，进入条件是 threaded bundle 的 shadow / benchmark 证明 source SHM hop 或 tail
已经成为可见瓶颈。详细实施计划见 `docs/gate_fastest_route_fusion_threaded_bundle_plan.md`。

## 已排除方向

本轮讨论明确排除以下方向作为主方案：

1. **Warmup 选 primary，运行中长期只用 primary。** 这不是融合选最快，只是启动期选路。
2. **Primary / standby stale 后切换。** 这能改善故障和退化场景，但不是每个 update 的 fastest-route selection。
3. **短窗口等待后择优。** 例如第一路到达后等待 `50us` / `100us` 再选。该方案会主动增加 hot path 延迟，不作为默认生产路径。
4. **策略内多源 `DataReader`。** 这会把去重、乱序、source health 和诊断带进策略热路径；策略仍应消费一条 canonical SHM。

## `BookTicker.id` 语义

当前 live path 中，`BookTicker.id` 不是 SHM 本地行号，也不是 data session 自己生成的 connection-local sequence。

- Gate live：`exchange/gate/sbe/book_ticker_decoder.h` 将 SBE `bbo.u` 写入 `BookTicker.id`；schema 中 `u` 的描述是 `Orderbook id`。
- Binance live：`exchange/binance/market_data/client.h` 将 JSON `u` / `update_id` 写入 `BookTicker.id`。
- `core/market_data/data_shm.h` 的 publisher 只 push 原始 `BookTicker`，不会重写 `id`。

因此，对 live 多路同 symbol 融合，`(symbol_id, BookTicker.id)` 可以作为同一 orderbook update
的 identity 候选。`id` 不能脱离 `symbol_id` 单独比较，也不应跨交易所比较。Gate 文档明确
`u` 是 order book update id；Binance 侧当前按 JSON `u` 使用同一候选 identity，并用 shadow 结果验证
收益和风险边界。

边界：

- Tardis / historical converter 当前会把 `BookTicker.id` 写成输入行序号；这不是交易所 update id，不能直接用于 live fastest-route 语义验证。
- 如果后续某种 historical / replay 数据要用于 fusion 算法回归，必须确认它保留了 exchange orderbook id，或显式写清该 replay 只验证算法流程，不验证跨 source update identity。

## 第一版主算法

第一版使用 per-symbol first-arrival-by-id，但 hot path 只做单调推进：

```text
state[symbol_id]:
  last_published_id
  last_published_source

on BookTicker(ticker, source_id):
  if ticker.id > state.last_published_id:
      ticker.local_ns = fusion_publish_ns
      publish ticker immediately to canonical SHM
      state.last_published_id = ticker.id
      state.last_published_source = source_id
      write one sidecar metadata record
  else:
      drop
```

这里的 publish 必须保持完整 `BookTicker` 原子性，不能把不同 source 的 bid / ask、price / size 或 timestamp 做字段级混合。

同一个 `(symbol_id, id)` 的 first arrival winner 是低延迟主事实。后到 source 的同 id 数据不比较、不修正、不写 metadata。`id` 跳变、缺失、乱序也不阻塞发布；只要 `ticker.id > last_published_id`，就立即推进 canonical stream。

`state` 是 fusion process 内部 per-symbol 本地状态，不写入 SHM，也不暴露给策略。最小结构是：

```cpp
struct SymbolFusionState {
  std::int64_t last_published_id;
  std::int32_t last_published_source;
};
```

这意味着 canonical stream 是 fusion 观察到的最快可发布 BBO stream，不是完整、连续、可回放修复的 order book stream。LeadLag 策略只需要 latest BBO，不用这条 stream 重建深度。

## 与 freshness merge 的关系

此前讨论过两种模型：

- A：`per-update first-arrival-wins`
- B：`per-symbol freshest-arrival merge`

当前结论是以 A 为主，B 只作为安全边界表达：

- `id > last_published_id` 表示该 symbol 的 canonical quote 被推进，立即输出。
- `id <= last_published_id` 表示这个 source 的样本已经不能推进 canonical stream，直接丢弃。

也就是说，fusion 不按 primary freshness threshold 切换 source，也不等待短窗口比较多个 source；它按 exchange update id 单调推进 canonical stream。

## 架构边界

推荐第一版采用独立 fusion process。下图用 Gate binary 名称举例；Binance 使用同一结构，
对应入口是 `binance_data_session` 和 `binance_book_ticker_fusion`。

```text
gate_data_session_0 -> source_0 BookTicker SHM ┐
gate_data_session_1 -> source_1 BookTicker SHM ├-> gate_book_ticker_fusion -> canonical Gate BookTicker SHM -> strategy
...                 -> ...                     │
gate_data_session_N -> source_N BookTicker SHM ┘
```

理由：

1. 策略、risk 和 order path 仍只读一条 canonical market data stream。
2. 多路选择、去重、乱序、source health 和诊断全部收敛在 fusion 层。
3. fusion 可以先 shadow 运行，不改变实盘策略输入。
4. 若后续 benchmark 证明 source SHM -> fusion -> canonical SHM 的 hop latency 对 tail 不可接受，再评估同进程 N 路 WebSocket 直接融合。

首轮落地时 `N` 是配置项，运行配置先设为 `4`。第一版不把 N 路 WS connection 合并进 fusion 进程；
每一路 source 仍是独立 data session process，fusion process 不连接交易所，只读 SHM 并输出 canonical SHM。

后续 threaded bundle 不改变这一段的 fusion 算法和 SHM ABI，只改变进程 / 线程部署形态：

```text
one bundle process
  source_0 data session thread -> source_0 BookTicker SHM
  source_1 data session thread -> source_1 BookTicker SHM
  ...
  fusion thread                -> canonical BookTicker SHM
  one log backend thread
```

这个形态的第一目标是减少 N 个 data session process 带来的 N 个 log backend thread 和进程管理成本。
它仍保留 source SHM，因此 `data_reader_recorder`、monitor 和 published fusion analyzer 可以继续复用。
是否把策略切到 bundle 输出的 canonical SHM，必须由 threaded bundle shadow 与多进程 shadow 的 p50 / p99 /
p99.9 / max、source latency、fusion hop、winner ratio 和 recorder overrun 对比结果决定。

4 路首版架构图：

```text
                    ┌──────────────────────────────────────────────┐
                    │                 Gate Exchange                 │
                    │          futures.book_ticker / SBE BBO         │
                    └──────────────┬────────────┬────────────┬──────┘
                                   │            │            │
                                   ▼            ▼            ▼

┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────────┐
│ gate_data_session_0   │  │ gate_data_session_1   │  │ gate_data_session_2   │  │ gate_data_session_3   │
│ process               │  │ process               │  │ process               │  │ process               │
│ WS source_id=0        │  │ WS source_id=1        │  │ WS source_id=2        │  │ WS source_id=3        │
│ owner thread CPU A    │  │ owner thread CPU B    │  │ owner thread CPU C    │  │ owner thread CPU D    │
│ decode SBE BBO        │  │ decode SBE BBO        │  │ decode SBE BBO        │  │ decode SBE BBO        │
└──────────┬───────────┘  └──────────┬───────────┘  └──────────┬───────────┘  └──────────┬───────────┘
           │                         │                         │                         │
           ▼                         ▼                         ▼                         ▼
┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────────┐  ┌──────────────────────┐
│ source_0 BookTicker  │  │ source_1 BookTicker  │  │ source_2 BookTicker  │  │ source_3 BookTicker  │
│ SHM                  │  │ SHM                  │  │ SHM                  │  │ SHM                  │
└──────────┬───────────┘  └──────────┬───────────┘  └──────────┬───────────┘  └──────────┬───────────┘
           │                         │                         │                         │
           └─────────────────────────┴────────────┬────────────┴─────────────────────────┘
                                                  ▼
                              ┌──────────────────────────────────┐
                              │ gate_book_ticker_fusion process  │
                              │ one hot owner thread CPU F        │
                              │ fixed round-robin source polling  │
                              │ per-symbol last_published_id      │
                              └────────────────┬─────────────────┘
                                               │
                                               ▼
                              ┌──────────────────────────────────┐
                              │ canonical Gate BookTicker SHM     │
                              │ same BookTicker ABI               │
                              │ local_ns = fusion_publish_ns      │
                              └────────────────┬─────────────────┘
                                               │
                                               ▼
                              ┌──────────────────────────────────┐
                              │ LeadLag strategy / recorder       │
                              │ reads canonical Gate source only  │
                              └──────────────────────────────────┘
```

fusion 内部第一版只使用一个 hot owner thread，按固定 round-robin 读取 N 个 source SHM。`max_events_per_source` 建议首轮设为 `1`，避免单 source burst 饿死其它 source；后续用 benchmark 或 shadow 证据调整。

## SHM 和时间戳语义

fusion 输出的 canonical SHM 仍使用现有 `BookTicker` ABI：

```text
BookTicker.id          = 原始 BookTicker.id
BookTicker.symbol_id   = 原始 symbol_id
BookTicker.exchange    = 原始 exchange
BookTicker.exchange_ns = 原始 exchange BBO timestamp
BookTicker.local_ns    = fusion_publish_ns
bid / ask fields       = winner source 的完整原始 BBO
```

因此 source bin 和 fusion bin 的 latency 口径分别是：

```text
source_latency_ns = source.local_ns - source.exchange_ns
fusion_latency_ns = fusion.local_ns - fusion.exchange_ns
```

fusion SHM 不额外加字段，避免影响现有 `DataReader`、recorder 和策略。

## Sidecar Metadata

为了离线 attribution，fusion 在每次发布 winner 时额外写一条 sidecar metadata binary record。该文件不参与策略热路径，不改变 canonical SHM ABI。

建议 record 字段：

```cpp
struct FusionMetadataRecord {
  std::int32_t source_id;
  std::int32_t symbol_id;
  std::int64_t book_ticker_id;
  std::int64_t exchange_ns;
  std::int64_t source_local_ns;
  std::int64_t fusion_publish_ns;
};
```

核心离线指标：

```text
source_latency_ns   = source_local_ns - exchange_ns
fusion_latency_ns   = fusion_publish_ns - exchange_ns
fusion_hop_ns       = fusion_publish_ns - source_local_ns
winner_ratio        = published_by_source / total_published
```

不在 hot path 中比较同 `(symbol_id, id)` 的 quote，也不写 dropped ticker metadata。

## Shadow 验证

在接入策略前，应先 shadow 跑多路 fusion。shadow 的含义是：fusion 真实读取 N 路 source 并输出 canonical SHM，
但策略暂时仍读原来的 source，不使用 canonical SHM 下单。

推荐 shadow 数据采集：

```text
source_0 SHM -> data_reader_recorder -> source_0.bin
source_1 SHM -> data_reader_recorder -> source_1.bin
source_2 SHM -> data_reader_recorder -> source_2.bin
source_3 SHM -> data_reader_recorder -> source_3.bin
fusion SHM   -> data_reader_recorder -> fusion.bin
fusion sidecar metadata             -> fusion_metadata.bin
```

离线分析读取各 source bin、fusion bin 和 metadata bin，输出：

1. 每一路 source 的 `source.local_ns - source.exchange_ns` 分布。
2. canonical fusion 的 `fusion.local_ns - fusion.exchange_ns` 分布。
3. metadata 中的 `fusion_publish_ns - source_local_ns` hop latency 分布。
4. 按 source / symbol 统计 winner ratio。
5. 对比 canonical 与单 source 的 latency p50 / p95 / p99 / max。

当前 analyzer 入口是 `scripts/market_data/analyze_book_ticker_fusion_latency.py` 的 published fusion 模式。示例：

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/market_data/analyze_book_ticker_fusion_latency.py \
  --source-bin 0=/home/liuxiang/tmp/<run_id>/source_0.bin \
  --source-bin 1=/home/liuxiang/tmp/<run_id>/source_1.bin \
  --source-bin 2=/home/liuxiang/tmp/<run_id>/source_2.bin \
  --source-bin 3=/home/liuxiang/tmp/<run_id>/source_3.bin \
  --fusion-bin /home/liuxiang/tmp/<run_id>/fusion.bin \
  --metadata-bin /home/liuxiang/tmp/<run_id>/fusion_metadata.bin \
  --output-dir /home/liuxiang/tmp/<run_id>/fusion_analysis
```

该模式输出 `sources`、`fusion`、`metadata`、`fusion_metadata_alignment`、`source_metadata_alignment` 和 `top_fusion_hop_outliers`。旧的 `--input LABEL=PATH` 4 路 combination 模式仍保留，用于离线模拟不同 source 组合的 latency 上界。

只有当 shadow 证据显示 canonical stream 明确改善 BBO freshness，且 fusion hop latency 可接受，
才应让策略切到 canonical SHM。

## Shadow 结果

详细结果归档在 `docs/gate_fastest_route_fusion_shadow_results.md`。

2026-06-14 已完成一次 30-symbol、`N=4`、30 分钟、release L4 shadow，Gate / Binance 同时运行：

| exchange | run directory |
| --- | --- |
| Gate | `/home/liuxiang/tmp/20260614_133704_gate_fusion_30symbols_4src_30m_l4_outlier_release/` |
| Binance | `/home/liuxiang/tmp/20260614_133704_binance_fusion_30symbols_4src_30m_l4_outlier_release/` |

运行配置：

- build diagnostics: `AQUILA_DATA_SESSION_DIAG_LEVEL=4`
- latency outlier threshold: `5ms`
- outlier log rate limit: `max_logs_per_second=1000`
- Gate: private plain，source CPU `16-19`，fusion CPU `20`，recorder CPU `21-25`
- Binance: public TLS，source CPU `0-3`，fusion CPU `4`，recorder CPU `5-9`

核心结果，单位为 `ms`：

| exchange | stream | p50 | p95 | p99 | p99.9 | max |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Gate | best single source by percentile | 0.144 | 0.261 | 0.380 | 3.751 | - |
| Gate | fusion | 0.144 | 0.234 | 0.259 | 0.301 | 17.099 |
| Binance | best single source by percentile | 1.179 | 1.731 | 1.902 | 3.196 | - |
| Binance | fusion | 1.109 | 1.641 | 1.754 | 1.926 | 3.544 |

tail 改善：

| exchange | p99 improvement vs best single | p99.9 improvement vs best single | `fusion > 5ms` |
| --- | ---: | ---: | ---: |
| Gate | 31.71% | 91.97% | 3 / 1,075,552 |
| Binance | 7.79% | 39.75% | 0 / 3,367,135 |

fusion hop：

| exchange | p50 | p95 | p99 | p99.9 | `hop > 5ms` |
| --- | ---: | ---: | ---: | ---: | ---: |
| Gate | 0.000544ms | 0.000686ms | 0.000794ms | 0.003790ms | 0 |
| Binance | 0.000920ms | 0.001178ms | 0.001395ms | 0.009458ms | 0 |

L4 attribution 结论：

1. Gate private plain 有 `kernel_rx_ns`，本次 `>5ms` source outlier 主导阶段是
   `exchange_ns -> kernel_rx_ns`；本机 kernel queue、read callback、parser 和 SHM publish 不是主要来源。
2. Binance public TLS 当前没有 `kernel_rx_ns`，不能严格区分网络和本机 kernel；但 parser / SHM publish
   不是 `>5ms` outlier 的主要来源。`source_3` 在本次 run 中异常慢，但 winner ratio 只有 `0.35%`，
   canonical fusion 避开了它。
3. 本次结果支持继续推进 fusion shadow 和策略接入设计，但策略切换前仍需要多轮重复 shadow、确认 source
   异常是否稳定、评估 L4 outlier log 对热路径的扰动，并把 canonical stream 与 LeadLag `lag_freshness_ns`
   / `stale_lag_quote` reject 对齐。

2026-06-14 已完成一次 `BTC_USDT` / `ETH_USDT`、`N=4`、30 分钟 release shadow：

- 运行目录：`/home/liuxiang/tmp/20260614_051319_gate_fusion_btc_eth_4src_30m_release/`
- 时间：`2026-06-14T05:16:02Z` 到 `2026-06-14T05:46:02Z`
- endpoint：4 条 Gate private plain connection，`fxws-private.gateapi.io:80`，`connect_ip=10.0.1.154`
- source CPU：`16-19`，fusion CPU：`20`，recorder CPU 手动修正到 `21-25`
- 所有 process exit status 为 `0`，stderr 为空，5 个 recorder `skipped=0` / `overruns=0`

本次 recorder config 的 affinity 没有实际生效，启动后约 1 分钟用 `taskset` 手动修正；
因此该 run 不用于证明 recorder affinity config 行为。

核心结果：

| metric | result |
| --- | ---: |
| fusion process published | 57,803 |
| fusion recorder records | 57,656 |
| fusion records matched metadata by identity | 57,656 / 57,656 |
| metadata without fusion recorder record | 147 |
| canonical fusion latency p50 | 190,598ns |
| canonical fusion latency p95 | 296,827ns |
| canonical fusion latency p99 | 379,303ns |
| canonical fusion latency p99.9 | 686,858ns |
| fusion hop p50 | 484ns |
| fusion hop p95 | 785ns |
| fusion hop p99 | 1,029ns |
| fusion hop p99.9 | 1,312ns |

单路 source 的 `source.local_ns - source.exchange_ns`：

| source | p50 | p95 | p99 | p99.9 |
| --- | ---: | ---: | ---: | ---: |
| 0 | 234,970ns | 465,719ns | 2,045,999ns | 14,853,771ns |
| 1 | 341,552ns | 763,578ns | 5,047,139ns | 27,463,312ns |
| 2 | 409,613ns | 890,819ns | 4,183,991ns | 11,446,142ns |
| 3 | 209,570ns | 370,879ns | 1,497,621ns | 10,656,569ns |

winner ratio：

| source | ratio |
| --- | ---: |
| 0 | 45.6879% |
| 1 | 3.2922% |
| 2 | 1.4619% |
| 3 | 49.5580% |

离线用 source bin 按同一 `(symbol_id, BookTicker.id)` 查 `source.local_ns` 最小 source，
再和 fusion metadata 的 winner 对比：

| scope | same | different | difference ratio |
| --- | ---: | ---: | ---: |
| at least one source available | 57,591 | 65 | 0.1127% |
| all four sources available | 56,267 | 64 | 0.1136% |

差异幅度 `fusion_winner_source_local_ns - source_bin_fastest_local_ns` 的 p50 为 `93ns`、
p99 为 `745ns`、max 为 `1,353ns`。这说明 fusion winner 与离线 source-bin fastest 基本一致，
少量差异符合 V1 语义：fusion 不等待四路都到齐，而是发布 fusion process 实际先处理到且能推进
`last_published_id` 的 source。

本次结果支持继续沿用独立 fusion process + sidecar metadata 的 V1 路线，并继续扩大 shadow；
但它只是 2-symbol / 30 分钟结果，不能单独作为策略切换依据。策略接入前仍需更多 symbol、更长时间、
明确 recorder affinity 行为，并结合 LeadLag `lag_freshness_ns` / `stale_lag_quote` reject 验证。

## 当前推荐结论

第一版设计应采用：

```text
独立 fusion process
+ N 路 exchange source SHM
+ per-symbol first-arrival-by-(symbol_id,id)
+ 不等待、不回看、不补洞、不比较 payload
+ canonical exchange BookTicker SHM
+ sidecar metadata bin
+ recorder bin 离线 latency 分析
```

这是真正的多路融合选最快，不是 primary / standby，也不是 stale 后 fallback。核心正确性依赖 live
`BookTicker.id` 在同 symbol 多 source 间表示同一个 exchange orderbook update；Gate 文档支持 `u`
是 order book update id，但未明确写出跨 connection 保证，因此第一版先用 shadow latency 和 attribution
证据确认收益，再接入策略。Binance 当前也按同一 identity 运行 shadow；由于 Binance source 走 TLS，
L4 attribution 暂时不能拆出 `kernel_rx_ns`。
