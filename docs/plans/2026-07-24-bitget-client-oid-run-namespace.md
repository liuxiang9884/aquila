# Bitget `clientOid` 跨 run 隔离实施计划

## 目标

修复 P8 live 暴露的 Bitget `clientOid` 跨进程 run 确定性复用问题，使 Aquila 发出的普通
Bitget 订单都携带可审计、固定宽度的 run namespace，并让 operation response 与 order
feedback 只进入创建该订单的 run。

本项只关闭
`docs/lead_lag_fixed_ordered_slot_pool_parallel.md` 中的 P8-01。P8-02/P8-03 及后续问题仍按
既定顺序单独处理。P8-01..P8-08 未关闭并取得 fresh 非实盘证据前，不合并 PR #13，也不
再次启动 `parallel > 1` 真实订单。

## 非目标

- 不改变 `uint64_t local_order_id`、`LocalOrderIdCodec`、`strategy_id` lane 或 SHM ABI。
- 不为 run namespace 建立全局注册表、落盘查重或交易所侧预查询。
- 不兼容发布旧 `a-<decimal local_order_id>` feedback；新 run 不消费旧格式。
- 不改变 emergency REST helper 的 `a-flat-...` 身份空间。
- 不增加 account limiter，不发送真实订单，不声明 P8-02/P8-03 已解决。

## 已锁定 contract

### Wire 格式

普通 Aquila Bitget 订单统一使用：

```text
a1-<run_namespace_base32>-<local_order_id_base36>
```

总长度固定为 29：

- `[0..2]`：固定 schema 与分隔符 `a1-`；
- `[3..14]`：12 字符 `run_namespace`；
- `[15]`：固定分隔符 `-`；
- `[16..28]`：13 字符 `local_order_id`，不足位左侧补 `0`。

`run_namespace` 使用大写 Crockford Base32
`0123456789ABCDEFGHJKMNPQRSTVWXYZ`，表示完整 60 bit。`local_order_id` 使用大写 Base36
`0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ`；13 位足以无损表示 `uint64_t`，parser 必须拒绝
溢出。

### 唯一性与审计边界

- fresh-run prepare 用 OS CSPRNG 生成一次 60-bit namespace，并以 12 位 Crockford
  Base32 写入 manifest、所有 route 的 run-scoped order-session config 和 feedback
  config。
- 不做查重。这里提供的是概率唯一性，不宣称数学上的全局唯一；一百万个 run 至少一次
  碰撞的近似概率约为 `4.3e-7`。
- manifest 固定记录 `client_oid_schema = "a1"` 与
  `client_oid_run_namespace = "<12 chars>"`。gateway、feedback、guard 对 manifest 和所有
  runtime config 做 exact equality 校验，缺失、保留模板值或不一致时 fail closed。
- 审计链为：固定宽度 `clientOid` → namespace + `local_order_id` → archived manifest 的
  namespace + `run_id` → 结构化订单与 execution-group 日志。
- checked-in session config 使用不可下单的保留模板 namespace；prepare 必须替换。登录诊断
  仍可读取模板，但 place/cancel encoder 必须拒绝模板值。

### 回报隔离

- operation response 必须同时匹配 request id、当前 run namespace 和
  `local_order_id`；namespace 不匹配按 correlation failure 处理。
- order feedback 中，合法 `a1` 且 namespace 与当前 run 相同的记录才允许发布到 SHM。
- 合法 `a1` 但 namespace 不同：记录 `foreign_run_namespace` 诊断并忽略，不触发
  continuity failure。
- 旧 `a-<decimal local_order_id>`：记录 `legacy_client_oid_ignored` 并忽略，不发布、不触发
  continuity failure。
- 非 Aquila `clientOid`：沿用忽略语义。
- 前缀和 namespace 命中当前 run、但长度、分隔符、字符集、Base36 或数值边界非法：视为
  当前 run 的不可恢复 malformed feedback，触发既有 continuity failure。

## 实施阶段

### 阶段 1：codec 与 config contract

1. 先增加会失败的 codec/config 测试，覆盖固定长度、固定 offset、字母表、`0`、
   `UINT64_MAX`、溢出、非法字符、非法长度、保留模板和缺失 namespace。
2. 在 Bitget trading 层增加无动态分配的 namespace value type 与固定宽度 codec。
3. 为 order-session / feedback-session config 增加 namespace，并让下单 encoder 显式接收
   已配置 namespace。
