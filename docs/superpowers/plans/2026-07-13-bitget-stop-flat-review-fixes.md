# Bitget Stop-and-Flat Review 修复实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 消除 Bitget stop-and-flat 的 gateway 延迟写单、非原子 REST snapshot、分页/限流和账户绑定缺口。

**Architecture:** Bitget manifest v2 绑定外部 gateway/feedback 进程；共享外围 guard 在 REST cleanup 前执行 mutation quiescence。Emergency helper 使用完整分页、`open-orders → positions → open-orders` 证明和按 symbol 批量撤单。

**Tech Stack:** Python 3、`unittest`、Linux `/proc`、Bitget UTA REST、现有 CMake/CTest。

---

### Task 1: 完整且保守的 Bitget REST flat snapshot

**Files:**
- Modify: `scripts/bitget/trading/emergency_flatten_futures.py`
- Modify: `scripts/lead_lag/run_live_with_guard.py`
- Test: `scripts/test/bitget/trading/emergency_flatten_futures_test.py`
- Test: `scripts/test/lead_lag/run_live_with_guard_test.py`

- [ ] **Step 1: 写失败测试**

增加 cursor 两页、cursor 循环、allowlist 本地过滤，以及第一次 open-orders 为空、position flat、第二次 open-orders 出现订单时不得 flat 的测试。Guard 测试断言 Bitget state reader 的请求顺序为 `orders, positions, orders`。

- [ ] **Step 2: 验证 RED**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

Expected: 新增断言因缺少 cursor 和第二次 open-orders 查询失败。

- [ ] **Step 3: 最小实现**

为 open-orders request 增加 `limit=100` 和可选 cursor；完整遍历页面并检测 cursor 循环。新增共享 flat snapshot helper，固定执行两次 open-orders 与一次 positions，并让 guard/helper 都使用该顺序。

- [ ] **Step 4: 验证 GREEN**

Run 同 Step 2。Expected: 全部通过。

- [ ] **Step 5: 提交**

```bash
git add scripts/bitget/trading/emergency_flatten_futures.py scripts/lead_lag/run_live_with_guard.py scripts/test/bitget/trading/emergency_flatten_futures_test.py scripts/test/lead_lag/run_live_with_guard_test.py
git commit -m "fix: make Bitget flat snapshots conservative"
```

### Task 2: 批量撤单与冷路径限流

**Files:**
- Modify: `scripts/bitget/trading/emergency_flatten_futures.py`
- Modify: `scripts/test/bitget/trading/emergency_flatten_futures_test.py`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: 写失败测试**

覆盖 allowlist 按 symbol 调用 `cancel-symbol-order`、dedicated-account category cancel、空初始订单仍执行范围撤单、响应逐项失败，以及多 symbol/position 请求按 5/s 和 10/s pacing。

