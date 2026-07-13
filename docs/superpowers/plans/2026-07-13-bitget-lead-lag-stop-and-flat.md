# Bitget LeadLag V1 Stop-and-Flat 实施计划

> **面向 agentic worker：** 必须使用 `superpowers:subagent-driven-development`（推荐）或
> `superpowers:executing-plans`，按任务逐项执行。所有步骤使用 checkbox 跟踪；本项目如使用
> subagent，必须遵守 `AGENTS.md` 的 `aquila_xhigh_worker` 约束。

**目标：** 为 Bitget `order_gateway` backend 实现与 Gate V1 同语义的 REST preflight、
emergency stop-and-flat、final flat 和 fresh-run isolation，同时保持现有 Gate 行为不变。

**架构：** 新增 Bitget UTA trading REST client 与 emergency helper；把现有 LeadLag guard 的
交易所相关能力收敛为 adapter，Gate 作为默认 adapter，Bitget adapter 增加 passphrase、UTA
response parser 和 Bitget flatten config。运行隔离通过生成 run-specific strategy/gateway/feedback
TOML overlay 并在 guard 启动前校验实现；V1 不修改 `local_order_id` allocator，不恢复旧 run。

**技术栈：** Python 3 标准库、`unittest`、TOML、Bitget UTA REST v3、现有 C++20 LeadLag/
OrderGateway runtime、POSIX SHM。

---

## 文件结构

### 新增文件

- `scripts/bitget/trading/place_futures_order.py`：Bitget UTA signed trading client、place/cancel
  request builder，以及默认 dry-run 的受控 CLI。
- `scripts/bitget/trading/emergency_flatten_futures.py`：Bitget allowlist/dedicated-account
  stop-and-flat helper。
- `scripts/test/bitget/trading/place_futures_order_test.py`：REST request、签名、response validation
  和 dry-run 测试。
- `scripts/test/bitget/trading/emergency_flatten_futures_test.py`：scope、parser、动作顺序、幂等和
  fail-closed 测试。
- `scripts/lead_lag/guard_exchange_adapter.py`：Gate/Bitget guard adapter 数据结构与 backend factory。
- `scripts/test/lead_lag/guard_exchange_adapter_test.py`：adapter credential、requester、state 和
  flatten config 测试。
- `scripts/lead_lag/prepare_bitget_live_run.py`：生成 run-specific strategy/gateway/feedback overlay
  与 manifest，不连接交易所、不读取账户。
- `scripts/test/lead_lag/prepare_bitget_live_run_test.py`：overlay、SHM、route 与 manifest 测试。

### 修改文件

- `scripts/lead_lag/run_live_with_guard.py`：保留 Gate 默认行为，注入 exchange adapter，支持
  Bitget passphrase、manifest 和 isolation validation。
- `scripts/test/lead_lag/run_live_with_guard_test.py`：Gate regression 与 Bitget orchestration 分支。
- `docs/bitget_trading.md`：迁移最终 V1 contract、验证入口与 live 阻断。
- `docs/lead_lag_live_operations.md`：增加 Bitget prepare/preflight/stop-and-flat 流程。
- `docs/lead_lag_reconcile_design.md`：把 Gate-only V1 边界扩展为 exchange-specific helper contract。
- `docs/project_onboarding_guide.md`：更新 Bitget P0 摘要与下一步。

### 完成后删除

实现、验证和专题文档迁移全部完成后，删除已完成态设计与计划：

- `docs/superpowers/specs/2026-07-13-bitget-lead-lag-stop-and-flat-design.md`
- `docs/superpowers/plans/2026-07-13-bitget-lead-lag-stop-and-flat.md`

---

### Task 1：Bitget UTA trading REST client

**Files:**

- Create: `scripts/bitget/trading/place_futures_order.py`
- Create: `scripts/test/bitget/trading/place_futures_order_test.py`
- Reuse: `scripts/bitget/account/query_bitget_account.py`

- [ ] **Step 1：写 request builder 与 response validation 的失败测试**

在测试中加入以下最小 contract：

```python
def test_build_reduce_only_market_close_request():
    request = orders.build_place_order_request(
        category="USDT-FUTURES",
        symbol="BTC_USDT",
        qty=Decimal("0.001"),
        side="sell",
        margin_mode="crossed",
        client_oid="a-flat-1700000000000-0",
        reduce_only=True,
    )

    assert request.method == "POST"
    assert request.endpoint_path == "/api/v3/trade/place-order"
    assert json.loads(request.body) == {
        "category": "USDT-FUTURES",
        "symbol": "BTCUSDT",
        "qty": "0.001",
        "side": "sell",
        "orderType": "market",
        "reduceOnly": "yes",
        "marginMode": "crossed",
        "clientOid": "a-flat-1700000000000-0",
    }


def test_build_cancel_request_prefers_exchange_order_id():
    request = orders.build_cancel_order_request(
        category="USDT-FUTURES",
        symbol="BTCUSDT",
        order_id="12345",
        client_oid="a-7",
    )
    assert json.loads(request.body)["orderId"] == "12345"
    assert "clientOid" not in json.loads(request.body)


def test_validate_response_rejects_non_success_code():
    with self.assertRaisesRegex(RuntimeError, "Bitget REST code=40010"):
        orders.validate_uta_response({"code": "40010", "msg": "request timed out"})
```

