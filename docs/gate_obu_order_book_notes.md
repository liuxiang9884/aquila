# Gate OBU OrderBook 讨论笔记

本文记录 2026-07-10 关于 Gate futures `futures.obu` / Order Book V2 与内部
`OrderBook` 结构的讨论结论。当前只是设计讨论和 quick probe 记录，不代表生产实现已经完成。

## 官方 channel 与频率

Gate futures 深度相关 channel 有两类：

| Channel | Subscribe payload 示例 | Level | Frequency |
| --- | --- | ---: | --- |
| `futures.order_book_update` | `["BTC_USDT", "100ms", "100"]` | `100` | `100ms` |
| `futures.order_book_update` | `["BTC_USDT", "100ms", "50"]` | `50` | `100ms` |
| `futures.order_book_update` | `["BTC_USDT", "100ms", "20"]` | `20` | `100ms` |
| `futures.order_book_update` | `["BTC_USDT", "20ms", "20"]` | `20` | `20ms` |
| `futures.obu` | `["ob.BTC_USDT.400"]` | `400` | `100ms` |
| `futures.obu` | `["ob.BTC_USDT.50"]` | `50` | `20ms` |

本轮讨论后续默认以 `futures.obu` 为主。`futures.obu` 的 `50` / `400` 是最大订阅深度，
不是保证每条 full snapshot 一定返回满档。

## OBU 语义

`futures.obu` 订阅 payload 是单个 stream name：

```json
["ob.BTC_USDT.50"]
```

关键语义：

- 第一条行情是 `full=true` snapshot。
- 后续消息是 incremental update；JSON incremental 通常没有 `full` 字段。
- 本地连续性以 `U` / `u` 判断：full snapshot 后本地 `depth_id = u`，后续要求
  `U == depth_id + 1`，否则应重新订阅并等待新的 full snapshot。
- `a` 表示 asks，`b` 表示 bids。
- 每个 level 是 `[price, size]`，`size` 是 absolute size；`size == "0"` 表示删除该 price level。
- 实测存在 incremental update 只有 `U` / `u` / `t`，但 bid/ask update arrays 都为空的情况。

## Quick Probe 证据

本轮用 Python + `websocket-client` 做了临时探测，产物写在 `/home/liuxiang/tmp`，不进入仓库：

```text
/home/liuxiang/tmp/gate_obu_inactive_depth_probe_latest.json
/home/liuxiang/tmp/gate_obu_inactive_depth_probe_summary_latest.json
/home/liuxiang/tmp/gate_obu_inactive_depth_updates_30s_latest.json
/home/liuxiang/tmp/gate_obu400_sbe_first_frame.bin
/home/liuxiang/tmp/gate_obu400_sbe_first_frame_summary.json
```

样本合约是低成交的 `BNT_USDT`、`CELR_USDT`、`ONE_USDT`。JSON WebSocket 使用：

```text
wss://fx-ws.gateio.ws/v4/ws/usdt
```

SBE WebSocket 使用：

```text
wss://fx-ws.gateio.ws/v4/ws/usdt/sbe
```

### JSON OBU snapshot 摘要

| Stream | bid count | ask count | best bid | best ask | 备注 |
| --- | ---: | ---: | --- | --- | --- |
| `ob.BNT_USDT.50` | 50 | 50 | `0.2672 / 33` | `0.2682 / 336` | 满 50 档 |
| `ob.CELR_USDT.50` | 50 | 50 | `0.001806 / 660` | `0.001814 / 672` | 满 50 档 |
| `ob.ONE_USDT.50` | 50 | 50 | `0.001196 / 3689` | `0.001206 / 86817` | 满 50 档 |
| `ob.BNT_USDT.400` | 59 | 51 | `0.2672 / 33` | `0.2682 / 336` | 不满 400 |
| `ob.CELR_USDT.400` | 58 | 60 | `0.001806 / 660` | `0.001814 / 672` | 不满 400 |
| `ob.ONE_USDT.400` | 62 | 57 | `0.001196 / 3689` | `0.001206 / 86817` | 不满 400 |

30 秒窗口内，低活跃合约的 update 很少；`level=50` 和 `level=400` 都观察到了
`size == 0` 删除语义。`CELR_USDT.400` 观察到一条 `U/u` 前进但 bid/ask arrays 为空的 update。

### SBE OBU layout 摘要

Python 手动解析 `ob.BNT_USDT.400` 第一条 SBE binary frame，结果：

