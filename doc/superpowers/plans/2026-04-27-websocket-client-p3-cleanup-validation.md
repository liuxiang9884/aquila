# WebSocket Client P3 Cleanup Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close WebSocket client P3 by verifying the build graph, refreshing final documentation, and recording v1.0 validation evidence before merging back to `dev` / `main`.

**Architecture:** P3 is a documentation and validation phase. It should not change the WebSocket hot-path implementation unless the G11 build graph audit finds a real build-system defect; otherwise it records the existing `aquila_core` STATIC library shape and closes the remaining review item.

**Tech Stack:** C++20, CMake, GoogleTest, Google Benchmark, OpenSSL, Linux.

---

## Scope

P3 covers:

- G11 build graph audit and closure.
- Roadmap / handoff refresh after P2-B completion.
- README build, test, benchmark, probe, and validation guide refresh.
- Final debug / release test evidence.
- Release benchmark evidence for the existing WebSocket benchmark set.
- A documented long-run validation command and a short smoke run in this branch; a full multi-hour run can be executed before merging to `main` if required by release policy.

P3 does not cover:

- New TLS read/write benchmarks.
- Active spin mixed benchmarks.
- Mask key pool / RNG optimization.
- Read/write budget policy changes.
- Any new exchange protocol behavior.

Those are tracked in `doc/websocket_client_future_optimizations.md`.

## File Structure