- [ ] **Step 2：运行测试，确认 RED**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
```

Expected: FAIL，原因是 `scripts/bitget/trading/place_futures_order.py` 尚不存在；创建空模块后应继续
因 `build_place_order_request` 等 API 缺失而失败。

- [ ] **Step 3：实现最小 request/client API**

实现以下公开类型和函数，名称在后续任务中保持不变：

```python
@dataclass(frozen=True)
class ApiRequest:
    method: str
    endpoint_path: str
    query_string: str = ""
    body: str = ""

    def to_public_dict(self) -> dict[str, str]:
        return {
            "method": self.method,
            "endpoint_path": self.endpoint_path,
            "query_string": self.query_string,
            "body": self.body,
        }


def decimal_text(value: Decimal) -> str:
    text = format(Decimal(value), "f")
    if Decimal(text) <= 0:
        raise ValueError("qty must be positive")
    return text


def normalize_symbol(symbol: str) -> str:
    value = symbol.strip().upper().replace("_", "").replace("/", "")
    if not value:
        raise ValueError("symbol must not be empty")
    return value


def build_place_order_request(
    category: str,
    symbol: str,
    qty: Decimal,
    side: str,
    margin_mode: str,
    client_oid: str,
    reduce_only: bool,
) -> ApiRequest:
    payload = {
        "category": account.normalize_category(category),
        "symbol": normalize_symbol(symbol),
        "qty": decimal_text(qty),
        "side": normalize_side(side),
        "orderType": "market",
        "reduceOnly": "yes" if reduce_only else "no",
        "marginMode": normalize_margin_mode(margin_mode),
        "clientOid": validate_client_oid(client_oid),
    }
    return ApiRequest(
        method="POST",
        endpoint_path="/api/v3/trade/place-order",
        body=json.dumps(payload, separators=(",", ":"), ensure_ascii=False),
    )


def build_cancel_order_request(
    category: str,
    symbol: str,
    order_id: str | None,
    client_oid: str | None,
) -> ApiRequest:
    identity = {"orderId": str(order_id)} if order_id else {"clientOid": str(client_oid)}
    payload = {
        **identity,
        "category": account.normalize_category(category),
        "symbol": normalize_symbol(symbol),
    }
    return ApiRequest(
        method="POST",
        endpoint_path="/api/v3/trade/cancel-order",
        body=json.dumps(payload, separators=(",", ":"), ensure_ascii=False),
    )


def validate_uta_response(payload: Any) -> Any:
    if not isinstance(payload, dict):
        raise RuntimeError("Bitget REST response must be an object")
    if payload.get("code") != "00000":
        raise RuntimeError(
            f"Bitget REST code={payload.get('code')} msg={payload.get('msg')}"
        )
    if "data" not in payload:
        raise RuntimeError("Bitget REST success response missing data")
    return payload["data"]
```

`SignedBitgetTradingClient.request_json()` 复用
`query_bitget_account.build_signature_headers()`、`request_path()` 和 `request_url()`，但必须把
`api_passphrase` 传入签名 header；GET 不发送 body，POST 发送 UTF-8 JSON body。HTTP、URL、空 body、
JSON decode 和 `code != "00000"` 都抛 `RuntimeError`。

CLI 默认只输出 request plan；只有显式 `--execute` 才调用 requester。CLI 的 credential 参数是环境
变量名，不输出 secret/passphrase。

- [ ] **Step 4：运行 focused tests，确认 GREEN**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/account/query_bitget_account_test.py
```

Expected: 全部 PASS，且只读 account CLI regression 不变。

- [ ] **Step 5：提交 Task 1**

```bash
git add scripts/bitget/trading/place_futures_order.py \
  scripts/test/bitget/trading/place_futures_order_test.py
git commit -m "feat: add Bitget UTA trading REST client"
```

---

### Task 2：Bitget flatten 数据模型、scope 与 read-only plan

**Files:**

- Create: `scripts/bitget/trading/emergency_flatten_futures.py`
- Create: `scripts/test/bitget/trading/emergency_flatten_futures_test.py`
- Reuse: `scripts/bitget/account/query_bitget_account.py`
- Reuse: `scripts/bitget/trading/place_futures_order.py`

- [ ] **Step 1：写 parser、flat predicate 与 scope 的失败测试**

覆盖以下独立行为：

```python
def test_position_is_flat_only_when_total_available_and_frozen_are_zero(self):
    position = flatten.PositionSnapshot(
        symbol="BTCUSDT",
        pos_side="long",
        total=Decimal("0"),
        available=Decimal("0"),
        frozen=Decimal("0.001"),
        margin_mode="crossed",
    )
    self.assertFalse(position.flat())


def test_allowlist_requires_at_least_one_symbol(self):
    with self.assertRaisesRegex(flatten.ScopeRefused, "allowlist"):
        flatten.validate_config(allowlist_config(symbols=[]))


def test_dedicated_account_requires_explicit_confirmation(self):
    with self.assertRaisesRegex(flatten.ScopeRefused, "confirm-dedicated-account"):
        flatten.validate_config(
            dedicated_config(confirm_dedicated_account=False)
        )


def test_parse_open_orders_rejects_symbol_outside_allowlist(self):
    payload = {"list": [{"symbol": "ETHUSDT", "orderId": "9", "clientOid": "a-9"}]}
    with self.assertRaisesRegex(flatten.RestFailure, "outside allowlist"):
        flatten.parse_open_orders(payload, {"BTCUSDT"})
```