```json
{
  "header": {
    "block_length": 36,
    "template_id": 3,
    "schema_id": 1,
    "version": 1
  },
  "fixed": {
    "full": 1,
    "first_id": 0,
    "last_id": 1625730577,
    "px_exponent": -6,
    "sz_exponent": -2
  },
  "bids_group_header": {
    "block_length": 16,
    "num_in_group": 60
  },
  "asks_group_header": {
    "block_length": 16,
    "num_in_group": 50
  }
}
```

`full` snapshot 和 incremental update 的 bid/ask entry 数量都来自 SBE repeating group header 的
`numInGroup`。`full=1` 只说明消息是完整 snapshot，不说明返回档数等于订阅 level。

SBE `obu` 的 levels 是 array of struct：

```text
bids group header:
  blockLength uint16 = 16
  numInGroup uint16 = bid level count

bids[0]:
  pxMantissa int64
  szMantissa int64

bids[1]:
  pxMantissa int64
  szMantissa int64
```

asks group 同理。`blockLength = 16` 正好对应 `int64 price mantissa + int64 size mantissa`。

仓库已有 SBE 生成代码和 dispatcher 识别：

```text
exchange/gate/sbe/generated/gate/messages/obu.hpp
exchange/gate/sbe/generated/gate/messages/orderBookUpdate.hpp
exchange/gate/sbe/message_dispatcher.h
```

但当前生产 decoder 只覆盖 `BookTicker` / `Trade`，还没有完整 OBU decoder、本地 order book
维护器或深度 typed channel。

## OrderBook 结构讨论

用户提出的第一版 snapshot 结构草案：

```cpp
template <std::size_t Level>
struct Orderbook {
    std::int64_t first_id;
    std::int64_t last_id;
    std::int64_t exchange_ns;
    std::int64_t event_ns;
    std::int64_t local_ns;

    std::int64_t ask_count;
    std::int64_t bid_count;
    double ask_price[Level];
    double ask_volume[Level];
    double bid_price[Level];
    double bid_volume[Level];
};
```

讨论结论：

- 如果下游只需要维护后的 order book snapshot，可以不把 `OrderBookUpdate` 作为公开结构。
  OBU full / incremental update 可以在 decoder / book builder 内部直接 apply。
- 该结构应被理解为 published snapshot，不是 raw OBU update。raw OBU update 中的 count 来自
  SBE `numInGroup`，published snapshot 中的 count 表示当前数组内有效 top-N 档数。
- `bid_count` / `ask_count` 有必要保留，因为 `obu.400` 对低活跃合约不保证满 400 档；
  即使发布 `OrderBook<10>`，启动期、冷门合约或恢复期也可能不满 10 档。
- `size == 0` 删除语义应在内部 book builder 处理，published snapshot 不应携带 size 为 0 的有效档位。
- `double` 作为发布给策略的 snapshot 字段可以与现有 `BookTicker` 风格对齐；
  但内部维护 book 时不建议用 `double` 做 price key，应使用 Gate SBE 的 mantissa / exponent、
  tick integer 或等价整数 price key，发布前再转成 `double`。
- 命名上建议后续实现时按项目 C++ 风格改为 `OrderBook` / `kLevel`。是否加入
  `symbol_id` / `exchange` 取决于发布通道是否是 per-symbol；若复用现有 typed channel / recorder
  风格，建议与 `BookTicker` / `Trade` 对齐并保留这两个字段。
- `ask_count` / `bid_count` 最大目前只需要覆盖 400 档，`uint16_t` 足够；是否继续用
  `int64_t` 可由后续 ABI 对齐和 padding 决定。

当前 `core/market_data/types.h` 工作区里已有一个未提交的 `Orderbook` 草案改动；下一轮继续实现前应先
确认是否沿用该草案、是否调整命名 / count 类型 / `symbol_id` 和 `exchange` 字段。

## 后续建议

1. 先决定 published `OrderBook<10>` 的 ABI：是否包含 `symbol_id` / `exchange`、count 类型、
   array-of-struct 还是 struct-of-arrays。
2. 若走 SBE OBU 实现，先加 Gate OBU decoder 单测，覆盖 full snapshot、incremental update、
   `numInGroup` 不满 level、empty update、`size == 0` 删除。
3. 本地 book builder 内部维护完整 source depth（如 50 或 400），发布时再截取 top 10；
   不要在 decoder 阶段截断到 10 档，否则 10 档外的价格移动进入 top 10 时会丢状态。
4. 设计 data session / SHM / recorder 前先明确这是新 feed，不能复用 `book_ticker` feed 语义。
5. 任何关于 OBU 相比 BBO 的延迟或 fillability 收益，都必须另做 live smoke / benchmark；
   本文 quick probe 只证明消息形态和字段语义。