- Modify: `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
  - Close G11 with exact build graph evidence.
  - Update summary so G1-G11 status is complete.
- Modify: `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`
  - Mark P2-B completed.
  - Add the P2-B plan/spec documents to associations.
  - Replace “P2-B next” with “P3 cleanup / validation”.
- Modify: `doc/agent-handoff-p1-websocket.md`
  - Convert the current top section from “P2-B next” to “P3 next / current”.
  - Keep historical P1 content clearly marked as archive.
- Modify: `README.md`
  - Add `ctest` commands.
  - Add current WebSocket benchmark list.
  - Add low-jitter benchmark guidance.
  - Add live probe and long-run validation commands.
- Create or modify: `doc/superpowers/plans/2026-04-27-websocket-client-p3-cleanup-validation.md`
  - This plan.

## Task 1: G11 Build Graph Audit

**Files:**
- Read: `CMakeLists.txt`
- Read: `core/CMakeLists.txt`
- Read: `core/websocket/CMakeLists.txt`
- Read: `core/aquila_core.cpp`
- Modify: `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`

- [ ] **Step 1: Confirm root CMake test/build entry**

Run:

```bash
sed -n '1,80p' CMakeLists.txt
```

Expected evidence:

```text
enable_testing()
add_subdirectory(core)
add_subdirectory(tools)
add_subdirectory(test)
add_subdirectory(benchmark)
```

- [ ] **Step 2: Confirm `aquila_core` library shape**

Run:

```bash
sed -n '1,80p' core/CMakeLists.txt
```

Expected evidence:

```text
add_library(aquila_core STATIC
    aquila_core.cpp
)
```

Also confirm `aquila_core` links:

```text
Threads::Threads
fmt::fmt-header-only
OpenSSL::SSL
OpenSSL::Crypto
```

- [ ] **Step 3: Confirm websocket headers attach to `aquila_core`**

Run:

```bash
sed -n '1,120p' core/websocket/CMakeLists.txt
```

Expected evidence:

```text
target_sources(aquila_core
    PRIVATE
        active_spin_loop.h
        ...
        websocket_client.h
)
```

If new headers are missing from this list, add them in this task. At current P2-B state, verify these headers are present:

```text
degraded_evaluator.h
frame_codec_types.h
frame_parser.h
mirrored_buffer.h
queued_frame_codec.h
reconnect_classifier.h
runtime_clock.h
```

- [ ] **Step 4: Confirm build anchor includes all public websocket headers**

Run:

```bash
sed -n '1,120p' core/aquila_core.cpp
```

Expected evidence: `core/aquila_core.cpp` includes every public `core/websocket/*.h` header needed to compile the STATIC library anchor.

If missing headers are found, add only the missing includes and do not refactor the file.

- [ ] **Step 5: Update G11 review section**

Edit `doc/reviews/2026-04-24-websocket-client-gap-analysis.md` under `### G11：构建图疑问（待核实）` and append:

```markdown
- 处理方案：fix（核实关闭）— 当前构建图保持 `aquila_core` 为 STATIC library，`core/aquila_core.cpp` 作为 build anchor，`core/websocket/CMakeLists.txt` 通过 `target_sources(aquila_core PRIVATE ...)` 把 header-only websocket 实现纳入 IDE / install graph 可见范围。该形态与最初 plan 的 STATIC 目标一致，不需要改成 INTERFACE。
- 验证证据：`CMakeLists.txt` 顶层已 `enable_testing()` 并加入 `core` / `tools` / `test` / `benchmark`；`core/CMakeLists.txt` 定义 `add_library(aquila_core STATIC aquila_core.cpp)` 并公开链接 `Threads::Threads`、`fmt::fmt-header-only`、`OpenSSL::SSL`、`OpenSSL::Crypto`；`core/websocket/CMakeLists.txt` 已列入当前 websocket headers；`core/aquila_core.cpp` include websocket public headers 作为编译锚点。debug / release build 与 websocket ctest 在 P3 验证阶段执行。
- 确认日期：2026-04-27
```

- [ ] **Step 6: Commit G11 closure**

Run:

```bash
git add doc/reviews/2026-04-24-websocket-client-gap-analysis.md core/websocket/CMakeLists.txt core/aquila_core.cpp
git commit -m "doc: close websocket build graph review"
```

If `core/websocket/CMakeLists.txt` and `core/aquila_core.cpp` did not change, stage only the review document.

## Task 2: Roadmap And Handoff Refresh

**Files:**
- Modify: `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`
- Modify: `doc/agent-handoff-p1-websocket.md`

- [ ] **Step 1: Update roadmap header**

In `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`, change:

```markdown
- 版本：`v0.2`
- 状态：`进行中`（P0 / P1 / P2-A 已完成，下一步 P2-B）
```

to:

```markdown
- 版本：`v0.3`
- 状态：`P3 收尾中`（P0 / P1 / P2-A / P2-B 已完成，当前 P3）
```

- [ ] **Step 2: Update associated documents**

Add these lines to the association list:

```markdown
- `doc/superpowers/plans/2026-04-26-websocket-client-p2b-hotpath-pacing.md`（Phase 2-B）
- `doc/superpowers/specs/2026-04-26-websocket-client-p2b-hotpath-pacing-design.md`（Phase 2-B 设计）
- `doc/websocket_client_future_optimizations.md`（P3 后优化 backlog）
```

- [ ] **Step 3: Update Gap → Phase table**

Replace the P2-B rows:

```markdown
| G2 | `DriveRead` 单次 `ReadSome` | **P2-B** | 待建（热路径节奏） |
| G4 | 心跳与业务写无协调 | **P2-B** | 同上 |
| G8 | 心跳粒度绑在 spin iteration | **P2-B** | 同上 |
| G11 | 构建图形态核对 | **P3** | 待建（或合并进 P2 收尾） |
```

with:

```markdown
| G2 | `DriveRead` 单次 `ReadSome` | **P2-B** | `2026-04-26-websocket-client-p2b-hotpath-pacing.md` |
| G4 | 心跳与业务写无协调 | **P2-B** | 同上 |
| G8 | 心跳粒度绑在 spin iteration | **P2-B** | 同上 |
| G11 | 构建图形态核对 | **P3** | `2026-04-27-websocket-client-p3-cleanup-validation.md` |
```

- [ ] **Step 4: Replace Phase 2-B status**

Change the Phase 2-B heading from:

```markdown
### Phase 2-B：热路径节奏（读循环 + 心跳 + 时钟）（下一步）
```

to:

```markdown
### Phase 2-B：热路径节奏（读循环 + 心跳 + 时钟）（已完成）
```

Replace the “核心决策 / 产出证据” draft wording with a completed summary:

```markdown
**已落地能力**：

- G2：`ConnectionConfig::max_reads_per_drive` / `read_until_would_block`，`CriticalSession::DriveRead()` bounded read pump，`TlsSocket::PendingReadableBytes()` 基于 `SSL_pending()`。
- G4：dedicated control write slot，业务 queue 满时 heartbeat ping / auto-pong 不再消耗业务 `PreparedWriteArena` slot；partial business frame 不被 control frame 打断。
- G8：`ClockSource` 与 `runtime_clock.h`，`ActiveSpinLoop` 支持 runtime clock source，`RuntimeSession` 复用 loop clock 做 heartbeat 和 degraded evaluation。
- 验证证据已回填到 review G2 / G4 / G8；P3 之后的性能优化候选项记录在 `doc/websocket_client_future_optimizations.md`。
```

- [ ] **Step 5: Update Phase 3 section**

Change:

```markdown
## Phase 3 — 收尾（待建）
```

to:

```markdown
## Phase 3 — 收尾（当前）
```

Set the plan path to:

```markdown
**plan**：`doc/superpowers/plans/2026-04-27-websocket-client-p3-cleanup-validation.md`
```

- [ ] **Step 6: Update bottom checklist**

Replace:

```markdown
- [ ] P2-B：立项并关闭 G2 / G4 / G8
- [ ] P3：关闭 G11，补长稳运行证据和最终交付文档
```

with:

```markdown
- [x] P2-B：关闭 G2 / G4 / G8
- [ ] P3：关闭 G11，补验证证据和最终交付文档
```

- [ ] **Step 7: Refresh handoff top section**

In `doc/agent-handoff-p1-websocket.md`, replace the initial “P2-B next” summary with:

```markdown
# Agent 接手说明：P1/P2 归档与 P3 接手提示

## 当前下一步

P0、P1、P2-A、P2-B 已完成并合入 `dev`。当前下一步是 **P3：收尾 / 验证 / 文档闭环**。

P3 的工作入口：

1. 从最新 `dev` 切出 `p3-cleanup-validation`。
2. 执行 `doc/superpowers/plans/2026-04-27-websocket-client-p3-cleanup-validation.md`。
3. 关闭 G11 构建图核对。
4. 更新 README、roadmap、review 文档。
5. 跑 debug / release build、websocket ctest、关键 release benchmark，并记录验证证据。
```

Keep the historical P1 details below and keep them clearly marked as archive.

- [ ] **Step 8: Commit roadmap/handoff refresh**

Run:

```bash
git add doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md doc/agent-handoff-p1-websocket.md
git commit -m "doc: refresh websocket p3 roadmap"
```

## Task 3: README Validation Guide Refresh

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add ctest command**

Under build instructions, add:

````markdown
Run the WebSocket regression suite with:

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
```

For release validation:

```bash
./build.sh release
ctest --test-dir build/release -R websocket_ --output-on-failure
```
````

- [ ] **Step 2: Update benchmark target list**

Ensure the WebSocket benchmark target list includes:

```text
prepared_write_benchmark
frame_codec_benchmark
third_party_frame_codec_comparison_benchmark
degraded_evaluator_benchmark
active_spin_benchmark
session_write_path_benchmark
session_read_path_benchmark
clock_source_benchmark
runtime_loopback_benchmark
affinity_policy_comparison_benchmark
cold_path_handshake_benchmark
```

- [ ] **Step 3: Add recommended release benchmark commands**

Add:

````markdown
Recommended P3 release benchmark smoke run:

```bash
taskset -c 2 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/active_spin_benchmark
taskset -c 2 ./build/release/benchmark/websocket/clock_source_benchmark
taskset -c 2 ./build/release/benchmark/websocket/runtime_loopback_benchmark
```
````

- [ ] **Step 4: Add long-run validation guidance**

Add:

````markdown
For long-run validation before merging to `main`, run a bounded live probe or
loopback runtime job under `timeout` and record final metrics:

```bash
timeout 4h ./build/release/tools/websocket_probe \
  --host fx-ws.gateio.ws \
  --port 443 \
  --target /v4/ws/usdt \
  --tls \
  --cpu 2
```

If external network access is not part of the validation environment, run the
local loopback integration test and release benchmarks instead, and explicitly
record that live exchange evidence was not collected in that environment.
````

- [ ] **Step 5: Commit README refresh**

Run:

```bash
git add README.md
git commit -m "doc: update websocket validation guide"
```

## Task 4: Final Verification Evidence

**Files:**
- Modify: `doc/reviews/2026-04-24-websocket-client-gap-analysis.md`
- Modify: `doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md`

- [ ] **Step 1: Run debug build**

Run:

```bash
./build.sh debug
```

Expected: command exits 0 and prints `Build completed successfully!`.

- [ ] **Step 2: Run debug websocket tests**

Run:

```bash
ctest --test-dir build/debug -R websocket_ --output-on-failure
```

Expected: all websocket tests pass.

- [ ] **Step 3: Run release build**

Run:

```bash
./build.sh release
```

Expected: command exits 0 and prints `Build completed successfully!`.

- [ ] **Step 4: Run release websocket tests**

Run:

```bash
ctest --test-dir build/release -R websocket_ --output-on-failure
```

Expected: all websocket tests pass.

- [ ] **Step 5: Run P3 release benchmark smoke set**

Run:

```bash
taskset -c 2 ./build/release/benchmark/websocket/session_read_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/session_write_path_benchmark
taskset -c 2 ./build/release/benchmark/websocket/active_spin_benchmark
taskset -c 2 ./build/release/benchmark/websocket/clock_source_benchmark
taskset -c 2 ./build/release/benchmark/websocket/runtime_loopback_benchmark
```

Expected: each benchmark exits 0 and reports `p50_ns`, `p99_ns`, and `p999_ns` counters.

- [ ] **Step 6: Optional live probe smoke**

Run only when network access is allowed:

```bash
timeout 15s ./build/debug/tools/websocket_probe \
  --host fx-ws.gateio.ws \
  --port 443 \
  --target /v4/ws/usdt \
  --tls \
  --cpu 2
```

Expected: the probe reaches `state=kActive` before `timeout` terminates it. Record exit code 124 as timeout-after-success if active was reached.

- [ ] **Step 7: Record verification evidence**

Append a P3 verification note to roadmap Phase 3:

```markdown
**P3 验证证据（2026-04-27）**：

- `./build.sh debug`：通过。
- `ctest --test-dir build/debug -R websocket_ --output-on-failure`：通过，记录通过数量。
- `./build.sh release`：通过。
- `ctest --test-dir build/release -R websocket_ --output-on-failure`：通过，记录通过数量。
- release benchmark smoke：记录每个 benchmark 的 p50 / p99 / p99.9。
- live probe：记录是否执行；如未执行，写明原因。
```

- [ ] **Step 8: Commit verification evidence**

Run:

```bash
git add doc/reviews/2026-04-24-websocket-client-gap-analysis.md doc/superpowers/plans/2026-04-24-websocket-client-review-roadmap.md
git commit -m "doc: record websocket p3 validation"
```

## Task 5: Branch Finalization

**Files:**
- None unless verification finds defects.

- [ ] **Step 1: Confirm clean status**

Run:

```bash
git status --short --branch
```

Expected:

```text
## p3-cleanup-validation
```

with no modified files.

- [ ] **Step 2: Push P3 branch**

Run:

```bash
git push -u origin p3-cleanup-validation
```

- [ ] **Step 3: Merge back to dev after review**

Run after P3 branch is accepted:

```bash
git checkout dev
git merge p3-cleanup-validation
git push origin dev
```

- [ ] **Step 4: Defer main merge until final approval**

Do not merge to `main` until the user explicitly confirms the P3 validation evidence is sufficient.

## Self-Review

- Spec coverage: G11, roadmap refresh, README validation guide, debug/release tests, release benchmark smoke, live probe guidance, and branch finalization are covered.
- Placeholder scan: no `TBD` / `TODO` placeholders are used.
- Scope check: TLS read/write benchmarks and active spin mixed benchmarks are intentionally excluded and remain in `doc/websocket_client_future_optimizations.md`.