- [ ] **Step 2：运行测试，确认 RED**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
```

Expected: FAIL，原因是 emergency helper 及其公开类型尚不存在。

- [ ] **Step 3：实现数据模型和 read-only query/plan**

公开类型固定为：

```python
@dataclass(frozen=True)
class FlattenConfig:
    category: str
    scope: str
    symbols: list[str]
    confirm_dedicated_account: bool
    dry_run: bool
    poll_timeout_sec: float
    poll_interval_sec: float
    max_position_count: int


@dataclass(frozen=True)
class PositionSnapshot:
    symbol: str
    pos_side: str
    total: Decimal
    available: Decimal
    frozen: Decimal
    margin_mode: str

    def flat(self) -> bool:
        return self.total == 0 and self.available == 0 and self.frozen == 0


@dataclass(frozen=True)
class OpenOrder:
    symbol: str
    order_id: str
    client_oid: str


def final_state_is_flat(
    positions: list[PositionSnapshot], open_orders: list[OpenOrder]
) -> bool:
    return not open_orders and all(position.flat() for position in positions)
```

Parser 必须先调用 `validate_uta_response()`，再要求 `data.list` 是 list。所有 quantity 使用
`Decimal(str(value))`；缺失、空值、NaN/Infinity、非法 `posSide`/`marginMode`、category/symbol
mismatch 都抛 `RestFailure`。Allowlist query 对每个 symbol 分别调用现有 read-only endpoint；
dedicated-account query 不带 symbol，完整扫描 category。

Dry-run summary 必须包含：`scope`、`initial_open_orders`、`initial_positions`、
`open_orders_to_cancel`、`positions_to_close`、`dry_run=true`，且不得调用任何 POST request。

- [ ] **Step 4：运行 focused tests，确认 GREEN**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
```

Expected: parser/scope/dry-run tests 全部 PASS。

- [ ] **Step 5：提交 Task 2**

```bash
git add scripts/bitget/trading/emergency_flatten_futures.py \
  scripts/test/bitget/trading/emergency_flatten_futures_test.py
git commit -m "feat: add Bitget emergency flatten planning"
```

---

### Task 3：Bitget cancel、reduce-only close 与 poll-until-flat

**Files:**

- Modify: `scripts/bitget/trading/emergency_flatten_futures.py`
- Modify: `scripts/test/bitget/trading/emergency_flatten_futures_test.py`

- [ ] **Step 1：写动作顺序、幂等和 fail-closed 的失败测试**

至少拆成以下测试：

```python
def uta_list(items):
    return {"code": "00000", "msg": "success", "data": {"list": items}}


def open_order(symbol, order_id):
    return {"symbol": symbol, "orderId": order_id, "clientOid": f"a-{order_id}"}


def long_position(symbol, total):
    return {
        "symbol": symbol,
        "posSide": "long",
        "total": total,
        "available": total,
        "frozen": "0",
        "marginMode": "crossed",
    }


def flat_position(symbol):
    return {
        "symbol": symbol,
        "posSide": "long",
        "total": "0",
        "available": "0",
        "frozen": "0",
        "marginMode": "crossed",
    }


class FakeClock:
    def __init__(self):
        self.now = 1700000000.0

    def time(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class ScriptedRequester:
    def __init__(
        self,
        initial_open_orders,
        positions,
        post_close_open_orders,
        final_positions,
        final_open_orders,
        cancel_error=None,
    ):
        self.open_order_results = deque(
            [initial_open_orders, post_close_open_orders, final_open_orders]
        )
        self.position_results = deque([positions, final_positions])
        self.cancel_error = cancel_error
        self.mutating_topics = []
        self.place_bodies = []

    @classmethod
    def flat_account(cls):
        return cls([], [flat_position("BTCUSDT")], [], [flat_position("BTCUSDT")], [])

    @classmethod
    def cancel_timeout_and_remains_nonflat(cls):
        return cls(
            [open_order("BTCUSDT", "11")],
            [long_position("BTCUSDT", "0.001")],
            [open_order("BTCUSDT", "11")],
            [long_position("BTCUSDT", "0.001")],
            [open_order("BTCUSDT", "11")],
            cancel_error=RuntimeError("cancel timeout"),
        )

    def __call__(self, request):
        if request.method == "GET" and request.endpoint_path.endswith("unfilled-orders"):
            return uta_list(self.open_order_results.popleft())
        if request.method == "GET" and request.endpoint_path.endswith("current-position"):
            return uta_list(self.position_results.popleft())
        if request.method == "POST" and request.endpoint_path.endswith("cancel-order"):
            self.mutating_topics.append("cancel-order")
            if self.cancel_error is not None:
                raise self.cancel_error
            return {"code": "00000", "msg": "success", "data": {"orderId": "11"}}
        if request.method == "POST" and request.endpoint_path.endswith("place-order"):
            self.mutating_topics.append("place-order")
            self.place_bodies.append(json.loads(request.body))
            return {"code": "00000", "msg": "success", "data": {"orderId": "21"}}
        raise AssertionError(f"unexpected request: {request}")


def test_cancels_before_close_then_cancels_again_and_polls_flat(self):
    requester = ScriptedRequester(
        initial_open_orders=[open_order("BTCUSDT", "11")],
        positions=[long_position("BTCUSDT", "0.001")],
        post_close_open_orders=[open_order("BTCUSDT", "12")],
        final_positions=[flat_position("BTCUSDT")],
        final_open_orders=[],
    )
    exit_code, summary = flatten.run_emergency_flatten(
        allowlist_config(), requester, FakeClock()
    )
    self.assertEqual(exit_code, flatten.EXIT_OK)
    self.assertEqual(summary["result"], "verified_flat")
    self.assertEqual(
        requester.mutating_topics,
        ["cancel-order", "place-order", "cancel-order"],
    )
    close_body = requester.place_bodies[0]
    self.assertEqual(close_body["side"], "sell")
    self.assertEqual(close_body["qty"], "0.001")
    self.assertEqual(close_body["reduceOnly"], "yes")
    self.assertEqual(close_body["orderType"], "market")


def test_flat_account_is_idempotent_and_sends_no_mutation(self):
    requester = ScriptedRequester.flat_account()
    exit_code, summary = flatten.run_emergency_flatten(
        allowlist_config(), requester, FakeClock()
    )
    self.assertEqual(exit_code, flatten.EXIT_OK)
    self.assertEqual(requester.mutating_topics, [])


def test_unknown_mutation_response_without_final_flat_fails_closed(self):
    requester = ScriptedRequester.cancel_timeout_and_remains_nonflat()
    exit_code, summary = flatten.run_emergency_flatten(
        allowlist_config(poll_timeout_sec=0), requester, FakeClock()
    )
    self.assertIn(exit_code, {flatten.EXIT_NOT_FLAT, flatten.EXIT_REST_FAILED})
    self.assertFalse(summary["ok"])
```

