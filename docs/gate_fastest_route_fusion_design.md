# Gate 多路最快行情融合设计讨论

本文记录 2026-06-13 对 Gate 多路行情融合的当前讨论结论。目标不是 primary / standby，也不是 primary stale 后切换 standby，而是让 N 路 Gate 行情同时参与，fusion 层为 canonical Gate `BookTicker` stream 选择最快一路。

## 目标

对同一个 Gate symbol，同一个 exchange orderbook update 可能从 N 个 source 到达：

```text
Gate source A
Gate source B
Gate source C
...
-> fastest-route fusion
-> canonical Gate BookTicker SHM
-> strategy
```

最快定义为：

```text
同一个 (symbol_id, BookTicker.id) 中，哪一路最先到达 fusion，哪一路就是该 update 的 winner。
```

winner 对应的完整 `BookTicker` 立即输出到 canonical Gate SHM；其他 source 后续到达同一个 `(symbol_id, id)` 时作为 duplicate / conflict / late sample 计数，不再输出。

第一版 hot path 里的“到达 fusion”指 fusion 进程实际读到并处理该 `BookTicker` 的顺序 / 时间。独立 fusion process 通过 SHM 轮询多 source 时，这个顺序可能受 fusion reader 调度影响；因此必须同时记录 `source_local_ns` 和 `fusion_receive_ns`。如果后续目标改为“最早到达本机 data session 的 source 获胜”，就需要改用 `source_local_ns` 或更靠近 socket 的 RX timestamp，但这可能要求等待或改变输出规则。

## 已排除方向

本轮讨论明确排除以下方向作为主方案：

1. **Warmup 选 primary，运行中长期只用 primary。** 这不是融合选最快，只是启动期选路。
2. **Primary / standby stale 后切换。** 这能改善故障和退化场景，但不是每个 update 的 fastest-route selection。
3. **短窗口等待后择优。** 例如第一路到达后等待 `50us` / `100us` 再选。该方案会主动增加 hot path 延迟，不作为默认生产路径。
4. **策略内多源 `DataReader`。** 这会把去重、乱序、source health 和诊断带进策略热路径；策略仍应消费一条 canonical Gate SHM。

## `BookTicker.id` 语义

当前 live path 中，`BookTicker.id` 不是 SHM 本地行号，也不是 data session 自己生成的 connection-local sequence。

- Gate live：`exchange/gate/sbe/book_ticker_decoder.h` 将 SBE `bbo.u` 写入 `BookTicker.id`；schema 中 `u` 的描述是 `Orderbook id`。
- Binance live：`exchange/binance/market_data/client.h` 将 JSON `u` / `update_id` 写入 `BookTicker.id`。
- `core/market_data/data_shm.h` 的 publisher 只 push 原始 `BookTicker`，不会重写 `id`。

因此，对 **Gate live 多路同 symbol 融合**，`(symbol_id, BookTicker.id)` 可以作为同一 orderbook update 的 identity 候选。`id` 不能脱离 `symbol_id` 单独比较，也不应跨交易所比较。

边界：

- Tardis / historical converter 当前会把 `BookTicker.id` 写成输入行序号；这不是交易所 update id，不能直接用于 live fastest-route 语义验证。
- 如果后续某种 historical / replay 数据要用于 fusion 算法回归，必须确认它保留了 Gate exchange orderbook id，或显式写清该 replay 只验证算法流程，不验证跨 source update identity。

## 主算法

第一版推荐使用 per-symbol first-arrival-by-id：

```text
state[symbol_id]:
  last_published_id
  last_published_quote
  last_published_source

on BookTicker(ticker, source_id):
  key = (ticker.symbol_id, ticker.id)

  if ticker.id > state.last_published_id:
      publish ticker immediately
      update state
      count source_win[source_id]
      if id jumps forward: count id_jump

  else if ticker.id == state.last_published_id:
      if quote/time matches last_published_quote:
          drop duplicate_late
      else:
          drop same_id_conflict

  else:
      drop out_of_order_late
```

这里的 publish 必须保持完整 `BookTicker` 原子性，不能把不同 source 的 bid / ask、price / size 或 timestamp 做字段级混合。