4. 保留 login-only 诊断能力，但任何普通订单 producer 使用保留模板时在发送前 fail closed。

### 阶段 2：operation response 与 feedback 隔离

1. 先增加 response/parser/SHM integration 的失败测试：
   - own namespace；
   - foreign namespace；
   - legacy；
   - non-Aquila；
   - 当前 namespace 下 malformed；
   - 只有 own namespace 进入对应 strategy lane。
2. operation response correlation 加入 namespace。
3. feedback parser 在解析业务字段前完成身份分类；foreign/legacy/non-Aquila 不污染
   continuity，当前 run malformed 保持 fail closed。
4. 增加并文档化必要的 stats / log key；日志保存完整 `client_oid`，不修改 SHM payload。

### 阶段 3：fresh-run prepare 与工具入口

1. LeadLag 与 gateway-smoke prepare 使用 `secrets.randbits(60)` 生成 namespace。
2. 每个 gateway route 生成独立的 run-scoped order-session TOML overlay，内容使用同一
   namespace；feedback overlay 使用同一值。
3. manifest schema 升级并记录 schema/namespace及新增 config 路径与 digest；prepare 后和
   guard 启动前都验证 manifest、所有 route、feedback exact equality。
4. gateway、feedback session、gateway smoke、RTT probe 全部采用新 codec。RTT probe 的
   live-preflight/execute 要求调用方显式提供非模板 namespace，并写入 metadata；工具不在
   execute 时静默生成不可审计 namespace。
5. emergency helper 保留 `a-flat-...`，并用测试/文档明确它不进入普通订单路由。

### 阶段 4：文档、性能与完整非实盘验证

1. 更新 Bitget trading contract、live operations、diagnostic fields、P8 状态与 onboarding
   摘要；不把实现推导和完整测试输出堆入 onboarding。
2. 保存修改前后的 encoder/parser microbenchmark 与生产形态 submit/feedback benchmark。
   本项不要求性能改善，但不得在低延迟主路径引入动态分配、锁或不可解释的尾延迟回归。
3. 运行 Debug、Release、ASAN focused tests，Python prepare/guard tests，以及 production-like
   gateway/feedback SHM integration。所有证据均为离线或 `orders_sent=0`，不得连接交易所。
4. 独立 review codec 数值边界、namespace ownership、parser 分类顺序、config/manifest
   fail-closed 和日志证据；修复后重新验证。

## 失败证据与验收

实现前必须由新增测试证明当前代码至少缺少：

- 两次 fresh prepare 产生不同 namespace，且相同 `local_order_id` 的 `clientOid` 不相交；
- 固定 29 字符 wire contract 与 `uint64_t` round trip；
- foreign/legacy feedback 不进入当前 SHM lane；
- 当前 namespace 的 malformed feedback 触发 continuity failure；
- manifest、所有 gateway route 与 feedback namespace 不一致时 guard 拒绝；
- 保留模板 namespace 不能编码 place/cancel；
- operation response 中 namespace 不匹配不能完成当前 request。

完成态需同时满足：

- 上述测试在 Debug、Release 和 ASAN focused 配置通过；
- 两个独立 Python prepare 进程的 fresh 输出包含不同 namespace；
- 所有 Bitget 普通订单 producer 的全仓检索只使用新 codec；
- fresh benchmark 未发现超出既有噪声带的主路径回归；
- `git diff --check`、相关 focused/full tests 与文档 contract 检查通过；
- 不发送真实订单，P8-01 状态只依据本轮 fresh 非实盘证据关闭。

## 风险与回滚

- 最大风险是 producer/consumer 只升级一侧，导致合法订单无法 correlation 或 feedback 被
  丢弃；通过 required config、manifest exact equality 和无 legacy 双读避免半升级进入 live。
- Base36 溢出或字符集实现错误会把回报路由到错误 local order；必须以边界测试和
  `from_chars` 等无分配、可检测溢出的实现约束。
- 多 route 使用不同 namespace 会把同一 run 分裂；prepare 只生成一次并复制，guard 逐 route
  比对。
- 若验证失败，回滚本项全部代码与 config/manifest schema；不得恢复旧格式后直接启动
  `parallel > 1` live，P8-01 继续保持 open。