- [ ] **Step 2：运行测试，确认 RED**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
```

Expected: FAIL，原因是 mutating workflow 和 polling 尚未实现。

- [ ] **Step 3：实现最小 stop-and-flat workflow**

实现顺序固定为：

```python
scoped_symbols = determine_scope_symbols(requester, config, summary)
initial_open_orders = query_scoped_open_orders(
    requester, config.category, scoped_symbols
)
cancel_open_orders(initial_open_orders, phase="before_close")
positions = query_scoped_positions(requester, config.category, scoped_symbols)
non_flat_positions = [position for position in positions if not position.flat()]
submit_reduce_only_close_orders(non_flat_positions)
post_close_open_orders = query_scoped_open_orders(
    requester, config.category, scoped_symbols
)
cancel_open_orders(post_close_open_orders, phase="after_close")
verified, polls, final_positions, final_open_orders = poll_until_flat(
    requester=requester,
    category=config.category,
    symbols=scoped_symbols,
    timeout_sec=config.poll_timeout_sec,
    interval_sec=config.poll_interval_sec,
    clock=clock,
)
return finish_summary(
    summary, verified, polls, final_positions, final_open_orders
)
```

Close side 由 `posSide` 决定：`long -> sell`，`short -> buy`；`qty=abs(total)`；使用 position 的
`marginMode`；`clientOid` 格式固定为 `a-flat-<epoch_ms>-<seq>`，先验证总长度不超过 32。不得根据
本地 LeadLag position 或原 order size 推导 close quantity。

请求异常后不得盲目重发同一个 cancel/place。Helper 可继续执行 read-only final snapshot；只有
snapshot 证明 flat 才返回成功，否则返回 `EXIT_REST_FAILED` 或 `EXIT_NOT_FLAT`。Dedicated-account
模式在任何 mutating request 前执行 `max_position_count` guard。

- [ ] **Step 4：运行 Bitget helper 全部测试，确认 GREEN**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
```

Expected: 全部 PASS。

- [ ] **Step 5：提交 Task 3**

```bash
git add scripts/bitget/trading/emergency_flatten_futures.py \
  scripts/test/bitget/trading/emergency_flatten_futures_test.py
git commit -m "feat: implement Bitget emergency stop and flat"
```

---

### Task 4：提取 LeadLag guard exchange adapter，并保持 Gate regression

**Files:**

- Create: `scripts/lead_lag/guard_exchange_adapter.py`
- Create: `scripts/test/lead_lag/guard_exchange_adapter_test.py`
- Modify: `scripts/lead_lag/run_live_with_guard.py`
- Modify: `scripts/test/lead_lag/run_live_with_guard_test.py`

- [ ] **Step 1：写 Gate 默认行为与 generic injection 的失败测试**

```python
def test_parse_args_defaults_to_gate(self):
    parsed = guard.parse_args(["--contract", "BTC_USDT", "--", "strategy"])
    self.assertEqual(parsed.exchange, "gate")


def test_guard_uses_adapter_flatten_config_builder(self):
    builder = RecordingFlattenConfigBuilder(result={"exchange": "test"})
    flatten_runner = FakeFlattenRunner((0, {"ok": True}))
    exit_code, _ = guard.run_guarded_live(
        config=config(),
        requester=lambda request: {},
        process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=1)),
        state_reader=FakeStateReader([flat_state()]),
        flatten_config_builder=builder,
        flatten_runner=flatten_runner,
    )
    self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
    self.assertEqual(builder.calls, [config()])
```