同一个 `(symbol_id, id)` 的 first arrival winner 是低延迟主事实；后到 source 的同 id 数据只用于诊断和一致性校验。若同 id 后到数据与 winner 的 quote 或 exchange timestamp 不一致，应计 `same_id_conflict`，默认不覆盖 canonical 输出。

## 与 freshness merge 的关系

此前讨论过两种模型：

- A：`per-update first-arrival-wins`
- B：`per-symbol freshest-arrival merge`

当前结论是以 A 为主，B 只作为安全边界表达：

- `id > last_published_id` 表示该 symbol 的 canonical quote 被推进，立即输出。
- `id == last_published_id` 表示同一个 update 的后到样本，只用于 duplicate / conflict 统计。
- `id < last_published_id` 表示 late / out-of-order，不输出。

也就是说，fusion 不按 primary freshness threshold 切换 source，也不等待短窗口比较多个 source；它按 exchange update id 单调推进 canonical stream。

## 架构边界

推荐第一版采用独立 fusion process：

```text
gate_data_session_A -> source A BookTicker SHM
gate_data_session_B -> source B BookTicker SHM
gate_data_session_C -> source C BookTicker SHM
fusion_process      -> canonical Gate BookTicker SHM
strategy            -> canonical Gate BookTicker SHM
```

理由：

1. 策略、risk 和 order path 仍只读一条 canonical Gate market data stream。
2. 多路选择、去重、乱序、source health 和诊断全部收敛在 fusion 层。
3. fusion 可以先 shadow 运行，不改变实盘策略输入。
4. 若后续 benchmark 证明 source SHM -> fusion -> canonical SHM 的 hop latency 对 tail 不可接受，再评估同进程 N 路 WebSocket 直接融合。

## 必要诊断

fusion 输出和 report 至少需要支持以下观测：

- `source_id`
- `source_endpoint` / `connect_ip` / TLS 或 plain
- `source_owner_cpu`
- `source_local_ns`
- `fusion_receive_ns`
- `fusion_publish_ns`
- `symbol_id`
- `book_ticker_id`
- `selection_reason`
- `source_win_count`
- `duplicate_late_count`
- `out_of_order_late_count`
- `same_id_conflict_count`
- `id_jump_count`
- `fusion_hop_latency_ns = fusion_publish_ns - source_local_ns`

其中 `fusion_receive_ns` 用于定义“到达 fusion”的先后；`source_local_ns` 用于拆分 data session ingress 到 fusion publish 的本地链路。
`id_jump_count` 只表示相邻已发布 `BookTicker.id` 非连续，不默认代表丢包；BBO stream 可能只在 best bid / ask 变化时发布，因此 orderbook id 自然跳变需要单独用 live 证据解释。

## Shadow 验证

在接入策略前，应先 shadow 跑多路 fusion，并生成对账报告：

1. 对每个 `(symbol_id, id)`，统计各 source 是否都出现、谁 first arrival、到达时间差分布。
2. 校验同 `(symbol_id, id)` 的 quote、`exchange_ns` 是否一致。
3. 统计 missing、duplicate、out-of-order、same-id conflict 和 id jump。
4. 对比单 source 与 canonical 的 `lag_freshness_ns` p50 / p95 / p99，以及 `stale_lag_quote` reject。
5. 统计 fusion hop latency p50 / p95 / p99 / max。

只有当 shadow 证据显示 `(symbol_id, id)` 在多路 Gate source 间可稳定对齐，且 canonical stream 明确改善 freshness 或 stale reject，才应让策略切到 canonical Gate SHM。

## 当前推荐结论

第一版设计应采用：

```text
独立 fusion process
+ N 路 Gate source SHM
+ per-symbol first-arrival-by-(symbol_id,id)
+ canonical Gate BookTicker SHM
```

这是真正的多路融合选最快，不是 primary / standby，也不是 stale 后 fallback。核心正确性依赖 live Gate `BookTicker.id` 在同 symbol 多 source 间表示同一个 exchange orderbook update；该前提需要先用 shadow report 验证。
