# Bitget Stop-and-Flat Review 修复设计

## 目标

修复多轮 review 新发现的 flat 证明竞态，使 Bitget LeadLag V1 只有在订单生产者已经停止、REST 查询覆盖完整 open orders、且 gateway、feedback、guard 绑定到同一轮配置和账户时，才能把运行结果标记为 flat。

本轮不引入跨进程持久化订单 ID，不恢复旧订单后继续交易，也不改变策略热路径。仍沿用已经批准的 strict stop-and-flat：停止完整交易栈、撤销范围内订单、reduce-only 平仓、REST 证明 flat、保持停机，下一轮使用新的 `run_id`。

## 已验证的问题

1. `OrderGatewayClient::Stop()` 只尝试写入 `kStop`，不等待 gateway 停止；strategy 子进程退出后 guard 会立即查询 REST。排队命令可能晚于 flat snapshot 到达交易所。
2. Bitget guard 和 emergency helper 先查 position、后查 open orders。开仓挂单在两次查询之间成交时，可能组合出旧的 flat position 与新的空 open-orders。
3. UTA open-orders 使用 cursor 分页，默认和最大单页均为 100；当前 helper 忽略 cursor。逐单撤单也无法稳定满足接口限速。
4. fresh-run manifest 只比较 credential env 名，未绑定实际外部进程、启动代次和当前 credential 值；真实 `--execute` 仍允许任意 REST base URL。
5. strategy 收到终止信号后如果不退出，外围 guard 会无限等待，无法进入 cleanup。

## 方案比较

### 方案 A：共享外围 guard 增加显式进程 barrier（采用）

`mark-applied` 记录并验证 gateway/feedback PID、进程启动代次、可执行文件和实际 config。Bitget guard 在 strategy 退出后先等待 gateway 因 `kStop` 自行退出；超时后依次发送 `SIGTERM`、`SIGKILL` 并等待。Feedback 同样在 REST cleanup 前停止。只有两个绑定进程都已消失，才允许 final REST check 或 emergency flatten。

优点是能证明 mutation producer 已停止，不修改交易热路径；PID reuse 通过 `/proc/<pid>/stat` start time 防护。缺点是 Linux-specific，但当前实盘环境和现有 SHM/PID 运维本来就是 Linux-specific。

### 方案 B：只等待固定时间

实现简单，但不能证明 gateway 已停止，队列满、进程卡住或网络延迟时仍会产生 false flat，因此拒绝。

### 方案 C：让 C++ strategy 同步等待 `kStopped`

可以缩小竞态，但 gateway 卡死时 strategy 仍无法保证进程退出，外围 guard 也无法安全接管；同时需要改变共享 runtime shutdown contract。当前先采用方案 A，后续可独立评估 C++ shutdown acknowledgement。

## 详细设计

### Fresh-run manifest v2

`prepare` 继续只生成配置，不读取账户、不创建 SHM。`mark-applied` 新增必填 `--gateway-pid` 和 `--feedback-pid`，并验证：

- PID 为当前存活进程，记录 `/proc/<pid>/stat` 的 start time，防止 PID reuse；
- executable basename 分别为 `bitget_order_gateway`、`bitget_order_feedback_session`；
- command line 包含 `--connect`，且 `--config` 精确等于 manifest 中对应 config；
- gateway 与 feedback config 的 category、position mode、margin mode和 private WebSocket endpoint 一致；
- gateway、feedback 进程环境中的三个 credential 值相同；任何 secret 都不写入 manifest、summary 或日志。

Bitget 真实 `--execute` 再次验证进程身份和当前 guard credential 值一致，并只允许默认生产 REST base URL。旧 v1 manifest fail closed，不做兼容迁移，因为 V1 尚未执行真实订单，且每轮本来就必须生成新 manifest。

### Mutation barrier

外围 guard 在 strategy 返回或抛异常后执行一次 quiescence：

1. 等待 gateway 在 grace period 内因 `kStop` 自行退出；
2. 仍存活则发送 `SIGTERM` 并等待；
3. 仍存活则发送 `SIGKILL` 并等待；
4. feedback 依次执行 `SIGTERM`、`SIGKILL`；
5. PID 不存在或 start time 已变化表示原绑定进程已经退出，不向复用该 PID 的新进程发信号；
6. 无法证明两个进程均停止时返回 guard exit `11`，不生成新的 flat 成功证明，并进入人工 handoff。

strategy 子进程自身也使用有界 `SIGTERM`/`SIGKILL`，避免无限等待。

### REST flat snapshot

所有 Bitget flat 判断使用以下顺序：

1. 完整查询 open orders（遍历 cursor）；
2. 查询 positions；
3. 再次完整查询 open orders；
4. 两次 open orders 都为空，且所有 position 的 `total/available/frozen` 均为 0，才能判定 flat。

查询按 category 获取全量数据，再在本地过滤 allowlist，避免 12 symbols 时按 symbol 放大 REST QPS。Cursor 必须为字符串、不得循环，页数超过交易所最大订单数推导的上限时 fail closed。

### 撤单和限流

Allowlist 模式使用 `/api/v3/trade/cancel-symbol-order` 按 symbol 撤单；dedicated-account 模式使用 category-wide cancel。即使首次快照为空，也执行一次范围撤单，覆盖查询与撤单之间出现的订单。撤单响应逐项校验；任何非成功结果走 unknown-result 后的独立 REST flat 验证。

多 symbol 撤单按 5 req/s 限速；多 position close 按 10 req/s 限速。Cleanup 始终优先正确性和确定性，不把这些 sleep 放入交易热路径。

## 测试

- 状态化 requester 模拟“position 查询后开仓挂单成交”，旧实现必须产生失败测试；修复后不能报告 flat。
- 覆盖 cursor 多页、cursor 循环、allowlist 本地过滤、第二次 open-orders 发现订单。
- 覆盖 cancel-symbol request、dedicated category cancel、逐项失败、5 req/s pacing。
- 覆盖 manifest PID/exe/cmdline/config/start-time/env 校验和 PID reuse。
- 覆盖 gateway 正常自行退出、TERM 后退出、KILL 后退出、无法退出时禁止 REST、feedback shutdown。
- 覆盖 strategy 忽略 TERM 后升级 KILL。
- 运行现有 Gate/Bitget helper、guard、gateway/runtime CTest 和完整 CTest，确保 Gate 路径不回归。

## 明确边界

- `/proc` 不可读、权限不足、PID 身份不一致或 credential 无法比较时一律 fail closed。
- 主机失效、guard 被 `SIGKILL` 或操作系统无法终止进程仍是人工 handoff 边界。
- 本轮只提供自动测试和非联网验证，不证明真实成交、真实 API 限速或 live stop-and-flat 已通过。