- [ ] **Step 2：运行 Gate guard tests，确认 RED**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/guard_exchange_adapter_test.py
```

Expected: FAIL，原因是 `--exchange`、adapter 和 `flatten_config_builder` 尚不存在。

- [ ] **Step 3：实现 adapter API 和最小 orchestration refactor**

公开 adapter contract 固定为：

```python
CredentialResolver = Callable[
    [list[str], str | None, str | None, str | None],
    GuardCredentialEnvNames,
]
RequesterFactory = Callable[
    [str, str, str | None, str, float],
    Any,
]


class GuardStateLike(Protocol):
    def flat(self) -> bool:
        raise NotImplementedError

    def to_summary(self) -> dict[str, Any]:
        raise NotImplementedError


StateReader = Callable[[Any, str, list[str]], GuardStateLike]


@dataclass(frozen=True)
class GuardCredentialEnvNames:
    api_key_env: str
    api_secret_env: str
    api_passphrase_env: str | None
    source: str


@dataclass(frozen=True)
class GuardExchangeAdapter:
    name: str
    credential_resolver: CredentialResolver
    requester_factory: RequesterFactory
    state_reader: StateReader
    flatten_config_builder: Callable[[GuardConfig], Any]
    flatten_runner: Callable[[Any, Any, Any], tuple[int, dict[str, Any]]]
```

`run_guarded_live()` 新增 `flatten_config_builder` 注入参数；默认仍指向现有 Gate builder。所有既有
Gate tests 和 exit code 保持不变。`parse_args()` 增加 `--exchange {gate,bitget}`，默认 `gate`；
`GuardConfig` 增加 `exchange: str = "gate"`，summary 增加 `exchange`，不改变既有字段含义。
把当前 `main()` 中从 parsed args 构造 adapter、
requester 并调用 orchestration 的部分提取为：

```python
def require_env(name: str) -> str:
    value = os.getenv(name)
    if not value:
        raise ValueError(f"missing env var {name}")
    return value


def run_from_args(
    args: argparse.Namespace,
    process_runner: ProcessRunner = run_strategy_process,
    clock: Any | None = None,
) -> tuple[int, dict[str, Any]]:
    adapter = get_guard_exchange_adapter(args.exchange)
    credentials = adapter.credential_resolver(
        strategy_command=args.strategy_command,
        explicit_api_key=args.api_key,
        explicit_api_secret=args.api_secret,
        explicit_api_passphrase=args.api_passphrase,
    )
    api_key = require_env(credentials.api_key_env)
    api_secret = require_env(credentials.api_secret_env)
    api_passphrase = (
        require_env(credentials.api_passphrase_env)
        if credentials.api_passphrase_env is not None
        else None
    )
    requester = adapter.requester_factory(
        api_key,
        api_secret,
        api_passphrase,
        args.base_url,
        args.timeout,
    )
    return run_guarded_live(
        config=config_from_args(args),
        requester=requester,
        process_runner=process_runner,
        state_reader=adapter.state_reader,
        flatten_config_builder=adapter.flatten_config_builder,
        flatten_runner=adapter.flatten_runner,
        clock=clock,
    )
```

`main()` 只负责调用 `run_from_args()`、打印 summary 并返回 exit code，使 CLI 行为可直接单元测试。

不要在此 Task 引入 Bitget adapter 行为；本提交只完成可测试的抽象与 Gate regression。

- [ ] **Step 4：运行 Gate regression，确认 GREEN**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/guard_exchange_adapter_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/emergency_flatten_futures_test.py
```

Expected: 全部 PASS。

- [ ] **Step 5：提交 Task 4**

```bash
git add scripts/lead_lag/guard_exchange_adapter.py \
  scripts/lead_lag/run_live_with_guard.py \
  scripts/test/lead_lag/guard_exchange_adapter_test.py \
  scripts/test/lead_lag/run_live_with_guard_test.py
git commit -m "refactor: add LeadLag guard exchange adapters"
```

---

### Task 5：接入 Bitget guard credential、state 与 flatten

**Files:**

- Modify: `scripts/lead_lag/guard_exchange_adapter.py`
- Modify: `scripts/lead_lag/run_live_with_guard.py`
- Modify: `scripts/test/lead_lag/guard_exchange_adapter_test.py`
- Modify: `scripts/test/lead_lag/run_live_with_guard_test.py`

- [ ] **Step 1：写 Bitget adapter 的失败测试**