- [ ] **Step 2: 验证 RED**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
```

Expected: endpoint、summary 和 pacing 断言失败。

- [ ] **Step 3: 最小实现**

新增 cancel-symbol request builder、逐项响应校验和基于注入 clock 的 pacing；close 下单复用同一 pacing helper。Summary 记录每次范围撤单动作，不记录 credential。

- [ ] **Step 4: 验证 GREEN 并同步字段文档**

Run 同 Step 2。Expected: 全部通过。更新 `docs/diagnostic_fields.md` 的撤单动作字段。

- [ ] **Step 5: 提交**

```bash
git add scripts/bitget/trading/emergency_flatten_futures.py scripts/test/bitget/trading/emergency_flatten_futures_test.py docs/diagnostic_fields.md
git commit -m "fix: bound Bitget emergency cancellation rate"
```

### Task 3: Manifest v2 外部进程和账户绑定

**Files:**
- Modify: `scripts/lead_lag/prepare_bitget_live_run.py`
- Modify: `scripts/lead_lag/run_live_with_guard.py`
- Test: `scripts/test/lead_lag/prepare_bitget_live_run_test.py`
- Test: `scripts/test/lead_lag/run_live_with_guard_test.py`

- [ ] **Step 1: 写失败测试**

使用临时 fake proc tree 覆盖 PID、start time、exe、cmdline、config、`--connect` 和 credential env 一致性；覆盖旧 manifest、非默认 live REST base URL和 PID reuse 拒绝。

- [ ] **Step 2: 验证 RED**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

Expected: v2 process binding API 尚不存在或缺少校验。

- [ ] **Step 3: 最小实现**

将 schema 升级到 v2；`mark-applied` 要求 gateway/feedback PID，读取 `/proc` 并记录 start time。Guard 再次校验绑定进程与当前 credential 值，但 summary/manifest 不保存 secret。Bitget `--execute` 只接受默认生产 REST base URL。

- [ ] **Step 4: 验证 GREEN**

Run 同 Step 2。Expected: 全部通过。

- [ ] **Step 5: 提交**

```bash
git add scripts/lead_lag/prepare_bitget_live_run.py scripts/lead_lag/run_live_with_guard.py scripts/test/lead_lag/prepare_bitget_live_run_test.py scripts/test/lead_lag/run_live_with_guard_test.py
git commit -m "fix: bind Bitget live manifests to processes"
```

### Task 4: Mutation quiescence 与有界子进程停止

**Files:**
- Modify: `scripts/lead_lag/run_live_with_guard.py`
- Test: `scripts/test/lead_lag/run_live_with_guard_test.py`
- Modify: `docs/diagnostic_fields.md`

- [ ] **Step 1: 写失败测试**

覆盖 gateway grace 内退出、TERM 后退出、KILL 后退出、身份变化不误杀新 PID、无法退出时不调用 REST/flatten；覆盖 feedback 停止和 strategy TERM 超时升级 KILL。

- [ ] **Step 2: 验证 RED**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
```

Expected: quiescence API 不存在或 guard 仍直接查询 REST。

- [ ] **Step 3: 最小实现**

在 process runner 与 guard 之间增加冷路径 quiescer。Strategy 返回后先停止 manifest 绑定的 gateway/feedback；只有 quiescence 成功才进入 final check/flatten。失败返回 exit `11` 和结构化 summary。

- [ ] **Step 4: 验证 GREEN 并同步字段文档**

Run 同 Step 2。Expected: 全部通过。记录 `runtime_isolation.quiescence` 字段。

- [ ] **Step 5: 提交**

```bash
git add scripts/lead_lag/run_live_with_guard.py scripts/test/lead_lag/run_live_with_guard_test.py docs/diagnostic_fields.md
git commit -m "fix: quiesce Bitget producers before REST flat"
```

### Task 5: 领域文档、完整验证和完成态清理

**Files:**
- Modify: `docs/bitget_trading.md`
- Modify: `docs/lead_lag_live_operations.md`
- Modify: `docs/lead_lag_reconcile_design.md`
- Modify: `docs/project_onboarding_guide.md`
- Delete after migration: `docs/superpowers/specs/2026-07-13-bitget-stop-flat-review-fixes-design.md`
- Delete after migration: `docs/superpowers/plans/2026-07-13-bitget-stop-flat-review-fixes.md`

- [ ] **Step 1: 同步当前事实源**

记录 manifest v2、PID/config/account binding、quiescence、完整分页、flat snapshot 顺序、exit code 和新的启动命令；不把完整推导复制到 onboarding。

- [ ] **Step 2: 删除完成态 spec/plan**

确认所有有效 contract 已迁移到领域文档后删除本次临时 spec/plan，保持一个当前事实源。

- [ ] **Step 3: 完整验证**

Run:

```bash
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/place_futures_order_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/bitget/trading/emergency_flatten_futures_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/prepare_bitget_live_run_test.py
/home/liuxiang/dev/pyenv/lx/bin/python scripts/test/lead_lag/run_live_with_guard_test.py
cmake --build build/debug -j 2
ctest --test-dir build/debug --output-on-failure
git diff --check
```

Expected: build 和所有测试通过，diff check 无输出。

- [ ] **Step 4: 提交文档**

```bash
git add docs
git commit -m "docs: update Bitget stop and flat operations"
```

- [ ] **Step 5: 新一轮只读 review**

重新检查 mutation barrier、REST 状态转换、PID reuse、unknown result、分页/限流和文档命令；只有一轮没有 Critical/Important 时结束。
