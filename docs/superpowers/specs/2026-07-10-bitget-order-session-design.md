# Bitget UTA OrderSession 设计

## 状态

- 日期：2026-07-10
- 范围：Bitget UTA v3 单路 `OrderSession`
- endpoint：`wss://vip-ws-uta.bitget.com/v3/ws/private`
- 账户模式：仅支持 `one_way_mode`
- 当前阶段不发送真实订单

## 目的

本文定义 Bitget UTA v3 `OrderSession` 的组件边界、协议映射、错误语义、连接状态和验证要求。实现沿用 Gate
`OrderSession` 的职责划分，但保留 Bitget 特有的 login、operation response 和错误码语义。

当前阶段只实现上行 place / cancel 通道及其直接 operation response。`OrderFeedbackSession`、订单生命周期事实、
feedback SHM、多路 order gateway、LeadLag 接线和真实订单 smoke 均不在本文范围内。

## 事实源

协议事实以 Bitget 官方文档为准：

- [UTA WebSocket Place Order](https://www.bitget.com/api-doc/uta/websocket/private/Place-Order-Channel)
- [UTA WebSocket Cancel Order](https://www.bitget.com/api-doc/uta/websocket/private/Cancel-Order-Channel)
- [UTA Best Practices](https://www.bitget.com/api-doc/uta/best-practices)
- [UTA WebSocket Error Code](https://www.bitget.com/api-doc/uta/error-code/websocket)
- [Bitget WebSocket 连接与登录](https://www.bitget.com/zh-CN/api-doc/common/websocket-intro)

仓库内 Gate 对照入口：

- `exchange/gate/trading/order_session.h`
- `exchange/gate/trading/order_request_encoder.h`
- `exchange/gate/trading/submit_response_parser.h`
- `exchange/gate/trading/order_session_runtime_adapter.h`
- `core/trading/order_manager.h`

## 已锁定边界

### 当前范围

- authenticated private WebSocket login。
- limit order place，支持 `gtc` / `ioc`。
- 单笔 cancel，优先使用已缓存 `orderId`，否则使用 `clientOid`。
- request id / `local_order_id` correlation。
- operation response 解析和错误分类。
- `OrderSessionRuntimeAdapter` 到 `core::OrderResponseEvent` 的转换。
- TOML config、dry-run、login-only probe、单元测试和 microbenchmark。

### 非目标

- `OrderFeedbackSession` 以及 `order` / `fill` / `fast-fill` topic。
- `filled`、`partially_filled`、`cancelled` 等订单生命周期确认。
- feedback SHM、continuity lost 和 REST reconcile 实现。
- 多路 SHM order gateway、UID/account 级 rate limit 和 route policy。
- LeadLag live wiring、真实下单或成交验证。
- `hedge_mode`、`posSide`、Gate dual mode 或公共 `position_side` ABI。
- market order、FOK、post-only、RPI、STP、TP/SL、modify、batch 和 cancel-all。
- isolated margin。

## 方案选择

### 采用：Bitget 专用实现

新增 `exchange/bitget/trading/*`，复用 `core/trading/*` contract，但不重构 Gate 热路径。Bitget 与 Gate 的 login、
response 阶段和错误码差异保留在各自 namespace 内。

### 不采用：先抽象通用 ExchangeOrderSession

当前两家协议没有稳定的一一对应关系。先提取公共基类会把 Bitget 单阶段 operation response 与 Gate 两阶段
ACK/final response 强行统一，增加错误状态被误分类的风险。

### 不采用：扩展 Bitget DataSession

public SBE 行情 session 与 private trading session 的认证、状态机、错误边界和运行风险不同。复用底层
`BasicWebSocketClient` 即可，不复用 data session 业务状态机。

## 组件结构

```text
BitgetOrderSession
  -> BasicWebSocketClient
  -> private WebSocket login
  -> place-order / cancel-order encoder
  -> request correlation
  -> operation response parser
  -> Bitget OrderResponse
  -> BitgetOrderSessionRuntimeAdapter
  -> core::OrderResponseEvent
```

建议入口：

```text
exchange/bitget/trading/order_types.h
exchange/bitget/trading/order_codecs.h
exchange/bitget/trading/order_signature.h
exchange/bitget/trading/order_signature.cpp
exchange/bitget/trading/order_request_encoder.h
exchange/bitget/trading/operation_response_parser.h
exchange/bitget/trading/order_session_config.h
exchange/bitget/trading/order_session_config.cpp
exchange/bitget/trading/order_session.h
exchange/bitget/trading/order_session_runtime_adapter.h
tools/bitget/bitget_order_session_probe.cpp
```

## 组件职责

### OrderSession

负责：

- 维护 private WebSocket connection 和 login state。
- 编码 login、place 和 cancel request。
- 维护 request sequence 到 request type、`local_order_id` 和诊断字段的 correlation。
- 解析 operation response，输出 ACK、明确拒绝或 `UnknownResult`。
- 缓存 `local_order_id -> orderId`，供 cancel 优先使用。
- 记录连接、发送、response 和错误 diagnostics。

不负责：

- 确认订单已进入 matching system。
- 确认 fill、partial fill 或 cancel terminal。
- 修改 `StrategyOrder` 或解释策略意图。
- REST reconcile、position/account 状态和 rate budget。
- 自动重发 place / cancel。

### RuntimeAdapter

负责：

- 把 Bitget response kind 转成 `core::OrderResponseKind`。
- 构造 `core::OrderResponseEvent`。
- 连接 `TradingRuntime::OnOrderResponse()`。

不负责：

- 维护订单生命周期。
- 把 ACK 提升为 accepted / cancelled。
- 重试或 reconcile。

## 连接和 login 状态

状态流：

```text
Disconnected
  -> Connecting
  -> Active
  -> LoginSent
  -> Ready
```

规则：

- WebSocket active 后立即发送 login。
- 只有 login response `event=login` 且 `code=0` 时进入 ready。
- login error、service upgrade `30033`、heartbeat timeout 或 connection phase 离开 active 时立即 not-ready。
- reconnect 后重新 login，不复用旧 login state。
- 断线时清空 request correlation 和 socket diagnostics；不合成 rejected，也不自动重发请求。
- 已经提交到 socket 但未收到 response 的订单保持未知，由后续 feedback/reconcile 设计解决。

`Ready()` 只表示当前连接已登录并可尝试发送 operation，不表示账户订单状态连续或已经 reconcile。

## Login 签名

login payload：

```json
{
  "op": "login",
  "args": [{
    "apiKey": "<api-key>",
    "passphrase": "<passphrase>",
    "timestamp": "<timestamp>",
    "sign": "<base64-hmac>"
  }]
}
```

签名输入：

```text
timestamp + "GET" + "/user/verify"
```

算法：

```text
Base64(HMAC-SHA256(secret, signing_input))
```

Bitget 公共 WebSocket 文档的描述性文字写 Unix millisecond timestamp，但同页 payload 示例使用 10 位 Unix seconds。
V1 按示例和现有 Bitget WebSocket 行为使用 Unix seconds，并把 timestamp 生成与签名编码拆开测试。login-only live smoke
是进入任何真实订单测试前的强制验证，不用 REST millisecond timestamp 替代 WebSocket login timestamp。

凭据默认环境变量：

```text
BITGET_TEST_KEY
BITGET_TEST_SECRET
BITGET_TEST_PASSPHRASE
```

日志不得输出 secret、passphrase、完整 signature 或 login payload。

## Request ID 和 clientOid

### Request ID

operation `id` 用于单连接 response correlation。V1 使用固定宽度整数 codec 编码 request type 和单调递增 sequence，
再写成十进制字符串：

```text
request_id = Encode(request_type, sequence)
```

request type 包含：

```text
kPlaceOrder
kCancelOrder
```

login payload 没有 operation `id`；login response 通过 `event=login` 和当前 connection phase 单独关联，不进入
place/cancel correlation map。

response parser 必须同时验证：

- `id` 可解析且存在于 correlation map。
- `topic` 与 request type 一致。
- success response 的 `clientOid` 可还原到相同 `local_order_id`。

唯一例外是官方明确给出的 place-order `40010`、`40725`、`45001` 示例：这些 error 只有 `id` 而没有
`topic`，仅允许按 request ID 中的 place-order type 关联为 `kUnknownResult`。其他缺失 `topic` 的 operation error
不得消费 correlation。

### clientOid

格式：

```text
a-<local_order_id>
```

`local_order_id` 为 `uint64_t` 十进制文本，完整格式最长 22 字符，满足 Bitget 1 到 32 字符约束。parser 不接受溢出、
负数、空 suffix、额外字符或不匹配 prefix。

`clientOid` 是订单关联事实；operation `id` 只关联一次 request/response，不作为订单生命周期 ID。

## Place request 映射

固定 envelope：

```json
{
  "op": "trade",
  "id": "<request-id>",
  "category": "usdt-futures",
  "topic": "place-order",
  "args": [{
    "symbol": "BTCUSDT",
    "orderType": "limit",
    "qty": "0.001",
    "price": "100000.0",
    "side": "buy",
    "timeInForce": "ioc",
    "reduceOnly": "NO",
    "marginMode": "crossed",
    "clientOid": "a-1"
  }]
}
```

映射表：

| core 字段 | Bitget 字段 | V1 规则 |
| --- | --- | --- |
| `symbol` | `symbol` | 使用 catalog 的 Bitget `exchange_symbol` |
| `type` | `orderType` | 只接受 `OrderType::kLimit` |
| `quantity_text` | `qty` | 原样写入 JSON string |
| `price_text` | `price` | 原样写入 JSON string |
| `side` | `side` | `kBuy -> buy`，`kSell -> sell` |
| `time_in_force` | `timeInForce` | `kGoodTillCancel -> gtc`，`kImmediateOrCancel -> ioc` |
| `reduce_only` | `reduceOnly` | `true -> YES`，`false -> NO` |
| `local_order_id` | `clientOid` | `a-<local_order_id>` |
| session config | `category` | 固定 `usdt-futures` |
| session config | `marginMode` | 固定 `crossed` |

`quantity` double 只用于本地基础校验和 diagnostics；wire value 始终使用已经按 instrument metadata 格式化的
`quantity_text`。Bitget USDT futures `qty` 单位是 base asset quantity，catalog 的 `notional_multiplier` /
`contract_multiplier` 为 `1.0`。

V1 不发送 `posSide`。TOML 中 `position_mode` 只能为 `one_way_mode`；其他值在 config parse 阶段拒绝。该配置是实现能力声明，
不是账户事实校验。

## Cancel request 映射

固定 envelope：

```json
{
  "op": "trade",
  "id": "<request-id>",
  "category": "usdt-futures",
  "topic": "cancel-order",
  "args": [{
    "orderId": "123456789",
    "clientOid": "a-1"
  }]
}
```

规则：

- 已缓存非零 `exchange_order_id` 时同时发送 `orderId` 和 `clientOid`。
- 没有 `exchange_order_id` 时只发送 `clientOid`。
- success ACK 不删除 order ID cache，因为 ACK 只表示 cancel request 已收到。
- cache 由后续 feedback 终态或显式 `ForgetExchangeOrderId()` 清理。
- V1 不在其他 connection 上重试 cancel。

## Operation response 语义

Bitget place/cancel 的 success response 只表示交易所已收到 request。place response 还会分配 `orderId`，但不表示订单已经进入
matching system；cancel response 也不表示订单已经完成撤销。

映射：

| Bitget response | Bitget kind | core kind | OrderManager 语义 |
| --- | --- | --- | --- |
| place `code=0` | `kAck` | `kAck` | 记录请求阶段 ACK，不确认 accepted |
| cancel `code=0` | `kAck` | `kAck` | 保持 `kCancelSent`，不确认 cancelled |
| place 明确业务错误 | `kRejected` | `kRejected` | 确定 request 被拒绝 |
| cancel 明确业务错误 | `kCancelRejected` | `kCancelRejected` | 恢复 cancel 前状态 |
| ambiguous/system error | `kUnknownResult` | `kUnknownResult` | 保持订单等待 feedback/reconcile |

V1 不新增 `core::OrderResponseKind::kCancelAck`。Gate 对 `ack=true` 同样使用通用 `kAck`；Bitget cancel success response
直接复用该 contract，避免把 ACK 误映射成 `kCancelAccepted`。

success place response 中的 `orderId` 可以写入 session cache，但 ACK 本身不推进 accepted 状态。

## Error 分类

### 本地确定失败

编码或写 socket 前失败直接返回 `OrderSendStatus`，不产生 exchange response：

```text
kNotLoggedIn
kNotActive
kInflightFull
kUnsupportedOrderType
kInvalidSymbol
kInvalidQuantityText
kInvalidPriceText
kInvalidLocalOrderId
kSignatureFailed
kEncodeBufferTooSmall
kNoPreparedWriteSlot
kWriteUnavailable
```

### 交易所明确拒绝

明确的参数、权限、余额、价格、数量、position mode 或订单不存在错误映射为 `kRejected` / `kCancelRejected`。原始 numeric
`code` 进入 exchange-specific response 和日志，不扩展 core ABI。

### UnknownResult

以下情况映射为 `kUnknownResult`：

- 官方 place 文档明确要求使用 `clientOid` 查询最终结果的 `40010`、`40725`、`45001`。
- 收到无法证明 request 未执行的 service/system error。
- response envelope 可关联到 request，但错误码不在明确拒绝 allowlist。

断线导致 response 缺失时，session 按 Gate 当前边界清理 correlation、进入 not-ready，但不合成 rejected。该订单的最终状态留给后续
feedback/reconcile；因此没有 feedback session 时禁止真实下单验证。

## Timestamp 映射

| Bitget 字段 | 本地字段 | 说明 |
| --- | --- | --- |
| 本机 request submit 前取时 | `request_send_local_ns` | Unix epoch ns |
| response ingress 本机取时 | `local_receive_ns` | Unix epoch ns |
| response `ts` millisecond | `exchange_ns` | `ts * 1'000'000`，溢出时 parse failure |
| place response `cTime` | Bitget diagnostics | 不映射为 accepted timestamp |

Bitget operation response 没有 Gate `x_in_time` / `x_out_time`，所以以下字段保持 `0`：

```text
exchange_request_ingress_ns
exchange_response_egress_ns
exchange_process_ns
```

WebSocket response 没有 HTTP status，`http_status` 保持 `0`。不得使用跨机器 clock 推导单程网络延迟；主指标仍是本地
`local_receive_ns - request_send_local_ns`。

## Correlation 和容量

- request correlation 使用预留容量的 `absl::flat_hash_map`，steady state 不触发 rehash。
- place/cancel 共享 `request_map_capacity`，默认与 Gate 相同为 `16384`。
- 达到容量时同步返回 `kInflightFull`，不覆盖旧记录，不阻塞等待。
- response 成功分类后删除对应 request correlation。
- unknown id、重复 response、topic/type mismatch 和 `clientOid` mismatch 只记录 diagnostics，不错误关联其他订单。
- `local_order_id -> orderId` cache 使用独立预留容量；容量耗尽时 session 停止接受新 place，而不是静默放弃 cancel 能力。

## Heartbeat 和连接限额

- private session 使用 Bitget application-level text `ping` / `pong`。
- 至少每 30 秒发送一次 `ping`；未在配置 deadline 内收到 `pong` 时主动重连。
- heartbeat message 计入 Bitget connection message budget。
- V1 不实现 UID/account 级 order rate limiter；该预算属于后续 order gateway / risk 层。
- V1 保留单连接 request capacity、WebSocket send status 和 exchange `30006` / `30007` diagnostics。
- `30004`、`30007`、`30033` 或其他 connection/authentication-close error 立即进入 not-ready 并请求重连；即使 error
  带 operation `id`，也必须先完成该 request 的保守 response 映射，再使 session 失效。

在 account 级 rate limit 落地前，不允许把 V1 `OrderSession` 直接用于 LeadLag live orders。

## Config

示例：

```toml
[order_session]
name = "bitget_order_session_high_availability"
category = "usdt-futures"
position_mode = "one_way_mode"
margin_mode = "crossed"
request_map_capacity = 16384
order_id_cache_capacity = 16384

[order_session.credentials]
api_key_env = "BITGET_TEST_KEY"
api_secret_env = "BITGET_TEST_SECRET"
api_passphrase_env = "BITGET_TEST_PASSPHRASE"

[order_session.websocket.endpoint]
host = "vip-ws-uta.bitget.com"
port = 443
target = "/v3/ws/private"
enable_tls = true
```

TOML 只配置一条 endpoint。V1 checked-in config 使用 high availability endpoint，不加入 high speed route 或自动 failover。

config parser 必须 fail-fast：

- 缺失 name 或 credential env name。
- `category != usdt-futures`。
- `position_mode != one_way_mode`。
- `margin_mode != crossed`。
- request/cache capacity 为 0。
- endpoint target 不是 private v3 path。
- TLS/port 配置自相矛盾。

当前 Bitget 账户在 2026-07-10 查询结果为 `hedge_mode`。真实下单前必须在 session 外通过只读 REST preflight 确认：

```text
holdMode == one_way_mode
accountMode == unified
UTA trade permission == read_write
open orders == empty
positions == flat
```

`OrderSession` 不在热路径中调用 REST，也不自行切换账户模式。

## Diagnostics

至少记录：

```text
bitget_order_session_connected
bitget_order_session_phase
bitget_order_send
bitget_order_response
bitget_order_response_error
bitget_order_session_summary
```

关键字段：

```text
request_type
request_sequence
local_order_id
exchange_order_id
response_kind
error_code
request_send_local_ns
local_receive_ns
exchange_ns
ack_rtt_ns
connection_id_hash
```

不记录 API key、secret、passphrase、signature 或完整 login payload。新增 log key 和字段时同步更新
`docs/diagnostic_fields.md`。

## 测试要求

### Codec 和签名

- request ID type/sequence round-trip、溢出和非法输入。
- `clientOid` round-trip、最大 `uint64_t`、错误 prefix 和超长输入。
- HMAC-SHA256/Base64 deterministic fixture。
- Unix seconds timestamp 写入 login payload。
- buffer 不足和凭据字段边界。

### Encoder

- login、limit GTC、limit IOC、buy/sell、reduce-only place。
- cancel by `orderId` + `clientOid` 和 cancel by `clientOid`。
- market/FOK/post-only/hedge/isolated 等 unsupported reject。
- quantity/price/symbol/client ID 超长或空值 reject。

### Response parser

- login success/error。
- place/cancel success ACK。
- 明确 reject、`40010`、`40725`、`45001` 和未分类 error。
- malformed JSON、缺字段、错误类型、unknown id、topic/type mismatch。
- `orderId` / `clientOid` mismatch。
- `ts` 转 ns 的正常值和 overflow。

### Session

- active 后 login，login success 后 ready。
- login failure、`30033`、disconnect 和 reconnect 状态转换。
- place/cancel correlation 创建、ACK 清理和 error 清理。
- cancel success 保持通用 `kAck`，不产生 `kCancelAccepted`。
- disconnect 清空 correlation 且不自动重发。
- inflight/cache capacity fail-fast。
- text ping/pong 和 heartbeat timeout。

### Adapter

- `kAck`、`kRejected`、`kCancelRejected`、`kUnknownResult` 映射。
- `local_order_id`、`exchange_order_id`、`local_receive_ns`、`exchange_ns` 保真。
- operation ACK 不推进 accepted/cancelled 终态。

## Benchmark 和验证

实现阶段至少提供 fixed-buffer encoder 和 operation response parser release microbenchmark。benchmark 只建立基线；没有固定 CPU、
命令、样本和 percentile 证据时不宣称性能收益。

最小验证顺序：

```bash
./build.sh debug
ctest --test-dir build/debug -R 'bitget_.*order|core_order' --output-on-failure
git diff --check
```

release 验证：

```bash
./build.sh release
ctest --test-dir build/release -R 'bitget_.*order' --output-on-failure
```

live 验证只允许 login-only：

```text
connect -> login -> Ready() -> heartbeat -> clean stop
```

验收证据必须包含 endpoint、连接时长、login response、heartbeat、reconnect/stop 结果和最终 metrics。当前阶段不调用
`place-order` / `cancel-order`，也不以 login success 证明 trade permission、订单正确性或交易延迟。

## 后续阶段入口

完成并验证本设计后，后续单独讨论：

1. `OrderFeedbackSession` 使用 `order` topic 确认 accepted、partial fill、filled 和 cancelled。
2. feedback continuity、REST reconcile 和 reconnect resume。
3. `fast-fill` 是否作为独立 `FillEvent`，以及与 order lifecycle 的去重关系。
4. UID/account 级 rate limit、multi-route order gateway 和 LeadLag wiring。
5. guarded real-order smoke 和 private endpoint A/B。