```python
def write_bitget_config_graph(base, route1_passphrase="BITGET_TEST_PASSPHRASE"):
    session0 = base / "session0.toml"
    session1 = base / "session1.toml"
    gateway = base / "gateway.toml"
    strategy = base / "strategy.toml"
    write_text(
        session0,
        """
        [order_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "BITGET_TEST_PASSPHRASE"
        """,
    )
    write_text(
        session1,
        f"""
        [order_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "{route1_passphrase}"
        """,
    )
    write_text(
        gateway,
        f"""
        [order_gateway]
        route_count = 2

        [[order_gateway.routes]]
        name = "route0"
        order_session_config = "{session0}"

        [[order_gateway.routes]]
        name = "route1"
        order_session_config = "{session1}"
        """,
    )
    write_text(
        strategy,
        f"""
        [strategy.order_gateway]
        config = "{gateway}"
        """,
    )
    return strategy, gateway


def test_bitget_gateway_credentials_require_matching_passphrase(self):
    with TemporaryDirectory() as tmp:
        strategy, _ = write_bitget_config_graph(Path(tmp))
        credentials = adapters.resolve_strategy_credentials(
            exchange="bitget",
            strategy_command=["lead_lag_strategy", "--config", str(strategy)],
        )
        self.assertEqual(credentials.api_key_env, "BITGET_TEST_KEY")
        self.assertEqual(credentials.api_secret_env, "BITGET_TEST_SECRET")
        self.assertEqual(credentials.api_passphrase_env, "BITGET_TEST_PASSPHRASE")


def test_bitget_routes_must_use_same_three_credentials(self):
    with TemporaryDirectory() as tmp:
        _, gateway = write_bitget_config_graph(
            Path(tmp), route1_passphrase="OTHER_PASSPHRASE"
        )
        with self.assertRaisesRegex(ValueError, "route 1 credentials"):
            adapters.resolve_order_gateway_credentials(
                exchange="bitget", gateway_config_path=gateway
            )


def test_bitget_nonzero_strategy_exit_runs_bitget_flatten(self):
    flatten_runner = FakeFlattenRunner(
        (guard.FLATTEN_EXIT_OK, {"ok": True, "result": "verified_flat"})
    )
    exit_code, summary = guard.run_guarded_live(
        config=replace(config(), exchange="bitget"),
        requester=lambda request: {},
        process_runner=FakeProcessRunner(guard.ProcessResult(exit_code=10)),
        state_reader=FakeStateReader([flat_state()]),
        flatten_config_builder=lambda guard_config: {
            "exchange": guard_config.exchange
        },
        flatten_runner=flatten_runner,
    )
    self.assertEqual(exit_code, guard.EXIT_EMERGENCY_FLATTENED)
    self.assertEqual(summary["exchange"], "bitget")
```

- [ ] **Step 2：运行 tests，确认 RED**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/guard_exchange_adapter_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

Expected: FAIL，原因是 Bitget adapter 尚未注册。

- [ ] **Step 3：实现 Bitget adapter 与 CLI credential resolution**

Bitget adapter：

- 从 `strategy.order_gateway.config` 遍历所有 route 的 order-session config；
- 读取 `api_key_env`、`api_secret_env`、`api_passphrase_env`，要求所有 route 完全一致；
- CLI 显式 override 使用 `--api-key`、`--api-secret`、`--api-passphrase`，Bitget 必须三项同时出现；
- 创建 `SignedBitgetTradingClient`；
- `state_reader` 调用 Bitget helper 的 allowlist query；
- `flatten_config_builder` 生成 Bitget `FlattenConfig(scope="allowlist")`；
- `flatten_runner` 调用 Bitget `run_emergency_flatten()`。

Gate 显式 override 仍只要求 key/secret；传 `--api-passphrase` 给 Gate 时返回 config error，防止 CLI
含义模糊。Summary 的 `credentials` 只记录 env 名称和 source，不读取或输出 secret/passphrase 内容。

- [ ] **Step 4：运行 Gate + Bitget guard tests，确认 GREEN**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/guard_exchange_adapter_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
```

Expected: 全部 PASS。

- [ ] **Step 5：提交 Task 5**

```bash
git add scripts/lead_lag/guard_exchange_adapter.py \
  scripts/lead_lag/run_live_with_guard.py \
  scripts/test/lead_lag/guard_exchange_adapter_test.py \
  scripts/test/lead_lag/run_live_with_guard_test.py
git commit -m "feat: guard Bitget LeadLag runs with REST flat"
```

---

### Task 6：生成并强制验证 fresh-run isolation

**Files:**

- Create: `scripts/lead_lag/prepare_bitget_live_run.py`
- Create: `scripts/test/lead_lag/prepare_bitget_live_run_test.py`
- Modify: `scripts/lead_lag/run_live_with_guard.py`
- Modify: `scripts/test/lead_lag/run_live_with_guard_test.py`

- [ ] **Step 1：写 overlay 与 validation 的失败测试**

```python
def write_text(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(dedent(text).strip() + "\n", encoding="utf-8")


def write_runtime_fixture_graph(base, route_count=1):
    order_session = base / "bitget_order_session.toml"
    gateway = base / "bitget_order_gateway.toml"
    feedback = base / "bitget_order_feedback.toml"
    strategy = base / "bitget_strategy.toml"
    write_text(
        order_session,
        """
        [order_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "BITGET_TEST_PASSPHRASE"
        """,
    )
    routes = "\n".join(
        dedent(
            f"""
            [[order_gateway.routes]]
            name = "route{route}"
            order_session_config = "{order_session}"
            worker_cpu_id = 16
            """
        ).strip()
        for route in range(route_count)
    )
    write_text(
        gateway,
        f"""
        [order_gateway]
        name = "bitget_order_gateway"
        shm_name = "aquila_bitget_order_gateway"
        route_count = {route_count}
        command_queue_capacity = 4096
        event_queue_capacity = 8192
        startup_ready_timeout_s = 30

        {routes}
        """,
    )
    write_text(
        feedback,
        """
        [order_feedback_session.credentials]
        api_key_env = "BITGET_TEST_KEY"
        api_secret_env = "BITGET_TEST_SECRET"
        api_passphrase_env = "BITGET_TEST_PASSPHRASE"

        [order_feedback_session.shm]
        shm_name = "aquila_bitget_order_feedback"
        channel_name = "orders"
        """,
    )
    write_text(
        strategy,
        f"""
        [strategy]
        mode = "live"

        [strategy.order_gateway]
        config = "{gateway}"

        [strategy.feedback]
        enabled = true
        shm_name = "aquila_bitget_order_feedback"
        channel_name = "orders"
        """,
    )
    return strategy, gateway, feedback


def test_prepare_generates_run_specific_gateway_and_feedback_shm(self):
    strategy, gateway, feedback = write_runtime_fixture_graph(self.output_dir)
    result = prepare.prepare_runtime_configs(
        run_id="20260713_010203_bitget_smoke",
        strategy_source=strategy,
        gateway_source=gateway,
        feedback_source=feedback,
        output_dir=self.output_dir,
    )
    self.assertEqual(result.gateway_shm, "aquila_bitget_order_gateway_20260713_010203_bitget_smoke")
    self.assertEqual(result.feedback_shm, "aquila_bitget_order_feedback_20260713_010203_bitget_smoke")
    strategy_data = tomllib.loads(result.strategy_config.read_text(encoding="utf-8"))
    self.assertEqual(strategy_data["strategy"]["feedback"]["shm_name"], result.feedback_shm)
    self.assertEqual(strategy_data["strategy"]["order_gateway"]["config"], str(result.gateway_config))


def test_guard_rejects_fixed_shm_for_bitget_execute(self):
    strategy, gateway, feedback = write_runtime_fixture_graph(self.output_dir)
    manifest = self.output_dir / "fixed_manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "schema": "aquila.bitget_lead_lag_live_manifest.v1",
                "run_id": "20260713_010203_bitget_smoke",
                "strategy_config": str(strategy),
                "gateway_config": str(gateway),
                "feedback_config": str(feedback),
                "gateway_shm": "aquila_bitget_order_gateway",
                "feedback_shm": "aquila_bitget_order_feedback",
                "route_count": 1,
                "external_configs_applied": True,
            }
        ),
        encoding="utf-8",
    )
    with self.assertRaisesRegex(ValueError, "run isolation"):
        guard.validate_bitget_run_isolation(
            manifest,
            ["lead_lag_strategy", "--config", str(strategy), "--execute"],
        )


def test_prepare_rejects_route_count_greater_than_one(self):
    strategy, gateway, feedback = write_runtime_fixture_graph(
        self.output_dir, route_count=2
    )
    with self.assertRaisesRegex(ValueError, "route_count must be 1"):
        prepare.prepare_runtime_configs(
            run_id="20260713_010203_bitget_smoke",
            strategy_source=strategy,
            gateway_source=gateway,
            feedback_source=feedback,
            output_dir=self.output_dir,
        )
```

- [ ] **Step 2：运行 tests，确认 RED**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

Expected: FAIL，原因是 prepare script、manifest 和 isolation validation 尚不存在。

- [ ] **Step 3：实现 deterministic runtime overlay 与 manifest**

`prepare_bitget_live_run.py` 复用现有 `write_toml_overlay()`，生成：

```text
/home/liuxiang/tmp/<run_id>/configs/strategy__<source>.toml
/home/liuxiang/tmp/<run_id>/configs/bitget_gateway__<source>.toml
/home/liuxiang/tmp/<run_id>/configs/bitget_feedback__<source>.toml
/home/liuxiang/tmp/<run_id>/configs/bitget_live_manifest.json
```

Manifest 固定字段：

```json
{
  "schema": "aquila.bitget_lead_lag_live_manifest.v1",
  "run_id": "20260713_010203_bitget_smoke",
  "strategy_config": "/home/liuxiang/tmp/20260713_010203_bitget_smoke/configs/strategy.toml",
  "gateway_config": "/home/liuxiang/tmp/20260713_010203_bitget_smoke/configs/gateway.toml",
  "feedback_config": "/home/liuxiang/tmp/20260713_010203_bitget_smoke/configs/feedback.toml",
  "gateway_shm": "aquila_bitget_order_gateway_<run_id>",
  "feedback_shm": "aquila_bitget_order_feedback_<run_id>",
  "route_count": 1,
  "external_configs_applied": false
}
```

Prepare 命令不连接交易所、不读取账户、不创建 SHM。Operator 启动 manifest 指向的 feedback/gateway
配置后，通过独立命令把 `external_configs_applied` 原子更新为 `true`；更新命令必须重新读取三个
TOML，验证 path、SHM、route_count 和 account credential env 名称一致后才允许写入。

`run_live_with_guard.py --exchange bitget --runtime-manifest <path>` 在执行 strategy 前重新验证：

- manifest schema/run_id；
- 路径都位于 `/home/liuxiang/tmp/<run_id>/configs/`；
- `external_configs_applied=true`；
- gateway/feedback SHM 都带 run_id 且与 strategy config 完全一致；
- `route_count=1`；
- strategy command 的 `--config` 等于 manifest strategy config；
- gateway/feedback/guard credentials 属于同一账户。

实现公开函数：

```python
def validate_bitget_run_isolation(
    manifest_path: Path,
    strategy_command: list[str],
) -> dict[str, Any]:
    """Return the validated manifest or raise ValueError before REST access."""
```

- [ ] **Step 4：运行 isolation 与 guard regression，确认 GREEN**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/guard_exchange_adapter_test.py
```

Expected: 全部 PASS。

- [ ] **Step 5：提交 Task 6**

```bash
git add scripts/lead_lag/prepare_bitget_live_run.py \
  scripts/test/lead_lag/prepare_bitget_live_run_test.py \
  scripts/lead_lag/run_live_with_guard.py \
  scripts/test/lead_lag/run_live_with_guard_test.py
git commit -m "feat: isolate Bitget LeadLag live runs"
```

---

### Task 7：文档迁移、最终验证与完成态计划清理

**Files:**

- Modify: `docs/bitget_trading.md`
- Modify: `docs/lead_lag_live_operations.md`
- Modify: `docs/lead_lag_reconcile_design.md`
- Modify: `docs/project_onboarding_guide.md`
- Delete: `docs/superpowers/specs/2026-07-13-bitget-lead-lag-stop-and-flat-design.md`
- Delete: `docs/superpowers/plans/2026-07-13-bitget-lead-lag-stop-and-flat.md`

- [ ] **Step 1：运行所有 Python focused tests**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/account/query_bitget_account_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/gate/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/guard_exchange_adapter_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

Expected: 全部 PASS，无 traceback 或 warning。

- [ ] **Step 2：运行 C++ focused regression**

```bash
ctest --test-dir build/debug -R '^bitget_(order|operation)' --output-on-failure
ctest --test-dir build/debug -R '(order_gateway|order_feedback|trading_runtime|lead_lag)' --output-on-failure
```

Expected: 全部 selected tests PASS。若当前 build 不包含最新提交，先运行：

```bash
TMPDIR=/home/liuxiang/tmp ./build.sh debug
```

- [ ] **Step 3：执行非 mutating CLI 验证**

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/bitget/trading/place_futures_order.py --help
/home/liuxiang/dev/pyenv/lx/bin/python scripts/bitget/trading/emergency_flatten_futures.py --help
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/prepare_bitget_live_run.py --help
/home/liuxiang/dev/pyenv/lx/bin/python scripts/lead_lag/run_live_with_guard.py --help
```

Expected: exit code 0；不访问网络、不读取账户、不提交订单。

- [ ] **Step 4：把稳定事实迁移到领域文档**

文档必须使用中文，并精确记录：

- V1 不修改 `local_order_id`，以 strict stop-and-flat + fresh-run isolation 替代 unique-ID P0；
- strategy-only restart 禁止；完整交易栈共享 run boundary；
- Bitget helper/guard/prepare CLI、exit code、summary 与 run directory；
- REST flat predicate、allowlist/dedicated-account scope 和 fail-closed 行为；
- 已完成的自动验证命令与尚未执行的 live evidence；
- `fanout=1` gateway passive IOC 和 LeadLag 真实订单仍需分别获得当次用户授权；
- outer guard `SIGKILL`、主机失效、REST 全不可用仍为明确残余风险。

Onboarding 只保留摘要、入口、验证命令和下一步，不复制实现细节。

- [ ] **Step 5：删除完成态 design/plan 并做文档检查**

```bash
git rm docs/superpowers/specs/2026-07-13-bitget-lead-lag-stop-and-flat-design.md
git rm docs/superpowers/plans/2026-07-13-bitget-lead-lag-stop-and-flat.md
git diff --check
rg '#include "evaluation/' core exchange tools
rg 'aquila_evaluation' core exchange tools
```

Expected: `git diff --check` 无输出；两条 evaluation boundary 命令无命中。

- [ ] **Step 6：提交最终文档迁移**

```bash
git add docs/bitget_trading.md docs/lead_lag_live_operations.md \
  docs/lead_lag_reconcile_design.md docs/project_onboarding_guide.md
git commit -m "docs: document Bitget LeadLag stop and flat"
```

- [ ] **Step 7：记录 live 证据门但不自动执行真实订单**

实现完成后只报告以下 gated 顺序，不在本计划执行阶段自动运行：

```text
read-only REST baseline
→ emergency dry-run
→ 用户单独授权 flat-account helper smoke
→ 用户单独授权 tiny-position stop-and-flat smoke
→ 用户单独授权 fanout=1 gateway passive IOC
→ 用户单独授权 signal-conditioned LeadLag smoke
```

在 tiny-position、gateway 或 LeadLag 任一真实订单授权缺失时，任务完成声明只能覆盖代码、自动测试
和非 mutating 验证，不能宣称已有 Bitget live safety/fillability/latency evidence。
